#include "lvm.h"
#include "lvm_std.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;

#define CHECK(cond) do {                                                  \
    checks++;                                                             \
    if (!(cond)) {                                                        \
        failures++;                                                       \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                     \
} while (0)

/* the program's standard input and output live in memory */
typedef struct {
    const char *in;
    size_t inpos;
    char out[16384];
    size_t outn;
} mem_io;

static int io_read(void *ud)
{
    mem_io *io = ud;

    return io->in[io->inpos] ? (unsigned char)io->in[io->inpos++] : -1;
}

static void io_write(void *ud, const char *s, size_t n)
{
    mem_io *io = ud;

    if (io->outn + n < sizeof io->out) {
        memcpy(io->out + io->outn, s, n);
        io->outn += n;
        io->out[io->outn] = '\0';
    }
}

/* compile + assemble + run; returns the status of the first step that failed */
static lvm_status run_lvc(const char *src, const char *input, mem_io *io, int64_t *ret, char *err, size_t errsz)
{
    char *text;
    size_t tl, bl;
    uint8_t *bc;
    lvm_module m;
    lvm_vm *vm;
    lvm_std *std;
    lvm_std_io sio;
    lvm_value r = lvm_nil();
    lvm_status st;

    memset(io, 0, sizeof *io);
    io->in = input ? input : "";
    st = lvm_compile(NULL, src, strlen(src), &text, &tl, err, errsz);
    if (st != LVM_OK) return st;
    st = lvm_assemble(NULL, text, tl, &bc, &bl, err, errsz);
    if (st != LVM_OK) {
        printf("  the generated assembly was rejected: %s\n%s\n", err, text);
        lvm_compile_free(NULL, text, tl);
        return st;
    }
    lvm_compile_free(NULL, text, tl);
    st = lvm_load(NULL, bc, bl, &m);
    lvm_assemble_free(NULL, bc, bl);
    if (st != LVM_OK) return st;

    vm = lvm_new(NULL, 1u << 16, 2048);
    sio.read_char = io_read;
    sio.write = io_write;
    sio.ud = io;
    std = lvm_std_new(vm, &sio);
    st = lvm_run(vm, &m, NULL, 0, &r);
    *ret = lvm_is_int(r) ? lvm_as_int(r) : 0;
    lvm_std_free(std);
    lvm_free(vm);
    lvm_module_free(&m);
    return st;
}

static int expect_prog(const char *src, const char *input, const char *want_out, int64_t want_ret)
{
    mem_io *io = malloc(sizeof *io);
    char err[256] = "";
    int64_t ret = 0;
    lvm_status st;
    int ok;

    st = run_lvc(src, input, io, &ret, err, sizeof err);
    ok = st == LVM_OK && strcmp(io->out, want_out) == 0 && ret == want_ret;
    if (!ok) {
        printf("  status %s (%s), exit value %lld\n  got:    \"%s\"\n  wanted: \"%s\"\n",
               lvm_strerror(st), err, (long long)ret, io->out, want_out);
    }
    free(io);
    return ok;
}

static int expect_compile_error(const char *src, const char *needle)
{
    mem_io *io = malloc(sizeof *io);
    char err[256] = "";
    int64_t ret;
    lvm_status st = run_lvc(src, "", io, &ret, err, sizeof err);
    int ok = st == LVM_ERR_COMPILE && strstr(err, needle) != NULL;

    if (!ok) printf("  wanted \"%s\", got status %d: \"%s\"\n", needle, (int)st, err);
    free(io);
    return ok;
}

/* wrap a body in main() */
#define MAIN(body) "int main() {\n" body "\n}\n"

