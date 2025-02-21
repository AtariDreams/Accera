////////////////////////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) Microsoft Corporation. All rights reserved.
//  Licensed under the MIT License. See LICENSE in the project root for license information.
////////////////////////////////////////////////////////////////////////////////////////////////////

#include "IRUtil.h"
#include "value/ValueAttributes.h"
#include "value/ValueEnums.h"

#include <ir/include/exec/ExecutionOptions.h>
#include <ir/include/nest/LoopNestOps.h>
#include <ir/include/value/ValueDialect.h>

#include <utilities/include/StringUtil.h>

#include <mlir/Dialect/Affine/Analysis/LoopAnalysis.h>
#include <mlir/Dialect/Affine/IR/AffineOps.h>
#include <mlir/Dialect/Arithmetic/IR/Arithmetic.h>
#include <mlir/Dialect/GPU/GPUDialect.h>
#include <mlir/Dialect/LLVMIR/LLVMDialect.h>
#include <mlir/Dialect/LLVMIR/ROCDLDialect.h>
#include <mlir/Dialect/MemRef/IR/MemRef.h>
#include <mlir/Dialect/SCF/SCF.h>
#include <mlir/Dialect/SPIRV/IR/SPIRVOps.h>
#include <mlir/Dialect/StandardOps/IR/Ops.h>
#include <mlir/IR/BlockAndValueMapping.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/Transforms/GreedyPatternRewriteDriver.h>

#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/TypeSwitch.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <set>

namespace vir = accera::ir::value;

namespace
{
static std::mutex _globalInsertMutex;
}

namespace accera::ir
{
namespace util
{
    void FillCanonicalPatternsRecursively(mlir::Operation* op, mlir::RewritePatternSet& patterns)
    {
        std::set<const void*> s;
        auto context = op->getContext();

        auto opInfo = op->getRegisteredInfo();
        if (opInfo && s.count(opInfo->getAsOpaquePointer()) == 0)
        {
            opInfo->getCanonicalizationPatterns(patterns, context);
            s.insert(opInfo->getAsOpaquePointer());
        }
        op->walk([&patterns, &s, context](mlir::Operation* childOp) {
            auto childInfo = childOp->getRegisteredInfo();
            if (childInfo && s.count(childInfo->getAsOpaquePointer()) == 0)
            {
                childInfo->getCanonicalizationPatterns(patterns, context);
                s.insert(childInfo->getAsOpaquePointer());
            }
        });
    }

    void CanonicalizeGreedily(mlir::Operation* op)
    {
        mlir::RewritePatternSet patterns(op->getContext());
        FillCanonicalPatternsRecursively(op, patterns);
        (void)applyPatternsAndFoldGreedily(op, std::move(patterns));
    }

    std::vector<int64_t> ConvertArrayAttrToIntVector(const mlir::ArrayAttr& inputArrayAttr)
    {
        return ArrayAttrToVector<int64_t, mlir::IntegerAttr>(
            inputArrayAttr,
            [](const mlir::IntegerAttr& intAttr) { return intAttr.getInt(); });
    }

    std::vector<loopnest::Index> ConvertArrayAttrToIndexVector(const mlir::ArrayAttr& inputArrayAttr)
    {
        return ArrayAttrToVector<loopnest::Index, loopnest::IndexAttr>(
            inputArrayAttr,
            [](const loopnest::IndexAttr& indexAttr) { return indexAttr.getValue(); });
    }

    mlir::ArrayAttr ConvertIndexVectorToArrayAttr(const std::vector<loopnest::Index>& inputVec, mlir::MLIRContext* context)
    {
        return VectorToArrayAttr<loopnest::Index, loopnest::IndexAttr>(
            inputVec,
            [&](const loopnest::Index& index) { return loopnest::IndexAttr::get(index, context); },
            context);
    }

    mlir::Attribute GetOneAttr(mlir::OpBuilder& builder, mlir::Type type)
    {
        if (type.isa<mlir::FloatType>())
            return builder.getFloatAttr(type, 1.0);
        if (type.isa<mlir::IndexType>())
            return builder.getIndexAttr(1);
        if (auto integerType = type.dyn_cast<mlir::IntegerType>())
            return builder.getIntegerAttr(type, mlir::APInt(type.cast<mlir::IntegerType>().getWidth(), 1));
        if (type.isa<mlir::RankedTensorType, mlir::VectorType>())
        {
            auto vtType = type.cast<mlir::ShapedType>();
            auto element = GetOneAttr(builder, vtType.getElementType());
            if (!element)
                return {};
            return mlir::DenseElementsAttr::get(vtType, element);
        }
        return {};
    }

    mlir::OpBuilder MakeBodyBuilder(mlir::AffineForOp forOp)
    {
        auto& region = forOp.region();
        mlir::Block* front = &region.front();
        return { front, std::prev(front->end()) };
    }

    mlir::Value CreateStackBuffer(mlir::OpBuilder& builder, mlir::Operation* anchorOp, mlir::MemRefType bufferType, int64_t alignment)
    {
        auto funcParent = anchorOp->getParentOfType<ir::value::ValueFuncOp>();
        mlir::Block& funcBlock = funcParent.front();

        mlir::OpBuilder::InsertionGuard insertGuard(builder);
        builder.setInsertionPointToStart(&funcBlock);
        auto tmpBuffer = builder.create<mlir::memref::AllocaOp>(anchorOp->getLoc(), bufferType, mlir::ValueRange{}, builder.getI64IntegerAttr(alignment));

        return tmpBuffer;
    }

    mlir::Value CreateGlobalBuffer(mlir::OpBuilder& builder, mlir::MemRefType bufferType, const std::string& namePrefix, const bool constant, Attribute attr, bool isExternal, bool appendUniqueSuffix)
    {
        auto insertionBlock = builder.getInsertionBlock();
        auto parentOp = insertionBlock->getParentOp();
        return CreateGlobalBuffer(builder, parentOp, bufferType, namePrefix, constant, attr, isExternal, appendUniqueSuffix);
    }

