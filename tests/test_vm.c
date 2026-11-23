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

static void k_str(const char *s)
{
    size_t n = strlen(s), i;
    u8(4);
    u32((uint32_t)n);
    for (i = 0; i < n; i++) u8((unsigned char)s[i]);
}

static uint8_t g_up[32]; /* upvalue descriptors for the next func() */
static unsigned g_nup;

#define UP(instack, idx) do {                                             \
    g_up[g_nup * 2] = (instack);                                          \
    g_up[g_nup * 2 + 1] = (idx);                                          \
    g_nup++;                                                              \
} while (0)

static void func(unsigned np, unsigned nr, const uint32_t *code, uint32_t n)
{
    uint32_t i;
    u8(np);
    u8(nr);
    u8(g_nup);
    for (i = 0; i < g_nup * 2; i++) u8(g_up[i]);
    g_nup = 0;
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

static lvm_status host_str(void *ud, lvm_value *args, int argc)
{
    (void)argc;
    args[0] = lvm_new_string((lvm_vm *)ud, "hi!", 3);
    return lvm_is_nil(args[0]) ? LVM_ERR_MEM : LVM_OK;
}

static size_t g_gc_min; /* when set, run() uses this gc threshold */
static size_t g_heap;   /* heap size right after the last run() */

static lvm_status run(lvm_value *out)
{
    lvm_module m;
    lvm_vm *vm;
    lvm_status st = lvm_load(NULL, bc, bn, &m);

    if (st != LVM_OK) return st;
    vm = lvm_new(NULL, 1024, 64);
    lvm_set_host(vm, 0, host_double, NULL);
    lvm_set_host(vm, 1, host_str, vm);
    if (g_gc_min) lvm_set_gc_threshold(vm, g_gc_min);
    st = lvm_run(vm, &m, NULL, 0, out);
    g_heap = lvm_heap_bytes(vm);
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

/* ---- strings ---- */

static void test_strings(void)
{
    lvm_value r;

    begin(3, 1, 0);
    k_str("hello");
    k_str("x");
    k_str("x");
    FUNC(0, 4, LVM_ABX(OP_LDK, 0, 0), LVM_ABC(OP_LEN, 1, 0, 0), LVM_ABC(OP_RET, 1, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 5);

    /* separate constants with equal content compare equal */
    begin(3, 1, 0);
    k_str("hello");
    k_str("x");
    k_str("x");
    FUNC(0, 3,
         LVM_ABX(OP_LDK, 0, 1),
         LVM_ABX(OP_LDK, 1, 2),
         LVM_ABC(OP_EQ, 1, 0, 1),
         LVM_ASBX(OP_JMP, 0, 2),
         LVM_ABC(OP_LDBOOL, 2, 0, 0),
         LVM_ABC(OP_RET, 2, 0, 0),
         LVM_ABC(OP_LDBOOL, 2, 1, 0),
         LVM_ABC(OP_RET, 2, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_bool(r) == 1);

    /* indexing yields bytes */
    begin(1, 1, 0);
    k_str("hello");
    FUNC(0, 3, LVM_ABX(OP_LDK, 0, 0), LVM_ASBX(OP_LDI, 1, 1),
         LVM_ABC(OP_GET, 2, 0, 1), LVM_ABC(OP_RET, 2, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 'e');

    /* strings are immutable */
    begin(1, 1, 0);
    k_str("abc");
    FUNC(0, 3, LVM_ABX(OP_LDK, 0, 0), LVM_ASBX(OP_LDI, 1, 0), LVM_ASBX(OP_LDI, 2, 1),
         LVM_ABC(OP_SET, 0, 1, 2), LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(run(&r) == LVM_ERR_TYPE);

    /* host-created string */
    begin(0, 1, 0);
    FUNC(0, 2, LVM_ABC(OP_HOST, 0, 0, 1), LVM_ABC(OP_LEN, 1, 0, 0), LVM_ABC(OP_RET, 1, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 3);
}

/* ---- arrays and maps ---- */

static void test_arrays(void)
{
    lvm_value r;

    begin(0, 1, 0);
    FUNC(0, 7,
         LVM_ABC(OP_NEWARR, 0, 0, 0),
         LVM_ASBX(OP_LDI, 1, 0), LVM_ASBX(OP_LDI, 2, 10), LVM_ABC(OP_SET, 0, 1, 2),
         LVM_ASBX(OP_LDI, 1, 1), LVM_ASBX(OP_LDI, 2, 20), LVM_ABC(OP_SET, 0, 1, 2),
         LVM_ASBX(OP_LDI, 1, 0), LVM_ABC(OP_GET, 3, 0, 1),
         LVM_ASBX(OP_LDI, 1, 1), LVM_ABC(OP_GET, 4, 0, 1),
         LVM_ABC(OP_ADD, 5, 3, 4),
         LVM_ABC(OP_LEN, 6, 0, 0),
         LVM_ABC(OP_MUL, 5, 5, 6),
         LVM_ABC(OP_RET, 5, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 60);

    /* reading past the end */
    begin(0, 1, 0);
    FUNC(0, 3, LVM_ABC(OP_NEWARR, 0, 0, 0), LVM_ASBX(OP_LDI, 1, 5),
         LVM_ABC(OP_GET, 2, 0, 1), LVM_ABC(OP_RET, 2, 0, 0));
    CHECK(run(&r) == LVM_ERR_BOUNDS);

    /* writing beyond len (only len itself appends) */
    begin(0, 1, 0);
    FUNC(0, 3, LVM_ABC(OP_NEWARR, 0, 0, 0), LVM_ASBX(OP_LDI, 1, 3), LVM_ASBX(OP_LDI, 2, 1),
         LVM_ABC(OP_SET, 0, 1, 2), LVM_ABC(OP_RET, 2, 0, 0));
    CHECK(run(&r) == LVM_ERR_BOUNDS);

    /* indexing a non-container */
    begin(0, 1, 0);
    FUNC(0, 3, LVM_ASBX(OP_LDI, 0, 1), LVM_ASBX(OP_LDI, 1, 0),
         LVM_ABC(OP_GET, 2, 0, 1), LVM_ABC(OP_RET, 2, 0, 0));
    CHECK(run(&r) == LVM_ERR_TYPE);

    /* an array can grow well past its initial capacity */
    begin(0, 1, 0);
    FUNC(0, 5,
         LVM_ABC(OP_NEWARR, 0, 2, 0),
         LVM_ASBX(OP_LDI, 1, 0), LVM_ASBX(OP_LDI, 2, 1000), LVM_ASBX(OP_LDI, 3, 1),
         LVM_ABC(OP_SET, 0, 1, 1),
         LVM_ABC(OP_ADD, 1, 1, 3),
         LVM_ABC(OP_LT, 1, 1, 2),
         LVM_ASBX(OP_JMP, 0, -4),
         LVM_ABC(OP_LEN, 4, 0, 0),
         LVM_ABC(OP_RET, 4, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 1000);
}

static void test_maps(void)
{
    lvm_value r;

    /* string keys compare by content */
    begin(3, 1, 0);
    k_str("a");
    k_str("b");
    k_str("a");
    FUNC(0, 6,
         LVM_ABC(OP_NEWMAP, 0, 0, 0),
         LVM_ABX(OP_LDK, 1, 0), LVM_ASBX(OP_LDI, 2, 1), LVM_ABC(OP_SET, 0, 1, 2),
         LVM_ABX(OP_LDK, 1, 1), LVM_ASBX(OP_LDI, 2, 2), LVM_ABC(OP_SET, 0, 1, 2),
         LVM_ABX(OP_LDK, 1, 2), LVM_ABC(OP_GET, 3, 0, 1),
         LVM_ABC(OP_LEN, 4, 0, 0),
         LVM_ASBX(OP_LDI, 5, 100),
         LVM_ABC(OP_MUL, 3, 3, 5),
         LVM_ABC(OP_ADD, 3, 3, 4),
         LVM_ABC(OP_RET, 3, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 102);

    /* assigning nil removes the key; missing keys read as nil */
    begin(1, 1, 0);
    k_str("a");
    FUNC(0, 5,
         LVM_ABC(OP_NEWMAP, 0, 0, 0),
         LVM_ABX(OP_LDK, 1, 0), LVM_ASBX(OP_LDI, 2, 1), LVM_ABC(OP_SET, 0, 1, 2),
         LVM_ABC(OP_LDNIL, 2, 0, 0), LVM_ABC(OP_SET, 0, 1, 2),
         LVM_ABC(OP_LEN, 3, 0, 0),
         LVM_ABC(OP_GET, 4, 0, 1),
         LVM_ABC(OP_RET, 3, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 0);

    begin(1, 1, 0);
    k_str("a");
    FUNC(0, 5,
         LVM_ABC(OP_NEWMAP, 0, 0, 0),
         LVM_ABX(OP_LDK, 1, 0), LVM_ASBX(OP_LDI, 2, 1), LVM_ABC(OP_SET, 0, 1, 2),
         LVM_ABC(OP_LDNIL, 2, 0, 0), LVM_ABC(OP_SET, 0, 1, 2),
         LVM_ABC(OP_GET, 4, 0, 1),
         LVM_ABC(OP_RET, 4, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_is_nil(r));

    /* 1 and 1.0 are the same key */
    begin(1, 1, 0);
    k_flt(1.0);
    FUNC(0, 4,
         LVM_ABC(OP_NEWMAP, 0, 0, 0),
         LVM_ASBX(OP_LDI, 1, 1), LVM_ASBX(OP_LDI, 2, 7), LVM_ABC(OP_SET, 0, 1, 2),
         LVM_ABX(OP_LDK, 1, 0), LVM_ABC(OP_GET, 3, 0, 1),
         LVM_ABC(OP_RET, 3, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 7);

    /* nil is not a valid key */
    begin(0, 1, 0);
    FUNC(0, 3, LVM_ABC(OP_NEWMAP, 0, 0, 0), LVM_ABC(OP_LDNIL, 1, 0, 0), LVM_ASBX(OP_LDI, 2, 1),
         LVM_ABC(OP_SET, 0, 1, 2), LVM_ABC(OP_RET, 2, 0, 0));
    CHECK(run(&r) == LVM_ERR_TYPE);

    /* 200 inserts, delete every multiple of 3, probe sequences must survive */
    begin(0, 1, 0);
    FUNC(0, 9,
         LVM_ABC(OP_NEWMAP, 0, 0, 0),
         LVM_ASBX(OP_LDI, 1, 0), LVM_ASBX(OP_LDI, 2, 200), LVM_ASBX(OP_LDI, 3, 1),
         LVM_ASBX(OP_LDI, 6, 3), LVM_ASBX(OP_LDI, 5, 0),
         LVM_ABC(OP_SET, 0, 1, 1),                 /*  6 */
         LVM_ABC(OP_ADD, 1, 1, 3),
         LVM_ABC(OP_LT, 1, 1, 2),
         LVM_ASBX(OP_JMP, 0, -4),
         LVM_ASBX(OP_LDI, 1, 0),                   /* 10 */
         LVM_ABC(OP_MOD, 4, 1, 6),                 /* 11 */
         LVM_ABC(OP_EQ, 0, 4, 5),
         LVM_ASBX(OP_JMP, 0, 2),
         LVM_ABC(OP_LDNIL, 7, 0, 0),
         LVM_ABC(OP_SET, 0, 1, 7),
         LVM_ABC(OP_ADD, 1, 1, 3),                 /* 16 */
         LVM_ABC(OP_LT, 1, 1, 2),
         LVM_ASBX(OP_JMP, 0, -8),
         LVM_ABC(OP_LEN, 4, 0, 0),                 /* 19 */
         LVM_ASBX(OP_LDI, 1, 199),
         LVM_ABC(OP_GET, 8, 0, 1),
         LVM_ABC(OP_ADD, 4, 4, 8),
         LVM_ABC(OP_RET, 4, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 133 + 199);
}

/* ---- garbage collection ---- */

static void test_gc(void)
{
    lvm_value r;

    /* nested live objects survive while 5000 rounds of garbage are collected */
    begin(0, 1, 0);
    FUNC(0, 10,
         LVM_ABC(OP_NEWARR, 0, 0, 0),
         LVM_ABC(OP_NEWARR, 1, 0, 0),
         LVM_ASBX(OP_LDI, 2, 0),
         LVM_ABC(OP_SET, 0, 2, 1),
         LVM_ASBX(OP_LDI, 3, 42),
         LVM_ABC(OP_SET, 1, 2, 3),
         LVM_ASBX(OP_LDI, 4, 0),
         LVM_ASBX(OP_LDI, 5, 5000),
         LVM_ASBX(OP_LDI, 6, 1),
         LVM_ABC(OP_NEWMAP, 7, 0, 0),              /* 9 */
         LVM_ABC(OP_NEWARR, 8, 0, 0),
         LVM_ABC(OP_SET, 8, 2, 4),
         LVM_ABC(OP_SET, 7, 4, 8),
         LVM_ABC(OP_ADD, 4, 4, 6),
         LVM_ABC(OP_LT, 1, 4, 5),
         LVM_ASBX(OP_JMP, 0, -7),
         LVM_ABC(OP_GET, 9, 0, 2),
         LVM_ABC(OP_GET, 9, 9, 2),
         LVM_ABC(OP_RET, 9, 0, 0));
    g_gc_min = 1024;
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 42);
    CHECK(g_heap < 8192); /* garbage was reclaimed, not accumulated */
    g_gc_min = 0;

    /* a caller's registers stay rooted while a callee triggers collections */
    begin(0, 2, 0);
    FUNC(0, 4,
         LVM_ABC(OP_NEWARR, 0, 0, 0),
         LVM_ASBX(OP_LDI, 1, 7),
         LVM_ASBX(OP_LDI, 2, 0),
         LVM_ABC(OP_SET, 0, 2, 1),
         LVM_ABX(OP_CALL, 1, 1),
         LVM_ASBX(OP_LDI, 2, 0),
         LVM_ABC(OP_GET, 3, 0, 2),
         LVM_ABC(OP_ADD, 3, 3, 1),
         LVM_ABC(OP_RET, 3, 0, 0));
    FUNC(0, 4,
         LVM_ASBX(OP_LDI, 0, 0),
         LVM_ASBX(OP_LDI, 1, 2000),
         LVM_ASBX(OP_LDI, 2, 1),
         LVM_ABC(OP_NEWMAP, 3, 4, 0),              /* 3 */
         LVM_ABC(OP_ADD, 0, 0, 2),
         LVM_ABC(OP_LT, 1, 0, 1),
         LVM_ASBX(OP_JMP, 0, -4),
         LVM_ABC(OP_RET, 2, 0, 0));
    g_gc_min = 512;
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 8);
    g_gc_min = 0;
}

static void test_data_format(void)
{
    begin(1, 1, 0);
    u8(4);
    u32(100); /* longer than the rest of the file */
    FUNC(0, 1, LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(load_only() == LVM_ERR_FORMAT);

    begin(1, 1, 0);
    u8(9); /* unknown constant tag */
    FUNC(0, 1, LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(load_only() == LVM_ERR_FORMAT);

    begin(0, 1, 0);
    FUNC(0, 2, LVM_ABC(OP_GET, 5, 0, 1), LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(load_only() == LVM_ERR_VERIFY);

    begin(0, 1, 0);
    FUNC(0, 2, LVM_ABC(OP_NEWARR, 9, 0, 0), LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(load_only() == LVM_ERR_VERIFY);
}

/* ---- closures ---- */

static void test_closure_counter(void)
{
    lvm_value r;

    /* f() increments a captured local; the local is shared while open */
    begin(0, 2, 0);
    FUNC(0, 5,
         LVM_ASBX(OP_LDI, 0, 0),
         LVM_ABX(OP_CLOSURE, 1, 1),
         LVM_ABC(OP_MOV, 2, 1, 0),
         LVM_ABC(OP_CALLC, 2, 0, 0),
         LVM_ABC(OP_MOV, 3, 1, 0),
         LVM_ABC(OP_CALLC, 3, 0, 0),
         LVM_ABC(OP_ADD, 4, 2, 3),
         LVM_ABC(OP_ADD, 4, 4, 0),
         LVM_ABC(OP_RET, 4, 0, 0));
    UP(1, 0);
    FUNC(0, 2,
         LVM_ABC(OP_GETUP, 0, 0, 0),
         LVM_ASBX(OP_LDI, 1, 1),
         LVM_ABC(OP_ADD, 0, 0, 1),
         LVM_ABC(OP_SETUP, 0, 0, 0),
         LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 1 + 2 + 2);
}

static void test_closure_outlives_frame(void)
{
    lvm_value r;

    /* make_adder(n) returns a closure over n; n is closed when make_adder returns */
    begin(0, 3, 0);
    FUNC(0, 3,
         LVM_ASBX(OP_LDI, 0, 100),
         LVM_ABX(OP_CALL, 0, 1),
         LVM_ASBX(OP_LDI, 1, 5),
         LVM_ABC(OP_CALLC, 0, 1, 0),
         LVM_ABC(OP_RET, 0, 0, 0));
    FUNC(1, 2,
         LVM_ABX(OP_CLOSURE, 1, 2),
         LVM_ABC(OP_RET, 1, 0, 0));
    UP(1, 0);
    FUNC(1, 3,
         LVM_ABC(OP_GETUP, 1, 0, 0),
         LVM_ABC(OP_ADD, 0, 0, 1),
         LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 105);
}

static void test_closure_shared_upvalue(void)
{
    lvm_value r;

    /* two closures over the same variable see each other's writes */
    begin(0, 3, 0);
    FUNC(0, 5,
         LVM_ASBX(OP_LDI, 0, 0),
         LVM_ABX(OP_CLOSURE, 1, 1),
         LVM_ABX(OP_CLOSURE, 2, 2),
         LVM_ABC(OP_MOV, 3, 1, 0), LVM_ABC(OP_CALLC, 3, 0, 0),
         LVM_ABC(OP_MOV, 3, 1, 0), LVM_ABC(OP_CALLC, 3, 0, 0),
         LVM_ABC(OP_MOV, 3, 1, 0), LVM_ABC(OP_CALLC, 3, 0, 0),
         LVM_ABC(OP_MOV, 4, 2, 0), LVM_ABC(OP_CALLC, 4, 0, 0),
         LVM_ABC(OP_RET, 4, 0, 0));
    UP(1, 0);
    FUNC(0, 2,
         LVM_ABC(OP_GETUP, 0, 0, 0), LVM_ASBX(OP_LDI, 1, 1),
         LVM_ABC(OP_ADD, 0, 0, 1), LVM_ABC(OP_SETUP, 0, 0, 0),
         LVM_ABC(OP_RET, 0, 0, 0));
    UP(1, 0);
    FUNC(0, 1, LVM_ABC(OP_GETUP, 0, 0, 0), LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 3);
}

static void test_closure_nested(void)
{
    lvm_value r;

    /* inner captures x through mid's upvalue (instack = 0) */
    begin(0, 3, 0);
    FUNC(0, 3,
         LVM_ASBX(OP_LDI, 0, 7),
         LVM_ABX(OP_CLOSURE, 1, 1),
         LVM_ABC(OP_CALLC, 1, 0, 0),
         LVM_ABC(OP_CALLC, 1, 0, 0),
         LVM_ABC(OP_RET, 1, 0, 0));
    UP(1, 0);
    FUNC(0, 2, LVM_ABX(OP_CLOSURE, 0, 2), LVM_ABC(OP_RET, 0, 0, 0));
    UP(0, 0);
    FUNC(0, 2,
         LVM_ABC(OP_GETUP, 0, 0, 0), LVM_ASBX(OP_LDI, 1, 1),
         LVM_ABC(OP_ADD, 0, 0, 1), LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 8);
}

static void test_closure_close_per_iteration(void)
{
    lvm_value r;

    /* CLOSE gives each iteration its own variable: 0 + 1 + 2 (not 2 + 2 + 2) */
    begin(0, 2, 0);
    FUNC(0, 8,
         LVM_ABC(OP_NEWARR, 0, 0, 0),
         LVM_ASBX(OP_LDI, 1, 0),
         LVM_ASBX(OP_LDI, 2, 3),
         LVM_ASBX(OP_LDI, 3, 1),
         LVM_ABC(OP_MOV, 4, 1, 0),                 /*  4 */
         LVM_ABX(OP_CLOSURE, 5, 1),
         LVM_ABC(OP_CLOSE, 4, 0, 0),
         LVM_ABC(OP_SET, 0, 1, 5),
         LVM_ABC(OP_ADD, 1, 1, 3),
         LVM_ABC(OP_LT, 1, 1, 2),
         LVM_ASBX(OP_JMP, 0, -7),
         LVM_ASBX(OP_LDI, 1, 0),
         LVM_ASBX(OP_LDI, 7, 0),
         LVM_ABC(OP_GET, 5, 0, 1),                 /* 13 */
         LVM_ABC(OP_CALLC, 5, 0, 0),
         LVM_ABC(OP_ADD, 7, 7, 5),
         LVM_ABC(OP_ADD, 1, 1, 3),
         LVM_ABC(OP_LT, 1, 1, 2),
         LVM_ASBX(OP_JMP, 0, -6),
         LVM_ABC(OP_RET, 7, 0, 0));
    UP(1, 4);
    FUNC(0, 1, LVM_ABC(OP_GETUP, 0, 0, 0), LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 3);
}

static void test_closure_errors(void)
{
    lvm_value r;

    begin(0, 1, 0);
    FUNC(0, 2, LVM_ASBX(OP_LDI, 0, 1), LVM_ABC(OP_CALLC, 0, 0, 0), LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(run(&r) == LVM_ERR_TYPE); /* not a closure */

    begin(0, 2, 0);
    FUNC(0, 2, LVM_ABX(OP_CLOSURE, 0, 1), LVM_ABC(OP_CALLC, 0, 0, 0), LVM_ABC(OP_RET, 0, 0, 0));
    FUNC(1, 2, LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(run(&r) == LVM_ERR_ARGS); /* wrong argument count */
}

static void test_closure_gc(void)
{
    lvm_value r;

    /* the array is only reachable through a closed upvalue while garbage piles up */
    begin(0, 3, 0);
    FUNC(0, 7,
         LVM_ABC(OP_NEWARR, 0, 0, 0),
         LVM_ASBX(OP_LDI, 1, 9),
         LVM_ASBX(OP_LDI, 2, 0),
         LVM_ABC(OP_SET, 0, 2, 1),
         LVM_ABX(OP_CALL, 0, 1),
         LVM_ASBX(OP_LDI, 3, 0),
         LVM_ASBX(OP_LDI, 4, 3000),
         LVM_ASBX(OP_LDI, 5, 1),
         LVM_ABC(OP_NEWMAP, 6, 4, 0),              /*  8 */
         LVM_ABC(OP_ADD, 3, 3, 5),
         LVM_ABC(OP_LT, 1, 3, 4),
         LVM_ASBX(OP_JMP, 0, -4),
         LVM_ABC(OP_CALLC, 0, 0, 0),
         LVM_ABC(OP_RET, 0, 0, 0));
    FUNC(1, 2, LVM_ABX(OP_CLOSURE, 1, 2), LVM_ABC(OP_RET, 1, 0, 0));
    UP(1, 0);
    FUNC(0, 2,
         LVM_ABC(OP_GETUP, 0, 0, 0),
         LVM_ASBX(OP_LDI, 1, 0),
         LVM_ABC(OP_GET, 0, 0, 1),
         LVM_ABC(OP_RET, 0, 0, 0));
    g_gc_min = 512;
    CHECK(run(&r) == LVM_OK && lvm_as_int(r) == 9);
    g_gc_min = 0;
}

static void test_closure_verification(void)
{
    begin(0, 1, 0);
    FUNC(0, 2, LVM_ABC(OP_GETUP, 0, 0, 0), LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(load_only() == LVM_ERR_VERIFY); /* no such upvalue */

    begin(0, 1, 0);
    UP(1, 0);
    FUNC(0, 1, LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(load_only() == LVM_ERR_VERIFY); /* entry cannot have upvalues */

    begin(0, 2, 0);
    FUNC(0, 2, LVM_ABX(OP_CLOSURE, 0, 1), LVM_ABC(OP_RET, 0, 0, 0));
    UP(1, 5);
    FUNC(0, 1, LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(load_only() == LVM_ERR_VERIFY); /* captures a register that does not exist */

    begin(0, 2, 0);
    FUNC(0, 2, LVM_ABX(OP_CALL, 0, 1), LVM_ABC(OP_RET, 0, 0, 0));
    UP(1, 0);
    FUNC(0, 1, LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(load_only() == LVM_ERR_VERIFY); /* static call of a closure-only function */

    begin(0, 1, 0);
    FUNC(0, 2, LVM_ABC(OP_CALLC, 1, 5, 0), LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(load_only() == LVM_ERR_VERIFY); /* argument window outside the frame */

    begin(0, 1, 0);
    UP(2, 0);
    FUNC(0, 1, LVM_ABC(OP_RET, 0, 0, 0));
    CHECK(load_only() == LVM_ERR_FORMAT); /* bad capture kind */
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

    test_strings();

    test_arrays();
    test_maps();

    test_gc();
    test_data_format();

    test_closure_counter();
    test_closure_outlives_frame();
    test_closure_shared_upvalue();
    test_closure_nested();
    test_closure_close_per_iteration();
    test_closure_errors();
    test_closure_gc();
    test_closure_verification();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
