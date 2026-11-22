#include "lvm.h"
#include "lvm_opcode.h"
#include "internal.h"

#include <math.h>
#include <string.h>

#define LVM_MAX_STACK (1u << 28)

typedef struct {
    const lvm_func *fn;
    const uint32_t *pc;
    uint32_t base;
} frame;

typedef struct {
    lvm_host_fn fn;
    void *ud;
} host_slot;

struct lvm_vm {
    lvm_allocator alloc;
    lvm_value *stack;
    uint32_t stack_slots;
    frame *frames;
    uint32_t max_frames;
    host_slot host[LVM_HOST_MAX];
};

lvm_vm *lvm_new(const lvm_allocator *a, uint32_t stack_slots, uint32_t max_frames)
{
    lvm_vm *vm;
    lvm_value *stack;
    frame *frames;

    if (!a) a = lvm_default_allocator();
    if (stack_slots == 0 || stack_slots > LVM_MAX_STACK ||
        max_frames == 0 || max_frames > LVM_MAX_STACK)
        return NULL;

    vm = LVM_MEM(a, NULL, 0, sizeof *vm);
    stack = LVM_MEM(a, NULL, 0, (size_t)stack_slots * sizeof *stack);
    frames = LVM_MEM(a, NULL, 0, (size_t)max_frames * sizeof *frames);
    if (!vm || !stack || !frames) {
        if (vm) LVM_MEM(a, vm, sizeof *vm, 0);
        if (stack) LVM_MEM(a, stack, (size_t)stack_slots * sizeof *stack, 0);
        if (frames) LVM_MEM(a, frames, (size_t)max_frames * sizeof *frames, 0);
        return NULL;
    }

    memset(vm, 0, sizeof *vm);
    vm->alloc = *a;
    vm->stack = stack;
    vm->stack_slots = stack_slots;
    vm->frames = frames;
    vm->max_frames = max_frames;
    return vm;
}

void lvm_free(lvm_vm *vm)
{
    lvm_allocator a;

    if (!vm) return;
    a = vm->alloc;
    LVM_MEM(&a, vm->stack, (size_t)vm->stack_slots * sizeof *vm->stack, 0);
    LVM_MEM(&a, vm->frames, (size_t)vm->max_frames * sizeof *vm->frames, 0);
    LVM_MEM(&a, vm, sizeof *vm, 0);
}

void lvm_set_host(lvm_vm *vm, unsigned id, lvm_host_fn fn, void *ud)
{
    if (id >= LVM_HOST_MAX) return;
    vm->host[id].fn = fn;
    vm->host[id].ud = ud;
}

/*
 * Dispatch: threaded code via computed goto where the compiler has it,
 * a plain switch everywhere else (define LVM_NO_COMPUTED_GOTO to force it).
 */
#if defined(__GNUC__) && !defined(LVM_NO_COMPUTED_GOTO)
#  define LVM_CGOTO 1
#  define NEXT() do { i = *pc++; goto *dispatch[LVM_OP(i)]; } while (0)
#  define CASE(x) L_##x:
#  define LVM_DISPATCH_BEGIN NEXT();
#  define LVM_DISPATCH_END
#else
#  define LVM_CGOTO 0
#  define LVM_DISPATCH_BEGIN for (;;) { i = *pc++; switch (LVM_OP(i)) {
#  define LVM_DISPATCH_END default: st = LVM_ERR_VERIFY; goto fail; } }
#  define CASE(x) case x:
#  define NEXT() break
#endif

static int num(lvm_value v, double *d)
{
    if (lvm_is_float(v)) { *d = lvm_as_float(v); return 1; }
    if (lvm_is_int(v))   { *d = (double)lvm_as_int(v); return 1; }
    return 0;
}

/*
 * Binary arithmetic. int op int stays int (promoted to float when the
 * result no longer fits 48 bits); anything else numeric becomes float.
 */
#define ARITH(INT_PRE, INT_EXPR, FLT_EXPR) do {                              \
    lvm_value x_ = R[LVM_B(i)], y_ = R[LVM_C(i)];                            \
    if (lvm_is_int(x_) && lvm_is_int(y_)) {                                  \
        int64_t a_ = lvm_as_int(x_), b_ = lvm_as_int(y_), r_;                \
        INT_PRE;                                                             \
        r_ = (INT_EXPR);                                                     \
        R[LVM_A(i)] = lvm_fits_int(r_) ? lvm_int(r_) : lvm_float((double)r_);\
    } else {                                                                 \
        double a_, b_;                                                       \
        if (!num(x_, &a_) || !num(y_, &b_)) { st = LVM_ERR_TYPE; goto fail; }\
        R[LVM_A(i)] = lvm_float(FLT_EXPR);                                   \
    }                                                                        \
} while (0)

