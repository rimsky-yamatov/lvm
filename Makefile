CC     ?= cc
CFLAGS ?= -O2
override CFLAGS += -std=c99 -Wall -Wextra -Iinclude

LIB_SRC = src/util.c src/loader.c src/verify.c src/vm.c \
          src/object.c src/gc.c src/container.c
LIB_OBJ = $(LIB_SRC:.c=.o)

all: liblvm.a lvmi

liblvm.a: $(LIB_OBJ)
	$(AR) rcs $@ $(LIB_OBJ)

src/%.o: src/%.c include/lvm.h include/lvm_value.h include/lvm_opcode.h src/internal.h
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

lvmi: tools/lvmi.c liblvm.a
	$(CC) $(CFLAGS) -o $@ tools/lvmi.c liblvm.a -lm

tests/test_vm: tests/test_vm.c liblvm.a
	$(CC) $(CFLAGS) -o $@ tests/test_vm.c liblvm.a -lm

test: tests/test_vm
	./tests/test_vm

# strict C99, switch dispatch only
portable: clean
	$(MAKE) CFLAGS="-O2 -pedantic -DLVM_NO_COMPUTED_GOTO" all test

clean:
	rm -f src/*.o liblvm.a lvmi tests/test_vm

.PHONY: all test portable clean
