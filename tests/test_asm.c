#include "lvm.h"

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

/* assemble + load + run; returns the status of the first step that failed */
static lvm_status run_src(const char *src, lvm_value *out, char *err, size_t errsz)
{
    uint8_t *bc;
    size_t n;
    lvm_module m;
    lvm_vm *vm;
    lvm_status st = lvm_assemble(NULL, src, strlen(src), &bc, &n, err, errsz);

    if (st != LVM_OK) return st;
    st = lvm_load(NULL, bc, n, &m);
    lvm_assemble_free(NULL, bc, n);
    if (st != LVM_OK) return st;
    vm = lvm_new(NULL, 4096, 256);
    st = lvm_run(vm, &m, NULL, 0, out);
    lvm_free(vm);
    lvm_module_free(&m);
    return st;
}

static int expect_int(const char *src, int64_t want)
{
    char err[256];
    lvm_value r;
    lvm_status st = run_src(src, &r, err, sizeof err);

    if (st != LVM_OK) {
        printf("  assemble/run failed: %s (%s)\n", lvm_strerror(st), err);
        return 0;
    }
    if (!lvm_is_int(r) || lvm_as_int(r) != want) {
        printf("  wrong result (wanted %lld)\n", (long long)want);
        return 0;
    }
    return 1;
}

static int expect_error(const char *src, lvm_status want, const char *needle)
{
    char err[256];
    lvm_value r;
    lvm_status st = run_src(src, &r, err, sizeof err);

    if (st != want || !strstr(err, needle)) {
        printf("  wanted '%s' with status %d, got status %d: '%s'\n", needle, (int)want, (int)st, err);
        return 0;
    }
    return 1;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    if (buf) buf[n] = '\0';
    fclose(f);
    return buf;
}

/* each example states its result on a line "; expect: N" */
static void test_example(const char *path)
{
    char *src = slurp(path);
    const char *tag;

    CHECK(src != NULL);
    if (!src) return;
    tag = strstr(src, "expect:");
    CHECK(tag != NULL);
    if (tag) {
        int ok = expect_int(src, strtoll(tag + 7, NULL, 10));
        if (!ok) printf("  in %s\n", path);
        CHECK(ok);
    }
    free(src);
}

static void test_syntax(void)
{
    /* case-insensitive, three comment styles, hex and negative numbers */
    CHECK(expect_int("main:\n  ldi r0, 0x10  ; sixteen\n  ldi r1, -6 # six\n"
                     "  add r2, r0, r1 // sum\n  ret r2\n", 10));

    /* a label may share a line with its instruction */
    CHECK(expect_int("main:\n  LDI R0, 7\n  JMP .end\n  LDI R0, 1\n.end: RET R0\n", 7));

    /* escapes, and comment characters inside strings */
    CHECK(expect_int("main:\n  LDK R0, \"a\\n\\x41\"\n  LEN R1, R0\n  RET R1\n", 3));
    CHECK(expect_int("main:\n  LDK R0, \"a;b#c//d\"\n  LEN R1, R0\n  RET R1\n", 8));

    /* constants beyond 16 bits */
    CHECK(expect_int("main:\n  LDK R0, 100000\n  RET R0\n", 100000));

    /* the same local label name in two functions */
    CHECK(expect_int("main:\n.loop:\n  LDI R0, 0\n  CALL R0, f\n  RET R0\n"
                     "f:\n  LDI R0, 3\n  LDI R1, 0\n  LDI R2, 1\n  LDI R3, 0\n"
                     ".loop:\n  ADD R1, R1, R0\n  SUB R0, R0, R2\n  JGT R0, R3, .loop\n  RET R1\n", 6));

    /* .params and argument registers */
    CHECK(expect_int("main:\n  LDI R0, 40\n  LDI R1, 2\n  CALL R0, add\n  RET R0\n"
                     "add:\n  .params 2\n  ADD R0, R0, R1\n  RET R0\n", 42));

    /* "main" is the entry wherever it sits; otherwise the first function */
    CHECK(expect_int("helper:\n  LDI R0, 1\n  RET R0\nmain:\n  LDI R0, 2\n  RET R0\n", 2));
    CHECK(expect_int("first:\n  LDI R0, 3\n  RET R0\nsecond:\n  LDI R0, 4\n  RET R0\n", 3));

    /* LDNIL with a range, NEWARR with a hint */
    CHECK(expect_int("main:\n  NEWARR R0, 8\n  LDI R1, 0\n  LDI R2, 5\n  SET R0, R1, R2\n"
                     "  LDNIL R3, R4\n  GET R5, R0, R1\n  RET R5\n", 5));
}

static void test_errors(void)
{
    CHECK(expect_error("main:\n  FOO R0\n", LVM_ERR_ASM, "line 2: unknown instruction 'FOO'"));
    CHECK(expect_error("main:\n  LDI R0\n", LVM_ERR_ASM, "takes 2 operand"));
    CHECK(expect_error("main:\n  LDI R300, 1\n", LVM_ERR_ASM, "R300"));
    CHECK(expect_error("main:\n  LDI X, 1\n", LVM_ERR_ASM, "expected a register"));
    CHECK(expect_error("main:\n  LDI R0, 100000\n  RET R0\n", LVM_ERR_ASM, "use LDK"));
    CHECK(expect_error("main:\n  LDI R0, 1\n  JMP .nowhere\n  RET R0\n", LVM_ERR_ASM, "undefined label"));
    CHECK(expect_error("main:\n.a:\n  RET R0\n.a:\n  RET R0\n", LVM_ERR_ASM, "duplicate label"));
    CHECK(expect_error("a:\n  RET R0\na:\n  RET R0\n", LVM_ERR_ASM, "duplicate function"));
    CHECK(expect_error("main:\n  CALL R0, nope\n  RET R0\n", LVM_ERR_ASM, "undefined function"));
    CHECK(expect_error("  LDI R0, 1\n", LVM_ERR_ASM, "outside a function"));
    CHECK(expect_error("main:\n  LDK R0, \"abc\n", LVM_ERR_ASM, "unterminated string"));
    CHECK(expect_error("main:\n  LDK R0, \"\\q\"\n  RET R0\n", LVM_ERR_ASM, "unknown escape"));
    CHECK(expect_error("main:\n  .nope 1\n  RET R0\n", LVM_ERR_ASM, "unknown directive"));
    CHECK(expect_error("main:\n", LVM_ERR_ASM, "no instructions"));
    CHECK(expect_error("", LVM_ERR_ASM, "no functions"));
    CHECK(expect_error("main:\n  RET R0, R1\n", LVM_ERR_ASM, "takes 1 operand"));
    CHECK(expect_error("main:\n  LDK R0, 1.5.5\n  RET R0\n", LVM_ERR_ASM, "bad constant"));
    CHECK(expect_error("main:\n  LDI R0, 1\n  RET R0\nf:\n  .upval stack R0\n  RET R0\n"
                       "g:\n  CALL R0, f\n  RET R0\n", LVM_ERR_ASM, "CLOSURE"));

    /* syntactically fine, but the verifier rejects it: no RET at the end */
    CHECK(expect_error("main:\n  LDI R0, 1\n", LVM_ERR_VERIFY, "verifier"));
}

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) test_example(argv[i]);
    test_syntax();
    test_errors();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
