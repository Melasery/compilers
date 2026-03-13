#ifndef IR_BUILDER_H
#define IR_BUILDER_H

#include "../Frontend/ast.h"
#include <llvm-c/Core.h>

// Generates an LLVM module from the given abstract syntax tree root node.
LLVMModuleRef buildIR(astNode* root);

#endif // IR_BUILDER_H
