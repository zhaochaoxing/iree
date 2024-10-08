// Copyright 2024 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Dialect/Util/IR/UtilDialect.h"
#include "iree/compiler/Dialect/Util/Transforms/PassDetail.h"
#include "iree/compiler/Dialect/Util/Transforms/Passes.h"
#include "llvm/Support/Debug.h"
#include "mlir/Analysis/DataFlow/DeadCodeAnalysis.h"
#include "mlir/Analysis/DataFlow/IntegerRangeAnalysis.h"
#include "mlir/Analysis/DataFlowFramework.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Arith/Transforms/Passes.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassRegistry.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#define DEBUG_TYPE "iree-util-optimize-arithmetic"
using llvm::dbgs;

using namespace mlir::dataflow;

namespace mlir::iree_compiler::IREE::Util {

namespace {

// An index_cast from i64 to index is a no-op on targets where index is
// 64 bits. But on targets where index is 32bits, it is a truncate. On these
// platforms, demoting to an index is only conservatively correct if all
// operands and all results are within the unsigned 32bit bounds.
// While there is a good chance that such arithmetic that exceeds these
// bounds is simply wrong/overflow-ridden, we opt to do no harm and preseve
// the exact results. This optimization is targeted at "small" sequences
// anyway and this catches everything known to exist. If needed, this rule
// could be dropped if it is ever appropriate to unconditionally assume
// 64bit semantics.
static constexpr uint64_t SAFE_INDEX_UNSIGNED_MAX_VALUE =
    std::numeric_limits<uint32_t>::max();

//===----------------------------------------------------------------------===//
// Signed -> Unsigned patterns
// Note that there is an upstream UnsignedWhenEquivalent pass but it uses
// DialectConversion and legality vs simple patterns, so we cannot use it.
// Some support code has been adapted from that pass, though.
//===----------------------------------------------------------------------===//

/// Succeeds when a value is statically non-negative in that it has a lower
/// bound on its value (if it is treated as signed) and that bound is
/// non-negative.
static bool staticallyLegalToConvertToUnsigned(DataFlowSolver &solver,
                                               Value v) {
  auto *result = solver.lookupState<IntegerValueRangeLattice>(v);
  if (!result || result->getValue().isUninitialized()) {
    return false;
  }
  const ConstantIntRanges &range = result->getValue().getValue();
  bool isNonNegative = range.smin().isNonNegative();
  Type type = v.getType();
  if (isa<IndexType>(type)) {
    bool canSafelyTruncate =
        range.umin().getZExtValue() <= SAFE_INDEX_UNSIGNED_MAX_VALUE &&
        range.umax().getZExtValue() <= SAFE_INDEX_UNSIGNED_MAX_VALUE;
    return isNonNegative && canSafelyTruncate;
  } else {
    return isNonNegative;
  }
}

/// Succeeds if an op can be converted to its unsigned equivalent without
/// changing its semantics. This is the case when none of its openands or
/// results can be below 0 when analyzed from a signed perspective.
static LogicalResult
staticallyLegalToConvertToUnsignedOp(DataFlowSolver &solver, Operation *op) {
  auto nonNegativePred = [&solver](Value v) -> bool {
    bool isNonNegative = staticallyLegalToConvertToUnsigned(solver, v);
    return isNonNegative;
  };
  return success(llvm::all_of(op->getOperands(), nonNegativePred) &&
                 llvm::all_of(op->getResults(), nonNegativePred));
}

template <typename Signed, typename Unsigned>
struct ConvertOpToUnsigned : public OpRewritePattern<Signed> {
  ConvertOpToUnsigned(MLIRContext *context, DataFlowSolver &solver)
      : OpRewritePattern<Signed>(context), solver(solver) {}

  LogicalResult matchAndRewrite(Signed op,
                                PatternRewriter &rewriter) const override {
    if (failed(staticallyLegalToConvertToUnsignedOp(solver, op)))
      return failure();
    rewriter.replaceOpWithNewOp<Unsigned>(op, op->getResultTypes(),
                                          op->getOperands(), op->getAttrs());
    return success();
  }

