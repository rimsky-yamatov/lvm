#include "lvm.h"
#include "lvm_opcode.h"

#include <stdio.h>
#include <string.h>

static int checks, failures;

#define CHECK(cond) do {                                                  \
    checks++;                                                             \
    if (!(cond)) {                                                        \
        failures++;                                                       \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                     \
} while (0)

/* ---- tiny bytecode builder ---- */

static uint8_t bc[8192];
static size_t bn;

static void u8(unsigned v)  { bc[bn++] = (uint8_t)v; }
static void u16(unsigned v) { u8(v & 0xFF); u8(v >> 8); }
static void u32(uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); }
static void u64(uint64_t v) { u32((uint32_t)v); u32((uint32_t)(v >> 32)); }

static void begin(uint32_t nk, uint32_t nf, uint32_t entry)
{
    bn = 0;
    memcpy(bc, "LVMB", 4);
    bn = 4;
    u16(1);
    u16(0);
    u32(nk);
    u32(nf);
    u32(entry);
}

static void k_int(int64_t v) { u8(2); u64((uint64_t)v); }

static void k_flt(double d)
{
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    u8(3);
    u64(u);
}

static void func(unsigned np, unsigned nr, const uint32_t *code, uint32_t n)
{
    uint32_t i;
    u8(np);
    u8(nr);
    u32(n);
    for (i = 0; i < n; i++) u32(code[i]);
}

#define FUNC(np, nr, ...) do {                                            \
    static const uint32_t c_[] = { __VA_ARGS__ };                         \
    func(np, nr, c_, (uint32_t)(sizeof c_ / sizeof c_[0]));               \
} while (0)

/* ---- helpers ---- */

static lvm_status host_double(void *ud, lvm_value *args, int argc)
{
    (void)ud;
    if (argc != 1 || !lvm_is_int(args[0])) return LVM_ERR_HOST;
    args[0] = lvm_int(lvm_as_int(args[0]) * 2);
    return LVM_OK;
}

static lvm_status run(lvm_value *out)
{
    lvm_module m;
    lvm_vm *vm;
    lvm_status st = lvm_load(NULL, bc, bn, &m);

    if (st != LVM_OK) return st;
    vm = lvm_new(NULL, 1024, 64);
    lvm_set_host(vm, 0, host_double, NULL);
    st = lvm_run(vm, &m, NULL, 0, out);
    lvm_free(vm);
    lvm_module_free(&m);
    return st;
}

static lvm_status load_only(void)
{
    lvm_module m;
    lvm_status st = lvm_load(NULL, bc, bn, &m);
    if (st == LVM_OK) lvm_module_free(&m);
    return st;
}

/* ---- arithmetic ---- */

