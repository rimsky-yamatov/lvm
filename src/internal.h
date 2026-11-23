#ifndef LVM_INTERNAL_H
#define LVM_INTERNAL_H

#include <stddef.h>

#include "lvm.h"

#define LVM_MEM(a, p, old, nw) ((a)->alloc((a)->ud, (p), (old), (nw)))

typedef struct {
    const lvm_func *fn;
    const uint32_t *pc;
    uint32_t base;
} lvm_frame;

typedef struct {
    lvm_host_fn fn;
    void *ud;
} lvm_host_slot;

struct lvm_vm {
    lvm_allocator alloc;
    lvm_value *stack;
    uint32_t stack_slots;
    lvm_frame *frames;
    uint32_t max_frames;
    lvm_host_slot host[LVM_HOST_MAX];
};

#endif
