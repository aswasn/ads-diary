SOURCES = $(PROG).cpp mongoose.c
CFLAGS = -g -std=c++11 -w -I -Wno-unused-function $(CFLAGS_EXTRA) $(MODULE_CFLAGS)

PROG = server
MODULE_CFLAGS=-DMG_ENABLE_THREADS -DMG_ENABLE_HTTP_WEBSOCKET=0
MODULE_CFLAGS+=-I/usr/local/include/hiredis -lhiredis
MODULE_CFLAGS+=-I../zeromq-install/include -L../zeromq-install/lib -lzmq

all: $(PROG)

CC = g++
CFLAGS += -pthread

$(PROG): $(SOURCES)
	$(CC) $(SOURCES) -o $@ $(CFLAGS)

$(PROG).exe: $(SOURCES)
	cl $(SOURCES) /I../.. /MD /Fe$@

clean:
	rm -rf *.gc* *.dSYM *.exe *.obj *.o a.out $(PROG)

