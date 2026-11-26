#ifndef LVM_STD_H
#define LVM_STD_H

#include "lvm.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Standard host library: printf, scanf, strings, containers, math.
 * lvm_std_new registers all of it on a vm; programs reach it with
 * HOST A, argc, id using the numbers below (the lvc compiler does that
 * for you).
 */
enum {
    LVM_STD_PRINT = 0, /* print(...): values separated by spaces, then a newline */
    LVM_STD_PRINTF,
    LVM_STD_SCAN,      /* scan(fmt): array of converted values, nil at end of input */
    LVM_STD_READLINE,
    LVM_STD_GETCHAR,
    LVM_STD_PUTCHAR,
    LVM_STD_INT,
    LVM_STD_FLOAT,
    LVM_STD_STR,
    LVM_STD_SUBSTR,
    LVM_STD_STRCMP,
    LVM_STD_CHR,
    LVM_STD_FIND,
    LVM_STD_SPLIT,
    LVM_STD_ARRAY,
    LVM_STD_MAP,
    LVM_STD_POP,
    LVM_STD_KEYS,
    LVM_STD_SORT,
    LVM_STD_SQRT,
    LVM_STD_SIN,
    LVM_STD_COS,
    LVM_STD_TAN,
    LVM_STD_ATAN2,
    LVM_STD_POW,
    LVM_STD_EXP,
    LVM_STD_LOG,
    LVM_STD_FLOOR,
    LVM_STD_CEIL,
    LVM_STD_ABS,
    LVM_STD_MIN,
    LVM_STD_MAX,
    LVM_STD_RAND,
    LVM_STD_SRAND,
    LVM_STD_CLOCK,
    LVM_STD_COUNT
};

typedef struct lvm_std lvm_std;

/* Where the library reads and writes. read_char returns a byte, or -1 at
 * the end of input. */
typedef struct {
    int (*read_char)(void *ud);
    void (*write)(void *ud, const char *s, size_t n);
    void *ud;
} lvm_std_io;

/* io == NULL uses stdin and stdout. Free it before the vm. */
lvm_std *lvm_std_new(lvm_vm *vm, const lvm_std_io *io);
void lvm_std_free(lvm_std *s);

#ifdef __cplusplus
}
#endif

#endif
