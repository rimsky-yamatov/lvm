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

/* dispatch: one switch per instruction */
#define LVM_DISPATCH_BEGIN for (;;) { i = *pc++; switch (LVM_OP(i)) {
#define LVM_DISPATCH_END default: st = LVM_ERR_VERIFY; goto fail; } }
#define CASE(x) case x:
#define NEXT() break

lvm_status lvm_run(lvm_vm *vm, const lvm_module *m,
                   const lvm_value *args, int nargs, lvm_value *result)
{
    const lvm_func *fn = &m->funcs[m->entry];
    const uint32_t *pc = fn->code;
    lvm_value *R = vm->stack;
    uint32_t base = 0, depth = 0, i = 0, k;
    lvm_status st = LVM_OK;

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
    LVM_DISPATCH_END

fail:
    return st;
}
