#include "semantic.h"
#include "ast.h"
#include <iostream>
#include <string>
#include <vector>

using namespace std;

// Ideally we'd use a class, but for simplicity:
// A stack of symbol tables (scopes)
// The last element is the current inner-most scope
vector<SymbolTable> scopes;

// Helper: Add a variable to the current scope
// Returns true if successful, false if already declared in THIS scope
bool declare(string name) {
  if (scopes.empty())
    return false; // Should not happen

  // Check if valid in current scope (top of stack)
  SymbolTable &current = scopes.back();
  for (size_t i = 0; i < current.size(); i++) {
    if (current[i] == name)
      return false; // Already declared!
  }

  current.push_back(name);
  return true;
}

// Helper: Check if a variable exists in ANY scope (from inner to outer)
bool isDeclared(string name) {
  // Iterate backwards through scopes
  for (int i = scopes.size() - 1; i >= 0; i--) {
    SymbolTable &scope = scopes[i];
    for (size_t j = 0; j < scope.size(); j++) {
      if (scope[j] == name)
        return true;
    }
  }
  return false;
}

// Main check function
int check(astNode *node) {
  if (!node)
    return 0;

  switch (node->type) {
  case ast_prog:
    scopes.push_back(SymbolTable());

    // Check children
    if (node->prog.func)
      if (check(node->prog.func) != 0)
        return 1;

    scopes.pop_back();
    break;

  case ast_func:
    // Function might have parameters (declarations)
    // Create a scope for parameters
    scopes.push_back(SymbolTable());

    if (node->func.param)
      if (check(node->func.param) != 0)
        return 1;

    // Function body is a block
    // It will push its OWN scope in ast_block case, which is fine
    if (node->func.body)
      if (check(node->func.body) != 0)
        return 1;

    scopes.pop_back();
    break;

  case ast_stmt:
    switch (node->stmt.type) {
    case ast_block:
      // BLOCK: Enter new scope
      scopes.push_back(SymbolTable());

      // Check statements
      if (node->stmt.block.stmt_list) {
        vector<astNode *> list = *node->stmt.block.stmt_list;
        for (size_t i = 0; i < list.size(); i++) {
          if (check(list[i]) != 0)
            return 1;
        }
      }

      // EXIT scope
      scopes.pop_back();
      break;

    case ast_decl:
      // DECLARATION: int x;
      if (!declare(node->stmt.decl.name)) {
        fprintf(stderr,
                "Error: Variable %s invalid declaration (already declared)\n",
                node->stmt.decl.name);
        return 1;
      }
      break;

    case ast_call:
      if (node->stmt.call.param)
        if (check(node->stmt.call.param) != 0)
          return 1;
      break;

    case ast_ret:
      if (node->stmt.ret.expr)
        if (check(node->stmt.ret.expr) != 0)
          return 1;
      break;

    case ast_while:
      if (check(node->stmt.whilen.cond) != 0)
        return 1;
      if (check(node->stmt.whilen.body) != 0)
        return 1;
      break;

    case ast_if:
      if (check(node->stmt.ifn.cond) != 0)
        return 1;
      if (check(node->stmt.ifn.if_body) != 0)
        return 1;
      if (node->stmt.ifn.else_body)
        if (check(node->stmt.ifn.else_body) != 0)
          return 1;
      break;

    case ast_asgn:
      // Check LHS variable (should be declared)
      // ast_asgn in ast.h likely has 'lhs' as astNode* (var) and 'rhs' as
      // astNode* (expr)
      if (check(node->stmt.asgn.lhs) != 0)
        return 1;
      if (check(node->stmt.asgn.rhs) != 0)
        return 1;
      break;

    default:
      break;
    }
    break;

  case ast_var:
    // VARIABLE USE: x = 1;
    if (!isDeclared(node->var.name)) {
      fprintf(stderr, "Error: Variable %s not declared\n", node->var.name);
      return 1;
    }
    break;

  case ast_bexpr:
    if (check(node->bexpr.lhs) != 0)
      return 1;
    if (check(node->bexpr.rhs) != 0)
      return 1;
    break;

  case ast_rexpr:
    if (check(node->rexpr.lhs) != 0)
      return 1;
    if (check(node->rexpr.rhs) != 0)
      return 1;
    break;

  // Constants and Externs don't need checks usually
  case ast_cnst:
  case ast_extern:
    break;
  }
  return 0;
}
