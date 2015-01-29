CXX 	= gcc -g

THREADSCAN = libthreadscan.so
MAIN	= main
TARGETS	= $(THREADSCAN) $(MAIN)

THREADSCAN_SRC = env.c wrappers.c alloc.c util.c thread.c	\
	proc.c threadscan.c
THREADSCAN_OBJ = $(THREADSCAN_SRC:.c=.o)

# The -fno-zero-initialized-in-bss flag appears to be busted.
#CFLAGS = -fno-zero-initialized-in-bss
CFLAGS := -O2
ifndef DEBUG
	CFLAGS := $(CFLAGS) -DNDEBUG
endif
LDFLAGS = -ldl -pthread

all:	$(TARGETS)

debug:
	DEBUG=1 make all

test:	$(TARGETS)
	LD_BIND_NOW=1 LD_PRELOAD="$$PWD/$(THREADSCAN)" ./$(MAIN)

$(THREADSCAN): $(THREADSCAN_OBJ)
	$(CXX) $(CFLAGS) -shared -Wl,-soname,$@ -o $@ $^ $(LDFLAGS)

$(MAIN): main.c $(THREADSCAN)
	gcc -o $@ $< -L. -lthreadscan -pthread

clean:
	rm -f *.o $(TARGETS) core

%.o: %.c
	$(CXX) $(CFLAGS) -o $@ -Wall -fPIC -c -ldl $<

