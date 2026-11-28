# The lvc language

A small C-like language that compiles to lvm bytecode.

```c
#include <stdio.h>

int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int main() {
    int n;
    printf("n? ");
    scanf("%d", &n);
    printf("fib(%d) = %d\n", n, fib(n));
    return 0;
}
```

```
lvmi prog.lvc               # compile and run; main's result is the exit status
lvcc prog.lvc               # compile to prog.lvmb
lvcc -S prog.lvc            # compile to assembly (prog.lvma)
```

From C, `lvm_compile` turns source into assembly text, `lvm_assemble` turns
that into bytecode, and `lvm_std_new` gives the program its `printf`, `scanf`
and friends (see `include/lvm_std.h`).

## How it differs from C

- Values are typed at run time, not at compile time. Type names in
  declarations are annotations: they decide the starting value
  (`int` 0, `float` 0.0, `bool` false, `string` "", `var` nil) and nothing
  else. Use `(int)x`, `(float)x` or `(string)x` to convert.
- Types: `int` (also `long short char`), `float` (`double`), `bool`,
  `string`, `array`, `map`, `var` (`auto`), and `void` for functions.
  `const static unsigned signed` are accepted and ignored.
- There are no pointers, structs or `switch`. Lines starting with `#` are
  skipped, so `#include <stdio.h>` is harmless.
- `int` is 48 bits wide and becomes a float when a result no longer fits.
  `int / int` truncates like C; with a float operand it is real division.
- `if`, `while` and friends treat `false`, `nil` and `0` as false and
  everything else (including `""`) as true. `&&`, `||`, `!` and comparisons
  give `true` or `false`.
- Strings are immutable byte strings. `+` joins two strings, `==` compares
  content, `s[i]` is the byte at `i` as an int, and `'a'` is the int 97.
- Arrays and maps are references: `b = a` makes `b` another name for the
  same array.

## Variables

```c
int x = 5, y;            // y starts at 0
float f = 2.5;
string s = "hi";
int a[10];               // an array of ten 0s
bool seen[100];          // an array of 100 falses
int primes[] = {2, 3, 5};
var list = [1, "two", 3.0];
var m = {"a": 1, "b": 2};    // a map; {} is an empty map
```

Globals are declared outside functions. They are set up in order before
`main` runs and may use earlier globals and call functions.

Arrays are indexed from 0. Writing to index `len(a)` appends; anything else
out of range throws. Map keys are ints, floats, bools or strings; reading a
missing key gives `nil`; assigning `nil` removes the key. `m.name` means
`m["name"]`.

## Statements and expressions

`if` / `else`, `while`, `do` ... `while`, `for` (with a declaration in the
first part), `break`, `continue`, `return`, blocks `{ }`, and
`try` / `catch` / `throw` (below).

Operators, highest precedence first: postfix `[] . ++ --`; prefix
`- ! ~ ++ -- (type)`; `* / %`; `+ -`; `<< >>`; `< <= > >=`; `== !=`; `&`;
`^`; `|`; `&&`; `||`; `?:`; assignment `= += -= *= /= %= &= |= ^= <<= >>=`.

## Functions

```c
int add(int a, int b) { return a + b; }
void hello() { printf("hi\n"); }
```

Functions can be called before they are defined. `main` must exist and take
no parameters. A function that ends without `return` gives `nil`.

## Exceptions

```c
try {
    risky();
} catch (e) {
    printf("failed: %s\n", e);
}
throw "something went wrong";    // any value can be thrown
```

Runtime errors are thrown as a message string: `"division by zero"`,
`"type error"`, `"index out of bounds"`, `"wrong number of arguments"`. An
exception that nothing catches stops the program; `lvmi` prints it and exits
with status 4. `break` and `continue` may leave a `try` block.

## Built-in functions

| function | does |
|----------|------|
| `printf(fmt, ...)` | formatted output; returns the number of bytes written |
| `print(...)`, `puts(s)` | values separated by spaces, then a newline |
| `scanf(fmt, &a, &b, ...)` | formatted input; returns how many values were stored, or -1 at the end of input |
| `readline()` | the next line without its newline, or `nil` at the end of input |
| `getchar()`, `putchar(c)` | one byte in or out (`getchar` gives -1 at the end) |
| `len(x)`, `strlen(x)` | length of a string, array or map |
| `push(a, v)`, `pop(a)` | append to an array, remove and return the last element |
| `remove(m, key)`, `keys(m)` | delete a key, an array of all keys (in no particular order) |
| `array(n [, fill])`, `map()` | a new array of `n` copies of `fill` (default `nil`), a new map |
| `sort(a)` | sort an array of numbers or of strings in place |
| `substr(s, start [, n])`, `find(s, sub [, from])` | slice a string, position of `sub` or -1 |
| `split(s, sep)`, `strcmp(a, b)`, `chr(n)` | split into an array, compare (-1, 0, 1), one-byte string |
| `int(x)`, `float(x)`, `str(x)` | conversions; `atoi` and `atof` are the same as `int` and `float` |
| `sqrt sin cos tan exp log pow atan2` | math, results are floats |
| `floor(x)`, `ceil(x)` | rounded down or up, as an int when it fits |
| `abs(x)`, `min(a, b)`, `max(a, b)` | |
| `rand()`, `srand(n)`, `clock()` | a 31-bit random number, seed it, seconds of CPU time |

### printf

Supports flags `- + space # 0`, width, precision and the conversions
`d i u x X o c s f F e E g G %`; length modifiers such as `l` are accepted
and ignored. `%d` and friends accept ints, bools and floats (cut to an int).
`%s` shows any value the way `print` does.

### scanf

Understands `%d %i %u %x %f %e %g %s %c` and `%%`. White space in the format
skips any white space in the input. The arguments after the format must be
`&` followed by a variable, an array element or a map field:

```c
int a, v[3];
scanf("%d %d", &a, &v[0]);
```

`print` shows arrays as `[1, 2]`, maps as `{"k": 1}` and strings inside
them in quotes.
