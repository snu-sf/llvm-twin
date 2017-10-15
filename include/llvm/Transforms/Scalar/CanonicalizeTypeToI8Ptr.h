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

#ifndef LLVM_TRANSFORMS_SCALAR_CANONICALIZETYPETOI8PTR_H
#define LLVM_TRANSFORMS_SCALAR_CANONICALIZETYPETOI8PTR_H

#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

class CanonicalizeTypeToI8PtrPass : public PassInfoMixin<CanonicalizeTypeToI8PtrPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};
}

#endif // LLVM_TRANSFORMS_SCALAR_CANONICALIZETYPETOI8PTR_H
