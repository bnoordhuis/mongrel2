CC	=gcc
CFLAGS	=-O2
AR	=ar

all:	sqlite3.a

sqlite3.a:	sqlite3.o
	$(AR) cr $@ $<

sqlite3.o:	sqlite3.c sqlite3.h
