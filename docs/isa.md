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
| ADD SUB MUL DIV MOD | A B C | R[A] = R[B] op R[C] |
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

## Verifier rules

- every register operand is inside the frame
- constant and function indices exist; CALL arguments fit the frame
- jump targets are inside the function
- EQ/LT/LE are followed by a JMP
- a function ends with RET or JMP (no fall-through)
- unknown opcodes are rejected