    ir::value::GlobalOp CreateGlobalBufferOp(mlir::OpBuilder& builder, mlir::Operation* anchorOp, mlir::MemRefType bufferType, std::string globalName, const bool constant, Attribute attr, bool isExternal, bool appendUniqueSuffix)
    {
        auto loc = anchorOp->getLoc();
        if (appendUniqueSuffix)
            globalName += "_" + std::to_string(GetUniqueId(anchorOp));

        mlir::OpBuilder::InsertionGuard guard(builder);

        mlir::Block* body;
        if (auto moduleValue = util::CastOrGetParentOfType<ir::value::ValueModuleOp>(anchorOp))
        {
            body = moduleValue.getBody();
        }
        else if (auto moduleGPU = util::CastOrGetParentOfType<mlir::gpu::GPUModuleOp>(anchorOp))
        {
            body = moduleGPU.getBody();
        }
        else
        {
            auto moduleBase = util::CastOrGetParentOfType<mlir::ModuleOp>(anchorOp);
            assert(moduleBase && "Expected to be inside a ValueModuleOp");
            body = moduleBase.getBody();
        }

        // Lock before accessing the global scope so that multi-threaded lowerings all access the appropriate global insert position
        std::lock_guard<std::mutex> lock(_globalInsertMutex);
        builder.setInsertionPoint(body, body->begin());
        return builder.create<accera::ir::value::GlobalOp>(loc, bufferType, constant, globalName, attr, /*addrSpace*/ 0, isExternal);
    }

    mlir::Value CreateGlobalBuffer(mlir::OpBuilder& builder, mlir::Operation* anchorOp, mlir::MemRefType bufferType, const std::string& namePrefix, const bool constant, Attribute attr, bool isExternal, bool appendUniqueSuffix)
    {
        auto globalOp = CreateGlobalBufferOp(builder, anchorOp, bufferType, namePrefix, constant, attr, isExternal, appendUniqueSuffix);

        auto insertionBlock = anchorOp->getBlock();
        auto it = insertionBlock->begin();
        auto end = insertionBlock->end();
        while (it != end && llvm::isa<mlir::arith::ConstantOp,
                                      ir::value::ReferenceGlobalOp>(it))
        {
            ++it;
        }
        mlir::OpBuilder::InsertionGuard guard(builder);
        builder.setInsertionPoint(insertionBlock, it);

        auto loc = anchorOp->getLoc();
        auto reference = builder.create<accera::ir::value::ReferenceGlobalOp>(loc, globalOp);
        return reference.getResult();
    }

    mlir::Value CreateSharedBuffer(mlir::OpBuilder& builder, mlir::MemRefType bufferType, const std::string& namePrefix)
    {
        // Todo: implement this method later
        return CreateGlobalBuffer(builder, bufferType, namePrefix);
    }

    mlir::Value CreateSharedBuffer(mlir::OpBuilder& builder, mlir::Operation* anchorOp, mlir::MemRefType bufferType, const std::string& namePrefix)
    {
        auto loc = anchorOp->getLoc();
        auto insertionBlock = anchorOp->getBlock();
        auto it = insertionBlock->begin();
        auto end = insertionBlock->end();
        while (it != end && llvm::isa<mlir::arith::ConstantOp,
                                      mlir::memref::AllocOp,
                                      mlir::memref::AllocaOp,
                                      mlir::LLVM::AllocaOp,
                                      ir::value::ReferenceGlobalOp,
                                      ir::value::AllocOp>(it))
        {
            ++it;
        }
        mlir::OpBuilder::InsertionGuard guard(builder);
        builder.setInsertionPoint(insertionBlock, it);
        auto op = builder.create<mlir::memref::AllocaOp>(loc, bufferType, mlir::ValueRange{}, builder.getI64IntegerAttr(32));

        return op.getResult();
    }

    mlir::Value CreatePrivateBuffer(mlir::OpBuilder& builder, mlir::MemRefType bufferType, const std::string& namePrefix)
    {
        // Todo: implement this method later
        return CreateGlobalBuffer(builder, bufferType, namePrefix);
    }

    mlir::Value CreatePrivateBuffer(mlir::OpBuilder& builder, mlir::Operation* anchorOp, mlir::MemRefType bufferType, const std::string& namePrefix)
    {
        // Todo: implement this method later
        return CreateGlobalBuffer(builder, anchorOp, bufferType, namePrefix);
    }

    mlir::Location GetLocation(mlir::OpBuilder& builder, std::string tag)
    {
        return mlir::FileLineColLoc::get(mlir::StringAttr::get(builder.getContext(), tag), 0, 0);
    }

    mlir::Location GetLocation(mlir::OpBuilder& builder, std::string tag, mlir::Location opLocation)
    {
        return builder.getFusedLoc(opLocation, { GetLocation(builder, tag) });
    }

    mlir::Location GetLocation(mlir::OpBuilder& builder, std::string filename, int64_t lineNumber)
    {
        utilities::ReplaceAll(filename, "\\", "/");
        return mlir::FileLineColLoc::get(builder.getStringAttr(filename), lineNumber, 0);
    }

    std::vector<mlir::Value> MultiDimAffineApply(mlir::OpBuilder& builder, mlir::Location loc, mlir::AffineMap map, const std::vector<mlir::Value>& operands, bool simplify)
    {
        std::vector<mlir::Value> result;
        result.reserve(map.getNumResults());
        for (unsigned int resultIdx = 0; resultIdx < map.getNumResults(); ++resultIdx)
        {
            auto singleResultSubMap = map.getSubMap({ resultIdx });
            mlir::AffineApplyOp applyOp;
            if (simplify)
            {
                applyOp = mlir::makeComposedAffineApply(builder, loc, singleResultSubMap, operands);
            }
            else
            {
                applyOp = builder.create<mlir::AffineApplyOp>(loc, singleResultSubMap, operands);
            }
            result.push_back(applyOp);
        }
        return result;
    }

    AffineMap MakeIdentityAccessMap(mlir::Value val, mlir::MLIRContext* context)
    {
        auto type = val.getType();
        assert(type.isa<MemRefType>() && "Value must be a memref type in order to be accessible");
        auto memRefType = type.cast<MemRefType>();
        auto valRank = memRefType.getRank();
        return AffineMap::getMultiDimIdentityMap(valRank, context);
    }

    mlir::AffineValueMap AffineApplyToAffineValueMap(mlir::AffineApplyOp applyOp)
    {
        mlir::AffineApplyOpAdaptor adaptor{ applyOp };
        return mlir::AffineValueMap(applyOp.map(), adaptor.mapOperands());
    }

    mlir::Type GetElementType(mlir::Type type)
    {
        auto result =
            mlir::TypeSwitch<mlir::Type, mlir::Type>(type)
                .Case([&](mlir::ShapedType t) { return t.getElementType(); })
                .Default([&](mlir::Type t) { return t; });
        return result;
    }

