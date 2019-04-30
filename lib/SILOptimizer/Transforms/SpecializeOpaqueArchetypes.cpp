#define DEBUG_TYPE "opaque-archetype-specializer"

#include "swift/AST/Types.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/TypeSubstCloner.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFG.h"

#include "llvm/Support/CommandLine.h"

llvm::cl::opt<bool>
    EnableOpaqueArchetypeSpecializer("enable-opaque-archetype-specializer",
                                     llvm::cl::init(false));

using namespace swift;

static Type substOpaqueTypesWithUnderlyingTypes(
    Type ty, SILFunction *context) {
  ReplaceOpaqueTypesWithUnderlyingTypes replacer(context);
  return ty.subst(replacer, replacer, SubstFlags::SubstituteOpaqueArchetypes);
}

static SubstitutionMap
substOpaqueTypesWithUnderlyingTypes(SubstitutionMap map, SILFunction *context) {
  ReplaceOpaqueTypesWithUnderlyingTypes replacer(context);
  return map.subst(replacer, replacer, SubstFlags::SubstituteOpaqueArchetypes);
}

namespace {
class OpaqueSpecializerCloner
    : public SILCloner<OpaqueSpecializerCloner> {

  using SuperTy = SILCloner<OpaqueSpecializerCloner>;

  SILBasicBlock *entryBlock;
  SILBasicBlock *cloneFromBlock;

  /// Cache for substituted types.
  llvm::DenseMap<SILType, SILType> TypeCache;

  SILFunction &Original;

public:
  friend class SILCloner<OpaqueSpecializerCloner>;
  friend class SILCloner<OpaqueSpecializerCloner>;
  friend class SILInstructionVisitor<OpaqueSpecializerCloner>;

  OpaqueSpecializerCloner(SILFunction &fun) : SuperTy(fun), Original(fun) {
    entryBlock = fun.getEntryBlock();
    cloneFromBlock = entryBlock->split(entryBlock->begin());
  }

  void clone();

protected:
  void insertOpaqueToConcreteAddressCasts(SILInstruction *orig,
                                          SILInstruction *cloned);

  void postProcess(SILInstruction *orig, SILInstruction *cloned) {
    SILCloner<OpaqueSpecializerCloner>::postProcess(orig, cloned);
    insertOpaqueToConcreteAddressCasts(orig, cloned);
  }

  void visitTerminator(SILBasicBlock *BB) {
    visit(BB->getTerminator());
  }

  void visitReturnInst(ReturnInst *Inst) {
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
    auto origResult = Inst->getOperand();
    auto clonedResult = getOpValue(Inst->getOperand());
    if (clonedResult->getType().getASTType() !=
        origResult->getType().getASTType()) {
      clonedResult = createCast(RegularLocation::getAutoGeneratedLocation(),
                                clonedResult, origResult->getType());
    }
    recordClonedInstruction(
        Inst,
        getBuilder().createReturn(getOpLocation(Inst->getLoc()), clonedResult));
  }

  void visitStructInst(StructInst *Inst) {
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
    auto elements = getOpValueArray<8>(Inst->getElements());
    auto structTy = getOpType(Inst->getType());
    auto *structDecl = structTy.getStructOrBoundGenericStruct();
    unsigned idx = 0;
    // Adjust field types if neccessary.
    for (VarDecl *field : structDecl->getStoredProperties()) {
      SILType loweredType = structTy.getFieldType(
          field, getBuilder().getFunction().getModule());
      if (elements[idx]->getType() != loweredType) {
        elements[idx] = createCast(getOpLocation(Inst->getLoc()), elements[idx],
                                   loweredType);
      }
      idx++;
    }
    recordClonedInstruction(
      Inst, getBuilder().createStruct(getOpLocation(Inst->getLoc()),
                                      getOpType(Inst->getType()), elements));
  }

  void visitTupleInst(TupleInst *Inst) {
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
    auto elements = getOpValueArray<8>(Inst->getElements());
    auto tupleTy = getOpType(Inst->getType());
    for (size_t i = 0, size = Inst->getElements().size(); i < size; ++i) {
      auto elementTy = tupleTy.getTupleElementType(i);
      if (Inst->getElement(i)->getType() != elementTy) {
        elements[i] =
            createCast(getOpLocation(Inst->getLoc()), elements[i], elementTy);
      }
    }
    recordClonedInstruction(
        Inst, getBuilder().createTuple(getOpLocation(Inst->getLoc()),
                                       getOpType(Inst->getType()), elements));
  }

  void visitEnumInst(EnumInst *Inst) {
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
    SILValue opd = SILValue();
    if (Inst->hasOperand()) {
      SILType caseTy = Inst->getType().getEnumElementType(
          Inst->getElement(), getBuilder().getFunction().getModule());
      opd = getOpValue(Inst->getOperand());
      if (opd->getType() != caseTy) {
        opd = createCast(getOpLocation(Inst->getLoc()), opd, caseTy);
      }
    }
    recordClonedInstruction(
        Inst, getBuilder().createEnum(getOpLocation(Inst->getLoc()), opd,
                                      Inst->getElement(),
                                      getOpType(Inst->getType())));
  }

  /// Projections should not change the type if the type is not specialized.
  void visitStructElementAddrInst(StructElementAddrInst *Inst) {
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
    auto opd = getOpValue(Inst->getOperand());
    recordClonedInstruction(
        Inst, getBuilder().createStructElementAddr(
                  getOpLocation(Inst->getLoc()), opd, Inst->getField()));
  }

  /// Projections should not change the type if the type is not specialized.
  void visitStructExtractInst(StructExtractInst *Inst) {
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
    auto opd = getOpValue(Inst->getOperand());
    recordClonedInstruction(
        Inst, getBuilder().createStructExtract(getOpLocation(Inst->getLoc()),
                                               opd, Inst->getField()));
  }
  /// Projections should not change the type if the type is not specialized.
  void visitTupleElementAddrInst(TupleElementAddrInst *Inst) {
    auto opd = getOpValue(Inst->getOperand());
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
    recordClonedInstruction(Inst, getBuilder().createTupleElementAddr(
                                      getOpLocation(Inst->getLoc()), opd,
                                      Inst->getFieldNo()));
  }
  /// Projections should not change the type if the type is not specialized.
  void visitTupleExtractInst(TupleExtractInst *Inst) {
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
    recordClonedInstruction(
        Inst, getBuilder().createTupleExtract(getOpLocation(Inst->getLoc()),
                                              getOpValue(Inst->getOperand()),
                                              Inst->getFieldNo()));
  }
  /// Projections should not change the type if the type is not specialized.
  void visitRefElementAddrInst(RefElementAddrInst *Inst) {
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
    recordClonedInstruction(
        Inst, getBuilder().createRefElementAddr(
                  getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                  Inst->getField()));
  }

  /// Projections should not change the type if the type is not specialized.
  void visitRefTailAddrInst(RefTailAddrInst *Inst) {
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
    recordClonedInstruction(
        Inst, getBuilder().createRefTailAddr(getOpLocation(Inst->getLoc()),
                                             getOpValue(Inst->getOperand()),
                                             Inst->getType()));
  }

  void visitYieldInst(YieldInst *Inst) {
    auto OrigValues = Inst->getYieldedValues();
    auto Values = getOpValueArray<8>(Inst->getYieldedValues());
    auto ResumeBB = getOpBasicBlock(Inst->getResumeBB());
    auto UnwindBB = getOpBasicBlock(Inst->getUnwindBB());
    for (auto idx : indices(Values)) {
      if (OrigValues[idx]->getType().getASTType() !=
          Values[idx]->getType().getASTType()) {
        Values[idx] = createCast(RegularLocation::getAutoGeneratedLocation(),
                                 Values[idx], OrigValues[idx]->getType());
      }
    }

    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
    recordClonedInstruction(
        Inst, getBuilder().createYield(getOpLocation(Inst->getLoc()), Values,
                                       ResumeBB, UnwindBB));
  }

  void visitCopyAddrInst(CopyAddrInst *Inst) {
    auto src = getOpValue(Inst->getSrc());
    auto dst = getOpValue(Inst->getDest());
    auto srcType = src->getType();
    auto destType = dst->getType();
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
    // If the types mismatch cast the operands to the non opaque archetype.
    if (destType.getASTType() != srcType.getASTType()) {
      if (srcType.getASTType()->hasOpaqueArchetype()) {
        src = getBuilder().createUncheckedAddrCast(
            getOpLocation(Inst->getLoc()), src, destType);
      } else if (destType.getASTType()->hasOpaqueArchetype()) {
        dst = getBuilder().createUncheckedAddrCast(
            getOpLocation(Inst->getLoc()), dst, srcType);
      }
    }
    recordClonedInstruction(
        Inst, getBuilder().createCopyAddr(getOpLocation(Inst->getLoc()), src,
                                          dst, Inst->isTakeOfSrc(),
                                          Inst->isInitializationOfDest()));
  }

  void visitStoreInst(StoreInst *Inst) {
    auto src = getOpValue(Inst->getSrc());
    auto dst = getOpValue(Inst->getDest());
    auto srcType = src->getType();
    auto destType = dst->getType();
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
    // If the types mismatch cast the operands to the non opaque archetype.
    if (destType.getASTType() != srcType.getASTType()) {
      if (srcType.getASTType()->hasOpaqueArchetype()) {
        assert(!srcType.isAddress());
        src = createCast(getOpLocation(Inst->getLoc()), src,
                         destType.getObjectType());
      } else if (destType.getASTType()->hasOpaqueArchetype()) {
        dst = getBuilder().createUncheckedAddrCast(
            getOpLocation(Inst->getLoc()), dst, srcType.getAddressType());
      }
    }

    if (!getBuilder().hasOwnership()) {
      switch (Inst->getOwnershipQualifier()) {
      case StoreOwnershipQualifier::Assign: {
        auto *li = getBuilder().createLoad(getOpLocation(Inst->getLoc()), dst,
                                           LoadOwnershipQualifier::Unqualified);
        auto *si = getBuilder().createStore(
            getOpLocation(Inst->getLoc()), src, getOpValue(Inst->getDest()),
            StoreOwnershipQualifier::Unqualified);
        getBuilder().emitDestroyValueOperation(getOpLocation(Inst->getLoc()),
                                               li);
        return recordClonedInstruction(Inst, si);
      }
      case StoreOwnershipQualifier::Init:
      case StoreOwnershipQualifier::Trivial:
      case StoreOwnershipQualifier::Unqualified:
        break;
      }

      return recordClonedInstruction(
          Inst,
          getBuilder().createStore(getOpLocation(Inst->getLoc()), src, dst,
                                   StoreOwnershipQualifier::Unqualified));
    }

    recordClonedInstruction(
        Inst, getBuilder().createStore(getOpLocation(Inst->getLoc()), src, dst,
                                       Inst->getOwnershipQualifier()));
  }

protected:

  SILType remapType(SILType Ty) {
    SILType &Sty = TypeCache[Ty];
    if (Sty)
      return Sty;

   // Apply the opaque types substitution.
    ReplaceOpaqueTypesWithUnderlyingTypes replacer(&Original);
    Sty = Ty.subst(Original.getModule(), replacer, replacer,
                   CanGenericSignature(), true);
    return Sty;
  }

  CanType remapASTType(CanType ty) {
    // Apply the opaque types substitution.
    return substOpaqueTypesWithUnderlyingTypes(ty, &Original)
        ->getCanonicalType();
  }

  ProtocolConformanceRef remapConformance(Type type,
                                          ProtocolConformanceRef conf) {
    // Apply the opaque types substitution.
    ReplaceOpaqueTypesWithUnderlyingTypes replacer(&Original);
    return conf.subst(type, replacer, replacer,
                      SubstFlags::SubstituteOpaqueArchetypes);
  }

  SubstitutionMap remapSubstitutionMap(SubstitutionMap Subs) {
    // Apply the opaque types substitution.
    return substOpaqueTypesWithUnderlyingTypes(Subs, &Original);
  }

  SILValue createCast(SILLocation loc, SILValue opd, SILType type) {
    auto &CurFn = getBuilder().getFunction();
    if (opd->getType().isAddress()) {
      return getBuilder().createUncheckedAddrCast(loc, opd, type);
    } else if (opd->getType().is<SILFunctionType>()) {
      return getBuilder().createConvertFunction(
          loc, opd, type, /*withoutActuallyEscaping*/ false);
    } else if (opd->getType().isTrivial(CurFn)) {
      return getBuilder().createUncheckedTrivialBitCast(loc, opd, type);
    } else {
      return getBuilder().createUncheckedRefCast(loc, opd, type);
    }
  }

  void fixUp(SILFunction *) {
    for (auto &BB : getBuilder().getFunction()) {
      for (auto &cloned : BB) {
        // Fix up the type of try_apply successor block arguments.
        if (auto *tryApply = dyn_cast<TryApplyInst>(&cloned)) {
          auto normalBB = tryApply->getNormalBB();
          SILFunctionConventions calleeConv(
              tryApply->getSubstCalleeType(),
              tryApply->getFunction()->getModule());
          auto normalBBType = (*normalBB->args_begin())->getType();
          auto applyResultType = calleeConv.getSILResultType();
          if (normalBBType != calleeConv.getSILResultType()) {
            auto origPhi = normalBB->getPhiArguments()[0];
            SILValue undef =
                SILUndef::get(normalBBType, getBuilder().getFunction());
            SmallVector<Operand *, 8> useList(origPhi->use_begin(),
                                              origPhi->use_end());
            for (auto *use : useList) {
              use->set(undef);
            }

            auto *newPhi = normalBB->replacePhiArgument(
                0, applyResultType, origPhi->getOwnershipKind());

            getBuilder().setInsertionPoint(normalBB->begin());
            auto cast = createCast(tryApply->getLoc(), newPhi, normalBBType);
            for (auto *use : useList) {
              use->set(cast);
            }
          }
        }
      }
    }
  }
};
} // namespace

