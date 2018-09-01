//===-- DecanonicalizeTypeToI8Ptr.h ----------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_DECANONICALIZETYPETOI8PTR_H
#define LLVM_TRANSFORMS_SCALAR_DECANONICALIZETYPETOI8PTR_H

#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

class DecanonicalizeTypeToI8PtrPass : public PassInfoMixin<DecanonicalizeTypeToI8PtrPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};
}

#endif // LLVM_TRANSFORMS_SCALAR_DECANONICALIZETYPETOI8PTR_H