static void test_expressions(void)
{
    CHECK(expect_prog(MAIN("printf(\"%d %d %d %d\\n\", 1 + 2 * 3, (1 + 2) * 3, 7 - 3 - 2, 100 / 10 / 5); return 0;"),
                      NULL, "7 9 2 2\n", 0));
    CHECK(expect_prog(MAIN("printf(\"%d %d %d %d\\n\", 1 << 2 + 1, 5 & 3 | 8, 6 ^ 3 & 1, ~5); return 0;"),
                      NULL, "8 9 7 -6\n", 0));
    CHECK(expect_prog(MAIN("printf(\"%d %d %d\\n\", -7 / 2, -7 % 3, 7 % -3); return 0;"),
                      NULL, "-3 -1 1\n", 0));
    CHECK(expect_prog(MAIN("printf(\"%d %d %d %d\\n\", 3 < 4, 3 >= 4, 3 == 3, 3 != 3); return 0;"),
                      NULL, "1 0 1 0\n", 0));
    CHECK(expect_prog(MAIN("int a = 5; printf(\"%d %d\\n\", a > 3 ? 10 : 20, a > 9 ? 10 : a > 4 ? 30 : 40); return 0;"),
                      NULL, "10 30\n", 0));
    CHECK(expect_prog(MAIN("print(1.5 + 1, 7 / 2, 7 / 2.0, 2.5 * 2, 10 % 4, 1e3);"),
                      NULL, "2.5 3 3.5 5.0 2 1000.0\n", 0));
    CHECK(expect_prog(MAIN("return 6 * 7;"), NULL, "", 42));
}

static void test_assignment(void)
{
    CHECK(expect_prog(MAIN("int a = 1, b = 2; a += b; b *= a; a <<= 2; printf(\"%d %d\\n\", a, b); return 0;"),
                      NULL, "12 6\n", 0));
    CHECK(expect_prog(MAIN("int a = 1; int b = a++ + ++a; int c = a--; printf(\"%d %d %d\\n\", a, b, c); return 0;"),
                      NULL, "2 4 3\n", 0));
    CHECK(expect_prog(MAIN("int a, b, c; a = b = c = 7; printf(\"%d%d%d\\n\", a, b, c); return 0;"),
                      NULL, "777\n", 0));
    CHECK(expect_prog(MAIN("int a[3]; a[0] = 1; a[1] += 5; a[2]--; a[0]++; printf(\"%d %d %d\\n\", a[0], a[1], a[2]); return 0;"),
                      NULL, "2 5 -1\n", 0));
    /* declarations start with a value that fits their type */
    CHECK(expect_prog(MAIN("int i; float f; bool b; string s; var v; print(i, f, b, s, v); return 0;"),
                      NULL, "0 0.0 false  nil\n", 0));
}

static void test_logic(void)
{
    /* && and || stop early, and the right side's effects do not happen */
    CHECK(expect_prog("int calls = 0;\n"
                      "bool t() { calls++; return true; }\n"
                      "bool f() { calls += 10; return false; }\n"
                      "int main() {\n"
                      "  bool a = f() && t(); bool b = t() || f(); bool c = f() || t();\n"
                      "  printf(\"%d %d %d calls=%d\\n\", a, b, c, calls); return 0; }\n",
                      NULL, "0 1 1 calls=22\n", 0));
    /* 0, nil and false are false; everything else is true */
    CHECK(expect_prog(MAIN("int z = 0; var n = nil; string e = \"\"; var arr = [];"
                           "printf(\"%d%d%d%d%d%d\\n\", !z, !n, !false, !e, !arr, !1); return 0;"),
                      NULL, "111000\n", 0));
    CHECK(expect_prog(MAIN("int i = 0; if (i) printf(\"yes\\n\"); else printf(\"no\\n\"); return 0;"),
                      NULL, "no\n", 0));
}

static void test_control_flow(void)
{
    CHECK(expect_prog(MAIN("int s = 0; for (int i = 1; i <= 10; i++) { if (i % 2) continue; if (i > 8) break; s += i; } return s;"),
                      NULL, "", 20));
    CHECK(expect_prog(MAIN("int i = 0; while (i < 5) i++; do { i += 10; } while (i < 5); return i;"),
                      NULL, "", 15));
    CHECK(expect_prog(MAIN("int x = 1; { int x = 2; x++; } return x;"), NULL, "", 1));
    CHECK(expect_prog(MAIN("int n = 0; for (;;) { if (++n == 4) break; } return n;"), NULL, "", 4));
    CHECK(expect_prog("int sum(int n) { if (n == 0) return 0; return n + sum(n - 1); }\n"
                      "int main() { return sum(100); }\n", NULL, "", 5050));
}

