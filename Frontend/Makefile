CC = g++
CFLAGS = -g -Wall


SRCS = miniC.tab.c lex.yy.c ast.c semantic.cpp
OBJS = $(SRCS:.c=.o)

miniC: miniC.tab.c lex.yy.c
	$(CC) $(CFLAGS) -o miniC miniC.tab.c lex.yy.c ast.c semantic.cpp -ll
miniC.tab.c: miniC.y
	bison -d miniC.y
lex.yy.c: miniC.l
	flex miniC.l
clean:
	rm -f miniC miniC.tab.c miniC.tab.h lex.yy.c *.o