static void test_int_arith(void)
{
    lvm_value r;

    begin(0, 1, 0);
    FUNC(0, 3, LVM_ASBX(OP_LDI, 0, 6), LVM_ASBX(OP_LDI, 1, 7),
         LVM_ABC(OP_MUL, 2, 0, 1), LVM_ABC(OP_RET, 2, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_is_int(r) && lvm_as_int(r) == 42);

    begin(0, 1, 0);
    FUNC(0, 3, LVM_ASBX(OP_LDI, 0, 7), LVM_ASBX(OP_LDI, 1, 2),
         LVM_ABC(OP_DIV, 2, 0, 1), LVM_ABC(OP_RET, 2, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_is_int(r) && lvm_as_int(r) == 3);

    begin(0, 1, 0);
    FUNC(0, 3, LVM_ASBX(OP_LDI, 0, 7), LVM_ASBX(OP_LDI, 1, 3),
         LVM_ABC(OP_MOD, 2, 0, 1), LVM_ABC(OP_RET, 2, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 1);

    begin(0, 1, 0);
    FUNC(0, 2, LVM_ASBX(OP_LDI, 0, -5), LVM_ABC(OP_NEG, 1, 0, 0),
         LVM_ABC(OP_RET, 1, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 5);
}

static void test_float_arith(void)
{
    lvm_value r;

    begin(2, 1, 0);
    k_flt(1.5);
    k_int(2);
    FUNC(0, 3, LVM_ABX(OP_LDK, 0, 0), LVM_ABX(OP_LDK, 1, 1),
         LVM_ABC(OP_ADD, 2, 0, 1), LVM_ABC(OP_RET, 2, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_is_float(r) && lvm_as_float(r) == 3.5);
}

static void test_int_overflow_promotes(void)
{
    lvm_value r;

    begin(1, 1, 0);
    k_int(LVM_INT_MAX);
    FUNC(0, 3, LVM_ABX(OP_LDK, 0, 0), LVM_ASBX(OP_LDI, 1, 1),
         LVM_ABC(OP_ADD, 2, 0, 1), LVM_ABC(OP_RET, 2, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_is_float(r) && lvm_as_float(r) == 140737488355328.0);
}

static void test_arith_errors(void)
{
    lvm_value r;

    begin(0, 1, 0);
    FUNC(0, 3, LVM_ASBX(OP_LDI, 0, 1), LVM_ASBX(OP_LDI, 1, 0),
         LVM_ABC(OP_DIV, 2, 0, 1), LVM_ABC(OP_RET, 2, 0, 0));
    CHECK(run(&r) == LVM_ERR_DIVZERO);

    begin(0, 1, 0);
    FUNC(0, 3, LVM_ABC(OP_LDNIL, 0, 0, 0), LVM_ASBX(OP_LDI, 1, 1),
         LVM_ABC(OP_ADD, 2, 0, 1), LVM_ABC(OP_RET, 2, 0, 0));
    CHECK(run(&r) == LVM_ERR_TYPE);
}

static void test_bitwise(void)
{
    lvm_value r;

    begin(0, 1, 0);
    FUNC(0, 3, LVM_ASBX(OP_LDI, 0, 1), LVM_ASBX(OP_LDI, 1, 4),
         LVM_ABC(OP_SHL, 2, 0, 1), LVM_ABC(OP_RET, 2, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 16);

    begin(0, 1, 0);
    FUNC(0, 3, LVM_ASBX(OP_LDI, 0, -16), LVM_ASBX(OP_LDI, 1, 2),
         LVM_ABC(OP_SHR, 2, 0, 1), LVM_ABC(OP_RET, 2, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == -4);

    begin(0, 1, 0);
    FUNC(0, 2, LVM_ASBX(OP_LDI, 0, 0), LVM_ABC(OP_NOT, 1, 0, 0),
         LVM_ABC(OP_RET, 1, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == -1);

    begin(0, 1, 0);
    FUNC(0, 3, LVM_ASBX(OP_LDI, 0, 12), LVM_ASBX(OP_LDI, 1, 10),
         LVM_ABC(OP_AND, 2, 0, 1), LVM_ABC(OP_RET, 2, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 8);
}

/* ---- control flow ---- */

static void test_loop(void)
{
    lvm_value r;

    /* sum 1..100 */
    begin(0, 1, 0);
    FUNC(0, 4,
         LVM_ASBX(OP_LDI, 0, 0),
         LVM_ASBX(OP_LDI, 1, 1),
         LVM_ASBX(OP_LDI, 2, 100),
         LVM_ASBX(OP_LDI, 3, 1),
         LVM_ABC(OP_ADD, 0, 0, 1),
         LVM_ABC(OP_ADD, 1, 1, 3),
         LVM_ABC(OP_LE, 1, 1, 2),
         LVM_ASBX(OP_JMP, 0, -4),
         LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 5050);
}

static void test_fib(void)
{
    lvm_value r;

    begin(0, 2, 0);
    FUNC(0, 2,
         LVM_ASBX(OP_LDI, 0, 20),
         LVM_ABX(OP_CALL, 0, 1),
         LVM_ABC(OP_RET, 0, 0, 0));
    FUNC(1, 5,
         LVM_ASBX(OP_LDI, 1, 2),
         LVM_ABC(OP_LT, 0, 0, 1),
         LVM_ASBX(OP_JMP, 0, 1),
         LVM_ABC(OP_RET, 0, 0, 0),
         LVM_ASBX(OP_LDI, 4, 1),
         LVM_ABC(OP_SUB, 2, 0, 4),
         LVM_ABX(OP_CALL, 2, 1),
         LVM_ABC(OP_SUB, 3, 0, 1),
         LVM_ABX(OP_CALL, 3, 1),
         LVM_ABC(OP_ADD, 0, 2, 3),
         LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 6765);
}

static void test_stack_overflow(void)
{
    lvm_value r;

    begin(0, 1, 0);
    FUNC(0, 1, LVM_ABX(OP_CALL, 0, 0), LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(run(&r) == LVM_ERR_STACK);
}

static void test_equality_and_branches(void)
{
    lvm_value r;

    /* 1 == 1.0 */
    begin(1, 1, 0);
    k_flt(1.0);
    FUNC(0, 3,
         LVM_ASBX(OP_LDI, 0, 1),
         LVM_ABX(OP_LDK, 1, 0),
         LVM_ABC(OP_EQ, 1, 0, 1),
         LVM_ASBX(OP_JMP, 0, 2),
         LVM_ABC(OP_LDBOOL, 2, 0, 0),
         LVM_ABC(OP_RET, 2, 0, 0),
         LVM_ABC(OP_LDBOOL, 2, 1, 0),
         LVM_ABC(OP_RET, 2, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_is_bool(r) && lvm_as_bool(r) == 1);

    /* nil is falsy */
    begin(0, 1, 0);
    FUNC(0, 2,
         LVM_ABC(OP_LDNIL, 0, 0, 0),
         LVM_ASBX(OP_JF, 0, 2),
         LVM_ASBX(OP_LDI, 1, 0),
         LVM_ABC(OP_RET, 1, 0, 0),
         LVM_ASBX(OP_LDI, 1, 1),
         LVM_ABC(OP_RET, 1, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 1);

    /* comparing nil with a number is a type error */
    begin(0, 1, 0);
    FUNC(0, 3,
         LVM_ABC(OP_LDNIL, 0, 0, 0),
         LVM_ASBX(OP_LDI, 1, 1),
         LVM_ABC(OP_LT, 1, 0, 1),
         LVM_ASBX(OP_JMP, 0, 0),
         LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(run(&r) == LVM_ERR_TYPE);
}

static void test_host(void)
{
    lvm_value r;

    begin(0, 1, 0);
    FUNC(0, 1,
         LVM_ASBX(OP_LDI, 0, 21),
         LVM_ABC(OP_HOST, 0, 1, 0),
         LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 42);

    /* unregistered host id */
    begin(0, 1, 0);
    FUNC(0, 1,
         LVM_ASBX(OP_LDI, 0, 21),
         LVM_ABC(OP_HOST, 0, 1, 7),
         LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(run(&r) == LVM_ERR_HOST);
}

/* ---- loader and verifier ---- */

static void test_bad_container(void)
{
    begin(0, 1, 0);
    FUNC(0, 1, LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(load_only() == LVM_OK);

    bc[0] = 'X';
    CHECK(load_only() == LVM_ERR_FORMAT);
    bc[0] = 'L';

    bn--;
    CHECK(load_only() == LVM_ERR_FORMAT); /* truncated */
    bn++;

    u8(0);
    CHECK(load_only() == LVM_ERR_FORMAT); /* trailing bytes */
}

static void test_verifier_rejects(void)
{
    begin(0, 1, 0);
    FUNC(0, 2, LVM_ABC(OP_MOV, 5, 0, 0), LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(load_only() == LVM_ERR_VERIFY); /* register out of range */

    begin(0, 1, 0);
    FUNC(0, 1, LVM_ASBX(OP_JMP, 0, 100));
    CHECK(load_only() == LVM_ERR_VERIFY); /* jump out of range */

    begin(0, 1, 0);
    FUNC(0, 1, LVM_ASBX(OP_LDI, 0, 1));
    CHECK(load_only() == LVM_ERR_VERIFY); /* falls off the end */

    begin(0, 1, 0);
    FUNC(0, 2, LVM_ABC(OP_EQ, 1, 0, 1), LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(load_only() == LVM_ERR_VERIFY); /* compare without jump */

    begin(0, 1, 0);
    FUNC(0, 1, LVM_ABX(OP_LDK, 0, 5), LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(load_only() == LVM_ERR_VERIFY); /* constant out of range */

    begin(0, 1, 0);
    FUNC(0, 1, LVM_ABX(OP_CALL, 0, 9), LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(load_only() == LVM_ERR_VERIFY); /* function out of range */

    begin(0, 1, 0);
    FUNC(0, 1, 0xFFu, LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(load_only() == LVM_ERR_VERIFY); /* unknown opcode */
}

int main(void)
{
    test_int_arith();
    test_float_arith();
    test_int_overflow_promotes();
    test_arith_errors();
    test_bitwise();

    test_loop();
    test_fib();
    test_stack_overflow();
    test_equality_and_branches();
    test_host();

    test_bad_container();
    test_verifier_rejects();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
