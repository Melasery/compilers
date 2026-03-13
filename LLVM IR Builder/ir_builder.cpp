#include "ir_builder.h"
#include <iostream>
#include <map>
#include <vector>
#include <queue>
#include <set>
#include <string>
#include <string.h>

using namespace std;

// global stuff I need to access everywhere
map<string, LLVMValueRef> var_map;
LLVMValueRef ret_ref;
LLVMBasicBlockRef retBB;
LLVMBuilderRef builder;
LLVMModuleRef module;
LLVMValueRef printFunc;
LLVMValueRef readFunc;

// for renaming
vector<map<string, string>> scopes_ir;
int rename_counter = 0;
vector<string> all_unique_vars;

// walks the AST and renames every variable to have a unique name
// also keeps track of all names so we can alloca them all in entry
void rename_ast(astNode* node) {
  if (!node) return;

  switch (node->type) {
    case ast_prog:
      scopes_ir.push_back(map<string, string>());
      rename_ast(node->prog.func);
      scopes_ir.pop_back();
      break;

    case ast_func:
      scopes_ir.push_back(map<string, string>());
      if (node->func.param)
        rename_ast(node->func.param);
      if (node->func.body)
        rename_ast(node->func.body);
      scopes_ir.pop_back();
      break;

    case ast_stmt:
      switch (node->stmt.type) {
        case ast_block: {
          scopes_ir.push_back(map<string, string>());
          if (node->stmt.block.stmt_list) {
            for (auto stmt : *node->stmt.block.stmt_list)
              rename_ast(stmt);
          }
          scopes_ir.pop_back();
          break;
        }
        case ast_decl: {
          string old_name = node->stmt.decl.name;
          string new_name = old_name + "_" + to_string(rename_counter++);
          scopes_ir.back()[old_name] = new_name;

          free(node->stmt.decl.name);
          node->stmt.decl.name = strdup(new_name.c_str());

          all_unique_vars.push_back(new_name);
          break;
        }
        case ast_asgn:
          rename_ast(node->stmt.asgn.lhs);
          rename_ast(node->stmt.asgn.rhs);
          break;
        case ast_call:
          if (node->stmt.call.param) rename_ast(node->stmt.call.param);
          break;
        case ast_ret:
          if (node->stmt.ret.expr) rename_ast(node->stmt.ret.expr);
          break;
        case ast_while:
          rename_ast(node->stmt.whilen.cond);
          rename_ast(node->stmt.whilen.body);
          break;
        case ast_if:
          rename_ast(node->stmt.ifn.cond);
          rename_ast(node->stmt.ifn.if_body);
          if (node->stmt.ifn.else_body) rename_ast(node->stmt.ifn.else_body);
          break;
      }
      break;

    case ast_var: {
      string old_name = node->var.name;
      string new_name = "";

      // search from innermost scope out
      for (int i = scopes_ir.size() - 1; i >= 0; i--) {
        if (scopes_ir[i].count(old_name)) {
          new_name = scopes_ir[i][old_name];
          break;
        }
      }
      if (!new_name.empty()) {
        free(node->var.name);
        node->var.name = strdup(new_name.c_str());
      }
      break;
    }
    case ast_rexpr:
      rename_ast(node->rexpr.lhs);
      rename_ast(node->rexpr.rhs);
      break;
    case ast_bexpr:
      rename_ast(node->bexpr.lhs);
      rename_ast(node->bexpr.rhs);
      break;
    case ast_uexpr:
      rename_ast(node->uexpr.expr);
      break;
    case ast_cnst:
    case ast_extern:
      break;
  }
}

