# lvm

Small register-based bytecode VM.

- pure C99, no JIT, no OS dependencies (the host provides I/O)
- 32-bit fixed-width instructions, 26 opcodes
- 64-bit NaN-boxed values (nil, bool, int48, float64)
- bytecode verifier: bad registers, jumps, constants and calls are
  rejected at load time, so untrusted bytecode can be run
- threaded dispatch via computed goto, plain `switch` fallback
- allocator callback, no hidden `malloc`

## Build

```
make          # liblvm.a and the lvmi runner
make test
make portable # strict C99 + switch dispatch
```

`-lm` is needed for `fmod`.

## Use

```c
#include "lvm.h"

lvm_module m;
lvm_load(NULL, bytes, len, &m);          /* parse + verify */

lvm_vm *vm = lvm_new(NULL, 65536, 4096); /* stack slots, max call depth */
lvm_set_host(vm, 0, my_host_fn, NULL);   /* HOST instruction ids */

lvm_value result;
lvm_run(vm, &m, NULL, 0, &result);

lvm_free(vm);
lvm_module_free(&m);
```

`lvmi file.lvmb` runs a bytecode file; host function 0 prints its arguments.

## Docs

- `docs/format.md` - file format
- `docs/isa.md` - instructions and verifier rules

## Status

Implemented: loader, verifier, arithmetic, bit ops, comparison, jumps,
calls, host calls.

Not yet: closures, arrays/maps, GC, exceptions, coroutines, tail calls,
debug info, assembler.
