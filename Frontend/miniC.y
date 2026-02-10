%{
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "ast.h"
#include "semantic.h"

using namespace std;

int yylex();
void yyerror(const char *s);

astNode* root;
%}

%union{
	int ival;
	char* sname;
	astNode* nval;
	vector<astNode*>* vec;
}

%token <ival> NUM
%token <sname> NAME
%token INT VOID IF ELSE WHILE RETURN PRINT READ EXTERN
%token PLUS MINUS MUL DIV LT GT LE GE EQ NE ASSIGN
%token SEMI COMMA LPAREN RPAREN LBRACE RBRACE

%type <nval> program extern_decl decl_list decl var_decl func_decl
%type <vec>  stmt_list
%type <nval> block_stmt stmt
%type <nval> expr term factor if_stmt while_stmt return_stmt call_stmt print_stmt read_stmt assign_stmt

%left PLUS MINUS
%left MUL DIV
%left LT GT LE GE EQ NE
%right ASSIGN

%start program

%%
program : extern_decl extern_decl decl_list { root = createProg($1, $2, $3); }

extern_decl : EXTERN VOID PRINT LPAREN INT RPAREN SEMI { $$ = createExtern("print"); }
	| EXTERN INT READ LPAREN RPAREN SEMI { $$ = createExtern("read"); }
decl_list : decl decl_list { $$ = $1; }
	| /* empty */ { $$ = NULL; }
decl : var_decl { $$ = $1; }
	| func_decl { $$ = $1; }
var_decl : INT NAME SEMI { $$ = createDecl($2); }
	| VOID NAME SEMI { $$ = createDecl($2); }
func_decl : INT NAME LPAREN RPAREN block_stmt { $$ = createFunc($2, NULL, $5); }
	| INT NAME LPAREN INT NAME RPAREN block_stmt { $$ = createFunc($2, createDecl($5), $7); }
	| VOID NAME LPAREN RPAREN block_stmt { $$ = createFunc($2, NULL, $5); }
	| VOID NAME LPAREN INT NAME RPAREN block_stmt { $$ = createFunc($2, createDecl($5), $7); }

block_stmt : LBRACE stmt_list RBRACE { $$ = createBlock($2); }
stmt_list : stmt_list stmt 
	{ 
		$$ = $1;
		$$->push_back($2);
	}
	| /* empty */ 
	{ 
		$$ = new vector<astNode*>(); 
	}
stmt : if_stmt { $$ = $1; }
	| while_stmt { $$ = $1; }
	| return_stmt { $$ = $1; }
	| print_stmt { $$ = $1; }
	| read_stmt { $$ = $1; }
	| call_stmt { $$ = $1; }
	| assign_stmt { $$ = $1; }
	| var_decl { $$ = $1; }
	| block_stmt { $$ = $1; }

if_stmt : IF LPAREN expr RPAREN stmt ELSE stmt { $$ = createIf($3, $5, $7); }
	| IF LPAREN expr RPAREN stmt { $$ = createIf($3, $5); }
while_stmt : WHILE LPAREN expr RPAREN stmt { $$ = createWhile($3, $5); }
return_stmt : RETURN expr SEMI { $$ = createRet($2); }
print_stmt : PRINT LPAREN expr RPAREN SEMI { $$ = createCall("print", $3); }
read_stmt : READ LPAREN NAME RPAREN SEMI { /* read logic */ }
call_stmt : NAME LPAREN expr RPAREN SEMI { $$ = createCall($1, $3); }
assign_stmt : NAME ASSIGN expr SEMI { $$ = createAsgn(createVar($1), $3); }

expr : term { $$ = $1; }
	| expr PLUS expr { $$ = createBExpr($1, $3, add); }
	| expr MINUS expr { $$ = createBExpr($1, $3, sub); }
	| expr MUL expr { $$ = createBExpr($1, $3, mul); }
	| expr DIV expr { $$ = createBExpr($1, $3, divide); }
	| expr LT expr { $$ = createRExpr($1, $3, lt); }
	| expr GT expr { $$ = createRExpr($1, $3, gt); }
	| expr LE expr { $$ = createRExpr($1, $3, le); }
	| expr GE expr { $$ = createRExpr($1, $3, ge); }
	| expr EQ expr { $$ = createRExpr($1, $3, eq); }
	| expr NE expr { $$ = createRExpr($1, $3, neq); }
term : factor { $$ = $1; }
factor : NAME { $$ = createVar($1); }
	| NUM { $$ = createCnst($1); }
	| LPAREN expr RPAREN { $$ = $2; }
%%


void yyerror(const char *s) {
	fprintf(stderr, "Error: %s\n", s);
}

int main() {
	yyparse();
	if(root){
		printNode(root);
		if (check(root) == 0) {
			printf("Semantic Analysis Passed.\n");
		}
	}
	return 0;
}
