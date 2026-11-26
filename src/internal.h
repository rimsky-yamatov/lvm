#ifndef LVM_INTERNAL_H
#define LVM_INTERNAL_H

#include <stddef.h>

#include "lvm.h"

#define LVM_MEM(a, p, old, nw) ((a)->alloc((a)->ud, (p), (old), (nw)))

typedef struct {
    uint32_t depth;         /* frame level that installed the handler */
    uint32_t reg;           /* register that receives the exception */
    const uint32_t *pc;     /* where the handler starts */
} lvm_handler;

typedef struct lvm_closure lvm_closure;
typedef struct lvm_upval lvm_upval;

typedef struct {
    const lvm_func *fn;
    const uint32_t *pc;
    uint32_t base;
    lvm_closure *cl;    /* closure running in this frame, or NULL */
    uint32_t ret;       /* absolute stack slot that receives the result */
} lvm_frame;

typedef struct {
    lvm_host_fn fn;
    void *ud;
} lvm_host_slot;

/* ---- heap objects ---- */

enum { LVM_OBJ_STRING = 1, LVM_OBJ_ARRAY, LVM_OBJ_MAP, LVM_OBJ_CLOSURE, LVM_OBJ_UPVAL };

typedef struct lvm_obj {
    struct lvm_obj *next;   /* gc list */
    uint8_t type;
    uint8_t mark;
    uint8_t perm;           /* owned by a module, never collected */
} lvm_obj;

typedef struct {
    lvm_obj o;
    uint32_t len;
    uint32_t hash;
    char data[1];           /* len bytes + NUL */
} lvm_string;

typedef struct {
    lvm_obj o;
    uint32_t len, cap;
    lvm_value *items;
} lvm_array;

typedef struct {
    lvm_value key, val;
} lvm_entry;

typedef struct {
    lvm_obj o;
    uint32_t count, cap;    /* cap is 0 or a power of two */
    lvm_entry *tab;         /* open addressing, linear probing */
} lvm_map;

/* An upvalue points at a stack slot while open and at its own storage once
 * closed. Open upvalues form a list ordered by slot, highest first. */
struct lvm_upval {
    lvm_obj o;
    lvm_value *v;
    lvm_value closed;
    lvm_upval *next;
};

struct lvm_closure {
    lvm_obj o;
    const lvm_func *fn;     /* lives in the module, which must outlive the closure */
    uint32_t nup;
    lvm_upval *up[];
};

#define LVM_EMPTY_KEY ((lvm_value)0)
#define LVM_CLOSURE_SIZE(n) (offsetof(lvm_closure, up) + (size_t)(n) * sizeof(lvm_upval *))
#define LVM_STRING_SIZE(len) (offsetof(lvm_string, data) + (size_t)(len) + 1)
#define LVM_GC_MIN (256u * 1024u)

static inline int lvm_is_ptr(lvm_value v)
{
    return lvm_is_boxed(v) && lvm_tag(v) == LVM_T_PTR;
}

static inline lvm_value lvm_from_obj(const lvm_obj *o)
{
    return lvm_box(LVM_T_PTR, (uint64_t)(uintptr_t)o);
}

static inline lvm_obj *lvm_to_obj(lvm_value v)
{
    return (lvm_obj *)(uintptr_t)(v & LVM_PAYLOAD);
}

static inline int lvm_is_obj(lvm_value v, int type)
{
    return lvm_is_ptr(v) && lvm_to_obj(v)->type == type;
}

struct lvm_vm {
    lvm_allocator alloc;
    lvm_value *stack;
    uint32_t stack_slots;
    lvm_frame *frames;
    uint32_t max_frames;
    lvm_handler *handlers;  /* active TRY blocks, innermost last */
    uint32_t nhandlers, max_handlers;
    lvm_host_slot host[LVM_HOST_MAX];

    /* heap and collector */
    lvm_obj *objs;
    size_t nobjs;
    size_t bytes, threshold, gc_min;
    lvm_obj **gray;
    size_t ngray, capgray;

    lvm_upval *open;        /* open upvalues, highest slot first */

    /* current frame, published by the interpreter for root scanning */
    const lvm_func *cur_fn;
    lvm_closure *cur_cl;
    uint32_t cur_base;
    uint32_t depth;
};

/* object.c */
lvm_obj *lvm_obj_new(lvm_vm *vm, int type, size_t size);
void lvm_obj_free(lvm_vm *vm, lvm_obj *o);
void lvm_obj_free_all(lvm_vm *vm);
size_t lvm_obj_bytes(const lvm_obj *o);
int lvm_obj_equal(lvm_value a, lvm_value b);
lvm_value lvm_obj_concat(lvm_vm *vm, lvm_value a, lvm_value b);   /* nil on failure */
lvm_string *lvm_string_perm(const lvm_allocator *a, const char *s, size_t len);
void lvm_string_free_perm(const lvm_allocator *a, lvm_string *s);

/* gc.c */
lvm_status lvm_gc_collect(lvm_vm *vm);
void lvm_gc_free(lvm_vm *vm);

/* closure.c */
lvm_closure *lvm_closure_new(lvm_vm *vm, const lvm_func *fn);   /* NULL on failure */
lvm_upval *lvm_upval_capture(lvm_vm *vm, lvm_value *slot);      /* NULL on failure */
void lvm_upvals_close(lvm_vm *vm, const lvm_value *level);

/* container.c */
lvm_value lvm_obj_array_new(lvm_vm *vm, uint32_t cap);   /* nil on failure */
lvm_value lvm_obj_map_new(lvm_vm *vm, uint32_t hint);    /* nil on failure */
lvm_status lvm_obj_get(lvm_value c, lvm_value k, lvm_value *out);
lvm_status lvm_obj_set(lvm_vm *vm, lvm_value c, lvm_value k, lvm_value v);
lvm_status lvm_obj_len(lvm_value c, lvm_value *out);

#endif
