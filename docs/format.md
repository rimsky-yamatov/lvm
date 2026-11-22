# Bytecode file format (version 1)

All integers are little-endian.

```
magic      "LVMB"
version    u16      = 1
flags      u16      = 0 (reserved)
nconsts    u32      <= 65536
nfuncs     u32      1..65536
entry      u32      index of the entry function

consts[nconsts]
  tag u8: 0 nil | 1 bool (u8 0/1) | 2 int (i64, must fit 48 bits) | 3 float (f64 bits)

funcs[nfuncs]
  nparams u8
  nregs   u8       >= 1, >= nparams
  ncode   u32      >= 1
  code    u32[ncode]
```

No bytes may follow the last function. The loader rejects anything
malformed; the verifier then checks every instruction (see `docs/isa.md`).
