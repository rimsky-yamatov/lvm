# Assembly language

```
main:
    LDI R0, 20
    LDI R1, 22
    ADD R2, R0, R1
    RET R2
```

```
lvma prog.lvma -o prog.lvmb    # assemble
lvmi prog.lvmb                 # run
lvmi prog.lvma                 # or assemble and run in one go
```

From C: `lvm_assemble` turns source text into a bytecode image, and the
result has already been through the verifier.

## Lines

One statement per line. `;`, `#` and `//` start a comment (not inside
strings). Mnemonics and register names are case-insensitive. Operands are
separated by commas.

| form | meaning |
|------|---------|
| `name:` | starts a function |
| `.name:` | local label, visible inside the current function only |
| `.directive ...` | function settings, see below |
| `OP operands` | an instruction |

A label may share a line with an instruction: `.end: RET R0`.

The entry function is `main` if there is one, otherwise the first function.

## Registers and values

Registers are `R0` to `R254`. The number of registers a function needs is
worked out from the ones it uses; `.regs N` can ask for more.

| operand | examples |
|---------|----------|
| integer | `42`, `-6`, `0x1F` |
| float | `1.5`, `-2e3`, `inf`, `-inf`, `nan` |
| string | `"hi\n"` with `\n \t \r \0 \\ \" \xHH` |
| `nil`, `true`, `false` | |

`LDI` takes a 16-bit integer. `LDK` takes any constant: an integer
(48 bits), float, string, `nil` or a boolean. Equal constants share one slot
in the constant pool.

## Directives

| directive | effect |
|-----------|--------|
| `.params N` | the function takes N arguments, in `R0..R(N-1)` |
| `.regs N` | make sure the frame has at least N registers |
| `.upval stack Rn` | the function captures register `n` of the function that creates it |
| `.upval up n` | the function copies upvalue `n` of the closure that creates it |

`.upval` lines are numbered in the order they appear: the first is upvalue 0
(see `GETUP` and `SETUP`).

## Instructions

Operands use the names from `docs/isa.md`.

```
MOV Ra, Rb          LDK Ra, constant     LDI Ra, int16
LDNIL Ra            LDNIL Ra, Rb         LDBOOL Ra, true|false
ADD SUB MUL DIV MOD AND OR XOR SHL SHR   Ra, Rb, Rc
NEG NOT             Ra, Rb
JMP .label          JT Ra, .label        JF Ra, .label
CALL Ra, function   CALLC Ra, argc       RET Ra
HOST Ra, argc, id
NEWARR Ra [, hint]  NEWMAP Ra [, hint]
GET Ra, Rb, Rc      SET Ra, Rb, Rc       LEN Ra, Rb
CLOSURE Ra, function    GETUP Ra, n      SETUP Ra, n      CLOSE Ra
TRY Ra, .label      ENDTRY               THROW Ra
```

`LDNIL Ra, Rb` clears the registers `Ra` to `Rb`.

The raw comparisons are `EQ`, `LT` and `LE` with a leading 0 or 1 (`EQ 1, Rb, Rc`);
they must be followed by a `JMP`. The compare-and-jump forms write that
pair for you:

| form | jumps when |
|------|------------|
| `JEQ Ra, Rb, .l` | `Ra == Rb` |
| `JNE Ra, Rb, .l` | `Ra != Rb` |
| `JLT Ra, Rb, .l` | `Ra < Rb` |
| `JLE Ra, Rb, .l` | `Ra <= Rb` |
| `JGT Ra, Rb, .l` | `Ra > Rb` |
| `JGE Ra, Rb, .l` | `Ra >= Rb` |

## Host functions

`HOST Ra, argc, id` calls host function `id` with `Ra`..`Ra+argc-1`; the
result replaces `Ra`. `lvmi` installs the standard library (`include/lvm_std.h`
lists the numbers): 0 is `print`, 1 is `printf`, and so on. `lvc` programs use
the same functions through their names.

## Calls

`CALL Ra, f` runs function `f`; its arguments are `Ra`, `Ra+1`, ... and the
result replaces `Ra`. The callee's registers overlap the caller's from `Ra`
upward, so keep live values below `Ra`.

`CALLC Ra, n` runs the closure in `Ra` with the arguments `Ra+1..Ra+n`; the
result replaces `Ra`. Functions with `.upval` lines are created with
`CLOSURE` and called with `CALLC`.

## Disassembling

```
lvmdis prog.lvmb        # print assembly
lvmdis -a prog.lvmb     # also show each instruction's pc and word
```

`lvmdis` accepts bytecode or assembly source. From C, use
`lvm_disassemble` on a loaded module.

The output is assembly again. The entry function is called `main`, the others
`f1`, `f2`, ... after their index, and every jump target gets a label
`.L<pc>`. A comparison followed by its jump is shown as one `Jcc` line. The
text assembles back to the same bytecode as the original, as long as the
original came from `lvma`; names, comments and label names of the source are
not kept in the bytecode, so they do not come back.

See `examples/` for complete programs.
