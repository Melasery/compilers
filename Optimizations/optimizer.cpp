#include <llvm-c/Core.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/Types.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <llvm-c/Core.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/Types.h>
#include <map>
#include <set>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define prt(x)                                                                 \
  if (x) {                                                                     \
    printf("%s\n", x);                                                         \
  }

bool changed = false;

/* read and load  */
LLVMModuleRef createLLVMModel(char *filename) {
  char *err = 0;
  LLVMMemoryBufferRef ll_f = 0;
  LLVMModuleRef m = 0;

  LLVMCreateMemoryBufferWithContentsOfFile(filename, &ll_f, &err);

  if (err != NULL) {
    prt(err);
    return NULL;
  }

  LLVMParseIRInContext(LLVMGetGlobalContext(), ll_f, &m, &err);

  if (err != NULL) {
    prt(err);
  }

  return m;
}

/*check if safe to eliminate load instructions */
bool isSafeCSE(LLVMValueRef instA, LLVMValueRef instB, LLVMBasicBlockRef bb) {
  if (LLVMGetInstructionOpcode(instA) == LLVMLoad) {
    LLVMValueRef ptrA = LLVMGetOperand(instA, 0);
    bool foundA = false;

    for (LLVMValueRef it = LLVMGetFirstInstruction(bb); it;
         it = LLVMGetNextInstruction(it)) {
      if (it == instA) {
        foundA = true;
        continue;
      }
      if (it == instB)
        break;

      if (foundA) {
        if (LLVMGetInstructionOpcode(it) == LLVMStore) {
          if (ptrA == LLVMGetOperand(it, 1))
            return false;
        } else if (LLVMGetInstructionOpcode(it) == LLVMCall) {
          return false;
        }
      }
    }
  }
  return true;
}

/* common subexpression elimination */
void runCSE(LLVMBasicBlockRef bb) {
  for (LLVMValueRef a = LLVMGetFirstInstruction(bb); a;
       a = LLVMGetNextInstruction(a)) {
    LLVMOpcode opA = LLVMGetInstructionOpcode(a);

    if (opA == LLVMCall || opA == LLVMStore || opA == LLVMAlloca ||
        LLVMIsATerminatorInst(a) || opA == LLVMGetElementPtr) {
      continue;
    }

    LLVMValueRef b = LLVMGetNextInstruction(a);
    while (b != NULL) {
      LLVMValueRef nextB = LLVMGetNextInstruction(b);
      LLVMOpcode opB = LLVMGetInstructionOpcode(b);

      if (opA == opB) {
        int nA = LLVMGetNumOperands(a);
        int nB = LLVMGetNumOperands(b);

        if (nA == nB) {
          bool match = true;
          for (int i = 0; i < nA; i++) {
            if (LLVMGetOperand(a, i) != LLVMGetOperand(b, i)) {
              match = false;
              break;
            }
          }

          if (match && isSafeCSE(a, b, bb)) {
            LLVMReplaceAllUsesWith(b, a);
            changed = true;
          }
        }
      }
      b = nextB;
    }
  }
}

/* dead code elimination */
void runDCE(LLVMValueRef func) {
  bool localChanged = true;
  while (localChanged) {
    localChanged = false;
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb;
         bb = LLVMGetNextBasicBlock(bb)) {
      LLVMValueRef inst = LLVMGetFirstInstruction(bb);
      while (inst) {
        LLVMValueRef next = LLVMGetNextInstruction(inst);
        LLVMOpcode op = LLVMGetInstructionOpcode(inst);

        if (op != LLVMCall && op != LLVMStore && !LLVMIsATerminatorInst(inst) &&
            op != LLVMAlloca && op != LLVMRet) {
          if (LLVMGetFirstUse(inst) == NULL) {
            LLVMInstructionEraseFromParent(inst);
            localChanged = true;
            changed = true;
          }
        }
        inst = next;
      }
    }
  }
}

/* constant folding */
void runCFold(LLVMValueRef func) {
  for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb;
       bb = LLVMGetNextBasicBlock(bb)) {
    for (LLVMValueRef inst = LLVMGetFirstInstruction(bb); inst;
         inst = LLVMGetNextInstruction(inst)) {
      LLVMOpcode op = LLVMGetInstructionOpcode(inst);

      if (op == LLVMAdd || op == LLVMSub || op == LLVMMul) {
        LLVMValueRef p1 = LLVMGetOperand(inst, 0);
        LLVMValueRef p2 = LLVMGetOperand(inst, 1);

        if (LLVMIsConstant(p1) && LLVMIsConstant(p2)) {
          LLVMValueRef val = NULL;
          if (op == LLVMAdd)
            val = LLVMConstAdd(p1, p2);
          else if (op == LLVMSub)
            val = LLVMConstSub(p1, p2);
          else if (op == LLVMMul)
            val = LLVMConstMul(p1, p2);

          if (val) {
            LLVMReplaceAllUsesWith(inst, val);
            changed = true;
          }
        }
      }
    }
  }
}

