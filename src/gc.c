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

static void mark_obj(lvm_vm *vm, lvm_obj *o)
{
    if (o->perm || o->mark) return;
    o->mark = 1;
    if (o->type != LVM_OBJ_STRING) vm->gray[vm->ngray++] = o;
}

static void mark_value(lvm_vm *vm, lvm_value v)
{
    if (lvm_is_ptr(v)) mark_obj(vm, lvm_to_obj(v));
}

static void trace(lvm_vm *vm, lvm_obj *o)
{
    uint32_t i;

    switch (o->type) {
    case LVM_OBJ_ARRAY: {
        const lvm_array *a = (const lvm_array *)o;
        for (i = 0; i < a->len; i++) mark_value(vm, a->items[i]);
        break;
    }
    case LVM_OBJ_MAP: {
        const lvm_map *m = (const lvm_map *)o;
        for (i = 0; i < m->cap; i++) {
            if (m->tab[i].key != LVM_EMPTY_KEY) {
                mark_value(vm, m->tab[i].key);
                mark_value(vm, m->tab[i].val);
            }
        }
        break;
    }
    case LVM_OBJ_CLOSURE: {
        const lvm_closure *c = (const lvm_closure *)o;
        for (i = 0; i < c->nup; i++) {
            if (c->up[i]) mark_obj(vm, &c->up[i]->o);
        }
        break;
    }
    case LVM_OBJ_UPVAL:
        mark_value(vm, *((const lvm_upval *)o)->v);
        break;
    }
}

lvm_status lvm_gc_collect(lvm_vm *vm)
{
    lvm_obj *o, **pp;
    lvm_upval **up;
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
        if (vm->cur_cl) mark_obj(vm, &vm->cur_cl->o);
        for (d = 0; d < vm->depth; d++) {
            size_t t = (size_t)vm->frames[d].base + vm->frames[d].fn->nregs;
            if (t > top) top = t;
            if (vm->frames[d].cl) mark_obj(vm, &vm->frames[d].cl->o);
        }
    }
    for (i = 0; i < top; i++) mark_value(vm, vm->stack[i]);
    while (vm->ngray) trace(vm, vm->gray[--vm->ngray]);

    /* dead upvalues must leave the open list before they are freed */
    for (up = &vm->open; *up;) {
        if ((*up)->o.mark) up = &(*up)->next;
        else *up = (*up)->next;
    }

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