// generates the LLVM IR value for an expression node
LLVMValueRef genIRExpr(astNode* node) {
  if (!node) return NULL;

  switch (node->type) {
    case ast_cnst:
      return LLVMConstInt(LLVMInt32Type(), node->cnst.value, 0);

    case ast_var: {
      //load from wherever we allocated this variable
      LLVMValueRef alloc_inst = var_map[string(node->var.name)];
      return LLVMBuildLoad2(builder, LLVMInt32Type(), alloc_inst, "load");
    }

    case ast_uexpr: {
      // unary minus is just 0 - expr
      LLVMValueRef expr_val = genIRExpr(node->uexpr.expr);
      LLVMValueRef zero = LLVMConstInt(LLVMInt32Type(), 0, 0);
      return LLVMBuildSub(builder, zero, expr_val, "neg");
    }

    case ast_bexpr: {
      LLVMValueRef lhs_val = genIRExpr(node->bexpr.lhs);
      LLVMValueRef rhs_val = genIRExpr(node->bexpr.rhs);

      switch (node->bexpr.op) {
        case add:    return LLVMBuildAdd(builder, lhs_val, rhs_val, "add");
        case sub:    return LLVMBuildSub(builder, lhs_val, rhs_val, "sub");
        case mul:    return LLVMBuildMul(builder, lhs_val, rhs_val, "mul");
        case divide: return LLVMBuildSDiv(builder, lhs_val, rhs_val, "div");
        default:     return NULL;
      }
    }

    case ast_rexpr: {
      LLVMValueRef lhs_val = genIRExpr(node->rexpr.lhs);
      LLVMValueRef rhs_val = genIRExpr(node->rexpr.rhs);

      LLVMIntPredicate op;
      switch (node->rexpr.op) {
        case lt:  op = LLVMIntSLT; break;
        case gt:  op = LLVMIntSGT; break;
        case le:  op = LLVMIntSLE; break;
        case ge:  op = LLVMIntSGE; break;
        case eq:  op = LLVMIntEQ;  break;
        case neq: op = LLVMIntNE;  break;
      }
      return LLVMBuildICmp(builder, op, lhs_val, rhs_val, "cmp");
    }

    case ast_stmt:
      // read() used as an expression (rhs of assignment etc.)
      if (node->stmt.type == ast_call && node->stmt.call.name &&
        string(node->stmt.call.name) == "read") {
        LLVMTypeRef readParamTypes[] = {};
        LLVMTypeRef readFuncType = LLVMFunctionType(LLVMInt32Type(), readParamTypes, 0, 0);
        return LLVMBuildCall2(builder, readFuncType, readFunc, NULL, 0, "call");
      }
      return NULL;

    default:
      return NULL;
  }
}