/* global constant propagation (store-load) */
void runGlobalConstProp(LLVMValueRef func) {
  std::set<LLVMValueRef> S;

  for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb;
       bb = LLVMGetNextBasicBlock(bb)) {
    for (LLVMValueRef inst = LLVMGetFirstInstruction(bb); inst;
         inst = LLVMGetNextInstruction(inst)) {
      if (LLVMGetInstructionOpcode(inst) == LLVMStore) {
        S.insert(inst);
      }
    }
  }

  std::map<LLVMBasicBlockRef, std::set<LLVMValueRef>> GEN, KILL, IN, OUT;

  for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb;
       bb = LLVMGetNextBasicBlock(bb)) {
    std::set<LLVMValueRef> genB, killB;
    for (LLVMValueRef I = LLVMGetFirstInstruction(bb); I;
         I = LLVMGetNextInstruction(I)) {
      if (LLVMGetInstructionOpcode(I) == LLVMStore) {
        genB.insert(I);

        LLVMValueRef memLocI = LLVMGetOperand(I, 1);
        std::set<LLVMValueRef> toRemoveGen;
        for (auto store : genB) {
          if (store != I && LLVMGetOperand(store, 1) == memLocI) {
            toRemoveGen.insert(store);
          }
        }
        for (auto store : toRemoveGen)
          genB.erase(store);

        for (auto store : S) {
          if (store != I && LLVMGetOperand(store, 1) == memLocI) {
            killB.insert(store);
          }
        }
      }
    }
    GEN[bb] = genB;
    KILL[bb] = killB;
    OUT[bb] = genB;
  }

  std::map<LLVMBasicBlockRef, std::set<LLVMBasicBlockRef>> preds;
  for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb;
       bb = LLVMGetNextBasicBlock(bb)) {
    LLVMValueRef term = LLVMGetBasicBlockTerminator(bb);
    if (term) {
      unsigned numSuccs = LLVMGetNumSuccessors(term);
      for (unsigned i = 0; i < numSuccs; i++) {
        LLVMBasicBlockRef succ = LLVMGetSuccessor(term, i);
        preds[succ].insert(bb);
      }
    }
  }

  bool dataFlowChanged = true;
  while (dataFlowChanged) {
    dataFlowChanged = false;
    for (LLVMBasicBlockRef B = LLVMGetFirstBasicBlock(func); B;
         B = LLVMGetNextBasicBlock(B)) {
      std::set<LLVMValueRef> inB;
      for (auto p : preds[B]) {
        for (auto store : OUT[p])
          inB.insert(store);
      }
      IN[B] = inB;

      std::set<LLVMValueRef> oldOut = OUT[B];
      std::set<LLVMValueRef> newOut = GEN[B];
      for (auto store : inB) {
        if (KILL[B].find(store) == KILL[B].end()) {
          newOut.insert(store);
        }
      }

      if (newOut != oldOut) {
        OUT[B] = newOut;
        dataFlowChanged = true;
      }
    }
  }

  std::set<LLVMValueRef> toDelete;
  for (LLVMBasicBlockRef B = LLVMGetFirstBasicBlock(func); B;
       B = LLVMGetNextBasicBlock(B)) {
    std::set<LLVMValueRef> R = IN[B];

    for (LLVMValueRef I = LLVMGetFirstInstruction(B); I;
         I = LLVMGetNextInstruction(I)) {
      if (LLVMGetInstructionOpcode(I) == LLVMStore) {
        R.insert(I);
        LLVMValueRef memLocI = LLVMGetOperand(I, 1);
        std::set<LLVMValueRef> toRemoveR;
        for (auto store : R) {
          if (store != I && LLVMGetOperand(store, 1) == memLocI) {
            toRemoveR.insert(store);
          }
        }
        for (auto store : toRemoveR)
          R.erase(store);
      } else if (LLVMGetInstructionOpcode(I) == LLVMLoad) {
        LLVMValueRef memLocLoad = LLVMGetOperand(I, 0);
        std::set<LLVMValueRef> reachingStores;

        for (auto store : R) {
          if (LLVMGetOperand(store, 1) == memLocLoad) {
            reachingStores.insert(store);
          }
        }

        if (!reachingStores.empty()) {
          bool allConstantAndSame = true;
          LLVMValueRef firstConstVal = NULL;

          for (auto store : reachingStores) {
            LLVMValueRef valStored = LLVMGetOperand(store, 0);
            if (!LLVMIsConstant(valStored)) {
              allConstantAndSame = false;
              break;
            }
            if (firstConstVal == NULL) {
              firstConstVal = valStored;
            } else if (firstConstVal != valStored) {
              allConstantAndSame = false;
              break;
            }
          }

          if (allConstantAndSame && firstConstVal != NULL) {
            LLVMReplaceAllUsesWith(I, firstConstVal);
            toDelete.insert(I);
            changed = true;
          }
        }
      }
    }
  }

  for (auto loadInst : toDelete) {
    LLVMInstructionEraseFromParent(loadInst);
  }
}

void optFunc(LLVMValueRef func, bool enable_global) {
  changed = true;
  while (changed) {
    changed = false;

    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb;
         bb = LLVMGetNextBasicBlock(bb)) {
      runCSE(bb);
    }

    runCFold(func);
    runDCE(func);

    if (enable_global) {
      runGlobalConstProp(func);
    }
  }
}

void optModule(LLVMModuleRef module, bool enable_global) {
  for (LLVMValueRef func = LLVMGetFirstFunction(module); func;
       func = LLVMGetNextFunction(func)) {
    if (LLVMGetFirstBasicBlock(func)) {
      optFunc(func, enable_global);
    }
  }
}

int main(int argc, char **argv) {
  LLVMModuleRef m;
  bool enable_global = false;
  int file_idx = 1;

  if (argc >= 2 && strcmp(argv[1], "-g") == 0) {
    enable_global = true;
    file_idx = 2;
  }

  if (argc >= file_idx + 1) {
    m = createLLVMModel(argv[file_idx]);
  } else {
    m = NULL;
    printf("Usage: %s [-g] <file.ll>\n", argv[0]);
    return 1;
  }

  if (m != NULL) {
    optModule(m, enable_global);
    if (argc >= file_idx + 2) {
      LLVMPrintModuleToFile(m, argv[file_idx + 1], NULL);
    } else {
      LLVMPrintModuleToFile(m, "optimized.ll", NULL);
    }
  } else {
    fprintf(stderr, "m is NULL\n");
  }

  return 0;
}
