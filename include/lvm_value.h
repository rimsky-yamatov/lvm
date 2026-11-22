#ifndef LVM_VALUE_H
#define LVM_VALUE_H

#include <stdint.h>
#include <string.h>

/*
 * 64-bit NaN-boxed value.
 *
 * Doubles are stored as-is (NaNs are canonicalised to 0x7FF8...).
 * Every other type lives in the negative quiet-NaN space:
 *
 *   1111 1111 1111 1ttt | 48-bit payload
 *   \_______________/\_/
 *     boxed marker    tag
 */
typedef uint64_t lvm_value;

#define LVM_BOXED     UINT64_C(0xFFF8000000000000)
#define LVM_PAYLOAD   UINT64_C(0x0000FFFFFFFFFFFF)
#define LVM_CANON_NAN UINT64_C(0x7FF8000000000000)

enum { LVM_T_NIL = 1, LVM_T_BOOL = 2, LVM_T_INT = 3, LVM_T_PTR = 4 };

#define LVM_INT_MAX ((int64_t)0x7FFFFFFFFFFF)
#define LVM_INT_MIN (-LVM_INT_MAX - 1)

static inline int lvm_fits_int(int64_t v)
{
    return v >= LVM_INT_MIN && v <= LVM_INT_MAX;
}

static inline lvm_value lvm_box(unsigned tag, uint64_t payload)
{
    return LVM_BOXED | ((uint64_t)tag << 48) | (payload & LVM_PAYLOAD);
}

static inline lvm_value lvm_nil(void)    { return lvm_box(LVM_T_NIL, 0); }
static inline lvm_value lvm_bool(int b)  { return lvm_box(LVM_T_BOOL, b ? 1 : 0); }
static inline lvm_value lvm_int(int64_t i) { return lvm_box(LVM_T_INT, (uint64_t)i); }

static inline lvm_value lvm_float(double d)
{
    uint64_t u;
    if (d != d) return LVM_CANON_NAN;
    memcpy(&u, &d, sizeof u);
    return u;
}

static inline int lvm_is_boxed(lvm_value v) { return (v & LVM_BOXED) == LVM_BOXED; }
static inline unsigned lvm_tag(lvm_value v) { return (unsigned)((v >> 48) & 7u); }

static inline int lvm_is_float(lvm_value v) { return !lvm_is_boxed(v); }
static inline int lvm_is_nil(lvm_value v)   { return v == lvm_nil(); }
static inline int lvm_is_bool(lvm_value v)  { return lvm_is_boxed(v) && lvm_tag(v) == LVM_T_BOOL; }
static inline int lvm_is_int(lvm_value v)   { return lvm_is_boxed(v) && lvm_tag(v) == LVM_T_INT; }

static inline double lvm_as_float(lvm_value v)
{
    double d;
    memcpy(&d, &v, sizeof d);
    return d;
}

static inline int lvm_as_bool(lvm_value v) { return (int)(v & 1u); }

/* sign-extend the 48-bit payload without relying on signed shifts */
static inline int64_t lvm_as_int(lvm_value v)
{
    uint64_t x = v & LVM_PAYLOAD;
    return (int64_t)((x ^ UINT64_C(0x800000000000)) - UINT64_C(0x800000000000));
}

static inline int lvm_is_truthy(lvm_value v)
{
    return v != lvm_nil() && v != lvm_bool(0);
}

#endif
