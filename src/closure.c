#include "lvm.h"
#include "internal.h"

lvm_closure *lvm_closure_new(lvm_vm *vm, const lvm_func *fn)
{
    uint32_t i;
    lvm_closure *c = (lvm_closure *)lvm_obj_new(vm, LVM_OBJ_CLOSURE,
                                                LVM_CLOSURE_SIZE(fn->nupvals));

    if (!c) return NULL;
    c->fn = fn;
    c->nup = fn->nupvals;
    for (i = 0; i < c->nup; i++) c->up[i] = NULL; /* filled in by the caller */
    return c;
}

/* Find or create the open upvalue for a stack slot, so that closures that
 * capture the same variable share it. */
lvm_upval *lvm_upval_capture(lvm_vm *vm, lvm_value *slot)
{
    lvm_upval *uv, **pp;

    for (uv = vm->open; uv && uv->v >= slot; uv = uv->next) {
        if (uv->v == slot) return uv;
    }

    /* the allocation may collect, which edits the open list: search again after */
    uv = (lvm_upval *)lvm_obj_new(vm, LVM_OBJ_UPVAL, sizeof *uv);
    if (!uv) return NULL;
    uv->closed = lvm_nil();
    uv->v = slot;
    pp = &vm->open;
    while (*pp && (*pp)->v > slot) pp = &(*pp)->next;
    uv->next = *pp;
    *pp = uv;
    return uv;
}

/* Move every open upvalue at or above `level` off the stack into the
 * upvalue itself. */
void lvm_upvals_close(lvm_vm *vm, const lvm_value *level)
{
    lvm_upval *uv;

    while ((uv = vm->open) != NULL && uv->v >= level) {
        uv->closed = *uv->v;
        uv->v = &uv->closed;
        vm->open = uv->next;
        uv->next = NULL;
    }
}
