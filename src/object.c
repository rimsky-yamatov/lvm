#include "lvm.h"
#include "internal.h"

#include <string.h>

static uint32_t fnv1a(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

/* Heap pointers must fit the 48-bit NaN-box payload. */
static int ptr_fits(const void *p)
{
    return ((uint64_t)(uintptr_t)p >> 48) == 0;
}

size_t lvm_obj_bytes(const lvm_obj *o)
{
    switch (o->type) {
    case LVM_OBJ_STRING:
        return LVM_STRING_SIZE(((const lvm_string *)o)->len);
    case LVM_OBJ_ARRAY:
        return sizeof(lvm_array) + (size_t)((const lvm_array *)o)->cap * sizeof(lvm_value);
    default:
        return sizeof(lvm_map) + (size_t)((const lvm_map *)o)->cap * sizeof(lvm_entry);
    }
}

lvm_obj *lvm_obj_new(lvm_vm *vm, int type, size_t size)
{
    lvm_obj *o = LVM_MEM(&vm->alloc, NULL, 0, size);

    if (!o) return NULL;
    if (!ptr_fits(o)) {
        LVM_MEM(&vm->alloc, o, size, 0);
        return NULL;
    }
    o->type = (uint8_t)type;
    o->mark = 0;
    o->perm = 0;
    o->next = vm->objs;
    vm->objs = o;
    vm->bytes += size;
    vm->nobjs++;
    return o;
}

void lvm_obj_free(lvm_vm *vm, lvm_obj *o)
{
    const lvm_allocator *a = &vm->alloc;

    switch (o->type) {
    case LVM_OBJ_STRING:
        LVM_MEM(a, o, LVM_STRING_SIZE(((lvm_string *)o)->len), 0);
        break;
    case LVM_OBJ_ARRAY: {
        lvm_array *arr = (lvm_array *)o;
        if (arr->items)
            LVM_MEM(a, arr->items, (size_t)arr->cap * sizeof(lvm_value), 0);
        LVM_MEM(a, arr, sizeof *arr, 0);
        break;
    }
    default: {
        lvm_map *map = (lvm_map *)o;
        if (map->tab)
            LVM_MEM(a, map->tab, (size_t)map->cap * sizeof(lvm_entry), 0);
        LVM_MEM(a, map, sizeof *map, 0);
        break;
    }
    }
}

void lvm_obj_free_all(lvm_vm *vm)
{
    while (vm->objs) {
        lvm_obj *o = vm->objs;
        vm->objs = o->next;
        lvm_obj_free(vm, o);
    }
    vm->nobjs = 0;
    vm->bytes = 0;
}

/* ---- strings ---- */

static void string_fill(lvm_string *s, const char *src, size_t len)
{
    s->len = (uint32_t)len;
    s->hash = fnv1a(src, len);
    if (len) memcpy(s->data, src, len);
    s->data[len] = '\0';
}

/* A string owned by a module (constant pool): not on any gc list. */
lvm_string *lvm_string_perm(const lvm_allocator *a, const char *src, size_t len)
{
    lvm_string *s = LVM_MEM(a, NULL, 0, LVM_STRING_SIZE(len));

    if (!s) return NULL;
    if (!ptr_fits(s)) {
        LVM_MEM(a, s, LVM_STRING_SIZE(len), 0);
        return NULL;
    }
    s->o.next = NULL;
    s->o.type = LVM_OBJ_STRING;
    s->o.mark = 0;
    s->o.perm = 1;
    string_fill(s, src, len);
    return s;
}

void lvm_string_free_perm(const lvm_allocator *a, lvm_string *s)
{
    LVM_MEM(a, s, LVM_STRING_SIZE(s->len), 0);
}

lvm_value lvm_new_string(lvm_vm *vm, const char *src, size_t len)
{
    lvm_string *s;

    if (len > 0x7FFFFFFFu) return lvm_nil();
    s = (lvm_string *)lvm_obj_new(vm, LVM_OBJ_STRING, LVM_STRING_SIZE(len));
    if (!s) return lvm_nil();
    string_fill(s, src, len);
    return lvm_from_obj(&s->o);
}

int lvm_is_string(lvm_value v)
{
    return lvm_is_obj(v, LVM_OBJ_STRING);
}

const char *lvm_string_data(lvm_value v, size_t *len)
{
    const lvm_string *s;

    if (!lvm_is_string(v)) return NULL;
    s = (const lvm_string *)lvm_to_obj(v);
    if (len) *len = s->len;
    return s->data;
}

/* identity, or content equality for strings */
int lvm_obj_equal(lvm_value a, lvm_value b)
{
    const lvm_string *x, *y;

    if (a == b) return 1;
    if (!lvm_is_string(a) || !lvm_is_string(b)) return 0;
    x = (const lvm_string *)lvm_to_obj(a);
    y = (const lvm_string *)lvm_to_obj(b);
    return x->len == y->len && x->hash == y->hash &&
           memcmp(x->data, y->data, x->len) == 0;
}

const char *lvm_type_name(lvm_value v)
{
    if (lvm_is_float(v)) return "float";
    if (lvm_is_nil(v)) return "nil";
    if (lvm_is_bool(v)) return "bool";
    if (lvm_is_int(v)) return "int";
    if (lvm_is_ptr(v)) {
        switch (lvm_to_obj(v)->type) {
        case LVM_OBJ_STRING: return "string";
        case LVM_OBJ_ARRAY:  return "array";
        case LVM_OBJ_MAP:    return "map";
        }
    }
    return "unknown";
}

size_t lvm_heap_bytes(const lvm_vm *vm)
{
    return vm->bytes;
}
