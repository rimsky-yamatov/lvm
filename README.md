# lvm

Small register-based bytecode VM.

- pure C99, no JIT, no OS dependencies (the host provides I/O)
- 32-bit fixed-width instructions, 31 opcodes
- 64-bit NaN-boxed values (nil, bool, int48, float64, string, array, map)
- precise mark-and-sweep GC
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

or with CMake (also used by CI):

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release
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
calls, host calls, strings, arrays, maps, garbage collection.

Not yet: closures, exceptions, coroutines, tail calls, debug info,
assembler.

## CI and releases

- `ci.yml` builds and tests on Linux, macOS and Windows for every push to
  `main` and every pull request. It also runs a strict-C99 build with switch
  dispatch, and the tests under ASan and UBSan.
- `release.yml` runs when a `v*` tag is pushed. It builds and tests on the
  three platforms, packages `lvmi`, the static library, headers and docs,
  and publishes a GitHub release with the archives, `SHA256SUMS` and notes
  listing the commits since the previous tag. Tags containing `-` (for
  example `v0.2.0-rc1`) are marked as pre-releases.

```
git tag v0.1.0
git push origin v0.1.0
```
