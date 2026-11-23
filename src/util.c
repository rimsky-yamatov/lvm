#include "lvm.h"

#include <stdlib.h>

static void *default_alloc(void *ud, void *ptr, size_t old_size, size_t new_size)
{
    (void)ud;
    (void)old_size;
    if (new_size == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, new_size);
}

static const lvm_allocator default_allocator = { default_alloc, NULL };

const lvm_allocator *lvm_default_allocator(void)
{
    return &default_allocator;
}

const char *lvm_strerror(lvm_status st)
{
    static const char *const msg[] = {
        "ok",
        "out of memory",
        "malformed bytecode file",
        "bytecode verification failed",
        "wrong number of arguments",
        "stack overflow",
        "type error",
        "division by zero",
        "host function error",
        "index out of bounds"
    };
    return (unsigned)st < sizeof msg / sizeof msg[0] ? msg[st] : "unknown error";
}
