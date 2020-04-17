//===--- XCOFFRelativeReferencedConstantDemotion.cpp ---------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/LLVMPasses/Passes.h"
#include "llvm/Pass.h"
#include "llvm/IR/PatternMatch.h"

using namespace llvm;
using namespace swift;

char XCOFFRelativeReferencedConstantDemotion::ID = 0;

INITIALIZE_PASS(XCOFFRelativeReferencedConstantDemotion,
                "xcoff-relative-referenced-constant-demotion", "",
                true, false)


ModulePass *swift::createXCOFFRelativeReferencedConstantDemotionPass() {
  initializeXCOFFRelativeReferencedConstantDemotionPass(
      *PassRegistry::getPassRegistry());
  return new XCOFFRelativeReferencedConstantDemotion();
}

void XCOFFRelativeReferencedConstantDemotion::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
}

static void forEachRelativelyReferencedGlobal(
    Constant *expr, function_ref<void(GlobalVariable *)> callback) {
  using namespace llvm::PatternMatch;
  Constant *maybeGlobal;
  if (match(expr, m_Sub(m_PtrToInt(m_Constant(maybeGlobal)), m_Value()))) {
    if (auto *global = dyn_cast<GlobalVariable>(maybeGlobal)) {
      callback(global);
    }
    return;
  }
  for (Value *op : expr->operand_values()) {
    forEachRelativelyReferencedGlobal(cast<Constant>(op), callback);
  }
}

static void demoteRelativelyReferencedConstants(Constant *expr) {
  forEachRelativelyReferencedGlobal(expr, [&](GlobalVariable *referenced) {
    if (!referenced->isConstant())
      return;
    referenced->setConstant(false); // must be first to break cycles
    demoteRelativelyReferencedConstants(referenced->getInitializer());
  });
}

/// The main entry point.
bool XCOFFRelativeReferencedConstantDemotion::runOnModule(Module &M) {
  for (GlobalVariable &global : M.globals()) {
    if (global.hasInitializer()) {
      if (global.isConstant()) {
        // FIXME: This is overkill; relative references to constants are okay.
        // But without doing this up front, we could end up demoting a dependency
        // later and retroactively invalidating this constant.
        // The right answer is to record these dependencies so we can update them
        // later if we need, but this is good enough for now.
        bool hasRelativelyReferencedGlobal = false;
        forEachRelativelyReferencedGlobal(global.getInitializer(),
                                          [&](GlobalVariable *) {
          hasRelativelyReferencedGlobal = true;
        });
        if (hasRelativelyReferencedGlobal) {
          global.setConstant(false);
        }
      }
      if (!global.isConstant()) {
        demoteRelativelyReferencedConstants(global.getInitializer());
      }
    }
  }
  return false;
}
