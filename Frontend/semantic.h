#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "ast.h"
#include <string>
#include <vector>

using namespace std;

// A simple symbol table for one scope
// Just a list of variable names declared in this block
typedef vector<string> SymbolTable;

// Function to check the AST for semantic errors
// Returns 0 if no errors, 1 if errors found
int check(astNode *node);

#endif
