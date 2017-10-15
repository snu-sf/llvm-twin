//===-- CanonicalizeTypeToI8Ptr.h - Canonicalize Type to i8*-----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This canonicalizes load i64 / load ty* to load i8* and
// store i64 / store ty* to store i8*. This helps SLPVectorizer to find
// consecutive memory accesses.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/CanonicalizeTypeToI8Ptr.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Transforms/Utils/Local.h"
using namespace llvm;

#define DEBUG_TYPE "canonicalize-to-i8ptr"

// Are we allowed to form a atomic load or store of this type?
/// FIXME: This is copied from InstCombineLoadStoreAlloca.cpp
/// This function should be removed later.
static bool isSupportedAtomicType(Type *Ty) {
  return Ty->isIntegerTy() || Ty->isPointerTy() || Ty->isFloatingPointTy();
}

/// \brief Helper to combine a load to a new type.
///
/// FIXME: This is copied from InstCombineLoadStoreAlloca.cpp
/// This function should be removed later.
static LoadInst *combineLoadToNewType(const DataLayout &DL,
                                      IRBuilder<> &Builder,
                                      LoadInst &LI, Type *NewTy,
                                      const Twine &Suffix = "") {
  assert((!LI.isAtomic() || isSupportedAtomicType(NewTy)) &&
         "can't fold an atomic load to requested type");
  
  Value *Ptr = LI.getPointerOperand();
  unsigned AS = LI.getPointerAddressSpace();
  SmallVector<std::pair<unsigned, MDNode *>, 8> MD;
  LI.getAllMetadata(MD);

  LoadInst *NewLoad = Builder.CreateAlignedLoad(
      Builder.CreateBitCast(Ptr, NewTy->getPointerTo(AS)),
      LI.getAlignment(), LI.isVolatile(), LI.getName() + Suffix);
  NewLoad->setAtomic(LI.getOrdering(), LI.getSyncScopeID());
  MDBuilder MDB(NewLoad->getContext());
  for (const auto &MDPair : MD) {
    unsigned ID = MDPair.first;
    MDNode *N = MDPair.second;
    // Note, essentially every kind of metadata should be preserved here! This
    // routine is supposed to clone a load instruction changing *only its type*.
    // The only metadata it makes sense to drop is metadata which is invalidated
    // when the pointer type changes. This should essentially never be the case
    // in LLVM, but we explicitly switch over only known metadata to be
    // conservatively correct. If you are adding metadata to LLVM which pertains
    // to loads, you almost certainly want to add it here.
    switch (ID) {
    case LLVMContext::MD_dbg:
    case LLVMContext::MD_tbaa:
    case LLVMContext::MD_prof:
    case LLVMContext::MD_fpmath:
    case LLVMContext::MD_tbaa_struct:
    case LLVMContext::MD_invariant_load:
    case LLVMContext::MD_alias_scope:
    case LLVMContext::MD_noalias:
    case LLVMContext::MD_nontemporal:
    case LLVMContext::MD_mem_parallel_loop_access:
      // All of these directly apply.
      NewLoad->setMetadata(ID, N);
      break;

    case LLVMContext::MD_nonnull:
      copyNonnullMetadata(LI, N, *NewLoad);
      break;
    case LLVMContext::MD_align:
    case LLVMContext::MD_dereferenceable:
    case LLVMContext::MD_dereferenceable_or_null:
      // These only directly apply if the new type is also a pointer.
      if (NewTy->isPointerTy())
        NewLoad->setMetadata(ID, N);
      break;
    case LLVMContext::MD_range:
      copyRangeMetadata(DL, LI, N, *NewLoad);
      break;
    }
  }
  return NewLoad;
}

/// \brief Combine a store to a new type.
///
/// FIXME: This is copied from InstCombineLoadStoreAlloca.cpp
/// This function should be removed later.
static StoreInst *combineStoreToNewValue(IRBuilder<> &Builder,
                                         StoreInst &SI, Value *V) {
  assert((!SI.isAtomic() || isSupportedAtomicType(V->getType())) &&
         "can't fold an atomic store of requested type");
  
  Value *Ptr = SI.getPointerOperand();
  unsigned AS = SI.getPointerAddressSpace();
  SmallVector<std::pair<unsigned, MDNode *>, 8> MD;
  SI.getAllMetadata(MD);

  StoreInst *NewStore = Builder.CreateAlignedStore(
      V, Builder.CreateBitCast(Ptr, V->getType()->getPointerTo(AS)),
      SI.getAlignment(), SI.isVolatile());
  NewStore->setAtomic(SI.getOrdering(), SI.getSyncScopeID());
  for (const auto &MDPair : MD) {
    unsigned ID = MDPair.first;
    MDNode *N = MDPair.second;
    // Note, essentially every kind of metadata should be preserved here! This
    // routine is supposed to clone a store instruction changing *only its
    // type*. The only metadata it makes sense to drop is metadata which is
    // invalidated when the pointer type changes. This should essentially
    // never be the case in LLVM, but we explicitly switch over only known
    // metadata to be conservatively correct. If you are adding metadata to
    // LLVM which pertains to stores, you almost certainly want to add it
    // here.
    switch (ID) {
    case LLVMContext::MD_dbg:
    case LLVMContext::MD_tbaa:
    case LLVMContext::MD_prof:
    case LLVMContext::MD_fpmath:
    case LLVMContext::MD_tbaa_struct:
    case LLVMContext::MD_alias_scope:
    case LLVMContext::MD_noalias:
    case LLVMContext::MD_nontemporal:
    case LLVMContext::MD_mem_parallel_loop_access:
      // All of these directly apply.
      NewStore->setMetadata(ID, N);
      break;

    case LLVMContext::MD_invariant_load:
    case LLVMContext::MD_nonnull:
    case LLVMContext::MD_range:
    case LLVMContext::MD_align:
    case LLVMContext::MD_dereferenceable:
    case LLVMContext::MD_dereferenceable_or_null:
      // These don't apply for stores.
      break;
    }
  }

  return NewStore;
}