void OpaqueSpecializerCloner::clone() {
  for (auto arg: entryBlock->getArguments())
    recordFoldedValue(arg, arg);
  cloneReachableBlocks(cloneFromBlock, {}, entryBlock,
                       true /*havePrepopulatedFunctionArgs*/);
  getBuilder().setInsertionPoint(entryBlock);
  getBuilder().createBranch(RegularLocation::getAutoGeneratedLocation(),
                            getOpBasicBlock(cloneFromBlock));
}

/// Update address uses of the opaque type archetype with the concrete type.
/// This is neccessary for apply instructions.
void OpaqueSpecializerCloner::insertOpaqueToConcreteAddressCasts(
    SILInstruction *orig, SILInstruction *cloned) {

  // Replace apply operands.
  if (auto apply = ApplySite::isa(cloned)) {
    SavedInsertionPointRAII restore(getBuilder());
    getBuilder().setInsertionPoint(apply.getInstruction());
    auto substConv = apply.getSubstCalleeConv();
    unsigned idx = 0;
    for (auto &opd : apply.getArgumentOperands()) {
      auto argConv = apply.getArgumentConvention(opd);
      auto argIdx = apply.getCalleeArgIndex(opd);
      auto argType = substConv.getSILArgumentType(argIdx);
      if (argType.getASTType() != opd.get()->getType().getASTType()) {
        if (argConv.isIndirectConvention()) {
          auto cast = getBuilder().createUncheckedAddrCast(apply.getLoc(),
                                                           opd.get(), argType);
          opd.set(cast);
        } else if (argType.is<SILFunctionType>()) {
          auto cast = getBuilder().createConvertFunction(
              apply.getLoc(), opd.get(), argType,
              /*withoutActuallyEscaping*/ false);
          opd.set(cast);
        } else if (argType.isTrivial(getBuilder().getFunction())) {
          auto cast = getBuilder().createUncheckedTrivialBitCast(
              apply.getLoc(), opd.get(), argType);
          opd.set(cast);
        } else {
          auto cast = getBuilder().createUncheckedRefCast(apply.getLoc(),
                                                          opd.get(), argType);
          opd.set(cast);
        }
      }
      ++idx;
    }
  }
}

