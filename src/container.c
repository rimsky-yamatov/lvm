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

/* ---- maps ---- */

static uint32_t mix(uint64_t x)
{
    x ^= x >> 33;
    x *= UINT64_C(0xFF51AFD7ED558CCD);
    x ^= x >> 33;
    return (uint32_t)x;
}

static uint32_t key_hash(lvm_value k)
{
    if (lvm_is_obj(k, LVM_OBJ_STRING))
        return ((const lvm_string *)lvm_to_obj(k))->hash;
    return mix(k);
}

/*
 * Keys: floats with an integral value are stored as ints (so 1 and 1.0
 * are the same key); nil and NaN are not valid keys. Returns 0 if invalid.
 * +0.0 never occurs as a stored key (it becomes int 0), so 0 can mark an
 * empty slot.
 */
static int norm_key(lvm_value *k)
{
    lvm_value v = *k;

    if (lvm_is_nil(v)) return 0;
    if (lvm_is_float(v)) {
        double d = lvm_as_float(v);
        if (d != d) return 0;
        if (d >= -140737488355328.0 && d < 140737488355328.0) {
            int64_t i = (int64_t)d;
            if ((double)i == d) *k = lvm_int(i);
        }
    }
    return 1;
}

static lvm_entry *map_find(const lvm_map *m, lvm_value k, uint32_t h)
{
    uint32_t mask, i;

    if (m->cap == 0) return NULL;
    mask = m->cap - 1;
    for (i = h & mask;; i = (i + 1) & mask) {
        lvm_entry *e = &m->tab[i];
        if (e->key == LVM_EMPTY_KEY) return NULL;
        if (lvm_obj_equal(e->key, k)) return e;
    }
}

static int map_resize(lvm_vm *vm, lvm_map *m, uint32_t ncap)
{
    lvm_entry *nt;
    uint32_t i, mask = ncap - 1;

    nt = LVM_MEM(&vm->alloc, NULL, 0, (size_t)ncap * sizeof *nt);
    if (!nt) return 0;
    memset(nt, 0, (size_t)ncap * sizeof *nt);

    for (i = 0; i < m->cap; i++) {
        uint32_t j;
        if (m->tab[i].key == LVM_EMPTY_KEY) continue;
        j = key_hash(m->tab[i].key) & mask;
        while (nt[j].key != LVM_EMPTY_KEY) j = (j + 1) & mask;
        nt[j] = m->tab[i];
    }
    if (m->tab) LVM_MEM(&vm->alloc, m->tab, (size_t)m->cap * sizeof *nt, 0);
    vm->bytes += (size_t)(ncap - m->cap) * sizeof *nt;
    m->tab = nt;
    m->cap = ncap;
    return 1;
}

/* backward-shift deletion keeps probe sequences intact without tombstones */
static void map_remove(lvm_map *m, lvm_entry *e)
{
    uint32_t mask = m->cap - 1;
    uint32_t i = (uint32_t)(e - m->tab), j, k;

    for (;;) {
        j = i;
        for (;;) {
            j = (j + 1) & mask;
            if (m->tab[j].key == LVM_EMPTY_KEY) {
                m->tab[i].key = LVM_EMPTY_KEY;
                m->tab[i].val = 0;
                m->count--;
                return;
            }
            k = key_hash(m->tab[j].key) & mask;
            /* entry j may stay if its home slot k lies cyclically in (i, j] */
            if (i <= j ? (i < k && k <= j) : (i < k || k <= j)) continue;
            break;
        }
        m->tab[i] = m->tab[j];
        i = j;
    }
}

lvm_value lvm_obj_map_new(lvm_vm *vm, uint32_t hint)
{
    lvm_map *m = (lvm_map *)lvm_obj_new(vm, LVM_OBJ_MAP, sizeof *m);

    if (!m) return lvm_nil();
    m->count = 0;
    m->cap = 0;
    m->tab = NULL;
    if (hint) {
        uint32_t cap = MAP_MIN;
        while ((uint64_t)cap * 3 < (uint64_t)hint * 4) cap *= 2;
        if (!map_resize(vm, m, cap)) return lvm_nil();
    }
    return lvm_from_obj(&m->o);
}

static lvm_status map_set(lvm_vm *vm, lvm_map *m, lvm_value k, lvm_value v)
{
    uint32_t h, mask, i;
    lvm_entry *e;

    if (!norm_key(&k)) return LVM_ERR_TYPE;
    h = key_hash(k);
    e = map_find(m, k, h);

    if (lvm_is_nil(v)) { /* assigning nil removes the key */
        if (e) map_remove(m, e);
        return LVM_OK;
    }
    if (e) {
        e->val = v;
        return LVM_OK;
    }
    if (((size_t)m->count + 1) * 4 > (size_t)m->cap * 3) {
        uint32_t ncap = m->cap ? m->cap * 2 : MAP_MIN;
        if (ncap > MAX_CAP || !map_resize(vm, m, ncap)) return LVM_ERR_MEM;
    }
    mask = m->cap - 1;
    for (i = h & mask; m->tab[i].key != LVM_EMPTY_KEY; i = (i + 1) & mask) {}
    m->tab[i].key = k;
    m->tab[i].val = v;
    m->count++;
    return LVM_OK;
}