// generates IR for a statement and returns the basic block we ended up in
LLVMBasicBlockRef genIRStmt(astNode* node, LLVMBasicBlockRef startBB) {
  if (!node) return startBB;
  if (node->type != ast_stmt) return startBB;

  switch (node->stmt.type) {
    case ast_asgn: {
      LLVMPositionBuilderAtEnd(builder, startBB);
      LLVMValueRef rhs_val = genIRExpr(node->stmt.asgn.rhs);
      LLVMValueRef alloc_inst = var_map[string(node->stmt.asgn.lhs->var.name)];
      LLVMBuildStore(builder, rhs_val, alloc_inst);
      return startBB;
    }

    case ast_call: {
      LLVMPositionBuilderAtEnd(builder, startBB);
      string func_name = node->stmt.call.name;

      if (func_name == "print") {
        LLVMValueRef arg = genIRExpr(node->stmt.call.param);
        LLVMValueRef args[] = {arg};
        LLVMTypeRef printParamTypes[] = {LLVMInt32Type()};
        LLVMTypeRef printFuncType = LLVMFunctionType(LLVMVoidType(), printParamTypes, 1, 0);
        LLVMBuildCall2(builder, printFuncType, printFunc, args, 1, "");
      } else if (func_name == "read") {
        LLVMTypeRef readParamTypes[] = {};
        LLVMTypeRef readFuncType = LLVMFunctionType(LLVMInt32Type(), readParamTypes, 0, 0);
        LLVMValueRef read_val = LLVMBuildCall2(builder, readFuncType, readFunc, NULL, 0, "call");
        // store into the variable passed to read if there is one
        if (node->stmt.call.param && node->stmt.call.param->type == ast_var) {
          LLVMValueRef alloc_inst = var_map[string(node->stmt.call.param->var.name)];
          LLVMBuildStore(builder, read_val, alloc_inst);
        }
      }
      return startBB;
    }

    case ast_while: {
      LLVMPositionBuilderAtEnd(builder, startBB);
      LLVMValueRef func = LLVMGetBasicBlockParent(startBB);

      // cond block checks the loop condition each iteration
      LLVMBasicBlockRef condBB = LLVMAppendBasicBlock(func, "cond");
      LLVMBasicBlockRef trueBB = LLVMAppendBasicBlock(func, "loop_body");

      LLVMBuildBr(builder, condBB);

      LLVMPositionBuilderAtEnd(builder, condBB);
      LLVMValueRef cond_val = genIRExpr(node->stmt.whilen.cond);

      // generate body first so its blocks appear before loop_end in the IR
      LLVMBasicBlockRef trueExitBB = genIRStmt(node->stmt.whilen.body, trueBB);

      LLVMBasicBlockRef falseBB = LLVMAppendBasicBlock(func, "loop_end");
      LLVMBuildCondBr(builder, cond_val, trueBB, falseBB);

      LLVMPositionBuilderAtEnd(builder, trueExitBB);
      LLVMBuildBr(builder, condBB);  // loop back

      return falseBB;
    }

    case ast_if: {
      LLVMPositionBuilderAtEnd(builder, startBB);
      LLVMValueRef func = LLVMGetBasicBlockParent(startBB);

      LLVMValueRef cond_val = genIRExpr(node->stmt.ifn.cond);

      LLVMBasicBlockRef trueBB  = LLVMAppendBasicBlock(func, "if_true");
      LLVMBasicBlockRef falseBB = LLVMAppendBasicBlock(func, "if_false");

      LLVMBuildCondBr(builder, cond_val, trueBB, falseBB);

      if (!node->stmt.ifn.else_body) {
        // no else, just fall through to falseBB
        LLVMBasicBlockRef ifExitBB = genIRStmt(node->stmt.ifn.if_body, trueBB);
        LLVMPositionBuilderAtEnd(builder, ifExitBB);
        LLVMBuildBr(builder, falseBB);
        return falseBB;
      } else {
        LLVMBasicBlockRef ifExitBB   = genIRStmt(node->stmt.ifn.if_body,   trueBB);
        LLVMBasicBlockRef elseExitBB = genIRStmt(node->stmt.ifn.else_body, falseBB);

        // both branches merge into endBB
        LLVMBasicBlockRef endBB = LLVMAppendBasicBlock(func, "if_end");

        LLVMPositionBuilderAtEnd(builder, ifExitBB);
        LLVMBuildBr(builder, endBB);

        LLVMPositionBuilderAtEnd(builder, elseExitBB);
        LLVMBuildBr(builder, endBB);

        return endBB;
      }
    }

    case ast_ret: {
      LLVMPositionBuilderAtEnd(builder, startBB);

      // store return value then jump to the return block
      LLVMValueRef ret_val = genIRExpr(node->stmt.ret.expr);
      LLVMBuildStore(builder, ret_val, ret_ref);
      LLVMBuildBr(builder, retBB);

      // need a new block to keep things valid even though nothing goes here
      LLVMValueRef func = LLVMGetBasicBlockParent(startBB);
      LLVMBasicBlockRef endBB = LLVMAppendBasicBlock(func, "after_ret");
      return endBB;
    }

    case ast_block: {
      LLVMBasicBlockRef prevBB = startBB;
      if (node->stmt.block.stmt_list) {
        for (auto stmt : *node->stmt.block.stmt_list)
          prevBB = genIRStmt(stmt, prevBB);
      }
      return prevBB;
    }

    case ast_decl:
      // already handled in the alloca pass, nothing to do here
      return startBB;
  }
  return startBB;
}

