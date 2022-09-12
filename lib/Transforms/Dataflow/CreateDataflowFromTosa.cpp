//===----------------------------------------------------------------------===//
//
// Copyright 2020-2021 The ScaleHLS Authors.
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/Dominance.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "scalehls/Transforms/Passes.h"
#include "scalehls/Transforms/Utils.h"

using namespace mlir;
using namespace scalehls;
using namespace hls;

/// Fuse the given operations into a new graph node. The fused node will be
/// created before the first operation and each operation will be inserted in
/// order. This method always succeeds even if the resulting IR is invalid.
static TaskOp fuseTosaOps(ArrayRef<Operation *> ops,
                          PatternRewriter &rewriter) {
  assert(!ops.empty() && "must fuse at least one op");
  SmallPtrSet<Operation *, 4> opsSet(ops.begin(), ops.end());

  SmallVector<Value, 8> inputValues;
  SmallVector<Value, 8> outputValues;
  for (auto op : ops) {
    // Collect input values.
    for (auto operand : op->getOperands())
      if (!opsSet.count(operand.getDefiningOp()))
        inputValues.push_back(operand);

    // Collect output values.
    for (auto result : op->getResults())
      if (llvm::any_of(result.getUsers(),
                       [&](Operation *user) { return !opsSet.count(user); }))
        outputValues.push_back(result);
  }

  // Create new graph node with all inputs and outputs.
  auto loc = rewriter.getUnknownLoc();
  rewriter.setInsertionPoint(ops.front());
  auto node =
      rewriter.create<TaskOp>(loc, ValueRange(outputValues), inputValues);
  auto nodeBlock = rewriter.createBlock(&node.body());

  // Replace internal input uses with node arguments.
  for (auto input : inputValues) {
    auto arg =
        nodeBlock->addArgument(input.getType(), rewriter.getUnknownLoc());
    input.replaceUsesWithIf(
        arg, [&](OpOperand &use) { return opsSet.count(use.getOwner()); });
  }

  // Replace external output uses with the node results.
  for (auto t : llvm::zip(outputValues, node.getResults()))
    std::get<0>(t).replaceUsesWithIf(std::get<1>(t), [&](OpOperand &use) {
      return !opsSet.count(use.getOwner());
    });

  // Move each targeted op into the new graph node.
  rewriter.setInsertionPointToEnd(nodeBlock);
  auto output = rewriter.create<YieldOp>(loc, outputValues);
  for (auto op : ops)
    op->moveBefore(output);
  return node;
}

namespace {
/// This pattern will outline ops with the specified type.
template <typename OpType>
struct OutlinePattern : public OpRewritePattern<OpType> {
  using OpRewritePattern<OpType>::OpRewritePattern;

  LogicalResult matchAndRewrite(OpType op,
                                PatternRewriter &rewriter) const override {
    if (op->template getParentOfType<TaskOp>())
      return failure();
    fuseTosaOps({op}, rewriter);
    return success();
  }
};
} // namespace

namespace {
/// This pattern will forward fuse ops with the specified type.
template <typename OpType>
struct ForwardFusePattern : public OpRewritePattern<OpType> {
  ForwardFusePattern(MLIRContext *context, DominanceInfo &DT,
                     PatternBenefit benefit = 1)
      : OpRewritePattern<OpType>(context, benefit), DT(DT) {}

  LogicalResult matchAndRewrite(OpType op,
                                PatternRewriter &rewriter) const override {
    if (op->template getParentOfType<TaskOp>())
      return failure();

    // We always select the dominating node as the target node to fuse.
    TaskOp targetNode;
    for (auto user : op->getUsers())
      if (auto node = dyn_cast<TaskOp>(user))
        if (!targetNode || (targetNode && DT.dominates(node, targetNode)))
          targetNode = node;

    if (targetNode)
      fuseTosaOps({op, targetNode}, rewriter);
    return success(targetNode);
  }

private:
  DominanceInfo &DT;
};
} // namespace

namespace {
/// This pattern will backward fuse ops with the specified type.
template <typename OpType>
struct BackwardFusePattern : public OpRewritePattern<OpType> {
  BackwardFusePattern(MLIRContext *context, DominanceInfo &DT,
                      PatternBenefit benefit = 1)
      : OpRewritePattern<OpType>(context, benefit), DT(DT) {}

