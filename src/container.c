#include "lvm.h"
#include "internal.h"

#include <string.h>

#define MAX_CAP (1u << 28)
#define MAP_MIN 8u

/* ---- arrays ---- */

lvm_value lvm_obj_array_new(lvm_vm *vm, uint32_t cap)
{
    lvm_array *a = (lvm_array *)lvm_obj_new(vm, LVM_OBJ_ARRAY, sizeof *a);

    if (!a) return lvm_nil();
    a->len = 0;
    a->cap = 0;
    a->items = NULL;
    if (cap) {
        a->items = LVM_MEM(&vm->alloc, NULL, 0, (size_t)cap * sizeof(lvm_value));
        if (!a->items) return lvm_nil(); /* a is on the gc list and consistent */
        a->cap = cap;
        vm->bytes += (size_t)cap * sizeof(lvm_value);
    }
    return lvm_from_obj(&a->o);
}

static int array_reserve(lvm_vm *vm, lvm_array *a, uint32_t need)
{
    uint32_t cap;
    lvm_value *p;

    if (need <= a->cap) return 1;
    if (need > MAX_CAP) return 0;
    cap = a->cap ? a->cap : 4;
    while (cap < need) cap *= 2;
    p = LVM_MEM(&vm->alloc, a->items, (size_t)a->cap * sizeof(lvm_value),
                (size_t)cap * sizeof(lvm_value));
    if (!p) return 0;
    vm->bytes += (size_t)(cap - a->cap) * sizeof(lvm_value);
    a->items = p;
    a->cap = cap;
    return 1;
}
