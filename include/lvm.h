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
    LVM_ERR_BOUNDS,
    LVM_ERR_ASM,
    LVM_ERR_UNCAUGHT,
    LVM_ERR_COMPILE
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

/* Run the module's entry function. If the program throws and nothing
 * catches it, the status is LVM_ERR_UNCAUGHT and *result is the thrown
 * value (valid until the next allocation or lvm_run on this vm). */
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

/* ---- assembler ----
 *
 * Turns assembly text (see docs/asm.md) into a bytecode file image that
 * lvm_load accepts. The result is verified. On failure err receives a
 * one-line message, e.g. "line 3: unknown instruction 'FOO'", and the
 * status is LVM_ERR_ASM (syntax) or LVM_ERR_VERIFY (rejected by the
 * verifier). Release the image with lvm_assemble_free.
 */
lvm_status lvm_assemble(const lvm_allocator *a, const char *src, size_t len,
                        uint8_t **out, size_t *out_len, char *err, size_t err_size);
void lvm_assemble_free(const lvm_allocator *a, uint8_t *buf, size_t len);

/* ---- compiler ----
 *
 * Compiles lvc source (see docs/lvc.md) to assembly text, which
 * lvm_assemble turns into bytecode. On failure the status is
 * LVM_ERR_COMPILE and err holds a message such as "line 4: undefined
 * variable 'x'". Release the text with lvm_compile_free.
 */
lvm_status lvm_compile(const lvm_allocator *a, const char *src, size_t len,
                       char **out, size_t *out_len, char *err, size_t err_size);
void lvm_compile_free(const lvm_allocator *a, char *text, size_t len);

/* ---- disassembler ----
 *
 * Prints a loaded module as assembly text. The text is accepted by
 * lvm_assemble again; for a module that came out of the assembler it
 * produces the same bytecode. Functions are called "main" (the entry) and
 * "f<index>", local labels ".L<pc>". The text is NUL-terminated and *len
 * does not count the terminator. Release it with lvm_disassemble_free.
 */
#define LVM_DIS_ADDR 1 /* append "; pc: word" to every instruction */

lvm_status lvm_disassemble(const lvm_allocator *a, const lvm_module *m, int flags,
                           char **out, size_t *len);
void lvm_disassemble_free(const lvm_allocator *a, char *text, size_t len);

#ifdef __cplusplus
}
#endif

#endif
