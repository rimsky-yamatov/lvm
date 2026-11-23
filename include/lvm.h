#ifndef LVM_H
#define LVM_H

#include <stddef.h>
#include <stdint.h>

#include "lvm_value.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LVM_VERSION  1
#define LVM_HOST_MAX 256

typedef enum {
    LVM_OK = 0,
    LVM_ERR_MEM,
    LVM_ERR_FORMAT,
    LVM_ERR_VERIFY,
    LVM_ERR_ARGS,
    LVM_ERR_STACK,
    LVM_ERR_TYPE,
    LVM_ERR_DIVZERO,
    LVM_ERR_HOST,
    LVM_ERR_BOUNDS
} lvm_status;

/* realloc-like allocator; new_size == 0 frees */
typedef void *(*lvm_alloc_fn)(void *ud, void *ptr, size_t old_size, size_t new_size);

typedef struct lvm_allocator {
    lvm_alloc_fn alloc;
    void *ud;
} lvm_allocator;

const lvm_allocator *lvm_default_allocator(void);
const char *lvm_strerror(lvm_status st);

/* ---- module ---- */

/* how a closure captures one of its upvalues when it is created */
typedef struct lvm_updesc {
    uint8_t instack; /* 1: register `index` of the creating frame,
                        0: upvalue `index` of the creating closure */
    uint8_t index;
} lvm_updesc;

typedef struct lvm_func {
    uint8_t nparams;
    uint8_t nregs;
    uint8_t nupvals;
    lvm_updesc *upvals;
    uint32_t ncode;
    uint32_t *code;
} lvm_func;

typedef struct lvm_module {
    lvm_allocator alloc;
    lvm_value *consts;
    uint32_t nconsts;
    lvm_func *funcs;
    uint32_t nfuncs;
    uint32_t entry;
} lvm_module;

/* parse + verify; a NULL allocator means the default one */
lvm_status lvm_load(const lvm_allocator *a, const uint8_t *buf, size_t len, lvm_module *out);
lvm_status lvm_verify(const lvm_module *m);
void lvm_module_free(lvm_module *m);

/* ---- vm ---- */

typedef struct lvm_vm lvm_vm;

/* host function: args[0..argc-1] in, result goes to args[0] */
typedef lvm_status (*lvm_host_fn)(void *ud, lvm_value *args, int argc);

lvm_vm *lvm_new(const lvm_allocator *a, uint32_t stack_slots, uint32_t max_frames);
void lvm_free(lvm_vm *vm);
void lvm_set_host(lvm_vm *vm, unsigned id, lvm_host_fn fn, void *ud);

/* run the module's entry function */
lvm_status lvm_run(lvm_vm *vm, const lvm_module *m,
                   const lvm_value *args, int nargs, lvm_value *result);

/* ---- heap values ----
 *
 * Strings, arrays and maps live on a garbage-collected heap owned by the
 * vm. A heap value stays valid while it is reachable from a register. A
 * value handed back by lvm_run is valid until the next lvm_run, lvm_gc or
 * allocation on that vm. Host functions that allocate may trigger a
 * collection: store each new value into args[] before allocating again.
 */

int lvm_is_string(lvm_value v);
const char *lvm_string_data(lvm_value v, size_t *len); /* NULL if not a string */
lvm_value lvm_new_string(lvm_vm *vm, const char *s, size_t len); /* nil on failure */
const char *lvm_type_name(lvm_value v);

lvm_status lvm_gc(lvm_vm *vm);
size_t lvm_heap_bytes(const lvm_vm *vm);
void lvm_set_gc_threshold(lvm_vm *vm, size_t min_bytes);

#ifdef __cplusplus
}
#endif

#endif
