#include "triton/Analysis/Utility.h"
#include "mlir/Analysis/SliceAnalysis.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "triton/Dialect/TritonGPU/IR/Dialect.h"
#include "triton/Dialect/TritonGPU/Transforms/Utility.h"
#include <fstream>

namespace mlir {

SmallVector<unsigned, 3> mmaVersionToInstrShape(int version,
                                                const ArrayRef<int64_t> &shape,
                                                RankedTensorType type) {
  if (version == 1)
    return {16, 16};
  else if (version == 2)
    return {16, 8};
  else if (version == 3) {
    unsigned k = 256 / type.getElementTypeBitWidth();
    if (shape[0] % 64 != 0 || shape[1] % 8 != 0) {
      assert(false && "type not supported");
      return {0, 0, 0};
    }
    auto eltType = type.getElementType();
    SmallVector<unsigned> validN;

    // MMAv3 with larger instruction shape is preferred.
    if (eltType.isFloat8E5M2() || eltType.isFloat8E4M3FNUZ() ||
        eltType.isF16() || eltType.isBF16() || eltType.isF32()) {
      validN.assign({256, 248, 240, 232, 224, 216, 208, 200, 192, 184, 176,
                     168, 160, 152, 144, 136, 128, 120, 112, 104, 96,  88,
                     80,  72,  64,  56,  48,  40,  32,  24,  16,  8});
    }

    if (eltType.isInteger(8)) {
      validN.assign({224, 208, 192, 176, 160, 144, 128, 112, 96, 80, 64, 48, 32,
                     24, 16, 8});
    }

    for (auto n : validN) {
      if (shape[1] % n == 0) {
        return {16, n, k};
      }
    }

    assert(false && "type not supported");
    return {0, 0, 0};
  } else {
    assert(false && "version not supported");
    return {0, 0};
  }
}

bool isLoadFromTensorPtr(triton::LoadOp op) {
  return mlir::triton::isTensorPointerType(op.getPtr().getType());
}

bool isStoreToTensorPtr(triton::StoreOp op) {
  return mlir::triton::isTensorPointerType(op.getPtr().getType());
}

Operation *getFirstUser(Value v) {
  DenseMap<Operation *, size_t> operationId;
  v.getParentBlock()->walk<WalkOrder::PostOrder>(
      [&](Operation *op) { operationId[op] = operationId.size(); });
  size_t minId = std::numeric_limits<size_t>::max();
  Operation *firstUser = nullptr;
  for (Operation *user : v.getUsers()) {
    assert(operationId.find(user) != operationId.end());
    size_t userId = operationId[user];
    if (userId < minId) {
      minId = userId;
      firstUser = user;
    }
  }
  assert(firstUser);
  return firstUser;
}

triton::gpu::SharedEncodingAttr getSharedEncoding(RankedTensorType tensorTy) {
  auto blockedLayout =
      tensorTy.getEncoding().cast<triton::gpu::BlockedEncodingAttr>();
  return triton::gpu::SharedEncodingAttr::get(
      tensorTy.getContext(), tensorTy.getShape(), blockedLayout.getOrder(),
      blockedLayout.getCTALayout(), tensorTy.getElementType());
}

//===----------------------------------------------------------------------===//
// GraphDumper
//===----------------------------------------------------------------------===//

GraphDumper::NodeInfo GraphDumper::onValue(Value value) const {
  return {{"shape", "box"}, {"style", "filled"}, {"fillcolor", "white"}};
}

GraphDumper::NodeInfo GraphDumper::onOperation(Operation *op) const {
  return {{"shape", "ellipse"}, {"style", "filled"}, {"fillcolor", "white"}};
}

std::string GraphDumper::dump(triton::FuncOp func) const {
  llvm::SetVector<Value> values;
  llvm::SetVector<Operation *> operations;

  func.walk([&](Operation *op) {
    operations.insert(op);
    for (Value operand : op->getOperands())
      values.insert(operand);
    for (Value result : op->getResults())
      values.insert(result);
  });

  std::ostringstream oss;
  oss << "// Generated by Triton GraphDumper\n"
      << "\n"
      << "digraph {\n";

  oss << "    // Value Nodes\n";
  for (Value value : values)
    oss << "    " << emitValueNode(value) << "\n";
  oss << "\n";

  oss << "    // Operation Nodes\n";
  for (Operation *op : operations)
    oss << "    " << emitOperationNode(op) << "\n";
  oss << "\n";

  oss << "    // Edges\n";
  for (Operation *op : operations) {
    for (Value operand : op->getOperands())
      oss << "    " << emitEdge(getUniqueId(operand), getUniqueId(op)) << "\n";
    for (Value result : op->getResults())
      oss << "    " << emitEdge(getUniqueId(op), getUniqueId(result)) << "\n";
  }

  oss << "}\n";
  return oss.str();
}

void GraphDumper::dumpToFile(triton::FuncOp func,
                             const std::string &filename) const {
  std::ofstream ofs(filename);
  ofs << dump(func);
}

std::string GraphDumper::getShapeStr(const Type &type) const {
  std::ostringstream oss;
  oss << "[";
  if (auto tensorTy = type.dyn_cast<RankedTensorType>()) {
    auto shape = tensorTy.getShape();
    for (unsigned i = 0; i < shape.size(); ++i) {
      if (i > 0)
        oss << ", ";
      oss << shape[i];
    }
  }
  oss << "]";
  return oss.str();
}

std::string GraphDumper::getUniqueId(Value value) const {
  std::ostringstream oss;
  oss << value.getImpl();
  return oss.str();
}

std::string GraphDumper::getUniqueId(Operation *op) const {
  std::ostringstream oss;
  oss << op;
  return oss.str();
}

std::string GraphDumper::emitNode(const std::string &id,
                                  const GraphDumper::NodeInfo info) const {
  std::ostringstream oss;
  oss << "\"" << id << "\" [";
  for (auto it = info.begin(); it != info.end(); ++it) {
    if (it != info.begin())
      oss << ", ";
    oss << it->first << " = \"" << it->second << "\"";
  }
  oss << "];";
  return oss.str();
}

std::string GraphDumper::emitEdge(const std::string &srcId,
                                  const std::string &destId) const {
  std::ostringstream oss;
  oss << "\"" << srcId << "\" -> \"" << destId << "\";";
  return oss.str();
}

std::string GraphDumper::emitValueNode(Value value) const {
  NodeInfo info = onValue(value);
  if (info.find("label") == info.end()) {
    std::string shapeStr = getShapeStr(value.getType());
    if (auto arg = value.dyn_cast<BlockArgument>())
      info["label"] =
          "BlockArg" + std::to_string(arg.getArgNumber()) + " " + shapeStr;
    else
      info["label"] = shapeStr;
  }
  return emitNode(getUniqueId(value), info);
}

std::string GraphDumper::emitOperationNode(Operation *op) const {
  NodeInfo info = onOperation(op);
  if (info.find("label") == info.end())
    info["label"] = op->getName().getStringRef().str();
  return emitNode(getUniqueId(op), info);
}

//===----------------------------------------------------------------------===//
// GraphLayoutMarker
//===----------------------------------------------------------------------===//

GraphDumper::NodeInfo GraphLayoutMarker::onValue(Value value) const {
  std::string color = getColor(value.getType());
  return {{"shape", "box"}, {"style", "filled"}, {"fillcolor", color}};
}

std::string GraphLayoutMarker::getColor(const Type &type) const {
  if (auto tensorTy = type.dyn_cast<RankedTensorType>()) {
    auto layout = tensorTy.getEncoding();
    if (layout.isa<triton::gpu::BlockedEncodingAttr>())
      return "green";
    else if (layout.isa<triton::gpu::SliceEncodingAttr>())
      return "yellow";
    else if (layout.isa<triton::gpu::MmaEncodingAttr>())
      return "lightslateblue";
    else if (layout.isa<triton::gpu::DotOperandEncodingAttr>())
      return "orange";
    else if (layout.isa<triton::gpu::SharedEncodingAttr>())
      return "orangered";
    else
      assert(0 && "Unrecognized layout");
  } else {
    return "white";
  }
}
// -------------------------------------------------------------------------- //

static std::optional<Attribute> inferDstEncoding(triton::ReduceOp op,
                                                 Attribute encoding) {
  return triton::gpu::SliceEncodingAttr::get(op->getContext(), op.getAxis(),
                                             encoding);
}

static std::optional<Attribute> inferDstEncoding(triton::ExpandDimsOp op,
                                                 Attribute encoding) {
  auto sliceEncoding = encoding.dyn_cast<triton::gpu::SliceEncodingAttr>();
  if (!sliceEncoding)
    return std::nullopt;
  if (op.getAxis() != sliceEncoding.getDim())
    return std::nullopt;
  return sliceEncoding.getParent();
}

static std::optional<Attribute> inferSrcEncoding(triton::ReduceOp op,
                                                 Attribute encoding) {
  auto sliceEncoding = encoding.dyn_cast<triton::gpu::SliceEncodingAttr>();
  if (!sliceEncoding)
    return std::nullopt;
  if (op.getAxis() != sliceEncoding.getDim())
    return std::nullopt;
  return sliceEncoding.getParent();
}

static std::optional<Attribute> inferSrcEncoding(triton::ExpandDimsOp op,
                                                 Attribute encoding) {
  return triton::gpu::SliceEncodingAttr::get(op->getContext(), op.getAxis(),
                                             encoding);
}

std::optional<Attribute> inferSrcEncoding(Operation *op, Attribute encoding) {
  if (auto reduceOp = dyn_cast<triton::ReduceOp>(op))
    return inferSrcEncoding(reduceOp, encoding);
  if (auto expand = dyn_cast<triton::ExpandDimsOp>(op))
    return inferSrcEncoding(expand, encoding);
  if (isa<triton::ViewOp, triton::CatOp>(op))
    return std::nullopt;
  return encoding;
}

std::optional<Attribute> inferDstEncoding(Operation *op, Attribute encoding) {
  if (auto reduceOp = dyn_cast<triton::ReduceOp>(op))
    return inferDstEncoding(reduceOp, encoding);
  if (auto expand = dyn_cast<triton::ExpandDimsOp>(op))
    return inferDstEncoding(expand, encoding);
  if (isa<triton::ViewOp, triton::CatOp>(op))
    return std::nullopt;
  return encoding;
}

bool isExpensiveLoadOrStore(Operation *op) {
  // Case 1: Pointer of tensor is always expensive
  auto operandType = op->getOperand(0).getType();
  if (triton::isTensorPointerType(operandType))
    return true;
  // Case 2a: A size 1 tensor is not expensive since all threads will load the
  // same
  if (isSingleValue(op->getOperand(0)))
    return false;
  // Case 2b: Tensor of pointers has more threads than elements
  // we can presume a high hit-rate that makes it cheap to load
  auto ptrType = op->getOperand(0).getType().cast<RankedTensorType>();
  auto mod = op->getParentOfType<ModuleOp>();
  int numWarps = triton::gpu::TritonGPUDialect::getNumWarps(mod);
  int threadsPerWarp = triton::gpu::TritonGPUDialect::getThreadsPerWarp(mod);
  if (ptrType.getNumElements() < numWarps * threadsPerWarp)
    return false;
  return true;
}

bool isExpensiveToRemat(Operation *op, Attribute &targetEncoding) {
  if (!op)
    return true;
  if (isa<triton::LoadOp, triton::StoreOp>(op))
    return isExpensiveLoadOrStore(op);
  if (isa<triton::CatOp>(op))
    return triton::gpu::isExpensiveCat(cast<triton::CatOp>(op), targetEncoding);
  if (isa<tensor::ExtractSliceOp, triton::gpu::AllocTensorOp,
          triton::gpu::InsertSliceAsyncOp, triton::AtomicRMWOp,
          triton::AtomicCASOp, triton::DotOp>(op))
    return true;
  if (isa<scf::YieldOp, scf::ForOp, scf::IfOp, scf::WhileOp, scf::ConditionOp>(
          op))
    return true;
  return false;
}

bool canFoldIntoConversion(Operation *op, Attribute targetEncoding) {
  if (isa<triton::CatOp>(op))
    return !triton::gpu::isExpensiveCat(cast<triton::CatOp>(op),
                                        targetEncoding);
  if (auto convert = dyn_cast<triton::gpu::ConvertLayoutOp>(op)) {
    if (targetEncoding.isa<triton::gpu::MmaEncodingAttr>()) {
      auto srcEncoding =
          convert.getOperand().getType().cast<RankedTensorType>().getEncoding();
      if (targetEncoding != srcEncoding)
        return false;
    }
    return true;
  }
  return isa<triton::gpu::ConvertLayoutOp, arith::ConstantOp,
             triton::MakeRangeOp, triton::SplatOp, triton::ViewOp>(op);
}

//

Operation *cloneWithInferType(mlir::OpBuilder &rewriter, Operation *op,
                              IRMapping &mapping) {
  Operation *newOp = rewriter.clone(*op, mapping);
  // if input types haven't changed, we're done
  bool preserveTypes =
      std::all_of(op->operand_begin(), op->operand_end(), [&](Value v) {
        return !mapping.contains(v) ||
               v.getType() == mapping.lookup(v).getType();
      });
  if (preserveTypes)
    return newOp;

  if (newOp->getNumResults() == 0)
    return newOp;
  auto origType = op->getResult(0).getType().dyn_cast<RankedTensorType>();
  auto argType = newOp->getOperand(0).getType().dyn_cast<RankedTensorType>();
  if (!origType || !argType)
    return newOp;
  auto newType = RankedTensorType::get(
      origType.getShape(), origType.getElementType(), argType.getEncoding());
  newOp->getResult(0).setType(newType);
  auto typeInfer = dyn_cast<InferTypeOpInterface>(newOp);
  if (typeInfer) {
    SmallVector<Type, 1> newTypes;
    auto success = typeInfer.inferReturnTypes(
        newOp->getContext(), newOp->getLoc(), newOp->getOperands(),
        newOp->getAttrDictionary(), newOp->getPropertiesStorage(),
        newOp->getRegions(), newTypes);
    if (succeeded(success)) {
      for (size_t i = 0; i < newTypes.size(); i++)
        newOp->getResult(i).setType(newTypes[i]);
    }
  }
  return newOp;
}

LogicalResult
getConvertBackwardSlice(Value root, SetVector<Value> &slice,
                        Attribute rootEncoding,
                        DenseMap<Value, Attribute> &layout,
                        std::function<bool(Operation *)> stopPropagation) {
  SmallVector<std::pair<Value, Attribute>> queue = {{root, rootEncoding}};
  while (!queue.empty()) {
    auto [currentValue, encoding] = queue.back();
    queue.pop_back();
    if (!currentValue.getType().isa<RankedTensorType>())
      continue;
    // Skip propagating through for op results for now.
    // TODO: enable this based on needs.
    if (currentValue.getDefiningOp<scf::ForOp>())
      return failure();
    slice.insert(currentValue);
    layout[currentValue] = encoding;
    if (auto *definingOp = currentValue.getDefiningOp()) {
      if (canFoldIntoConversion(definingOp, encoding))
        continue;
      if (stopPropagation && stopPropagation(definingOp))
        continue;
      if (isa<triton::CatOp>(definingOp))
        return failure();
      for (Value operand : definingOp->getOperands()) {
        auto srcEncoding = inferSrcEncoding(definingOp, encoding);
        if (!srcEncoding)
          return failure();
        if (slice.count(operand) == 0)
          queue.push_back({operand, *srcEncoding});
      }
      continue;
    }
    auto blockArg = cast<BlockArgument>(currentValue);
    Block *block = blockArg.getOwner();
    Operation *parentOp = block->getParentOp();
    if (auto forOp = dyn_cast<scf::ForOp>(parentOp)) {
      OpOperand &initOperand = forOp.getOpOperandForRegionIterArg(blockArg);
      Value yieldOperand = forOp.getBody()->getTerminator()->getOperand(
          blockArg.getArgNumber() - forOp.getNumInductionVars());
      queue.push_back({initOperand.get(), encoding});
      queue.push_back({yieldOperand, encoding});
      continue;
    }
    // TODO: add support for WhileOp and other region types.
    return failure();
  }
  return success();
}

// TODO(thomas): this is duplicated with what is in GPUToLLVM
//  Convert an \param index to a multi-dim coordinate given \param shape and
//  \param order.
SmallVector<Value> delinearize(OpBuilder &b, Location loc, Value linear,
                               ArrayRef<unsigned> shape,
                               ArrayRef<unsigned> order) {
  unsigned rank = shape.size();
  assert(rank == order.size());
  auto reordered = reorder(shape, order);
  auto reorderedMultiDim = delinearize(b, loc, linear, reordered);
  SmallVector<Value> multiDim(rank);
  for (unsigned i = 0; i < rank; ++i) {
    multiDim[order[i]] = reorderedMultiDim[i];
  }
  return multiDim;
}

SmallVector<Value> delinearize(OpBuilder &b, Location loc, Value linear,
                               ArrayRef<unsigned> shape) {
  unsigned rank = shape.size();
  assert(rank > 0);
  SmallVector<Value> multiDim(rank);
  if (rank == 1) {
    multiDim[0] = linear;
  } else {
    Value remained = linear;
    for (auto &&en : llvm::enumerate(shape.drop_back())) {
      auto dimSize = b.create<arith::ConstantIntOp>(loc, en.value(), 32);
      multiDim[en.index()] = b.create<arith::RemSIOp>(loc, remained, dimSize);
      remained = b.create<arith::DivSIOp>(loc, remained, dimSize);
    }
    multiDim[rank - 1] = remained;
  }
  return multiDim;
}

Value linearize(OpBuilder &b, Location loc, ArrayRef<Value> multiDim,
                ArrayRef<unsigned> shape, ArrayRef<unsigned> order) {
  return linearize(b, loc, reorder<Value>(multiDim, order),
                   reorder<unsigned>(shape, order));
}

Value linearize(OpBuilder &b, Location loc, ArrayRef<Value> multiDim,
                ArrayRef<unsigned> shape) {
  auto rank = multiDim.size();
  Value linear = b.create<arith::ConstantIntOp>(loc, 0, 32);
  if (rank > 0) {
    linear = multiDim.back();
    for (auto [dim, dimShape] :
         llvm::reverse(llvm::zip(multiDim.drop_back(), shape.drop_back()))) {
      Value dimSize = b.create<arith::ConstantIntOp>(loc, dimShape, 32);
      linear = b.create<arith::AddIOp>(
          loc, b.create<arith::MulIOp>(loc, linear, dimSize), dim);
    }
  }
  return linear;
}

void getBackwardSliceSCFAware(Operation *op, SetVector<Operation *> *slices) {
  SmallVector<Operation *> queue = {op};
  while (!queue.empty()) {
    Operation *currentOp = queue.back();
    queue.pop_back();
    SetVector<Operation *> temp;
    auto filter = [slices](Operation *sliceOp) {
      return slices->count(sliceOp) == 0;
    };
    mlir::getBackwardSlice(currentOp, &temp, filter);
    for (Operation *sliceOp : temp) {
      if (auto forOp = dyn_cast<scf::ForOp>(sliceOp)) {
        queue.push_back(forOp.getBody()->getTerminator());
      }
    }
    slices->insert(temp.begin(), temp.end());
  }
}

void getForwardSliceSCFAware(Value root, SetVector<Operation *> *slices) {
  SmallVector<Value> queue = {root};
  while (!queue.empty()) {
    Value currentValue = queue.back();
    queue.pop_back();
    SetVector<Operation *> temp;
    auto filter = [slices](Operation *sliceOp) {
      return slices->count(sliceOp) == 0;
    };
    mlir::getForwardSlice(currentValue, &temp, filter);
    for (Operation *sliceOp : temp) {
      if (auto yieldOp = dyn_cast<scf::YieldOp>(sliceOp)) {
        auto forOp = yieldOp->getParentOfType<scf::ForOp>();
        if (forOp)
          queue.append(forOp->getResults().begin(), forOp->getResults().end());
      }
    }
    slices->insert(temp.begin(), temp.end());
  }
}

namespace {

/// Detect dead arguments in scf.for op by assuming all the values are dead and
/// propagate liveness property.
struct ForOpDeadArgElimination : public OpRewritePattern<scf::ForOp> {
  using OpRewritePattern<scf::ForOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(scf::ForOp forOp,
                                PatternRewriter &rewriter) const final {
    Block &block = *forOp.getBody();
    auto yieldOp = cast<scf::YieldOp>(block.getTerminator());
    // Assume that nothing is live at the beginning and mark values as live
    // based on uses.
    DenseSet<Value> aliveValues;
    SmallVector<Value> queue;
    // Helper to mark values as live and add them to the queue of value to
    // propagate if it is the first time we detect the value as live.
    auto markLive = [&](Value val) {
      if (!forOp->isAncestor(val.getParentRegion()->getParentOp()))
        return;
      if (aliveValues.insert(val).second)
        queue.push_back(val);
    };
    // Mark all yield operands as live if the associated forOp result has any
    // use.
    for (auto result : llvm::enumerate(forOp.getResults())) {
      if (!result.value().use_empty())
        markLive(yieldOp.getOperand(result.index()));
    }
    if (aliveValues.size() == forOp.getNumResults())
      return failure();
    // Operations with side-effects are always live. Mark all theirs operands as
    // live.
    block.walk([&](Operation *op) {
      if (!isa<scf::YieldOp, scf::ForOp>(op) && !wouldOpBeTriviallyDead(op)) {
        for (Value operand : op->getOperands())
          markLive(operand);
      }
    });
    // Propagate live property until reaching a fixed point.
    while (!queue.empty()) {
      Value value = queue.pop_back_val();
      if (auto nestedFor = value.getDefiningOp<scf::ForOp>()) {
        auto result = value.cast<OpResult>();
        OpOperand &forOperand = nestedFor.getOpOperandForResult(result);
        markLive(forOperand.get());
        auto nestedYieldOp =
            cast<scf::YieldOp>(nestedFor.getBody()->getTerminator());
        Value nestedYieldOperand =
            nestedYieldOp.getOperand(result.getResultNumber());
        markLive(nestedYieldOperand);
        continue;
      }
      if (auto nestedIf = value.getDefiningOp<scf::IfOp>()) {
        auto result = value.cast<OpResult>();
        for (scf::YieldOp nestedYieldOp :
             {nestedIf.thenYield(), nestedIf.elseYield()}) {
          Value nestedYieldOperand =
              nestedYieldOp.getOperand(result.getResultNumber());
          markLive(nestedYieldOperand);
        }
        continue;
      }
      if (Operation *def = value.getDefiningOp()) {
        // TODO: support while ops.
        if (isa<scf::WhileOp>(def))
          return failure();
        for (Value operand : def->getOperands())
          markLive(operand);
        continue;
      }
      // If an argument block is live then the associated yield operand and
      // forOp operand are live.
      auto arg = value.cast<BlockArgument>();
      if (auto forOwner = dyn_cast<scf::ForOp>(arg.getOwner()->getParentOp())) {
        if (arg.getArgNumber() < forOwner.getNumInductionVars())
          continue;
        unsigned iterIdx = arg.getArgNumber() - forOwner.getNumInductionVars();
        Value yieldOperand =
            forOwner.getBody()->getTerminator()->getOperand(iterIdx);
        markLive(yieldOperand);
        markLive(forOwner.getInitArgs()[iterIdx]);
      }
    }
    SmallVector<unsigned> deadArg;
    for (auto yieldOperand : llvm::enumerate(yieldOp->getOperands())) {
      if (aliveValues.contains(yieldOperand.value()))
        continue;
      if (yieldOperand.value() == block.getArgument(yieldOperand.index() + 1))
        continue;
      deadArg.push_back(yieldOperand.index());
    }
    if (deadArg.empty())
      return failure();
    rewriter.updateRootInPlace(forOp, [&]() {
      // For simplicity we just change the dead yield operand to use the
      // associated argument and leave the operations and argument removal to
      // dead code elimination.
      for (unsigned deadArgIdx : deadArg) {
        BlockArgument arg = block.getArgument(deadArgIdx + 1);
        yieldOp.setOperand(deadArgIdx, arg);
      }
    });
    return success();
  }
};

} // namespace

void populateForOpDeadArgumentElimination(RewritePatternSet &patterns) {
  patterns.add<ForOpDeadArgElimination>(patterns.getContext());
}

} // namespace mlir
