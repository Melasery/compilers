

#include <llvm-c/Core.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/Types.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace std;

// physical register names indexed 0-2; -1 spilled
static const char *reg_names[] = {"%ebx", "%ecx", "%edx"};

// reg_map: instruction to physical reg index (0/1/2) or -1 (spill)
static map<LLVMValueRef, int> reg_map;

// per-BB liveness helpers
static map<LLVMValueRef, int> inst_index;
static map<LLVMValueRef, pair<int, int>> live_range;

// stack layout
static map<LLVMValueRef, int> offset_map;
static int localMem;

// basic block labels
static map<LLVMBasicBlockRef, string> bb_labels;

LLVMModuleRef createLLVMModel(char *filename) {
  char *err = nullptr;
  LLVMMemoryBufferRef ll_f = nullptr;
  LLVMModuleRef m = nullptr;

  LLVMCreateMemoryBufferWithContentsOfFile(filename, &ll_f, &err);
  if (err) {
    fprintf(stderr, "%s\n", err);
    return nullptr;
  }

  LLVMParseIRInContext(LLVMGetGlobalContext(), ll_f, &m, &err);
  if (err) {
    fprintf(stderr, "%s\n", err);
  }

  return m;
}

/*
 * compute_liveness: fills inst_index and live_range for one basic block.
 *
 * skip alloca instructions
 * every other instruction gets a sequential index.
 * live_range[I] = (def_index, last_use_index).
 */
void compute_liveness(LLVMBasicBlockRef bb) {
  inst_index.clear();
  live_range.clear();
  int idx = 0;
  // 1st pass: assign indices and record definition points
  for (LLVMValueRef I = LLVMGetFirstInstruction(bb); I;
       I = LLVMGetNextInstruction(I)) {
    if (LLVMGetInstructionOpcode(I) == LLVMAlloca)
      continue;
    inst_index[I] = idx;
    // start = end = def index; we'll push end forward with uses
    live_range[I] = {idx, idx};
    idx++;
  }

  // 2nd pass: for each instruction, update the endpoint of its operands
  for (LLVMValueRef I = LLVMGetFirstInstruction(bb); I;
       I = LLVMGetNextInstruction(I)) {
    if (LLVMGetInstructionOpcode(I) == LLVMAlloca)
      continue;
    if (inst_index.find(I) == inst_index.end())
      continue;
    int cur_idx = inst_index[I];
    int n_ops = LLVMGetNumOperands(I);
    for (int i = 0; i < n_ops; i++) {
      LLVMValueRef operand = LLVMGetOperand(I, i);
      // only track instructions defined in this same BB
      if (inst_index.find(operand) != inst_index.end()) {
        // extend the end of the operand's live range
        if (live_range[operand].second < cur_idx)
          live_range[operand].second = cur_idx;
      }
    }
  }
}

/*
 * find_spill: given the instruction being assigned (instr) and a sorted list
 * of candidates, find one that:
 *   1. reg_map value != -1
 *   2. overlapping liveness with instr
 *
 * sorted_list is ordered by decreasing live-range endpoint so we spill the
 * one that lives longest
 */
LLVMValueRef find_spill(LLVMValueRef instr,
                        const vector<LLVMValueRef> &sorted_list) {
  if (inst_index.find(instr) == inst_index.end())
    return nullptr;
  int instr_start = live_range[instr].first;
  int instr_end = live_range[instr].second;
  for (LLVMValueRef V : sorted_list) {
    if (V == instr)
      continue;
    auto it = reg_map.find(V);
    if (it == reg_map.end() || it->second == -1)
      continue;
    if (live_range.find(V) == live_range.end())
      continue;
    int v_start = live_range[V].first;
    int v_end = live_range[V].second;
    // overlapping: ranges [a,b] and [c,d] overlap when a<=d && c<=b
    if (v_start <= instr_end && instr_start <= v_end)
      return V;
  }
  return nullptr;
}

/*
 * Check whether the live range of an operand ends exactly at instruction Instr.
 * Used to decide when to release registers.
 */
static bool live_range_ends_at(LLVMValueRef operand, LLVMValueRef instr) {
  if (inst_index.find(operand) == inst_index.end())
    return false;
  if (live_range.find(operand) == live_range.end())
    return false;
  if (inst_index.find(instr) == inst_index.end())
    return false;
  return live_range[operand].second == inst_index[instr];
}

/*
 * reg_alloc: local linear-scan register allocation for the entire function.
 * runs per-BB; reg_map accumulates results across BBs.
 */