static void test_functions_and_globals(void)
{
    CHECK(expect_prog("int g = 3; int a[4]; string name = \"lvc\";\n"
                      "void set(int i, int v) { a[i] = v; g += v; }\n"
                      "int main() { set(1, 10); set(2, 20); printf(\"%s %d %d %d\\n\", name, g, a[1], a[2]); return 0; }\n",
                      NULL, "lvc 33 10 20\n", 0));
    CHECK(expect_prog("int twice(int x) { return x * 2; }\n"
                      "int main() { return twice(twice(twice(3))) + twice(1); }\n", NULL, "", 26));
    /* functions may be used before they are defined */
    CHECK(expect_prog("int main() { return later(5); }\nint later(int x) { return x + 1; }\n", NULL, "", 6));
    CHECK(expect_prog("int base = 100; int next = base + 1;\nint main() { return next; }\n", NULL, "", 101));
}

static void test_containers(void)
{
    CHECK(expect_prog(MAIN("var a = [3, 1, 2]; push(a, 0); sort(a); print(a, len(a), pop(a), len(a)); return 0;"),
                      NULL, "[0, 1, 2] 4 3 3\n", 0));
    CHECK(expect_prog(MAIN("var m = {\"x\": 1}; m.y = 2; m[\"z\"] = m.x + m.y; remove(m, \"x\");"
                           "print(len(m), m.z, m.x, m.missing == nil); return 0;"),
                      NULL, "2 3 nil true\n", 0));
    CHECK(expect_prog(MAIN("int a[2] = {5, 6}; var rows = array(2); rows[0] = a; rows[1] = [7];"
                           "print(rows[0][1], rows[1][0], len(rows)); return 0;"),
                      NULL, "6 7 2\n", 0));
    CHECK(expect_prog(MAIN("var grid = array(3); for (int i = 0; i < 3; i++) grid[i] = array(3, 0);"
                           "grid[1][2] = 9; print(grid); return 0;"),
                      NULL, "[[0, 0, 0], [0, 0, 9], [0, 0, 0]]\n", 0));
}

static void test_strings(void)
{
    CHECK(expect_prog(MAIN("string s = \"abc\"; s += \"def\"; print(s, len(s), substr(s, 2, 3), find(s, \"de\"), find(s, \"zz\"));"
                           "return 0;"), NULL, "abcdef 6 cde 3 -1\n", 0));
    CHECK(expect_prog(MAIN("string s = \"hey\"; print(s[0], chr(s[1]), s == \"hey\", s != \"HEY\", strcmp(\"a\", \"b\"));"
                           "return 0;"), NULL, "104 e true true -1\n", 0));
    CHECK(expect_prog(MAIN("print(str(42) + \"!\", int(\"17abc\") + 1, float(\"2.5\") * 2, (int)3.9, (float)3);"
                           "return 0;"), NULL, "42! 18 5.0 3 3.0\n", 0));
    CHECK(expect_prog(MAIN("var p = split(\"a--b--c\", \"--\"); print(p, len(p)); return 0;"),
                      NULL, "[\"a\", \"b\", \"c\"] 3\n", 0));
    CHECK(expect_prog(MAIN("printf(\"tab\\there \\\"q\\\" \\\\ \\x41 %c\\n\", 66); return 0;"),
                      NULL, "tab\there \"q\" \\ A B\n", 0));
}

static void test_printf(void)
{
    CHECK(expect_prog(MAIN("printf(\"[%5d][%-5d][%05d][%+d][%c][%s][%%][%x][%X][%o]\\n\", 42, 42, 42, 7, 65, \"str\", 255, 255, 8);"
                           "return 0;"),
                      NULL, "[   42][42   ][00042][+7][A][str][%][ff][FF][10]\n", 0));
    CHECK(expect_prog(MAIN("printf(\"[%5.2f][%.0f][%e][%g][%f]\\n\", 3.14159, 2.5, 1500.0, 0.5, 2);"
                           "return 0;"),
                      NULL, "[ 3.14][2][1.500000e+03][0.5][2.000000]\n", 0));
    CHECK(expect_prog(MAIN("printf(\"[%8s][%-8s][%.2s]\\n\", \"ab\", \"ab\", \"abcdef\"); return 0;"),
                      NULL, "[      ab][ab      ][ab]\n", 0));
    CHECK(expect_prog(MAIN("int n = printf(\"abc\"); printf(\" %d\\n\", n); return 0;"), NULL, "abc 3\n", 0));
    CHECK(expect_prog(MAIN("printf(\"%d %s\\n\", 3.9, true); return 0;"), NULL, "3 true\n", 0));
}