namespace {
class OpaqueArchetypeSpecializer : public SILFunctionTransform {
  void run() override {
    if (!EnableOpaqueArchetypeSpecializer)
      return;

    auto *context = getFunction();

    if (!context->shouldOptimize())
      return;

    auto opaqueArchetypeWouldChange = [=](CanType ty) -> bool {
      if (!ty->hasOpaqueArchetype())
        return false;

      return ty.findIf([=](Type type) -> bool {
        if (auto opaqueTy = type->getAs<OpaqueTypeArchetypeType>()) {
          auto opaque = opaqueTy->getDecl();
          return ReplaceOpaqueTypesWithUnderlyingTypes::
              shouldPerformSubstitution(opaque, context);
        }
        return false;
      });
    };

    // Look for opaque type archetypes.
    bool foundOpaqueArchetype = false;
    for (auto &BB : *getFunction()) {
      for (auto &inst : BB) {
        auto hasOpaqueOperand = [&](SILInstruction &inst) -> bool {
          // Check the operands for opaque types.
          for (auto &opd : inst.getAllOperands())
            if (opaqueArchetypeWouldChange(opd.get()->getType().getASTType()))
              return true;
          return false;
        };
        if ((foundOpaqueArchetype = hasOpaqueOperand(inst)))
          break;
        auto hasOpaqueResult = [&](SILInstruction &inst) -> bool {
          // Check the results for opaque types.
          for (const auto &res : inst.getResults())
            if (opaqueArchetypeWouldChange(res->getType().getASTType()))
              return true;
          return false;
        };
        if ((foundOpaqueArchetype = hasOpaqueResult(inst)))
          break;
      }
      if (foundOpaqueArchetype)
        break;
    }

    if (foundOpaqueArchetype) {
      OpaqueSpecializerCloner s(*getFunction());
      s.clone();
      removeUnreachableBlocks(*getFunction());
      invalidateAnalysis(SILAnalysis::InvalidationKind::FunctionBody);
    }
  }
};
} // end anonymous namespace

SILTransform *swift::createOpaqueArchetypeSpecializer() {
  return new OpaqueArchetypeSpecializer();
}