    int64_t GetUniqueId(mlir::Operation* where)
    {
        static const std::string UniqueIDAttrName = "acc_next_unique_id";

        // We're going to be modifying a unique ID tagged on the ValueModuleOp, so lock access to the attribute while we're reading it and modifying it
        static std::mutex _uniqueIDMutex;
        std::lock_guard<std::mutex> lock(_uniqueIDMutex);

        // Fetch the next unique ID from the ValueModuleOp parent
        // If it doesn't exist, then return 0 and store the value 1 as the next value
        auto vModuleOp = CastOrGetParentOfType<vir::ValueModuleOp>(where);
        assert(vModuleOp && "Can only get a unique id within the context of a ValueModuleOp");
        int64_t currentId = -1;
        if (auto nextIdAttr = vModuleOp->getAttrOfType<mlir::IntegerAttr>(UniqueIDAttrName))
        {
            currentId = nextIdAttr.getInt();
        }
        else
        {
            currentId = 0;
        }
        int64_t nextId = currentId + 1;
        mlir::OpBuilder builder(where);
        vModuleOp->setAttr(UniqueIDAttrName, builder.getI64IntegerAttr(nextId));

        return currentId;
    }

    mlir::Operation* CloneRecursively(mlir::OpBuilder& builder, mlir::Operation* op, mlir::BlockAndValueMapping& mapping)
    {
        for (auto operand : op->getOperands())
        {
            if (!mapping.contains(operand))
            {
                if (operand.isa<mlir::BlockArgument>())
                {
                    mapping.map(operand, operand);
                }
                else
                {
                    auto definingOp = operand.getDefiningOp();
                    auto clonedOp = CloneRecursively(builder, definingOp, mapping);
                    for (unsigned idx = 0, e = clonedOp->getNumResults(); idx != e; ++idx)
                    {
                        mapping.map(definingOp->getResult(idx), clonedOp->getResult(idx));
                    }
                }
            }
        }

        return builder.clone(*op, mapping);
    }

    std::optional<vir::ExecutionTarget> ResolveExecutionTarget(mlir::Operation* op, bool exact /* = false */)
    {
        // modules can define the execution target
        // search if the current module specifies the execution target
        auto getExecTarget = [](Operation* op_) { return op_->getAttrOfType<vir::ExecutionTargetAttr>(vir::ValueFuncOp::getExecTargetAttrName()); };

        Operation* execAwareOp = op;
        auto execTargetAttr = getExecTarget(execAwareOp);
        while (!exact &&
               execAwareOp &&
               !mlir::isa<mlir::FunctionOpInterface>(execAwareOp) &&
               !execTargetAttr)
        {
            if ((execAwareOp = execAwareOp->getParentOfType<mlir::FunctionOpInterface>()))
            {
                execTargetAttr = getExecTarget(execAwareOp);
            }
        }

        if (execTargetAttr)
        {
            return execTargetAttr.getValue();
        }

        if (!execAwareOp)
        {
            return std::nullopt;
        }

        return mlir::TypeSwitch<Operation*, std::optional<vir::ExecutionTarget>>(execAwareOp)
            .Case([](mlir::gpu::GPUFuncOp) {
                return vir::ExecutionTarget::GPU;
            })
            .Case([](mlir::spirv::FuncOp) {
                return vir::ExecutionTarget::GPU;
            })
            .Case([](mlir::FuncOp) {
                return vir::ExecutionTarget::CPU;
            })
            .Case([](mlir::LLVM::LLVMFuncOp) {
                return vir::ExecutionTarget::CPU;
            })
            .Default([](Operation* op_) {
                op_->emitWarning("Couldn't determine execution environment");
                return std::nullopt;
            });
    }

    vir::ExecutionRuntime ResolveExecutionRuntime(mlir::Operation* op, bool exact /* = false */)
    {
        auto execRuntimeAttrName = ir::value::ValueModuleOp::getExecRuntimeAttrName();

        auto getExecRuntime = [&](Operation* op_) {
            return op_->getAttrOfType<vir::ExecutionRuntimeAttr>(execRuntimeAttrName);
        };

        Operation* moduleLikeOp = op;
        auto execRuntimeAttr = getExecRuntime(moduleLikeOp);
        // if the runtime attribute is not found in the rcv.module, then
        // search the mlir.module for the runtime (using a fully qualified attribute name)
        if (!exact && op && !execRuntimeAttr)
        {
            if ((moduleLikeOp = op->getParentOfType<vir::ValueModuleOp>()))
            {
                execRuntimeAttr = getExecRuntime(moduleLikeOp);
            }
            if (!execRuntimeAttr && (moduleLikeOp = op->getParentOfType<mlir::ModuleOp>()))
            {
                execRuntimeAttr = getExecRuntime(moduleLikeOp);
            }
        }

        // the runtime attribute was not set by the user, so set it to NONE
        if (!execRuntimeAttr)
        {
            return vir::ExecutionRuntime::NONE;
        }

        return execRuntimeAttr.getValue();
    }

    std::optional<std::pair<int, int>> ResolveWarpSize(const vir::ExecutionRuntime runtime)
    {
        if (runtime == vir::ExecutionRuntime::CUDA)
            return std::make_pair(8, 4); // 32

        if (runtime == vir::ExecutionRuntime::ROCM)
            return std::make_pair(8, 8); // 64

        return std::nullopt;
    }

    mlir::Operation* CreateGPUControlBarrier(mlir::OpBuilder& builder, const std::string scope, std::optional<mlir::Location> loc /*= std::nullopt*/)
    {
        auto barrierScope = vir::symbolizeEnum<value::BarrierScope>(scope);
        assert(barrierScope && "Invalid barrier scope");
        return builder.create<vir::BarrierOp>(
            loc.value_or(builder.getUnknownLoc()),
            vir::BarrierScopeAttr::get(builder.getContext(), *barrierScope));
    }

    std::optional<int64_t> GetDimSizeAt(const loopnest::Index& dimensionIndex, mlir::Operation* where)
    {
        assert(where != nullptr);
        mlir::Operation* parentOp = where;

        while ((parentOp = parentOp->getParentOp()) && !mlir::isa<vir::ValueFuncOp>(parentOp) && !mlir::isa<mlir::FuncOp>(parentOp) && !mlir::isa<loopnest::KernelOp>(parentOp))
        {
            if (auto subdomainIndexOrderAttr = parentOp->getAttrOfType<ArrayAttr>("subdomainIndexOrder"))
            {
                auto subdomainIndexOrder = util::ConvertArrayAttrToIndexVector(subdomainIndexOrderAttr);
                auto iter = std::find(subdomainIndexOrder.begin(), subdomainIndexOrder.end(), dimensionIndex);

                if (iter != subdomainIndexOrder.end())
                {
                    if (auto subdomainSizeAttr = parentOp->getAttrOfType<ArrayAttr>("subdomainSize"))
                    {
                        auto subdomainSizes = util::ConvertArrayAttrToIntVector(subdomainSizeAttr);
                        assert(subdomainSizes.size() == subdomainIndexOrder.size() && "subdomainSize and subdomainIndexOrder must have the same number of elements");

                        size_t idx = std::distance(subdomainIndexOrder.begin(), iter);
                        auto dimensionSize = subdomainSizes[idx];
                        return dimensionSize;
                    }
                }
            }
        }

        return {};
    }