static bool canonicalizeLoadInst(LoadInst &LI) {
  if (!LI.isUnordered())
    return false;

  if (LI.use_empty())
    return false;

  // swifterror values can't be bitcasted.
  // NOTE: This condition is copied from combineLoadToOperationType
  // in InstCombineLoadStoreAlloca.cpp
  if (LI.getPointerOperand()->isSwiftError())
    return false;

  Type *Ty = LI.getType();
  const DataLayout &DL = LI.getModule()->getDataLayout();

  PointerType *I8PtrTy = PointerType::get(
        Type::getIntNTy(LI.getContext(), 8), 0);
  if (Ty == I8PtrTy)
    // It is already i8* ty.
    return false;
  if (Ty->isIntegerTy() &&
      Ty->getIntegerBitWidth() != DL.getPointerSizeInBits(0))
    // Cannot canonicalize this load.
    return false;
  if (!Ty->isPointerTy() && !Ty->isIntegerTy())
    // Do not canonicalize this load.
    return false;
  
  // All of users of LI should be StoreInst.
  if (!all_of(LI.users(), [&LI](User *U) {
        auto *SI = dyn_cast<StoreInst>(U);
        return SI && SI->getPointerOperand() != &LI &&
               !SI->getPointerOperand()->isSwiftError();
      }))
    return false;

  // Try to canonicalize integer loads which are only ever stored to operate
  // over i8* pointers. The width of the integer should be equal to the
  // size of a pointer.
  IRBuilder<> Builder(&LI);
  LoadInst *NewLoad = combineLoadToNewType(DL, Builder, LI, I8PtrTy);

  // Replace all the stores with stores of the newly loaded value.
  for (auto UI = LI.user_begin(), UE = LI.user_end(); UI != UE;) {
    auto *SI = cast<StoreInst>(*UI++);
    Builder.SetInsertPoint(SI);
    combineStoreToNewValue(Builder, *SI, NewLoad);
    SI->eraseFromParent();
  }
  assert(LI.use_empty() && "Failed to remove all users of the load!");
  LI.eraseFromParent();
  return true;
}

static bool runOnBasicBlock(BasicBlock &BB) {
  bool Changed = false;
  SmallVector<LoadInst*, 32> Worklist;
  for (BasicBlock::iterator DI = BB.begin(), DE = BB.end(); DI != DE;) {
    Instruction *Inst = &*DI++;
    LoadInst *LI = dyn_cast<LoadInst>(Inst);
    if (!LI) continue;
    Worklist.push_back(LI);
  }
  for (auto I = Worklist.begin(); I != Worklist.end(); I++)
    Changed |= canonicalizeLoadInst(**I);
  return Changed;
}

static bool canonicalize(Function &F) {
  bool Changed = false;
  for (BasicBlock &BB : F) {
    Changed |= runOnBasicBlock(BB);
  }
  return Changed;
}

PreservedAnalyses CanonicalizeTypeToI8PtrPass::run(Function &F, FunctionAnalysisManager &FAM) {
  canonicalize(F);
  return PreservedAnalyses::all();
}

namespace {
class CanonicalizeTypeToI8Ptr : public FunctionPass {
public:
  static char ID;
  CanonicalizeTypeToI8Ptr() : FunctionPass(ID) {
    initializeCanonicalizeTypeToI8PtrPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override {
    if (skipFunction(F))
      return false;
    FunctionAnalysisManager DummyFAM;
    auto PA = Impl.run(F, DummyFAM);
    return !PA.areAllPreserved();
  }
private:
  CanonicalizeTypeToI8PtrPass Impl;
};

}

char CanonicalizeTypeToI8Ptr::ID = 0;
INITIALIZE_PASS(CanonicalizeTypeToI8Ptr, "canonicalize-to-i8ptr",
                "Canonicalize load/stores to i8*", false, false)

FunctionPass *llvm::createCanonicalizeTypeToI8PtrPass() {
  return new CanonicalizeTypeToI8Ptr();
}