  LogicalResult matchAndRewrite(OpType op,
                                PatternRewriter &rewriter) const override {
    if (op->template getParentOfType<TaskOp>())
      return failure();

    // We always select the dominated node as the target node to fuse.
    TaskOp targetNode;
    for (auto operand : op->getOperands())
      if (auto node = operand.template getDefiningOp<TaskOp>())
        if (!targetNode || (targetNode && DT.dominates(targetNode, node)))
          targetNode = node;

    if (targetNode)
      fuseTosaOps({targetNode, op}, rewriter);
    return success(targetNode);
  }

private:
  DominanceInfo &DT;
};
} // namespace

namespace {
/// This pattern will fuse constant ops with the specified type.
struct ConstFusePattern : public OpRewritePattern<tosa::ConstOp> {
  using OpRewritePattern<tosa::ConstOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tosa::ConstOp op,
                                PatternRewriter &rewriter) const override {
    if (op->getParentOfType<TaskOp>())
      return failure();

    bool hasChanged = false;
    for (auto &use : llvm::make_early_inc_range(op->getUses()))
      if (auto node = dyn_cast<TaskOp>(use.getOwner())) {
        rewriter.setInsertionPoint(node);
        auto newOp = cast<tosa::ConstOp>(rewriter.clone(*op));
        use.set(newOp.getResult());
        fuseTosaOps({newOp, node}, rewriter);
        hasChanged = true;
      }
    return success(hasChanged);
  }
};
} // namespace

ScheduleOp wrapWithScheduleOp(Block *block) {
  OpBuilder builder(block, block->begin());
  ValueRange returnValues(block->getTerminator()->getOperands());
  auto loc = builder.getUnknownLoc();
  auto schedule = builder.create<ScheduleOp>(loc, returnValues);

  auto &scheduleBlock = schedule.body().emplaceBlock();
  builder.setInsertionPointToEnd(&scheduleBlock);
  builder.create<ReturnOp>(loc, returnValues);

  auto &scheduleOps = scheduleBlock.getOperations();
  auto &parentOps = block->getOperations();
  scheduleOps.splice(scheduleBlock.begin(), parentOps,
                     std::next(parentOps.begin()), std::prev(parentOps.end()));

  block->getTerminator()->setOperands(schedule.getResults());
  return schedule;
}

namespace {
struct CreateDataflowFromTosa
    : public CreateDataflowFromTosaBase<CreateDataflowFromTosa> {
  void runOnOperation() override {
    auto func = getOperation();
    auto context = func.getContext();
    auto DT = DominanceInfo(func);

    mlir::RewritePatternSet patterns(context);
    patterns.add<OutlinePattern<tosa::Conv2DOp>>(context);
    patterns.add<OutlinePattern<tosa::AvgPool2dOp>>(context);
    patterns.add<OutlinePattern<tosa::MaxPool2dOp>>(context);
    patterns.add<OutlinePattern<tosa::MatMulOp>>(context);
    patterns.add<OutlinePattern<tosa::MulOp>>(context);
    patterns.add<OutlinePattern<tosa::AddOp>>(context);
    patterns.add<OutlinePattern<tosa::SubOp>>(context);
    patterns.add<OutlinePattern<tosa::RsqrtOp>>(context);
    patterns.add<BackwardFusePattern<tosa::ClampOp>>(context, DT);
    patterns.add<BackwardFusePattern<tosa::TransposeOp>>(context, DT);
    patterns.add<ForwardFusePattern<tosa::ReshapeOp>>(context, DT);
    (void)applyPatternsAndFoldGreedily(func, std::move(patterns));

    patterns.clear();
    patterns.add<OutlinePattern<tosa::TransposeOp>>(context);
    patterns.add<ConstFusePattern>(context);
    (void)applyPatternsAndFoldGreedily(func, std::move(patterns));

    wrapWithScheduleOp(&func.front());
  }
};
} // namespace

std::unique_ptr<Pass> scalehls::createCreateDataflowFromTosaPass() {
  return std::make_unique<CreateDataflowFromTosa>();
}