static void test_scanf(void)
{
    CHECK(expect_prog(MAIN("int a; string s; float f; int n = scanf(\"%d %s %f\", &a, &s, &f);"
                           "printf(\"%d: %d %s %.1f\\n\", n, a, s, f); return 0;"),
                      "12 abc 3.5\n", "3: 12 abc 3.5\n", 0));
    /* a mismatch stops the scan; the end of the input gives -1 */
    CHECK(expect_prog(MAIN("int a = 7; int n = scanf(\"%d\", &a); printf(\"%d %d\\n\", n, a);"
                           "int m = scanf(\"%d\", &a); printf(\"%d\\n\", m); return 0;"),
                      "x", "0 7\n0\n", 0));
    CHECK(expect_prog(MAIN("int a; int n = scanf(\"%d\", &a); int m = scanf(\"%d\", &a); printf(\"%d %d %d\\n\", n, a, m);"
                           "return 0;"),
                      "5", "1 5 -1\n", 0));
    CHECK(expect_prog(MAIN("int v[3]; for (int i = 0; i < 3; i++) scanf(\"%d\", &v[i]); print(v);"
                           "var m = map(); scanf(\"%d\", &m.k); print(m.k); return 0;"),
                      "1 2 3 9", "[1, 2, 3]\n9\n", 0));
    CHECK(expect_prog(MAIN("int a, b; scanf(\"%d,%d\", &a, &b); printf(\"%d\\n\", a * b); return 0;"),
                      "6,7", "42\n", 0));
    CHECK(expect_prog(MAIN("int c = getchar(); string l = readline(); putchar(c + 1); print(\"|\" + l + \"|\", getchar());"
                           "return 0;"),
                      "Abc\nx", "B|bc| 120\n", 0));
}

static void test_exceptions(void)
{
    CHECK(expect_prog(MAIN("try { throw 42; } catch (e) { printf(\"%d\\n\", e + 1); } return 0;"), NULL, "43\n", 0));
    CHECK(expect_prog("void boom(int n) { if (n == 0) throw \"bottom\"; boom(n - 1); }\n"
                      "int main() { try { boom(5); } catch (e) { print(\"caught\", e); } return 0; }\n",
                      NULL, "caught bottom\n", 0));
    /* break and continue leave the try block cleanly */
    CHECK(expect_prog(MAIN("int n = 0; for (int i = 0; i < 6; i++) { try { if (i == 1) continue; if (i == 4) break; n += i; } catch (e) { n = -1; } }"
                           "try { throw 1; } catch (e) { n += 100; } return n;"),
                      NULL, "", 105));
    CHECK(expect_prog(MAIN("try { try { throw 1; } catch (e) { throw e + 10; } } catch (e) { printf(\"%d\\n\", e); } return 0;"),
                      NULL, "11\n", 0));
    CHECK(expect_prog(MAIN("try { var a = [1]; a[3] = 0; } catch (e) { print(e); }"
                           "try { int x = \"a\" - 1; } catch (e) { print(e); }"
                           "try { print(len(5)); } catch (e) { print(e); } return 0;"),
                      NULL, "index out of bounds\ntype error\ntype error\n", 0));
    /* an exception nobody catches ends the run */
    {
        mem_io *io = malloc(sizeof *io);
        char err[256];
        int64_t ret;
        CHECK(run_lvc(MAIN("throw 5;"), "", io, &ret, err, sizeof err) == LVM_ERR_UNCAUGHT);
        free(io);
    }
}

static void test_gc_pressure(void)
{
    /* thousands of short-lived strings and arrays while some stay alive */
    CHECK(expect_prog("string pad(int n) { string s = \"\"; for (int i = 0; i < n; i++) s += \"x\"; return s; }\n"
                      "int main() {\n"
                      "  var keep = [];\n"
                      "  int total = 0;\n"
                      "  for (int i = 0; i < 2000; i++) {\n"
                      "    var tmp = [i, str(i), pad(i % 20)];\n"
                      "    if (i % 500 == 0) push(keep, tmp);\n"
                      "    total += len(tmp[2]);\n"
                      "  }\n"
                      "  print(len(keep), keep[3][1], total);\n"
                      "  return 0; }\n", NULL, "4 1500 19000\n", 0));
}