/* arithmetic shift right without relying on implementation-defined behaviour */
static int64_t asr(int64_t a, unsigned n)
{
    return a < 0 ? (int64_t)~(~(uint64_t)a >> n) : (int64_t)((uint64_t)a >> n);
}

/* Bitwise ops work on ints only; results wrap to 48 bits. */
#define BITOP(EXPR) do {                                                     \
    lvm_value x_ = R[LVM_B(i)], y_ = R[LVM_C(i)];                            \
    int64_t a_, b_;                                                          \
    if (!lvm_is_int(x_) || !lvm_is_int(y_)) { st = LVM_ERR_TYPE; goto fail; }\
    a_ = lvm_as_int(x_);                                                     \
    b_ = lvm_as_int(y_);                                                     \
    R[LVM_A(i)] = lvm_int(EXPR);                                             \
} while (0)

static int veq(lvm_value x, lvm_value y)
{
    double a, b;
    if (lvm_is_int(x) && lvm_is_int(y)) return x == y;
    if (num(x, &a) && num(y, &b)) return a == b;
    return x == y;
}

/* Comparison skips the following JMP unless the result equals A. */
#define CMP(OP) do {                                                         \
    lvm_value x_ = R[LVM_B(i)], y_ = R[LVM_C(i)];                            \
    int c_;                                                                  \
    if (lvm_is_int(x_) && lvm_is_int(y_)) {                                  \
        c_ = lvm_as_int(x_) OP lvm_as_int(y_);                               \
    } else {                                                                 \
        double a_, b_;                                                       \
        if (!num(x_, &a_) || !num(y_, &b_)) { st = LVM_ERR_TYPE; goto fail; }\
        c_ = a_ OP b_;                                                       \
    }                                                                        \
    if (c_ != (int)LVM_A(i)) pc++;                                           \
} while (0)