    std::vector<std::pair<loopnest::Index, mlir::Value>> ResolveUnrealizedNestIndices(mlir::Operation* where)
    {
        std::vector<std::pair<loopnest::Index, mlir::Value>> result;
        if (auto kernelOp = CastOrGetParentOfType<loopnest::KernelOp>(where))
        {
            auto symbolicIndexOps = kernelOp.getIndices();
            for (auto& symbolicIndexOp : symbolicIndexOps)
            {
                result.emplace_back(std::make_pair(symbolicIndexOp.getValue(), symbolicIndexOp));
            }
        }
        return result;
    }

    std::vector<mlir::Value> GetCurrentIndexIVs(const std::vector<loopnest::Index>& loopIndices, mlir::Operation* where)
    {
        return GetCurrentIndexIVs(loopIndices, where->getBlock());
    }

    std::vector<mlir::Value> GetCurrentIndexIVs(const std::vector<loopnest::Index>& loopIndices, mlir::Block* where)
    {
        std::vector<mlir::Value> ivs(loopIndices.size());

        auto blockParentOp = where->getParentOp();

        std::vector<std::pair<loopnest::Index, mlir::Value>> unrealizedLoopNestIndices = ResolveUnrealizedNestIndices(blockParentOp);

        // First check the unrealizedLoopNestIndices for any loopnest indices that haven't been resolved to full AffineForOps yet
        for (const auto& indexIVPair : unrealizedLoopNestIndices)
        {
            const auto& currentIndex = indexIVPair.first;
            const auto& currentIV = indexIVPair.second;
            auto it = std::find_if(loopIndices.begin(), loopIndices.end(), [&](const loopnest::Index& searchIndex) {
                return (searchIndex == currentIndex) ||
                       (searchIndex.GetId() == loopnest::Index::DefaultID &&
                        searchIndex.GetName() == currentIndex.GetName());
            });
            if (it != loopIndices.end())
            {
                size_t idx = std::distance(loopIndices.begin(), it);
                assert(ivs[idx] == nullptr && "Found same index on multiple loops");
                ivs[idx] = currentIV;
            }
        }

        mlir::AffineForOp currentParentLoop;
        if (mlir::isa<mlir::AffineForOp>(blockParentOp))
        {
            currentParentLoop = mlir::dyn_cast<mlir::AffineForOp>(blockParentOp);
        }
        else
        {
            currentParentLoop = blockParentOp->getParentOfType<mlir::AffineForOp>();
        }

        while (currentParentLoop != nullptr)
        {
            if (auto indexAttr = currentParentLoop->getAttrOfType<loopnest::IndexAttr>("index"))
            {
                auto currentIndex = indexAttr.getValue();

                // If the indices we're looking for have a default ID, then only compare by the name of the index
                // This is to support well-known named loops created internally by Accera
                // If the ID's are not the default, then compare IDs as well
                auto it = std::find_if(loopIndices.begin(), loopIndices.end(), [&](const loopnest::Index& searchIndex) {
                    return (searchIndex == currentIndex) ||
                           (searchIndex.GetId() == loopnest::Index::DefaultID &&
                            searchIndex.GetName() == currentIndex.GetName());
                });

                if (it != loopIndices.end())
                {
                    size_t idx = std::distance(loopIndices.begin(), it);
                    assert(ivs[idx] == nullptr && "Found same index on multiple loops");
                    ivs[idx] = currentParentLoop.getInductionVar();
                }
            }
            currentParentLoop = currentParentLoop->getParentOfType<mlir::AffineForOp>();
        }

        for ([[maybe_unused]] auto iv : ivs)
        {
            assert(iv != nullptr && "Couldn't find all loop indices");
        }

        return ivs;
    }

    std::vector<loopnest::Index> GetIndicesForLoopIVs(const std::vector<mlir::Value>& loopIVs)
    {
        std::vector<loopnest::Index> loopIndices;

        for (const auto& loopIV : loopIVs)
        {
            mlir::AffineForOp loop = mlir::getForInductionVarOwner(loopIV);
            assert(loop != nullptr && "Couldn't find loop with the given IV");
            if (auto indexAttr = loop->getAttrOfType<loopnest::IndexAttr>("index"))
            {
                loopIndices.push_back(indexAttr.getValue());
            }
            else
            {
                assert(false && "Found an AffineForOp with the given IV, but it did not have an IndexAttr");
            }
        }

        return loopIndices;
    }

    mlir::AffineMap ConcatenateAndShiftAffineDimsAndMaps(mlir::OpBuilder& builder, mlir::AffineMap leftMap, mlir::AffineMap rightMap)
    {
        // Differs from mlir::concatAffineMaps in that it shifts the dimensions of the right-hand map
        // and uses all of the dimensions from the two maps independently instead of merging the dimensions

        unsigned dimShift = leftMap.getNumDims();
        std::vector<mlir::AffineExpr> dimReplacements;
        std::vector<mlir::AffineExpr> symReplacements;
        for (unsigned originalDimIdx = 0; originalDimIdx < rightMap.getNumDims(); ++originalDimIdx)
        {
            dimReplacements.push_back(builder.getAffineDimExpr(originalDimIdx + dimShift));
        }
        auto shiftedRightMap = rightMap.replaceDimsAndSymbols(dimReplacements, symReplacements, rightMap.getNumDims() + dimShift, rightMap.getNumSymbols());

        auto leftMapExprs = leftMap.getResults().vec();
        auto concatedExprs = shiftedRightMap.getResults().vec();
        concatedExprs.insert(concatedExprs.begin(), leftMapExprs.begin(), leftMapExprs.end());

        auto concatedMap = mlir::AffineMap::get(shiftedRightMap.getNumDims(), shiftedRightMap.getNumSymbols(), concatedExprs, builder.getContext());

        return concatedMap;
    }

