//===-- DecanonicalizeTypeToI8Ptr.h ----------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/DecanonicalizeTypeToI8Ptr.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Transforms/Utils/Local.h"
using namespace llvm;

#define DEBUG_TYPE "decanonicalize-i8ptr"

bool optimizeLoadI8Ptr(LoadInst *Load) {
  Value *Ptr = Load->getPointerOperand();
  if (!Load->isSimple() || Ptr->isSwiftError())
    return false;

  // Load should be load i8*, i8** Ptr
  PointerType *I8PtrTy = PointerType::get(Type::getIntNTy(Load->getContext(), 8), 0);
  if (Load->getType() != I8PtrTy)
    return false;

  auto GetBitCastOp = [](Value *V) -> Value* {
      if (BitCastInst *BI = dyn_cast<BitCastInst>(V)) {
        return BI->getOperand(0);
      } else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(V)) {
        if (CE->getOpcode() == Instruction::BitCast)
          return CE->getOperand(0);
      }
      return nullptr;
    };

  // Ptr = bitcast ty** Ptr0 to i8**
  Value *Ptr0 = GetBitCastOp(Ptr);
  if (Ptr0 == nullptr)
    return false;
  if (!Ptr0->getType()->getPointerElementType()->isPtrOrPtrVectorTy())
    return false;

  // All stores should be `store i8* Load, i8** Ptr2`
  if (!all_of(Load->users(), [&Load, &Ptr0, &GetBitCastOp](User *U) {
        auto *SI = dyn_cast<StoreInst>(U);
        if (!SI) return false;

        Value *Ptr = SI->getPointerOperand();
        Value *Ptr2 = GetBitCastOp(Ptr);
        if (Ptr2 == nullptr) return false;

        // store Load, (bitcast Ptr2)
        return Ptr != Load && !Ptr->isSwiftError() &&
               Ptr2->getType() == Ptr0->getType();
      }))
    return false;

  const DataLayout &DL = Load->getModule()->getDataLayout();

  // Create a simplified load.
  SmallVector<std::pair<unsigned, MDNode *>, 8> MD;
  Load->getAllMetadata(MD);

  IRBuilder<> Builder(Load);
  LoadInst *NewLoad = Builder.CreateAlignedLoad(Ptr0, Load->getAlignment(),
      Load->isVolatile(), Load->getName() + ".smp");
  NewLoad->setAtomic(Load->getOrdering(), Load->getSyncScopeID());
  // Copying metadata..
  // This code is copied from InstCombine's combineLoadToNewType.
  MDBuilder MDB(Load->getContext());
  for (const auto &MDPair:MD) {
    unsigned ID = MDPair.first;
    MDNode *N = MDPair.second;
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
				copyNonnullMetadata(*Load, N, *NewLoad);
				break;
			case LLVMContext::MD_align:
			case LLVMContext::MD_dereferenceable:
			case LLVMContext::MD_dereferenceable_or_null:
				// These only directly apply if the new type is also a pointer.
				if (Ptr0->getType()->getPointerElementType()->isPointerTy())
					NewLoad->setMetadata(ID, N);
				break;
			case LLVMContext::MD_range:
				copyRangeMetadata(DL, *Load, N, *NewLoad);
				break;
		}
  }

  // Create simplified stores.
  for (auto UI = Load->user_begin(), UE = Load->user_end(); UI != UE;) {
    auto *SI = cast<StoreInst>(*UI++);
    // store Load, (bitcast Ptr2 to i8**)
    // ->
    // store NewLoad, Ptr2
    Value *Ptr2Org = SI->getPointerOperand();
    Value *Ptr2 = GetBitCastOp(Ptr2Org);
    // Replace Load with NewLoad.
    // Load was a value operand.
    Builder.SetInsertPoint(SI);

    StoreInst *NewStore = Builder.CreateAlignedStore(NewLoad, Ptr2,
        SI->getAlignment(), SI->isVolatile());
    NewStore->setAtomic(SI->getOrdering(), SI->getSyncScopeID());

    // Copying metadata..
    // This code is copied from InstCombine's combineStoreToNewValue.
    SmallVector<std::pair<unsigned, MDNode *>, 8> MD;
    SI->getAllMetadata(MD);
		for (const auto &MDPair : MD) {
			unsigned ID = MDPair.first;
			MDNode *N = MDPair.second;
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
				NewStore->setMetadata(ID, N);
				break;
			 case LLVMContext::MD_invariant_load:
			case LLVMContext::MD_nonnull:
			case LLVMContext::MD_range:
			case LLVMContext::MD_align:
			case LLVMContext::MD_dereferenceable:
			case LLVMContext::MD_dereferenceable_or_null:
				break;
			}
		}

    // Erase the store.
    SI->eraseFromParent();
    // Erase tie bitcast, if possible
    if (BitCastInst *BCI = dyn_cast<BitCastInst>(Ptr2Org))
      if (BCI->use_empty())
        BCI->eraseFromParent();
  }

  assert(Load->use_empty() && "Failed to remove all users of the load!");

  // Erase the old load.
  Load->eraseFromParent();
  // Remove the bitcast Ptr, if possible
  if (BitCastInst *BI = dyn_cast<BitCastInst>(Ptr))
    if (BI->use_empty())
      BI->eraseFromParent();

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
    Changed |= optimizeLoadI8Ptr(*I);
  return Changed;
}

static bool run(Function &F) {
  bool Changed = false;
  for (BasicBlock &BB : F) {
    Changed |= runOnBasicBlock(BB);
  }
  return Changed;
}

PreservedAnalyses DecanonicalizeTypeToI8PtrPass::run(Function &F, FunctionAnalysisManager &FAM) {
  if (!::run(F))
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  PA.preserve<GlobalsAA>();
  return PA;
}

namespace {
class DecanonicalizeTypeToI8Ptr : public FunctionPass {
public:
  static char ID;
  DecanonicalizeTypeToI8Ptr() : FunctionPass(ID) {
    initializeDecanonicalizeTypeToI8PtrPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addPreserved<DominatorTreeWrapperPass>();
    AU.addPreserved<GlobalsAAWrapperPass>();
    FunctionPass::getAnalysisUsage(AU);
  }

  bool runOnFunction(Function &F) override {
    if (skipFunction(F))
      return false;
    FunctionAnalysisManager DummyFAM;
    auto PA = Impl.run(F, DummyFAM);
    return !PA.areAllPreserved();
  }
private:
  DecanonicalizeTypeToI8PtrPass Impl;
};

}

char DecanonicalizeTypeToI8Ptr::ID = 0;
INITIALIZE_PASS(DecanonicalizeTypeToI8Ptr, "decanonicalize-i8ptr",
                "Decanonicalize load/stores to i8*", false, false)

FunctionPass *llvm::createDecanonicalizeTypeToI8PtrPass() {
  return new DecanonicalizeTypeToI8Ptr();
}