static void test_compile_errors(void)
{
    CHECK(expect_compile_error(MAIN("return y;"), "line 2: undefined variable 'y'"));
    CHECK(expect_compile_error(MAIN("int x; int x;"), "already declared"));
    CHECK(expect_compile_error(MAIN("foo();"), "undefined function 'foo'"));
    CHECK(expect_compile_error("int f(int a) { return a; }\nint main() { return f(1, 2); }\n", "takes 1 argument"));
    CHECK(expect_compile_error(MAIN("break;"), "outside a loop"));
    CHECK(expect_compile_error(MAIN("x = ;"), "expected an expression"));
    CHECK(expect_compile_error(MAIN("return 1"), "expected ';'"));
    CHECK(expect_compile_error("int f() { return 1; }\n", "no main function"));
    CHECK(expect_compile_error("int main(int a) { return a; }\n", "main takes no parameters"));
    CHECK(expect_compile_error("int len(int a) { return a; }\nint main() { return 0; }\n", "built-in"));
    CHECK(expect_compile_error("int f() { return 1; }\nint f() { return 2; }\nint main() { return 0; }\n", "defined twice"));
    CHECK(expect_compile_error("int g; int g;\nint main() { return 0; }\n", "declared twice"));
    CHECK(expect_compile_error(MAIN("int a[2][2];"), "multi-dimensional"));
    CHECK(expect_compile_error(MAIN("scanf(\"%d\", 5);"), "&variable"));
    CHECK(expect_compile_error(MAIN("int a = &a;"), "scanf"));
    CHECK(expect_compile_error(MAIN("printf(\"abc);"), "unterminated string"));
    CHECK(expect_compile_error(MAIN("int a = 1 + ;"), "expected an expression"));
    CHECK(expect_compile_error(MAIN("5 = 3;"), "left side of an assignment"));
    CHECK(expect_compile_error(MAIN("int a = @;"), "unexpected character"));
    CHECK(expect_compile_error(MAIN("if (1) { return 0; "), "missing '}'"));
    CHECK(expect_compile_error(MAIN("int a[];"), "needs a size"));
    CHECK(expect_compile_error(MAIN("int v = 140737488355328;"), "48 bits"));
}

/* the examples carry their input and expected output in "// input:" and "// output:" lines */
static void test_example(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *src, input[1024] = "", want[4096] = "";
    long n;
    const char *p;
    mem_io *io;
    char err[256] = "";
    int64_t ret = 0, want_ret = 0;
    lvm_status st;

    CHECK(f != NULL);
    if (!f) return;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    src = malloc((size_t)n + 1);
    CHECK(fread(src, 1, (size_t)n, f) == (size_t)n);
    src[n] = '\0';
    fclose(f);

    for (p = src; strncmp(p, "// ", 3) == 0; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : p + strlen(p)) {
        const char *eol = strchr(p, '\n');
        size_t l = eol ? (size_t)(eol - p) : strlen(p);
        if (strncmp(p, "// input: ", 10) == 0) {
            strncat(input, p + 10, l - 10);
            strcat(input, "\n");
        } else if (strncmp(p, "// output: ", 11) == 0) {
            strncat(want, p + 11, l - 11);
            strcat(want, "\n");
        } else if (strncmp(p, "// exit: ", 9) == 0) {
            want_ret = atoi(p + 9);
        }
        if (!eol) break;
    }

    io = malloc(sizeof *io);
    st = run_lvc(src, input, io, &ret, err, sizeof err);
    if (st != LVM_OK || strcmp(io->out, want) != 0 || ret != want_ret) {
        printf("  in %s: status %s (%s)\n  got:    \"%s\"\n  wanted: \"%s\"\n",
               path, lvm_strerror(st), err, io->out, want);
        CHECK(0);
    } else {
        CHECK(1);
    }
    free(io);
    free(src);
}

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) test_example(argv[i]);
    test_expressions();
    test_assignment();
    test_logic();
    test_control_flow();
    test_functions_and_globals();
    test_containers();
    test_strings();
    test_printf();
    test_scanf();
    test_exceptions();
    test_gc_pressure();
    test_compile_errors();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