void reg_alloc(LLVMValueRef func) {
  reg_map.clear();

  for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb;
       bb = LLVMGetNextBasicBlock(bb)) {

    // available physical regs for this BB: 0=ebx, 1=ecx, 2=edx
    set<int> available = {0, 1, 2};

    compute_liveness(bb);

    // build sorted_list: instructions in this BB sorted by
    // DECREASING live-range endpoint
    vector<LLVMValueRef> sorted_list;
    for (auto &kv : live_range)
      sorted_list.push_back(kv.first);
    sort(sorted_list.begin(), sorted_list.end(),
         [](LLVMValueRef a, LLVMValueRef b) {
           return live_range[a].second > live_range[b].second;
         });

    for (LLVMValueRef I = LLVMGetFirstInstruction(bb); I;
         I = LLVMGetNextInstruction(I)) {

      LLVMOpcode op = LLVMGetInstructionOpcode(I);

      if (op == LLVMAlloca)
        continue;
      // (store, unconditional branch, conditional branch, void call)
      // For store/branch: no result; just reclaim dying operand registers.
      // For call: we handle specially later, but void calls have no result.
      bool is_void_inst = false;
      if (op == LLVMStore || LLVMIsATerminatorInst(I))
        is_void_inst = true;
      if (op == LLVMCall && LLVMGetTypeKind(LLVMTypeOf(I)) == LLVMVoidTypeKind)
        is_void_inst = true;

      if (is_void_inst) {
        int n_ops = LLVMGetNumOperands(I);
        for (int i = 0; i < n_ops; i++) {
          LLVMValueRef op_val = LLVMGetOperand(I, i);
          if (live_range_ends_at(op_val, I)) {
            auto it = reg_map.find(op_val);
            if (it != reg_map.end() && it->second != -1)
              available.insert(it->second);
          }
        }
        continue;
      }

      // Case 1: add/sub/mul where first operand's register can be reused
      if (op == LLVMAdd || op == LLVMSub || op == LLVMMul) {
        LLVMValueRef op0 = LLVMGetOperand(I, 0);
        LLVMValueRef op1 = LLVMGetOperand(I, 1);

        auto it0 = reg_map.find(op0);
        if (it0 != reg_map.end() && it0->second != -1 &&
            live_range_ends_at(op0, I)) {
          // reuse the register of op0
          int R = it0->second;
          reg_map[I] = R;
          // reclaim op1's register if it dies here
          if (live_range_ends_at(op1, I)) {
            auto it1 = reg_map.find(op1);
            if (it1 != reg_map.end() && it1->second != -1)
              available.insert(it1->second);
          }
          continue;
        }
      }

      // Case 2: a physical register is available
      if (!available.empty()) {
        int R = *available.begin();
        available.erase(available.begin());
        reg_map[I] = R;
        // reclaim dying operand registers
        int n_ops = LLVMGetNumOperands(I);
        for (int i = 0; i < n_ops; i++) {
          LLVMValueRef op_val = LLVMGetOperand(I, i);
          if (live_range_ends_at(op_val, I)) {
            auto it = reg_map.find(op_val);
            if (it != reg_map.end() && it->second != -1)
              available.insert(it->second);
          }
        }
        continue;
      }

      // Case 3: no physical register available
      {
        LLVMValueRef V = find_spill(I, sorted_list);
        if (V == nullptr) {
          // nothing to spill
          reg_map[I] = -1;
        } else {
          // compare: spill V if V has more uses (or longer range) than I
          int v_uses = 0;
          int instr_uses = 0;
          for (LLVMUseRef u = LLVMGetFirstUse(V); u; u = LLVMGetNextUse(u))
            v_uses++;
          for (LLVMUseRef u = LLVMGetFirstUse(I); u; u = LLVMGetNextUse(u))
            instr_uses++;

          bool spill_instr = (v_uses > instr_uses) ||
                             (live_range[I].second >= live_range[V].second &&
                              v_uses == instr_uses);

          if (spill_instr) {
            // spill current instruction instead
            reg_map[I] = -1;
          } else {
            // spill V, give its register to I
            int R = reg_map[V];
            reg_map[V] = -1;
            reg_map[I] = R;
          }
        }
        // reclaim dying operand registers
        int n_ops = LLVMGetNumOperands(I);
        for (int i = 0; i < n_ops; i++) {
          LLVMValueRef op_val = LLVMGetOperand(I, i);
          if (live_range_ends_at(op_val, I)) {
            auto it = reg_map.find(op_val);
            if (it != reg_map.end() && it->second != -1)
              available.insert(it->second);
          }
        }
      }
    } // for each instruction
  }   // for each BB
}

