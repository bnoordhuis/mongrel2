CC	=gcc
CFLAGS	=-O2
LDFLAGS	=-ldl -pthread
AR	=ar

all:	sqlite3.a sqlite3

clean:
	rm -rf *.o sqlite3.a sqlite3

sqlite3:	shell.o sqlite3.a
	$(CC) $(LDFLAGS) -o $@ $< sqlite3.a

shell.o:	shell.c sqlite3.h

sqlite3.a:	sqlite3.o
	$(AR) cr $@ $<

sqlite3.o:	sqlite3.c sqlite3.h
