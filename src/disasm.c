/* Disassembler: module -> assembly text that lvm_assemble accepts. */
#include "lvm.h"
#include "lvm_opcode.h"
#include "internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const lvm_allocator *a;
    char *buf;
    size_t len, cap;
    int failed;
} sb;

static void sb_put(sb *b, const char *s, size_t n)
{
    if (b->failed) return;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        char *nb;
        while (nc < b->len + n + 1) nc *= 2;
        nb = LVM_MEM(b->a, b->buf, b->cap, nc);
        if (!nb) {
            b->failed = 1;
            return;
        }
        b->buf = nb;
        b->cap = nc;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static void sb_str(sb *b, const char *s)
{
    sb_put(b, s, strlen(s));
}

static void sb_fmt(sb *b, const char *fmt, ...)
{
    char tmp[128];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < sizeof tmp) sb_put(b, tmp, (size_t)n);
    else b->failed = 1; /* every format used here is short */
}

/* ---- constants ---- */

static void put_float(sb *b, double d)
{
    char tmp[40];

    if (d != d) { sb_str(b, "nan"); return; }
    if (d > 1.7976931348623157e308) { sb_str(b, "inf"); return; }
    if (d < -1.7976931348623157e308) { sb_str(b, "-inf"); return; }
    sprintf(tmp, "%.17g", d);
    sb_str(b, tmp);
    /* "3" would read back as an integer */
    if (!strpbrk(tmp, ".eE")) sb_str(b, ".0");
}

static void put_string(sb *b, const lvm_string *s)
{
    uint32_t i;

    sb_str(b, "\"");
    for (i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->data[i];
        switch (c) {
        case '\n': sb_str(b, "\\n"); break;
        case '\t': sb_str(b, "\\t"); break;
        case '\r': sb_str(b, "\\r"); break;
        case '\\': sb_str(b, "\\\\"); break;
        case '"':  sb_str(b, "\\\""); break;
        default:
            if (c < 0x20 || c > 0x7E) sb_fmt(b, "\\x%02X", c);
            else sb_put(b, (const char *)&c, 1);
        }
    }
    sb_str(b, "\"");
}

static void put_const(sb *b, lvm_value v)
{
    if (lvm_is_float(v)) put_float(b, lvm_as_float(v));
    else if (lvm_is_int(v)) sb_fmt(b, "%lld", (long long)lvm_as_int(v));
    else if (lvm_is_bool(v)) sb_str(b, lvm_as_bool(v) ? "true" : "false");
    else if (lvm_is_string(v)) put_string(b, (const lvm_string *)lvm_to_obj(v));
    else sb_str(b, "nil");
}

static void put_func_name(sb *b, const lvm_module *m, uint32_t idx)
{
    if (idx == m->entry) sb_str(b, "main");
    else sb_fmt(b, "f%lu", (unsigned long)idx);
}