void getOffsetMap(LLVMValueRef func) {
  offset_map.clear();
  localMem = 4; // start at 4
  // Handle function parameter
  LLVMValueRef param = nullptr;
  if (LLVMCountParams(func) > 0) {
    param = LLVMGetParam(func, 0);
    offset_map[param] = 8; // caller pushed it; lives at 8(%ebp)
  }
  for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb;
       bb = LLVMGetNextBasicBlock(bb)) {
    for (LLVMValueRef I = LLVMGetFirstInstruction(bb); I;
         I = LLVMGetNextInstruction(I)) {
      LLVMOpcode op = LLVMGetInstructionOpcode(I);

      if (op == LLVMAlloca) {
        localMem += 4;
        offset_map[I] = -localMem;
      } else if (op == LLVMStore) {
        LLVMValueRef val = LLVMGetOperand(I, 0);  // value being stored
        LLVMValueRef dest = LLVMGetOperand(I, 1); // destination pointer
        if (param && val == param) {
          // store of the parameter into its alloca slot
          // propagate the parameter's real offset to dest
          if (offset_map.count(val)) {
            int x = offset_map[val];
            offset_map[dest] = x;
          }
        } else if (!LLVMIsConstant(val) && !(param && val == param)) {
          // link val's stack slot to dest's slot
          if (offset_map.count(dest)) {
            int x = offset_map[dest];
            offset_map[val] = x;
          }
        }
      } else if (op == LLVMLoad) {
        LLVMValueRef src = LLVMGetOperand(I, 0);
        if (offset_map.count(src))
          offset_map[I] = offset_map[src];
      }
    }
  }
}

void createBBLabels(LLVMValueRef func) {
  bb_labels.clear();
  int cnt = 0;
  for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb;
       bb = LLVMGetNextBasicBlock(bb)) {
    bb_labels[bb] = ".BB" + to_string(cnt++);
  }
}

static void printDirectives(FILE *out, const char *funcName) {
  fprintf(out, "\t.text\n");
  fprintf(out, "\t.globl %s\n", funcName);
  fprintf(out, "\t.type %s, @function\n", funcName);
  fprintf(out, "%s:\n", funcName);
}

static void printFunctionEnd(FILE *out) {
  fprintf(out, "\tleave\n");
  fprintf(out, "\tret\n");
}

// Returns the display string for a physical register index, or nullptr
static const char *physReg(LLVMValueRef v) {
  auto it = reg_map.find(v);
  if (it != reg_map.end() && it->second != -1)
    return reg_names[it->second];
  return nullptr;
}

// True if v is spilled (in reg_map with value -1)
static bool isSpilled(LLVMValueRef v) {
  auto it = reg_map.find(v);
  return (it != reg_map.end() && it->second == -1);
}
// True if v has a known stack offset
static bool inMemory(LLVMValueRef v) { return offset_map.count(v) != 0; }

// Emit "movl <src>, <dst>" for an arbitrary value src into register dst
// src can be: constant, register-allocated instruction, or memory instruction
static void emitLoad(FILE *out, LLVMValueRef src, const char *dst_reg) {
  if (LLVMIsConstant(src)) {
    long long cv = LLVMConstIntGetSExtValue(src);
    fprintf(out, "\tmovl $%lld, %s\n", cv, dst_reg);
  } else if (const char *r = physReg(src)) {
    if (strcmp(r, dst_reg) != 0)
      fprintf(out, "\tmovl %s, %s\n", r, dst_reg);
  } else if (inMemory(src)) {
    fprintf(out, "\tmovl %d(%%ebp), %s\n", offset_map[src], dst_reg);
  }
}

