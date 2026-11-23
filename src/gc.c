#include "lvm.h"
#include "internal.h"

/*
 * Precise, non-incremental mark and sweep.
 *
 * Roots are the register stack up to the highest live frame. Every slot
 * inside a live frame holds a valid value: frames are nil-initialised on
 * entry, and parameters are live slots of the caller.
 *
 * The gray stack is sized to the object count up front, so marking cannot
 * fail halfway through.
 */

static void mark_value(lvm_vm *vm, lvm_value v)
{
    lvm_obj *o;

    if (!lvm_is_ptr(v)) return;
    o = lvm_to_obj(v);
    if (o->perm || o->mark) return;
    o->mark = 1;
    if (o->type != LVM_OBJ_STRING) vm->gray[vm->ngray++] = o;
}

static void trace(lvm_vm *vm, lvm_obj *o)
{
    uint32_t i;

    if (o->type == LVM_OBJ_ARRAY) {
        const lvm_array *a = (const lvm_array *)o;
        for (i = 0; i < a->len; i++) mark_value(vm, a->items[i]);
    } else {
        const lvm_map *m = (const lvm_map *)o;
        for (i = 0; i < m->cap; i++) {
            if (m->tab[i].key != LVM_EMPTY_KEY) {
                mark_value(vm, m->tab[i].key);
                mark_value(vm, m->tab[i].val);
            }
        }
    }
}

lvm_status lvm_gc_collect(lvm_vm *vm)
{
    lvm_obj *o, **pp;
    size_t top = 0, i;
    uint32_t d;

    if (vm->capgray < vm->nobjs) {
        size_t cap = vm->nobjs + vm->nobjs / 2 + 16;
        lvm_obj **g = LVM_MEM(&vm->alloc, vm->gray, vm->capgray * sizeof *g, cap * sizeof *g);
        if (!g) return LVM_ERR_MEM;
        vm->gray = g;
        vm->capgray = cap;
    }
    vm->ngray = 0;

    if (vm->cur_fn) {
        top = (size_t)vm->cur_base + vm->cur_fn->nregs;
        for (d = 0; d < vm->depth; d++) {
            size_t t = (size_t)vm->frames[d].base + vm->frames[d].fn->nregs;
            if (t > top) top = t;
        }
    }
    for (i = 0; i < top; i++) mark_value(vm, vm->stack[i]);
    while (vm->ngray) trace(vm, vm->gray[--vm->ngray]);

    pp = &vm->objs;
    while ((o = *pp) != NULL) {
        if (o->mark) {
            o->mark = 0;
            pp = &o->next;
        } else {
            *pp = o->next;
            vm->bytes -= lvm_obj_bytes(o);
            vm->nobjs--;
            lvm_obj_free(vm, o);
        }
    }

    vm->threshold = vm->bytes * 2;
    if (vm->threshold < vm->gc_min) vm->threshold = vm->gc_min;
    return LVM_OK;
}

void lvm_gc_free(lvm_vm *vm)
{
    if (vm->gray)
        LVM_MEM(&vm->alloc, vm->gray, vm->capgray * sizeof *vm->gray, 0);
    vm->gray = NULL;
    vm->capgray = 0;
}

lvm_status lvm_gc(lvm_vm *vm)
{
    return lvm_gc_collect(vm);
}

void lvm_set_gc_threshold(lvm_vm *vm, size_t min_bytes)
{
    vm->gc_min = min_bytes;
    vm->threshold = min_bytes;
}
