# Instruction set

32-bit instructions, little-endian.

```
| op:8 | A:8 | B:8 | C:8 |    ABC
| op:8 | A:8 |    Bx:16  |    ABx (unsigned) / AsBx (signed)
```

`R[x]` is a register of the current frame. Jumps are relative to the
instruction after the jump.

| op | form | effect |
|----|------|--------|
| MOV    | A B   | R[A] = R[B] |
| LDK    | A Bx  | R[A] = K[Bx] |
| LDI    | A sBx | R[A] = int(sBx) |
| LDNIL  | A B   | R[A..A+B] = nil |
| LDBOOL | A B   | R[A] = (B != 0) |
| ADD SUB MUL DIV MOD | A B C | R[A] = R[B] op R[C]; `ADD` of two strings concatenates them |
| NEG    | A B   | R[A] = -R[B] |
| AND OR XOR SHL SHR | A B C | integer bit ops, shift count masked to 6 bits |
| NOT    | A B   | R[A] = ~R[B] |
| EQ     | A B C | if (R[B] == R[C]) != A then skip next instruction |
| LT LE  | A B C | same, with < and <= |
| JMP    | sBx   | pc += sBx |
| JT JF  | A sBx | jump if R[A] is truthy / falsy |
| CALL   | A Bx  | call func Bx with args in R[A..]; result lands in R[A] |
| RET    | A     | return R[A] |
| HOST   | A B C | host function C gets R[A..A+B-1]; result goes to R[A] |
| NEWARR | A B   | R[A] = new array, capacity hint B |
| NEWMAP | A B   | R[A] = new map, size hint B |
| GET    | A B C | R[A] = R[B][R[C]] |
| SET    | A B C | R[A][R[B]] = R[C] |
| LEN    | A B   | R[A] = length of string, array or map R[B] |
| CLOSURE | A Bx | R[A] = closure of func Bx, capturing its upvalues |
| GETUP  | A B   | R[A] = upvalue B of the running closure |
| SETUP  | A B   | upvalue B of the running closure = R[A] |
| CLOSE  | A     | close the upvalues of registers R[A..] |
| CALLC  | A B   | call closure R[A] with args R[A+1..A+B]; result replaces R[A] |
| TRY    | A sBx | install a handler at pc + sBx; it receives the exception in R[A] |
| ENDTRY |       | remove the innermost handler of this frame |
| THROW  | A     | throw R[A] |

## Semantics

- Numbers: 48-bit ints and float64. `int op int` stays int and is
  promoted to float when the result leaves the 48-bit range. Mixed
  operands are computed as float. `int / int` and `int % int` are C-style
  (truncating); dividing an int by zero is an error.
- Bit ops need ints and wrap to 48 bits. `SHR` is arithmetic.
- `EQ` compares numbers by value (`1 == 1.0`); NaN is never equal.
  `LT`/`LE` need numbers.
- Truthiness: only `nil` and `false` are falsy.
- Calls: the callee's frame starts at the caller's R[A], so arguments
  are passed without copying. Registers past the parameters start as nil.

## Heap values

Strings, arrays and maps live on a garbage-collected heap (precise,
non-incremental mark and sweep). Pointers are stored in the 48-bit payload
of a NaN-boxed value, so the allocator must return addresses below 2^48.

- Strings are immutable byte strings and compare by content. `GET` on a
  string yields the byte at that index as an int. String constants come
  from the constant pool; hosts create more with `lvm_new_string`.
- Arrays: `GET` needs an int index in `[0, len)`. `SET` accepts
  `[0, len]`; `len` itself appends. Anything else is `LVM_ERR_BOUNDS`.
- Maps: keys are ints, floats, bools, strings or heap values. Strings
  compare by content, other heap values by identity. A float with an
  integral value is the same key as the int (`1` and `1.0`). `nil` and NaN
  are not valid keys. `GET` of a missing key gives nil; `SET` with a nil
  value removes the key.
- A collection runs before an allocation when the heap has outgrown its
  threshold. Roots are all registers of all live frames.

## Closures

A function lists the upvalues it needs: each entry says whether to capture
a register of the frame that creates the closure (`instack = 1`, `index` is
the register) or to copy an upvalue of the creating closure (`instack = 0`).
`CLOSURE` builds the closure and fills its upvalues from that list.

An upvalue is open while its variable is still in a register: closures that
capture the same register share one upvalue, so writes through any of them
are visible to all. When the frame returns, its open upvalues are closed:
the value moves into the upvalue and lives on with the closure. `CLOSE A`
does the same early for registers `A` and up, which gives a loop body a
fresh variable each iteration.

`CALLC` needs the argument count to equal the function's `nparams`
(`LVM_ERR_ARGS` otherwise). The callee's registers start at `R[A+1]`.
Functions with upvalues are only called through `CALLC`; `CALL` is for
plain functions. A closure refers to its function in the module, so the
module must stay loaded while closures exist.

## Exceptions

`TRY A sBx` installs a handler: if something is thrown before the matching
`ENDTRY`, execution continues at `pc + sBx` in the frame that executed
`TRY`, with the thrown value in `R[A]`. Frames above it are discarded and
their open upvalues are closed. Handlers nest; the innermost one wins.

```
    TRY R0, .catch
    ...                 ; code that may throw
    ENDTRY
    JMP .after
.catch:
    ...                 ; R0 holds the exception
.after:
```

- `THROW A` throws any value. With no handler left, `lvm_run` stops with
  `LVM_ERR_UNCAUGHT` and hands the value back through `result`.
- A function that returns drops the handlers it installed, so `ENDTRY`
  may be left out on the way out.
- `ENDTRY` can only remove a handler installed by the running frame;
  anything else is `LVM_ERR_VERIFY`.
- These runtime errors can be caught: type errors, division by zero,
  out-of-bounds access, a wrong argument count for `CALLC`, and failing
  host functions. The handler receives the message as a string, such as
  `"division by zero"`. Out of memory, stack overflow and handler-stack
  overflow cannot be caught and end the run.
- A function may end with `THROW`.

## Verifier rules

- every register operand is inside the frame
- constant and function indices exist; CALL arguments fit the frame
- jump targets are inside the function
- EQ/LT/LE are followed by a JMP
- a function ends with RET or JMP (no fall-through)
- upvalue indices are below the function's `nupvals`, and each capture of
  a `CLOSURE` target names a register of the frame or an upvalue of the
  creating function
- `CALL` targets have no upvalues; the entry function has none
- `CALLC` argument registers lie inside the frame
- `TRY` targets lie inside the function; a function ends with `RET`, `JMP` or `THROW`
- unknown opcodes are rejected