void codegen(LLVMValueRef func, FILE *out) {
  const char *funcName = LLVMGetValueName(func);

  createBBLabels(func);

  printDirectives(out, funcName);

  getOffsetMap(func);

  // Prologue
  fprintf(out, "\tpushl %%ebp\n");
  fprintf(out, "\tmovl %%esp, %%ebp\n");
  fprintf(out, "\tsubl $%d, %%esp\n", localMem);
  fprintf(out, "\tpushl %%ebx\n");

  // Run register allocation
  reg_alloc(func);
  // Emit code for each basic block
  for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb;
       bb = LLVMGetNextBasicBlock(bb)) {
    fprintf(out, "%s:\n", bb_labels[bb].c_str());

    for (LLVMValueRef I = LLVMGetFirstInstruction(bb); I;
         I = LLVMGetNextInstruction(I)) {
      LLVMOpcode op = LLVMGetInstructionOpcode(I);

      if (op == LLVMRet) {
        // ret void: just pop ebx, printFunctionEnd
        // ret i32 A:
        LLVMValueRef retVal =
            LLVMGetNumOperands(I) > 0 ? LLVMGetOperand(I, 0) : nullptr;
        if (retVal) {
          if (LLVMIsConstant(retVal)) {
            long long cv = LLVMConstIntGetSExtValue(retVal);
            fprintf(out, "\tmovl $%lld, %%eax\n", cv);
          } else if (const char *r = physReg(retVal)) {
            fprintf(out, "\tmovl %s, %%eax\n", r);
          } else if (inMemory(retVal)) {
            fprintf(out, "\tmovl %d(%%ebp), %%eax\n", offset_map[retVal]);
          }
        }
        fprintf(out, "\tpopl %%ebx\n");
        printFunctionEnd(out);
        continue;
      }

      // ALLOCA
      if (op == LLVMAlloca) {
        continue;
      }

      // LOAD
      //  %a = load i32, i32* %b
      if (op == LLVMLoad) {
        const char *r = physReg(I);
        if (r) {
          LLVMValueRef src = LLVMGetOperand(I, 0);
          if (inMemory(src))
            fprintf(out, "\tmovl %d(%%ebp), %s\n", offset_map[src], r);
        }
        // if spilled, the load result lives in memory already via offset_map
        continue;
      }

      // STORE
      // store i32 A, i32* %b
      if (op == LLVMStore) {
        LLVMValueRef val = LLVMGetOperand(I, 0);  // value
        LLVMValueRef dest = LLVMGetOperand(I, 1); // ptr

        // If storing the parameter: ignore
        if (LLVMCountParams(func) > 0 && val == LLVMGetParam(func, 0))
          continue;

        if (LLVMIsConstant(val)) {
          long long cv = LLVMConstIntGetSExtValue(val);
          if (inMemory(dest))
            fprintf(out, "\tmovl $%lld, %d(%%ebp)\n", cv, offset_map[dest]);
        } else {
          // val is a tempo
          if (const char *r = physReg(val)) {
            if (inMemory(dest))
              fprintf(out, "\tmovl %s, %d(%%ebp)\n", r, offset_map[dest]);
          } else {
            // val is spilled
            int c1 = inMemory(val) ? offset_map[val] : 0;
            int c2 = inMemory(dest) ? offset_map[dest] : 0;
            if (inMemory(val) && inMemory(dest)) {
              fprintf(out, "\tmovl %d(%%ebp), %%eax\n", c1);
              fprintf(out, "\tmovl %%eax, %d(%%ebp)\n", c2);
            }
          }
        }
        continue;
      }

      // CALL
      if (op == LLVMCall) {
        // save caller-save regs
        fprintf(out, "\tpushl %%ecx\n");
        fprintf(out, "\tpushl %%edx\n");

        // get the function being called (last operand)
        LLVMValueRef callee = LLVMGetOperand(I, LLVMGetNumOperands(I) - 1);
        const char *callee_name = LLVMGetValueName(callee);
        bool has_param = LLVMGetNumOperands(I) > 1; // at least one arg + callee

        if (has_param) {
          LLVMValueRef P = LLVMGetOperand(I, 0);
          if (LLVMIsConstant(P)) {
            long long cv = LLVMConstIntGetSExtValue(P);
            fprintf(out, "\tpushl $%lld\n", cv);
          } else if (const char *r = physReg(P)) {
            fprintf(out, "\tpushl %s\n", r);
          } else if (inMemory(P)) {
            fprintf(out, "\tpushl %d(%%ebp)\n", offset_map[P]);
          }
        }

        fprintf(out, "\tcall %s\n", callee_name);

        if (has_param) {
          fprintf(out, "\taddl $4, %%esp\n");
        }

        fprintf(out, "\tpopl %%edx\n");
        fprintf(out, "\tpopl %%ecx\n");

        // if the call has a result (non-void)
        if (LLVMGetTypeKind(LLVMTypeOf(I)) != LLVMVoidTypeKind) {
          if (const char *r = physReg(I)) {
            fprintf(out, "\tmovl %%eax, %s\n", r);
          } else if (inMemory(I)) {
            fprintf(out, "\tmovl %%eax, %d(%%ebp)\n", offset_map[I]);
          }
        }
        continue;
      }

      // BRANCH
      if (LLVMIsATerminatorInst(I) && op == LLVMBr) {
        if (LLVMGetNumOperands(I) == 1) {
          // unconditional: br label %b
          LLVMBasicBlockRef target =
              LLVMValueAsBasicBlock(LLVMGetOperand(I, 0));
          fprintf(out, "\tjmp %s\n", bb_labels[target].c_str());
        } else {
          // conditional: br i1 %cond, label %true_bb, label %false_bb
          // operand 0 = condition, operand 2 = true, operand 1 = false
          LLVMValueRef cond = LLVMGetOperand(I, 0);
          LLVMBasicBlockRef true_bb =
              LLVMValueAsBasicBlock(LLVMGetOperand(I, 2));
          LLVMBasicBlockRef false_bb =
              LLVMValueAsBasicBlock(LLVMGetOperand(I, 1));

          const char *L1 = bb_labels[true_bb].c_str();
          const char *L2 = bb_labels[false_bb].c_str();

          // find the predicate from the icmp instruction
          LLVMIntPredicate pred = LLVMGetICmpPredicate(cond);
          const char *jxx = "je"; // default
          switch (pred) {
          case LLVMIntSLT:
            jxx = "jl";
            break;
          case LLVMIntSGT:
            jxx = "jg";
            break;
          case LLVMIntSLE:
            jxx = "jle";
            break;
          case LLVMIntSGE:
            jxx = "jge";
            break;
          case LLVMIntEQ:
            jxx = "je";
            break;
          case LLVMIntNE:
            jxx = "jne";
            break;
          default:
            jxx = "je";
            break;
          }
          fprintf(out, "\t%s %s\n", jxx, L1);
          fprintf(out, "\tjmp %s\n", L2);
        }
        continue;
      }

      // ARITHMETIC: ADD / SUB / MUL
      if (op == LLVMAdd || op == LLVMSub || op == LLVMMul) {
        LLVMValueRef A = LLVMGetOperand(I, 0);
        LLVMValueRef B = LLVMGetOperand(I, 1);

        // R = assigned reg of I, or %eax if spilled
        const char *R = physReg(I);
        if (!R)
          R = "%eax";

        const char *arith_op = (op == LLVMAdd)   ? "addl"
                               : (op == LLVMSub) ? "subl"
                                                 : "imull";

        // Load A into R
        emitLoad(out, A, R);

        // Apply op with B
        if (LLVMIsConstant(B)) {
          long long cv = LLVMConstIntGetSExtValue(B);
          fprintf(out, "\t%s $%lld, %s\n", arith_op, cv, R);
        } else if (const char *rb = physReg(B)) {
          fprintf(out, "\t%s %s, %s\n", arith_op, rb, R);
        } else if (inMemory(B)) {
          fprintf(out, "\t%s %d(%%ebp), %s\n", arith_op, offset_map[B], R);
        }

        // If I was spilled, store result from %eax to memory
        if (isSpilled(I) && inMemory(I)) {
          fprintf(out, "\tmovl %%eax, %d(%%ebp)\n", offset_map[I]);
        }
        continue;
      }

      // ICMP (COMPARE)
      if (op == LLVMICmp) {
        LLVMValueRef A = LLVMGetOperand(I, 0);
        LLVMValueRef B = LLVMGetOperand(I, 1);

        // only need to emit cmp; it sets flags for the branch.
        //  R = assigned reg of I, or %eax as temp
        const char *R = physReg(I);
        if (!R)
          R = "%eax";

        // Load A into R
        emitLoad(out, A, R);

        // Emit cmpl B, R
        if (LLVMIsConstant(B)) {
          long long cv = LLVMConstIntGetSExtValue(B);
          fprintf(out, "\tcmpl $%lld, %s\n", cv, R);
        } else if (const char *rb = physReg(B)) {
          fprintf(out, "\tcmpl %s, %s\n", rb, R);
        } else if (inMemory(B)) {
          fprintf(out, "\tcmpl %d(%%ebp), %s\n", offset_map[B], R);
        }
        continue;
      }
    }
  }
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <input.ll> <output.s>\n", argv[0]);
    return 1;
  }

  LLVMModuleRef m = createLLVMModel(argv[1]);
  if (!m) {
    fprintf(stderr, "Failed to load module.\n");
    return 1;
  }

  FILE *out = fopen(argv[2], "w");
  if (!out) {
    perror(argv[2]);
    return 1;
  }

  for (LLVMValueRef func = LLVMGetFirstFunction(m); func;
       func = LLVMGetNextFunction(func)) {
    if (!LLVMGetFirstBasicBlock(func))
      continue; // skip external declarations
    codegen(func, out);
  }

  fclose(out);
  return 0;
}