lvm_status lvm_run(lvm_vm *vm, const lvm_module *m,
                   const lvm_value *args, int nargs, lvm_value *result)
{
    const lvm_func *fn = &m->funcs[m->entry];
    const uint32_t *pc = fn->code;
    lvm_value *R = vm->stack;
    uint32_t base = 0, depth = 0, i = 0, k;
    lvm_status st = LVM_OK;
#if LVM_CGOTO
    static const void *const dispatch[OP_COUNT] = {
        [OP_MOV] = &&L_OP_MOV,
        [OP_LDK] = &&L_OP_LDK,
        [OP_LDI] = &&L_OP_LDI,
        [OP_LDNIL] = &&L_OP_LDNIL,
        [OP_LDBOOL] = &&L_OP_LDBOOL,
        [OP_ADD] = &&L_OP_ADD,
        [OP_SUB] = &&L_OP_SUB,
        [OP_MUL] = &&L_OP_MUL,
        [OP_DIV] = &&L_OP_DIV,
        [OP_MOD] = &&L_OP_MOD,
        [OP_NEG] = &&L_OP_NEG,
        [OP_AND] = &&L_OP_AND,
        [OP_OR] = &&L_OP_OR,
        [OP_XOR] = &&L_OP_XOR,
        [OP_NOT] = &&L_OP_NOT,
        [OP_SHL] = &&L_OP_SHL,
        [OP_SHR] = &&L_OP_SHR,
        [OP_EQ] = &&L_OP_EQ,
        [OP_LT] = &&L_OP_LT,
        [OP_LE] = &&L_OP_LE,
        [OP_JMP] = &&L_OP_JMP,
        [OP_JT] = &&L_OP_JT,
        [OP_JF] = &&L_OP_JF,
        [OP_CALL] = &&L_OP_CALL,
        [OP_RET] = &&L_OP_RET,
        [OP_HOST] = &&L_OP_HOST
    };
#endif

    if (nargs < 0 || (unsigned)nargs != fn->nparams) return LVM_ERR_ARGS;
    if (fn->nregs > vm->stack_slots) return LVM_ERR_STACK;
    for (k = 0; k < fn->nregs; k++)
        R[k] = (k < (uint32_t)nargs) ? args[k] : lvm_nil();

    LVM_DISPATCH_BEGIN
    CASE(OP_RET) {
        lvm_value v = R[LVM_A(i)];
        if (depth == 0) {
            if (result) *result = v;
            return LVM_OK;
        }
        R[0] = v; /* callee R[0] aliases the caller's call register */
        depth--;
        fn = vm->frames[depth].fn;
        pc = vm->frames[depth].pc;
        base = vm->frames[depth].base;
        R = vm->stack + base;
        NEXT();
    }
    CASE(OP_MOV)
        R[LVM_A(i)] = R[LVM_B(i)];
        NEXT();
    CASE(OP_LDK)
        R[LVM_A(i)] = m->consts[LVM_BX(i)];
        NEXT();
    CASE(OP_LDI)
        R[LVM_A(i)] = lvm_int(LVM_SBX(i));
        NEXT();
    CASE(OP_LDNIL)
        for (k = 0; k <= LVM_B(i); k++) R[LVM_A(i) + k] = lvm_nil();
        NEXT();
    CASE(OP_LDBOOL)
        R[LVM_A(i)] = lvm_bool((int)LVM_B(i));
        NEXT();
    CASE(OP_ADD)
        ARITH((void)0, a_ + b_, a_ + b_);
        NEXT();
    CASE(OP_SUB)
        ARITH((void)0, a_ - b_, a_ - b_);
        NEXT();
    CASE(OP_MUL) {
        lvm_value x = R[LVM_B(i)], y = R[LVM_C(i)];
        if (lvm_is_int(x) && lvm_is_int(y)) {
            int64_t p = lvm_as_int(x), q = lvm_as_int(y);
            double d = (double)p * (double)q;
            /* |p*q| < 1e14 < 2^47: exact in int64 and fits 48 bits */
            R[LVM_A(i)] = (d > -1.0e14 && d < 1.0e14) ? lvm_int(p * q) : lvm_float(d);
        } else {
            double p, q;
            if (!num(x, &p) || !num(y, &q)) { st = LVM_ERR_TYPE; goto fail; }
            R[LVM_A(i)] = lvm_float(p * q);
        }
        NEXT();
    }
    CASE(OP_DIV)
        ARITH(if (b_ == 0) { st = LVM_ERR_DIVZERO; goto fail; }, a_ / b_, a_ / b_);
        NEXT();
    CASE(OP_MOD)
        ARITH(if (b_ == 0) { st = LVM_ERR_DIVZERO; goto fail; }, a_ % b_, fmod(a_, b_));
        NEXT();
    CASE(OP_NEG) {
        lvm_value x = R[LVM_B(i)];
        if (lvm_is_int(x)) {
            int64_t r = -lvm_as_int(x);
            R[LVM_A(i)] = lvm_fits_int(r) ? lvm_int(r) : lvm_float((double)r);
        } else if (lvm_is_float(x)) {
            R[LVM_A(i)] = lvm_float(-lvm_as_float(x));
        } else {
            st = LVM_ERR_TYPE;
            goto fail;
        }
        NEXT();
    }
    CASE(OP_AND)
        BITOP(a_ & b_);
        NEXT();
    CASE(OP_OR)
        BITOP(a_ | b_);
        NEXT();
    CASE(OP_XOR)
        BITOP(a_ ^ b_);
        NEXT();
    CASE(OP_NOT) {
        lvm_value x = R[LVM_B(i)];
        if (!lvm_is_int(x)) { st = LVM_ERR_TYPE; goto fail; }
        R[LVM_A(i)] = lvm_int(~lvm_as_int(x));
        NEXT();
    }
    CASE(OP_SHL)
        BITOP((int64_t)((uint64_t)a_ << (b_ & 63)));
        NEXT();
    CASE(OP_SHR)
        BITOP(asr(a_, (unsigned)(b_ & 63)));
        NEXT();
    CASE(OP_EQ)
        if (veq(R[LVM_B(i)], R[LVM_C(i)]) != (int)LVM_A(i)) pc++;
        NEXT();
    CASE(OP_LT)
        CMP(<);
        NEXT();
    CASE(OP_LE)
        CMP(<=);
        NEXT();
    CASE(OP_JMP)
        pc += LVM_SBX(i);
        NEXT();
    CASE(OP_JT)
        if (lvm_is_truthy(R[LVM_A(i)])) pc += LVM_SBX(i);
        NEXT();
    CASE(OP_JF)
        if (!lvm_is_truthy(R[LVM_A(i)])) pc += LVM_SBX(i);
        NEXT();
    CASE(OP_CALL) {
        /* args are already in R[A..]; the callee's window starts there */
        const lvm_func *callee = &m->funcs[LVM_BX(i)];
        uint32_t nbase = base + LVM_A(i);

        if (depth >= vm->max_frames || nbase > vm->stack_slots ||
            callee->nregs > vm->stack_slots - nbase) {
            st = LVM_ERR_STACK;
            goto fail;
        }
        vm->frames[depth].fn = fn;
        vm->frames[depth].pc = pc;
        vm->frames[depth].base = base;
        depth++;

        fn = callee;
        pc = callee->code;
        base = nbase;
        R = vm->stack + base;
        for (k = callee->nparams; k < callee->nregs; k++) R[k] = lvm_nil();
        NEXT();
    }
    CASE(OP_HOST) {
        const host_slot *h = &vm->host[LVM_C(i)];
        if (!h->fn) { st = LVM_ERR_HOST; goto fail; }
        st = h->fn(h->ud, &R[LVM_A(i)], (int)LVM_B(i));
        if (st != LVM_OK) goto fail;
        NEXT();
    }
    LVM_DISPATCH_END

fail:
    return st;
}