  DataFlowSolver &solver;
};

//===----------------------------------------------------------------------===//
// Int64 -> unsigned index demotion
// Torch does a lot of indexy manipulation using scalar i64 ops. We undo these
// here and treat them as index when safe to do so. Since the casts can block
// optimizations, it can be useful to eliminate them when possible.
//===----------------------------------------------------------------------===//

struct ConvertUnsignedI64IndexCastProducerToIndex
    : public OpRewritePattern<arith::IndexCastUIOp> {
  ConvertUnsignedI64IndexCastProducerToIndex(MLIRContext *context,
                                             DataFlowSolver &solver)
      : OpRewritePattern(context), solver(solver) {}

  LogicalResult matchAndRewrite(arith::IndexCastUIOp op,
                                PatternRewriter &rewriter) const override {
    Type inType = op.getIn().getType();
    Type outType = op.getOut().getType();
    if (!inType.isSignlessInteger(64) && isa<IndexType>(outType))
      return failure();

    auto pred = [&](Value v) -> bool {
      auto *result = solver.lookupState<IntegerValueRangeLattice>(v);
      if (!result || result->getValue().isUninitialized()) {
        return false;
      }
      const ConstantIntRanges &range = result->getValue().getValue();
      bool isInBounds =
          range.umin().getZExtValue() <= SAFE_INDEX_UNSIGNED_MAX_VALUE &&
          range.umax().getZExtValue() <= SAFE_INDEX_UNSIGNED_MAX_VALUE;
      return isInBounds;
    };
    auto isOpStaticallyLegal = [&](Operation *op) -> bool {
      return llvm::all_of(op->getOperands(), pred) &&
             llvm::all_of(op->getResults(), pred);
    };

    Operation *producer = op.getIn().getDefiningOp();
    if (!isa_and_present<arith::AddIOp, arith::CeilDivUIOp, arith::DivUIOp,
                         arith::MaxUIOp, arith::MinUIOp, arith::MulIOp,
                         arith::RemUIOp, arith::SubIOp>(producer))
      return failure();
    if (!isOpStaticallyLegal(producer))
      return failure();

    rewriter.modifyOpInPlace(producer, [&]() {
      rewriter.setInsertionPoint(producer);
      for (auto &operand : producer->getOpOperands()) {
        if (operand.get().getType() != inType)
          continue;
        Value newOperand = rewriter.create<arith::IndexCastUIOp>(
            producer->getLoc(), outType, operand.get());
        operand.set(newOperand);
      }
      producer->getResult(0).setType(outType);
    });

    return success();
  }

  DataFlowSolver &solver;
};

//===----------------------------------------------------------------------===//
// Pass setup
//===----------------------------------------------------------------------===//

class DataFlowListener : public RewriterBase::Listener {
public:
  DataFlowListener(DataFlowSolver &s) : s(s) {}

protected:
  void notifyOperationErased(Operation *op) override {
    s.eraseState(op);
    for (Value res : op->getResults())
      flushValue(res);
  }
  void notifyOperationModified(Operation *op) override {
    for (Value res : op->getResults())
      flushValue(res);
  }
  void notifyOperationReplaced(Operation *op, Operation *replacement) override {
    for (Value res : op->getResults())
      flushValue(res);
  }

  void notifyOperationReplaced(Operation *op, ValueRange replacement) override {
    for (Value res : op->getResults())
      flushValue(res);
  }

  void flushValue(Value value) {
    SmallVector<Value> worklist;
    SmallVector<Value> process;
    worklist.push_back(value);

    while (!worklist.empty()) {
      process.clear();
      process.swap(worklist);
      for (Value childValue : process) {
        auto *state = s.lookupState<IntegerValueRangeLattice>(childValue);
        if (!state) {
          continue;
        }
        s.eraseState(childValue);
        for (auto user : childValue.getUsers()) {
          for (Value result : user->getResults()) {
            worklist.push_back(result);
          }
        }
      }
    }
  }

  DataFlowSolver &s;
};

class OptimizeIntArithmeticPass
    : public OptimizeIntArithmeticBase<OptimizeIntArithmeticPass> {
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<arith::ArithDialect>();
    registry.insert<IREE::Util::UtilDialect>();
  }

  void runOnOperation() override {
    Operation *op = getOperation();
    MLIRContext *ctx = op->getContext();
    DataFlowSolver solver;
    solver.load<DeadCodeAnalysis>();
    solver.load<IntegerRangeAnalysis>();
    DataFlowListener listener(solver);
    RewritePatternSet patterns(ctx);

    // Populate upstream arith patterns.
    arith::populateIntRangeOptimizationsPatterns(patterns, solver);

    // Populate canonicalization patterns.
    auto arithDialectTypeID =
        ctx->getOrLoadDialect<arith::ArithDialect>()->getTypeID();
    for (const RegisteredOperationName &name : ctx->getRegisteredOperations()) {
      if (name.getDialect().getTypeID() == arithDialectTypeID)
        name.getCanonicalizationPatterns(patterns, ctx);
    }

    // Populate unsigned conversion patterns.
    patterns.add<ConvertUnsignedI64IndexCastProducerToIndex,
                 ConvertOpToUnsigned<arith::CeilDivSIOp, arith::CeilDivUIOp>,
                 ConvertOpToUnsigned<arith::DivSIOp, arith::DivUIOp>,
                 ConvertOpToUnsigned<arith::FloorDivSIOp, arith::DivUIOp>,
                 ConvertOpToUnsigned<arith::IndexCastOp, arith::IndexCastUIOp>,
                 ConvertOpToUnsigned<arith::RemSIOp, arith::RemUIOp>,
                 ConvertOpToUnsigned<arith::MinSIOp, arith::MinUIOp>,
                 ConvertOpToUnsigned<arith::MaxSIOp, arith::MaxUIOp>,
                 ConvertOpToUnsigned<arith::ExtSIOp, arith::ExtUIOp>>(ctx,
                                                                      solver);

    GreedyRewriteConfig config;
    // Results in fewer recursive data flow flushes/cycles on modification.
    config.useTopDownTraversal = false;
    config.listener = &listener;

    FrozenRewritePatternSet frozenPatterns(std::move(patterns));
    for (int i = 0;; ++i) {
      if (failed(solver.initializeAndRun(op))) {
        emitError(op->getLoc()) << "failed to perform int range analysis";
        return signalPassFailure();
      }

      bool changed = false;
      if (failed(applyPatternsAndFoldGreedily(op, frozenPatterns, config,
                                              &changed))) {
        emitError(op->getLoc())
            << "int arithmetic optimization failed to converge on iteration "
            << i;
        return signalPassFailure();
      }

      if (!changed)
        break;
    }
  }
};

} // namespace

std::unique_ptr<OperationPass<void>> createOptimizeIntArithmetic() {
  return std::make_unique<OptimizeIntArithmeticPass>();
}

} // namespace mlir::iree_compiler::IREE::Util