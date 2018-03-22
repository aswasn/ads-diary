SOURCES = $(PROG).c ../mongoose.c
CFLAGS = -g -W -Wall -Werror -I../ -Wno-unused-function $(CFLAGS_EXTRA) $(MODULE_CFLAGS)

PROG = restful_server
MODULE_CFLAGS=-DMG_ENABLE_THREADS -DMG_ENABLE_HTTP_WEBSOCKET=0
MODULE_CFLAGS+=-I/usr/local/include/hiredis -lhiredis

all: $(PROG)

ifeq ($(OS), Windows_NT)
# TODO(alashkin): enable SSL in Windows
CFLAGS += -lws2_32
CC = gcc
else
CFLAGS += -pthread
endif

$(PROG): $(SOURCES)
	$(CC) $(SOURCES) -o $@ $(CFLAGS)

$(PROG).exe: $(SOURCES)
	cl $(SOURCES) /I../.. /MD /Fe$@

clean:
	rm -rf *.gc* *.dSYM *.exe *.obj *.o a.out $(PROG)