LLVMModuleRef buildIR(astNode* root) {
  module = LLVMModuleCreateWithName("my_module");
  LLVMSetTarget(module, "x86_64-pc-linux-gnu");

  // set up extern declarations for print and read
  LLVMTypeRef printParamTypes[] = {LLVMInt32Type()};
  LLVMTypeRef printFuncType = LLVMFunctionType(LLVMVoidType(), printParamTypes, 1, 0);
  printFunc = LLVMAddFunction(module, "print", printFuncType);

  LLVMTypeRef readParamTypes[] = {};
  LLVMTypeRef readFuncType = LLVMFunctionType(LLVMInt32Type(), readParamTypes, 0, 0);
  readFunc = LLVMAddFunction(module, "read", readFuncType);

  // reset everything before we start
  rename_counter = 0;
  all_unique_vars.clear();
  scopes_ir.clear();
  var_map.clear();

  // part 1: rename all variables to unique names
  rename_ast(root);

  astNode* funcNode = root->prog.func;
  if (!funcNode) return module;

  // create the function
  int paramCount = (funcNode->func.param) ? 1 : 0;
  LLVMTypeRef paramTypes[] = {LLVMInt32Type()};
  LLVMTypeRef myFuncType = LLVMFunctionType(LLVMInt32Type(), paramTypes, paramCount, 0);
  LLVMValueRef myFunc = LLVMAddFunction(module, funcNode->func.name, myFuncType);

  builder = LLVMCreateBuilder();

  // entry block: put all allocas here
  LLVMBasicBlockRef entryBB = LLVMAppendBasicBlock(myFunc, "entry");
  LLVMPositionBuilderAtEnd(builder, entryBB);

  for (string name : all_unique_vars) {
    LLVMValueRef alloc = LLVMBuildAlloca(builder, LLVMInt32Type(), name.c_str());
    var_map[name] = alloc;
  }

  // alloca for the return value
  ret_ref = LLVMBuildAlloca(builder, LLVMInt32Type(), "ret_val");

  // store the param into its alloca right away
  if (funcNode->func.param) {
    LLVMValueRef p = LLVMGetParam(myFunc, 0);
    string param_name = funcNode->func.param->stmt.decl.name;
    LLVMBuildStore(builder, p, var_map[param_name]);
  }

  // return block: load ret_val and return it
  retBB = LLVMAppendBasicBlock(myFunc, "return");
  LLVMPositionBuilderAtEnd(builder, retBB);
  LLVMValueRef load_ret = LLVMBuildLoad2(builder, LLVMInt32Type(), ret_ref, "load_ret");
  LLVMBuildRet(builder, load_ret);

  // part 2: generate IR for the function body
  LLVMBasicBlockRef exitBB = genIRStmt(funcNode->func.body, entryBB);

  // if we fell off the end with no return, just branch to retBB
  if (!LLVMGetBasicBlockTerminator(exitBB)) {
    LLVMPositionBuilderAtEnd(builder, exitBB);
    LLVMBuildBr(builder, retBB);
  }

  // BFS from entry to find all reachable blocks
  set<LLVMBasicBlockRef> reachable;
  queue<LLVMBasicBlockRef> q;
  q.push(entryBB);
  reachable.insert(entryBB);

  while (!q.empty()) {
    LLVMBasicBlockRef curr = q.front();
    q.pop();

    LLVMValueRef term_inst = LLVMGetBasicBlockTerminator(curr);
    if (!term_inst) continue;

    unsigned succs = LLVMGetNumSuccessors(term_inst);
    for (unsigned i = 0; i < succs; i++) {
      LLVMBasicBlockRef s = LLVMGetSuccessor(term_inst, i);
      if (!reachable.count(s)) {
        reachable.insert(s);
        q.push(s);
      }
    }
  }

  // delete any blocks that are unreachable
  vector<LLVMBasicBlockRef> to_delete;
  LLVMBasicBlockRef blk = LLVMGetFirstBasicBlock(myFunc);
  while (blk) {
    if (!reachable.count(blk))
      to_delete.push_back(blk);
    blk = LLVMGetNextBasicBlock(blk);
  }
  for (auto b : to_delete)
    LLVMDeleteBasicBlock(b);

  var_map.clear();
  LLVMDisposeBuilder(builder);

  return module;
}