    bool IsSubdomainEmpty(mlir::Operation* where)
    {
        mlir::Operation* parentOp = where;

        while ((parentOp = parentOp->getParentOp()) &&
               !mlir::isa<vir::ValueFuncOp>(parentOp) &&
               !mlir::isa<mlir::FuncOp>(parentOp) &&
               !mlir::isa<loopnest::KernelOp>(parentOp))
        {
            if (auto subdomainSizeAttr = parentOp->getAttrOfType<ArrayAttr>("subdomainSize"))
            {
                auto subdomainSizes = util::ConvertArrayAttrToIntVector(subdomainSizeAttr);
                for (auto subdomainDimSize : subdomainSizes)
                {
                    if (subdomainDimSize == 0)
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    void InlineAllRegionOpsBeforeOp(mlir::PatternRewriter& rewriter, mlir::Region& regionToInsert, mlir::Operation* op)
    {
        mlir::OpBuilder::InsertionGuard guard(rewriter);
        rewriter.setInsertionPoint(op);

        auto parentBlock = op->getBlock();
        auto parentRegion = parentBlock->getParent();

        // Get an iterator to the position of this op in the parent block
        mlir::Block::iterator insertPosition(op);

        // Split the parent block before the op we're inserting before
        auto opContainerBlock = rewriter.splitBlock(parentBlock, insertPosition);

        // Inline the contents of the given block in the spot between blocks that we
        // just created with the splitBlock() call
        rewriter.inlineRegionBefore(regionToInsert, opContainerBlock);

        // Now we have 3 blocks to pay attention to:
        // The original block that we split off from, the predecessorBlock
        // The block we inlined before the target op, the inlinedBlock
        // The block that our target op and anything that came after it is in, the successorBlock == opContainerBlock
        // There may also be blocks before the predecessorBlock and/or after the successorBlock that we shouldn't touch

        // Find the successorBlock with an iterator and step back from it to find the inlinedBlock and the predecessorBlock
        auto successorBlock = opContainerBlock;

        auto blockIter = parentRegion->begin();
        while (blockIter != parentRegion->end() && &(*blockIter) != successorBlock)
        {
            ++blockIter;
        }
        assert(&(*blockIter) == successorBlock && "Failed to find the successorBlock we created as part of lowering");
        // Now step back to find the other blocks
        --blockIter;
        auto& inlinedBlock = *blockIter;
        --blockIter;
        auto& predecessorBlock = *blockIter;

        // We want to erase any terminator in the inlinedBlock that was originally in the regionToInsert's block
        auto inlinedBlockTerminator = inlinedBlock.getTerminator();
        rewriter.eraseOp(inlinedBlockTerminator);

        // Now merge the inlinedBlock into the predecessorBlock
        rewriter.mergeBlocks(&inlinedBlock, &predecessorBlock);

        // Now merge the successorBlock into the predecessorBlock
        rewriter.mergeBlocks(successorBlock, &predecessorBlock);
    }

    // TODO: use StringAttr for id to avoid the extra conversion
    mlir::Operation* FindOpWithSymbolName(const llvm::StringRef& id, mlir::Operation* rootOp)
    {
        auto symTableOp = mlir::SymbolTable::getNearestSymbolTable(rootOp);
        auto idAttr = mlir::StringAttr::get(rootOp->getContext(), id);
        auto symbolOp = mlir::SymbolTable::lookupNearestSymbolFrom(symTableOp, idAttr);
        return symbolOp;
    }

    mlir::LogicalResult PromoteIfSingleIteration(mlir::PatternRewriter& rewriter, mlir::AffineForOp forOp)
    {
        // Copied and modified from llvm-project/mlir/lib/Dialect/Affine/Utils/LoopUtils.cpp : mlir::promoteIfSingleIteration()
        // Modified to work during a lowering pass (i.e. erase ops via the PatternRewriter rather than erasing the ops directly)
        // and to work within a ValueFuncOp as opposed to a std FuncOp

        llvm::Optional<uint64_t> tripCount = mlir::getConstantTripCount(forOp);
        if (!tripCount || tripCount.getValue() != 1)
            return mlir::failure();

        if (forOp.getLowerBoundMap().getNumResults() != 1)
            return mlir::failure();

        // Replaces all IV uses to its single iteration value.
        auto iv = forOp.getInductionVar();
        auto* parentBlock = forOp->getBlock();
        if (!iv.use_empty())
        {
            if (forOp.hasConstantLowerBound())
            {
                mlir::OpBuilder topBuilder(forOp->getParentOfType<vir::ValueFuncOp>().getBody());
                auto constOp = topBuilder.create<mlir::arith::ConstantIndexOp>(
                    forOp.getLoc(), forOp.getConstantLowerBound());
                iv.replaceAllUsesWith(constOp);
            }
            else
            {
                auto lbOperands = forOp.getLowerBoundOperands();
                auto lbMap = forOp.getLowerBoundMap();
                mlir::OpBuilder builder(parentBlock, mlir::Block::iterator(forOp));
                if (lbMap == builder.getDimIdentityMap())
                {
                    // No need of generating an affine.apply.
                    iv.replaceAllUsesWith(lbOperands[0]);
                }
                else
                {
                    auto affineApplyOp =
                        builder.create<mlir::AffineApplyOp>(forOp.getLoc(), lbMap, lbOperands);
                    iv.replaceAllUsesWith(affineApplyOp);
                }
            }
        }
        // Move the loop body operations, except for its terminator, to the loop's
        // containing block.
        rewriter.eraseOp(forOp.getBody()->getTerminator());

        parentBlock->getOperations().splice(mlir::Block::iterator(forOp),
                                            forOp.getBody()->getOperations());

        rewriter.eraseOp(forOp);
        return mlir::success();
    }

    bool OperationsAreEqual(mlir::Operation* lhs, mlir::Operation* rhs)
    {
        if (lhs == rhs)
        {
            return true;
        }

        // Check that the operations have the same type, operands, and attributes

        // Check op type
        auto lhsOpInfo = lhs->getRegisteredInfo();
        auto rhsOpInfo = rhs->getRegisteredInfo();
        if (lhsOpInfo->getTypeID() != rhsOpInfo->getTypeID())
        {
            return false;
        }

        // Check operands
        if (lhs->getNumOperands() != rhs->getNumOperands())
        {
            return false;
        }
        if (!std::equal(lhs->operand_begin(), lhs->operand_end(), rhs->operand_begin(), rhs->operand_end()))
        {
            return false;
        }

        // Check attributes
        auto lhsAttrDict = lhs->getAttrDictionary();
        auto rhsAttrDict = rhs->getAttrDictionary();
        if (lhsAttrDict.size() != rhsAttrDict.size())
        {
            return false;
        }
        for (auto namedAttr : lhsAttrDict.getValue())
        {
            auto lhsAttr = namedAttr.getValue();
            auto rhsAttr = rhsAttrDict.get(namedAttr.getName());
            if (lhsAttr != rhsAttr)
            {
                return false;
            }
        }

        return true;
    }

    mlir::Value CreateConstantRangeForOpIterationCounter(mlir::OpBuilder& builder, mlir::Location loc, mlir::AffineForOp forOp)
    {
        mlir::OpBuilder::InsertionGuard insertGuard(builder);
        builder.setInsertionPointToStart(forOp.getBody());

        assert(forOp.hasConstantBounds() && "AffineForOp must have constant bounds");
        auto lowerBound = forOp.getConstantLowerBound();
        auto step = forOp.getStep();

        // Compute (iv - lowerBound) / step
        auto iterCounterMap = AffineMap::get(1, 0, (builder.getAffineDimExpr(0) - builder.getAffineConstantExpr(lowerBound)).floorDiv(step));
        return builder.create<mlir::AffineApplyOp>(loc, iterCounterMap, mlir::ValueRange{ forOp.getInductionVar() });
    }

    mlir::Operation* GetFirstOp(mlir::Operation* left, mlir::Operation* right)
    {
        assert(left->getBlock() == right->getBlock() && "This utility only supports ops in the same block");
        auto block = left->getBlock();
        auto beginIter = block->begin();
        auto endIter = block->end();
        for (auto iter = beginIter; iter != endIter; ++iter)
        {
            if (&(*iter) == left)
            {
                return left;
            }
            else if (&(*iter) == right)
            {
                return right;
            }
        }
        assert(false && "Neither op found in block");
        return nullptr;
    }

    template <typename MemoryOp>
    mlir::AffineMap GetMemRefIndexToMemoryLocationMap(mlir::MLIRContext* context, MemoryOp op)
    {
        auto memRefType = op.memref().getType().template cast<mlir::MemRefType>();

        auto memRefMap = memRefType.getLayout().getAffineMap();
        if (memRefMap.isIdentity())
        {
            auto stridedLayout = mlir::makeCanonicalStridedLayoutExpr(memRefType.getShape(), context);
            memRefMap = mlir::AffineMap::get(memRefType.getRank(), 0, stridedLayout);
        }
        return memRefMap;
    }

    template <typename AffineMemoryOp>
    mlir::AffineMap GetAffineOpIndexToMemoryLocationMap(mlir::MLIRContext* context, AffineMemoryOp op)
    {
        auto composedMemRefMap = GetMemRefIndexToMemoryLocationMap(context, op);
        mlir::AffineMap affineOpMap = op.getAffineMapAttr().getValue();
        mlir::AffineMap accessMapComposition = composedMemRefMap.compose(affineOpMap);
        assert(accessMapComposition.getNumResults() == 1);
        return accessMapComposition;
    }

    mlir::AffineMap GetIndexToMemoryLocationMap(mlir::MLIRContext* context, mlir::AffineStoreOp op)
    {
        return GetAffineOpIndexToMemoryLocationMap(context, op);
    }

    mlir::AffineMap GetIndexToMemoryLocationMap(mlir::MLIRContext* context, mlir::AffineLoadOp op)
    {
        return GetAffineOpIndexToMemoryLocationMap(context, op);
    }

    mlir::AffineMap GetIndexToMemoryLocationMap(mlir::MLIRContext* context, mlir::memref::StoreOp op)
    {
        return GetMemRefIndexToMemoryLocationMap(context, op);
    }

    mlir::AffineMap GetIndexToMemoryLocationMap(mlir::MLIRContext* context, mlir::memref::LoadOp op)
    {
        return GetMemRefIndexToMemoryLocationMap(context, op);
    }

    TempOpCleanupGuard::TempOpCleanupGuard(std::stack<mlir::Operation*>* opStack, mlir::PatternRewriter& rewriter) :
        _opStack(opStack),
        _rewriter(rewriter)
    {}

    TempOpCleanupGuard::~TempOpCleanupGuard()
    {
        while (!_opStack->empty())
        {
            auto eraseOp = _opStack->top();
            assert(eraseOp->use_empty());
            _rewriter.eraseOp(eraseOp);
            _opStack->pop();
        }
    }

    mlir::Attribute MemorySpaceToAttribute(const value::MemorySpace& memorySpace, mlir::MLIRContext* context)
    {
        return mlir::IntegerAttr::get(mlir::IntegerType::get(context, 64), static_cast<int64_t>(memorySpace));
    }

    value::MemorySpace AttributeToMemorySpace(mlir::Attribute memorySpaceAttr)
    {
        return static_cast<value::MemorySpace>(memorySpaceAttr.cast<mlir::IntegerAttr>().getInt());
    }

    mlir::AffineMap GetMajorIdentityMap(unsigned dims, unsigned results, mlir::MLIRContext* context)
    {
        assert(dims >= results && "Dimension mismatch");
        auto id = mlir::AffineMap::getMultiDimIdentityMap(dims, context);
        return mlir::AffineMap::get(dims, 0, id.getResults().take_front(results), context);
    }

    void EraseAllOpsInBlock(mlir::PatternRewriter& rewriter, mlir::Block& block)
    {
        for (auto& op : llvm::make_early_inc_range(llvm::reverse(block)))
        {
            assert(op.use_empty() && "expected 'op' to have no uses");
            rewriter.eraseOp(&op);
        }
    }

    mlir::Type ToSignlessMLIRType(mlir::OpBuilder& builder, mlir::Type type)
    {
        if (type.isIntOrFloat())
        {
            if (auto width = type.getIntOrFloatBitWidth(); type.isInteger(width))
            {
                return builder.getIntegerType(width);
            }
        }
        return type; // pass-through, no signless change
    }

    mlir::Value ToSignlessMLIRValue(mlir::OpBuilder& builder, mlir::Value value)
    {
        auto type = value.getType();
        if (auto signlessType = ToSignlessMLIRType(builder, type); signlessType != type)
        {
            // Cast from signed to signless
            // cf. mlir/lib/Conversion/TosaToLinalg/TosaToLinalg.cpp
            return builder.create<mlir::UnrealizedConversionCastOp>(value.getLoc(), signlessType, value).getResult(0);
        }
        return value; // pass-through, no signless change
    }

    mlir::Operation* GetDefiningOpOrForLoop(mlir::Value val)
    {
        if (mlir::isForInductionVar(val)) // AffineForOp
        {
            return mlir::getForInductionVarOwner(val);
        }
        else if (auto scfForOp = mlir::scf::getForInductionVarOwner(val)) // SCFForOp
        {
            return scfForOp;
        }
        else // Arbitrary other op
        {
            return val.getDefiningOp();
        }
    }

    template <typename _TyOp>
    auto GetROCDLGPUIndex(mlir::OpBuilder& builder, mlir::Location& loc)
    {
        return builder.create<mlir::arith::IndexCastOp>(loc, builder.create<_TyOp>(loc, builder.getI32Type()), builder.getIndexType());
    }

    mlir::Value getWarpIdOp(mlir::OpBuilder& builder, mlir::Location& loc, const mlir::gpu::Dimension dim, const vir::ExecutionRuntime execRuntime)
    {
        auto [warpSizeX, warpSizeY] = ResolveWarpSize(execRuntime).value();
        const auto warpSize = warpSizeX * warpSizeY;
        auto tid = builder.create<mlir::gpu::ThreadIdOp>(loc, builder.getIndexType(), dim);
        return builder.create<vir::WarpIdOp>(loc, builder.getIndexType(), tid, static_cast<uint8_t>(dim), warpSize);
    }

    mlir::Value GetGPUIndex(value::Processor idxType, mlir::OpBuilder& builder, mlir::Location& loc, const vir::ExecutionRuntime execRuntime)
    {
        switch (idxType)
        {
        case value::Processor::ThreadX:
            return builder.create<mlir::gpu::ThreadIdOp>(loc, builder.getIndexType(), mlir::gpu::Dimension::x);
        case value::Processor::WarpX:
            return getWarpIdOp(builder, loc, mlir::gpu::Dimension::x, execRuntime);
        case value::Processor::ThreadY:
            return builder.create<mlir::gpu::ThreadIdOp>(loc, builder.getIndexType(), mlir::gpu::Dimension::y);
        case value::Processor::WarpY:
            return getWarpIdOp(builder, loc, mlir::gpu::Dimension::y, execRuntime);
        case value::Processor::ThreadZ:
            return builder.create<mlir::gpu::ThreadIdOp>(loc, builder.getIndexType(), mlir::gpu::Dimension::z);
        case value::Processor::BlockX:
            return builder.create<mlir::gpu::BlockIdOp>(loc, builder.getIndexType(), mlir::gpu::Dimension::x);
        case value::Processor::BlockY:
            return builder.create<mlir::gpu::BlockIdOp>(loc, builder.getIndexType(), mlir::gpu::Dimension::y);
        case value::Processor::BlockZ:
            return builder.create<mlir::gpu::BlockIdOp>(loc, builder.getIndexType(), mlir::gpu::Dimension::z);
        case value::Processor::BlockDimX:
            return builder.create<mlir::gpu::BlockDimOp>(loc, builder.getIndexType(), mlir::gpu::Dimension::x);
        case value::Processor::BlockDimY:
            return builder.create<mlir::gpu::BlockDimOp>(loc, builder.getIndexType(), mlir::gpu::Dimension::y);
        case value::Processor::BlockDimZ:
            return builder.create<mlir::gpu::BlockDimOp>(loc, builder.getIndexType(), mlir::gpu::Dimension::z);
        case value::Processor::GridDimX:
            return builder.create<mlir::gpu::GridDimOp>(loc, builder.getIndexType(), mlir::gpu::Dimension::x);
        case value::Processor::GridDimY:
            return builder.create<mlir::gpu::GridDimOp>(loc, builder.getIndexType(), mlir::gpu::Dimension::y);
        case value::Processor::GridDimZ:
            return builder.create<mlir::gpu::GridDimOp>(loc, builder.getIndexType(), mlir::gpu::Dimension::z);
        default:
            llvm_unreachable("Unexpected");
        }
    }

    int GetDimValByDimIndex(accera::ir::targets::Dim3 dims, mlir::gpu::Dimension dimIndex)
    {
        switch (dimIndex)
        {
        case mlir::gpu::Dimension::x:
            return dims.x;
        case mlir::gpu::Dimension::y:
            return dims.y;
        case mlir::gpu::Dimension::z:
            return dims.z;
        default:
            return -1;
        }
    }

    template <typename OpTy>
    accera::ir::targets::GPU GetGPUFuncLaunchHelper(OpTy vFuncOrLambdaOp)
    {
        auto launchAttr = vFuncOrLambdaOp->template getAttrOfType<mlir::ArrayAttr>(vFuncOrLambdaOp.getGPULaunchAttrName());
        assert(launchAttr != nullptr);
        return accera::ir::targets::GPU::FromArrayAttr(launchAttr);
    }

    accera::ir::targets::GPU GetGPUFuncLaunchInfo(mlir::Operation* where)
    {
        return mlir::TypeSwitch<mlir::Operation*, accera::ir::targets::GPU>(where)
            .Case([&](ir::value::ValueFuncOp vFuncOp) { return GetGPUFuncLaunchHelper(vFuncOp); })
            .Case([&](ir::value::ValueLambdaOp vLambdaOp) { return GetGPUFuncLaunchHelper(vLambdaOp); })
            .Default([](mlir::Operation*) {
                assert(false && "Can only resolve gpu launch info for ir::value::ValueFuncOp and ir::value::ValueLambdaOp");
                return accera::ir::targets::GPU{};
            });
    }

    int64_t GetBlockDimSize(mlir::Operation* where, mlir::gpu::Dimension dimId)
    {
        if (auto gpuFunc = where->getParentOfType<mlir::gpu::GPUFuncOp>())
        {
            auto blockIdxAttr = gpuFunc->getAttrOfType<ArrayAttr>("blockSize");
            assert(blockIdxAttr && "Couldn't resolve block size");
            auto blockDimSize = blockIdxAttr.getValue()[static_cast<size_t>(dimId)].cast<IntegerAttr>().getInt();
            return blockDimSize;
        }
        else
        {
            mlir::Operation* vFuncOp = where->getParentOfType<ir::value::ValueFuncOp>();
            mlir::Operation* vLambdaOp = where->getParentOfType<ir::value::ValueLambdaOp>();
            if (vFuncOp == nullptr && vLambdaOp == nullptr)
            {
                assert(false && "Can only resolve block dim size inside of a gpu::GPUFuncOp, ir::value::ValueFuncOp, or ir::value::ValueLambdaOp");
                return -1;
            }
            // Prefer using the ValueLambdaOp as inner loopnests will be a ValueLambdaOp nested inside of a ValueFuncOp
            auto op = vLambdaOp != nullptr ? vLambdaOp : vFuncOp;
            auto gpuParams = GetGPUFuncLaunchInfo(op);
            auto blockDimVal = GetDimValByDimIndex(gpuParams.block, dimId);
            assert(blockDimVal != -1 && "Couldn't resolve block size");
            return blockDimVal;
        }
    }

    int64_t GetGridDimSize(mlir::Operation* where, mlir::gpu::Dimension dimId)
    {
        if (auto gpuFunc = where->getParentOfType<mlir::gpu::GPUFuncOp>())
        {
            auto gridIdxAttr = gpuFunc->getAttrOfType<ArrayAttr>("gridSize");
            assert(gridIdxAttr && "Couldn't resolve grid size");
            auto gridDimSize = gridIdxAttr.getValue()[static_cast<size_t>(dimId)].cast<IntegerAttr>().getInt();
            return gridDimSize;
        }
        else
        {
            mlir::Operation* vFuncOp = where->getParentOfType<ir::value::ValueFuncOp>();
            mlir::Operation* vLambdaOp = where->getParentOfType<ir::value::ValueLambdaOp>();
            if (vFuncOp == nullptr && vLambdaOp == nullptr)
            {
                assert(false && "Can only resolve grid dim size inside of a gpu::GPUFuncOp, ir::value::ValueFuncOp, or ir::value::ValueLambdaOp");
                return -1;
            }
            auto op = vLambdaOp != nullptr ? vLambdaOp : vFuncOp;
            auto gpuParams = GetGPUFuncLaunchInfo(op);
            auto gridDimVal = GetDimValByDimIndex(gpuParams.grid, dimId);
            assert(gridDimVal != -1 && "Couldn't resolve grid size");
            return gridDimVal;
        }
    }

    int64_t GetBlockDimSize(mlir::gpu::BlockDimOp op)
    {
        return GetBlockDimSize(op, op.dimension());
    }

    int64_t GetGridDimSize(mlir::gpu::GridDimOp op)
    {
        return GetGridDimSize(op, op.dimension());
    }

    mlir::Value GetCurrentGPUBlockThreadID(mlir::OpBuilder& builder, mlir::Location loc)
    {
        auto threadXSym = builder.getAffineSymbolExpr(0);

        auto threadYSym = builder.getAffineSymbolExpr(1);
        auto blockDimXSym = builder.getAffineSymbolExpr(2);

        auto threadZSym = builder.getAffineSymbolExpr(3);
        auto blockDimYSym = builder.getAffineSymbolExpr(4);

        auto threadXOp = GetGPUIndex(vir::Processor::ThreadX, builder, loc);
        auto threadYOp = GetGPUIndex(vir::Processor::ThreadY, builder, loc);
        auto threadZOp = GetGPUIndex(vir::Processor::ThreadZ, builder, loc);

        auto blockDimXOp = GetGPUIndex(vir::Processor::BlockDimX, builder, loc);
        auto blockDimYOp = GetGPUIndex(vir::Processor::BlockDimY, builder, loc);
        auto blockDimZOp = GetGPUIndex(vir::Processor::BlockDimZ, builder, loc);
        if (GetBlockDimSize(blockDimZOp.getDefiningOp<mlir::gpu::BlockDimOp>()) == 1) // 2D or 1D block
        {
            if (GetBlockDimSize(blockDimYOp.getDefiningOp<mlir::gpu::BlockDimOp>()) == 1)
            {
                // 1D block
                auto flattenedTidMap = mlir::AffineMap::get(0, 1, threadXSym);
                return builder.create<mlir::AffineApplyOp>(loc, flattenedTidMap, mlir::ValueRange{ threadXOp });
            }

            // 2D block
            auto flattenedTidExpr = (threadYSym * blockDimXSym) + threadXSym;
            auto flattenedTidMap = mlir::AffineMap::get(0, 3, flattenedTidExpr);
            return builder.create<mlir::AffineApplyOp>(loc, flattenedTidMap, mlir::ValueRange{ threadXOp, threadYOp, blockDimXOp });
        }

        // 3D block
        auto flattenedTidExpr = (threadZSym * blockDimXSym * blockDimYSym) + (threadYSym * blockDimXSym) + threadXSym;
        auto flattenedTidMap = mlir::AffineMap::get(0, 5, flattenedTidExpr);
        return builder.create<mlir::AffineApplyOp>(loc, flattenedTidMap, mlir::ValueRange{ threadXOp, threadYOp, threadZOp, blockDimXOp, blockDimYOp, blockDimZOp });
    }

    mlir::Value GetCurrentGPUWarpThreadID(mlir::OpBuilder& builder, mlir::Location loc)
    {
        auto insertionBlock = builder.getInsertionBlock();
        auto parentOp = insertionBlock->getParentOp();
        auto [warpSizeX, warpSizeY] = ResolveWarpSize(ResolveExecutionRuntime(parentOp)).value();
        const auto warpSize = warpSizeX * warpSizeY;

        auto blockTidSym = builder.getAffineSymbolExpr(0);
        auto blockTidToWarpTidMap = mlir::AffineMap::get(0, 1, blockTidSym % warpSize);

        auto blockTid = GetCurrentGPUBlockThreadID(builder, loc);

        return builder.create<mlir::AffineApplyOp>(loc, blockTidToWarpTidMap, mlir::ValueRange{ blockTid });
    }

    bool ShapesMatch(mlir::ShapedType lhs, mlir::ShapedType rhs)
    {
        if (lhs == nullptr || rhs == nullptr)
        {
            return false;
        }
        auto lhsShape = lhs.getShape();
        auto rhsShape = rhs.getShape();
        return lhsShape.size() == rhsShape.size() && std::equal(lhsShape.begin(), lhsShape.end(), rhsShape.begin());
    }

#define IS_IMPLICITLY_CASTABLE_IF(sourceType, targetType, conditional)       \
    if (source.isa<sourceType>() && target.isa<targetType>() && conditional) \
    {                                                                        \
        return true;                                                         \
    }

#define IS_IMPLICITLY_CASTABLE(sourceType, targetType) IS_IMPLICITLY_CASTABLE_IF(sourceType, targetType, true);

    bool IsImplicitlyCastable(mlir::Type source, mlir::Type target)
    {
        IS_IMPLICITLY_CASTABLE(mlir::IntegerType, mlir::IndexType);
        IS_IMPLICITLY_CASTABLE_IF(mlir::IntegerType, mlir::IntegerType, (source.getIntOrFloatBitWidth() <= target.getIntOrFloatBitWidth()));
        IS_IMPLICITLY_CASTABLE_IF(mlir::IntegerType, mlir::FloatType, (source.getIntOrFloatBitWidth() <= target.getIntOrFloatBitWidth()));
        IS_IMPLICITLY_CASTABLE_IF(mlir::FloatType, mlir::FloatType, (source.getIntOrFloatBitWidth() <= target.getIntOrFloatBitWidth()));

        auto sourceShapedTypeOrNull = source.dyn_cast<mlir::ShapedType>();
        auto targetShapedTypeOrNull = target.dyn_cast<mlir::ShapedType>();
        IS_IMPLICITLY_CASTABLE_IF(mlir::VectorType, mlir::VectorType, (ShapesMatch(sourceShapedTypeOrNull, targetShapedTypeOrNull) && IsImplicitlyCastable(GetElementType(source), GetElementType(target))));
        return false;
    }

} // namespace util
} // namespace accera::ir
