%{
#include "ast.h"
#include "miniC.tab.h"
#include <string.h>
#include <stdio.h>

void yyerror(const char *s);
%}

%%

"+"		{ return PLUS; }
"-"		{ return MINUS; }
"*"		{ return MUL; }
"/"		{ return DIV; }
"<"		{ return LT; }
">"		{ return GT; }
"<="		{ return LE; }
">="		{ return GE; }
"=="		{ return EQ; }
"!="		{ return NE; }
"="		{ return ASSIGN; }
"int"		{ return INT; }
"void"		{ return VOID; }
"extern"	{ return EXTERN; }
"if"		{ return IF; }
"else"		{ return ELSE; }
"while"		{ return WHILE; }
"return"	{ return RETURN; }
"print"		{ return PRINT; }
"read"		{ return READ; }
";"		{ return SEMI; }
","		{ return COMMA; }
"("		{ return LPAREN; }
")"		{ return RPAREN; }
"{"		{ return LBRACE; }
"}"		{ return RBRACE; }

[a-zA-Z_][a-zA-Z0-9_]*	{
									// printf("%s", yytext);
									yylval.sname = strdup(yytext);
									return NAME;
								}
[0-9]+	{
             //printf("%s",yytext);
						 yylval.ival = atoi(yytext);
						 return NUM;
						}
[ \t\n]	{ /* ignore */ }
"//".*	{ /* ignore comment */ }
.		{ return(yytext[0]); }
%%
int yywrap(){
	return 1;
}