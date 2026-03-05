#ifndef _OBJC_RENAME_H_
#define _OBJC_RENAME_H_

#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

namespace llvm {

ModulePass *createObjCRenamePass(bool flag);
void initializeObjCRenamePass(PassRegistry &Registry);

} // namespace llvm

#endif
