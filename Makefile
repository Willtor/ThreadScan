CXX 	= gcc -g

INSTALL_DIR = /usr/local

THREADSCAN = libthreadscan.so
TARGETS	= $(THREADSCAN)

THREADSCAN_SRC = queue.c env.c wrappers.c alloc.c util.c thread.c	\
	proc.c child.c threadscan.c
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

$(THREADSCAN): $(THREADSCAN_OBJ)
	$(CXX) $(CFLAGS) -shared -Wl,-soname,$@ -o $@ $^ $(LDFLAGS)

$(INSTALL_DIR)/lib/$(THREADSCAN): $(THREADSCAN)
	cp $< $@

$(INSTALL_DIR)/include/threadscan.h: include/threadscan.h
	cp $< $@

install: $(INSTALL_DIR)/lib/$(THREADSCAN) $(INSTALL_DIR)/include/threadscan.h
	ldconfig

clean:
	rm -f *.o $(TARGETS) core

%.o: %.c
	$(CXX) $(CFLAGS) -o $@ -Wall -fPIC -c -ldl $<

