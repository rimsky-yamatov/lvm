/*
 * Standard host library. This is the only part of liblvm that touches
 * stdio and the C math library; the VM core does not need it.
 */
#include "lvm.h"
#include "lvm_std.h"
#include "internal.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct lvm_std {
    lvm_vm *vm;
    lvm_std_io io;
    int peeked;         /* pushed-back input byte, or -2 for none */
    uint64_t rng;
};

/* ---- input and output ---- */

static int stdio_read(void *ud)
{
    (void)ud;
    fflush(stdout); /* so prompts show up before we wait for input */
    return getchar();
}

static void stdio_write(void *ud, const char *s, size_t n)
{
    (void)ud;
    fwrite(s, 1, n, stdout);
}

static void out_bytes(lvm_std *s, const char *p, size_t n)
{
    if (n) s->io.write(s->io.ud, p, n);
}

static int in_get(lvm_std *s)
{
    int c;

    if (s->peeked != -2) {
        c = s->peeked;
        s->peeked = -2;
        return c;
    }
    return s->io.read_char(s->io.ud);
}

static void in_unget(lvm_std *s, int c)
{
    if (c >= 0) s->peeked = c;
}

/* ---- growable text ---- */

typedef struct {
    char *p;
    size_t n, cap;
    int bad;
} tb;

static void tb_put(tb *b, const char *s, size_t n)
{
    if (b->bad) return;
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        char *np;
        while (nc < b->n + n + 1) nc *= 2;
        np = realloc(b->p, nc);
        if (!np) {
            b->bad = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

static void tb_str(tb *b, const char *s)
{
    tb_put(b, s, strlen(s));
}

static void tb_free(tb *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

/* appends vsnprintf output of any length */
static void tb_vfmt(tb *b, const char *fmt, va_list ap)
{
    va_list ap2;
    int n;

    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) {
        b->bad = 1;
        return;
    }
    tb_put(b, "", 0);
    if (b->bad) return;
    if (b->n + (size_t)n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        char *np;
        while (nc < b->n + (size_t)n + 1) nc *= 2;
        np = realloc(b->p, nc);
        if (!np) {
            b->bad = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    vsnprintf(b->p + b->n, (size_t)n + 1, fmt, ap);
    b->n += (size_t)n;
}

static void tb_fmt(tb *b, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    tb_vfmt(b, fmt, ap);
    va_end(ap);
}

/* ---- value formatting ---- */

/* shortest text that reads back as the same double; always shows it is a float */
static void fmt_double(tb *b, double d)
{
    char tmp[40];
    int prec;

    if (d != d) { tb_str(b, "nan"); return; }
    if (d > DBL_MAX) { tb_str(b, "inf"); return; }
    if (d < -DBL_MAX) { tb_str(b, "-inf"); return; }
    for (prec = 15; prec <= 17; prec++) {
        snprintf(tmp, sizeof tmp, "%.*g", prec, d);
        if (strtod(tmp, NULL) == d) break;
    }
    tb_str(b, tmp);
    if (!strpbrk(tmp, ".eE")) tb_str(b, ".0");
}

static void fmt_quoted(tb *b, const char *s, size_t n)
{
    size_t i;

    tb_str(b, "\"");
    for (i = 0; i < n; i++) {
        switch (s[i]) {
        case '"':  tb_str(b, "\\\""); break;
        case '\\': tb_str(b, "\\\\"); break;
        case '\n': tb_str(b, "\\n"); break;
        case '\t': tb_str(b, "\\t"); break;
        default:   tb_put(b, s + i, 1); break;
        }
    }
    tb_str(b, "\"");
}

/* how print() and str() show a value; strings inside containers are quoted */
static void fmt_value(tb *b, lvm_value v, int depth, int quote)
{
    uint32_t i;

    if (lvm_is_float(v)) {
        fmt_double(b, lvm_as_float(v));
    } else if (lvm_is_int(v)) {
        tb_fmt(b, "%lld", (long long)lvm_as_int(v));
    } else if (lvm_is_bool(v)) {
        tb_str(b, lvm_as_bool(v) ? "true" : "false");
    } else if (lvm_is_nil(v)) {
        tb_str(b, "nil");
    } else if (lvm_is_string(v)) {
        size_t n;
        const char *d = lvm_string_data(v, &n);
        if (quote) fmt_quoted(b, d, n);
        else tb_put(b, d, n);
    } else if (lvm_is_obj(v, LVM_OBJ_ARRAY)) {
        const lvm_array *a = (const lvm_array *)lvm_to_obj(v);
        if (depth > 6) { tb_str(b, "[...]"); return; }
        tb_str(b, "[");
        for (i = 0; i < a->len; i++) {
            if (i) tb_str(b, ", ");
            fmt_value(b, a->items[i], depth + 1, 1);
        }
        tb_str(b, "]");
    } else if (lvm_is_obj(v, LVM_OBJ_MAP)) {
        const lvm_map *m = (const lvm_map *)lvm_to_obj(v);
        int first = 1;
        if (depth > 6) { tb_str(b, "{...}"); return; }
        tb_str(b, "{");
        for (i = 0; i < m->cap; i++) {
            if (m->tab[i].key == LVM_EMPTY_KEY) continue;
            if (!first) tb_str(b, ", ");
            first = 0;
            fmt_value(b, m->tab[i].key, depth + 1, 1);
            tb_str(b, ": ");
            fmt_value(b, m->tab[i].val, depth + 1, 1);
        }
        tb_str(b, "}");
    } else {
        tb_str(b, "<closure>");
    }
}

/* ---- print and printf ---- */

static lvm_status finish_text(lvm_std *s, tb *b, lvm_value *args)
{
    if (b->bad) {
        tb_free(b);
        return LVM_ERR_MEM;
    }
    out_bytes(s, b->p, b->n);
    args[0] = lvm_int((int64_t)b->n);
    tb_free(b);
    return LVM_OK;
}

static lvm_status host_print(void *ud, lvm_value *args, int argc)
{
    tb b = { NULL, 0, 0, 0 };
    int k;

    for (k = 0; k < argc; k++) {
        if (k) tb_str(&b, " ");
        fmt_value(&b, args[k], 0, 0);
    }
    tb_str(&b, "\n");
    return finish_text((lvm_std *)ud, &b, args);
}

static int arg_ll(lvm_value v, long long *out)
{
    if (lvm_is_int(v)) { *out = (long long)lvm_as_int(v); return 1; }
    if (lvm_is_bool(v)) { *out = lvm_as_bool(v); return 1; }
    if (lvm_is_float(v)) {
        double d = lvm_as_float(v);
        if (d != d || d <= -9.2e18 || d >= 9.2e18) return 0;
        *out = (long long)d;
        return 1;
    }
    return 0;
}

static int arg_double(lvm_value v, double *out)
{
    if (lvm_is_float(v)) { *out = lvm_as_float(v); return 1; }
    if (lvm_is_int(v)) { *out = (double)lvm_as_int(v); return 1; }
    return 0;
}

/*
 * printf: flags, width, precision and the conversions d i u x X o c s f F e E g G %.
 * Length modifiers are accepted and ignored. Integer conversions take ints,
 * bools and floats (truncated); %s shows any value the way print does.
 */
static lvm_status host_printf(void *ud, lvm_value *args, int argc)
{
    lvm_std *st = ud;
    const char *fmt;
    size_t fl, i;
    tb b = { NULL, 0, 0, 0 };
    int ai = 1;
    lvm_status err = LVM_OK;

    fmt = argc >= 1 ? lvm_string_data(args[0], &fl) : NULL;
    if (!fmt) return LVM_ERR_TYPE;

    for (i = 0; i < fl && err == LVM_OK; i++) {
        char spec[40], conv;
        size_t sl = 0;

        if (fmt[i] != '%') {
            tb_put(&b, fmt + i, 1);
            continue;
        }
        spec[sl++] = '%';
        i++;
        while (i < fl && strchr("-+ #0", fmt[i]) && sl < 8) spec[sl++] = fmt[i++];
        while (i < fl && fmt[i] >= '0' && fmt[i] <= '9' && sl < 14) spec[sl++] = fmt[i++];
        if (i < fl && fmt[i] == '.' && sl < 15) {
            spec[sl++] = fmt[i++];
            while (i < fl && fmt[i] >= '0' && fmt[i] <= '9' && sl < 20) spec[sl++] = fmt[i++];
        }
        while (i < fl && strchr("hlLzjt", fmt[i])) i++;
        if (i >= fl) { err = LVM_ERR_TYPE; break; }
        conv = fmt[i];

        if (conv == '%') {
            tb_str(&b, "%");
            continue;
        }
        if (ai >= argc) { err = LVM_ERR_ARGS; break; } /* not enough arguments */

        switch (conv) {
        case 'd': case 'i': {
            long long v;
            if (!arg_ll(args[ai], &v)) { err = LVM_ERR_TYPE; break; }
            strcpy(spec + sl, "lld");
            tb_fmt(&b, spec, v);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            long long v;
            if (!arg_ll(args[ai], &v)) { err = LVM_ERR_TYPE; break; }
            spec[sl++] = 'l';
            spec[sl++] = 'l';
            spec[sl++] = conv;
            spec[sl] = '\0';
            tb_fmt(&b, spec, (unsigned long long)v);
            break;
        }
        case 'c': {
            long long v;
            char ch;
            if (!arg_ll(args[ai], &v)) { err = LVM_ERR_TYPE; break; }
            ch = (char)v;
            tb_put(&b, &ch, 1);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double v;
            if (!arg_double(args[ai], &v)) { err = LVM_ERR_TYPE; break; }
            spec[sl++] = conv;
            spec[sl] = '\0';
            tb_fmt(&b, spec, v);
            break;
        }
        case 's': {
            tb t = { NULL, 0, 0, 0 };
            if (lvm_is_string(args[ai])) {
                size_t n;
                const char *d = lvm_string_data(args[ai], &n);
                tb_put(&t, d, n);
            } else {
                fmt_value(&t, args[ai], 0, 0);
            }
            if (sl == 1) { /* no flags, width or precision: copy as is */
                tb_put(&b, t.p ? t.p : "", t.n);
            } else {
                spec[sl++] = 's';
                spec[sl] = '\0';
                tb_fmt(&b, spec, t.p ? t.p : "");
            }
            if (t.bad) b.bad = 1;
            tb_free(&t);
            break;
        }
        default:
            err = LVM_ERR_TYPE;
            break;
        }
        ai++;
    }

    if (err != LVM_OK) {
        tb_free(&b);
        return err;
    }
    return finish_text(st, &b, args);
}

/* ---- input: scan, readline, getchar, putchar ---- */

static int is_ws(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static int is_dig(int c)
{
    return c >= '0' && c <= '9';
}

static void skip_ws(lvm_std *s, int *eof)
{
    int c;

    while ((c = in_get(s)) != -1 && is_ws(c)) {}
    if (c == -1) *eof = 1;
    else in_unget(s, c);
}

/* an integer in the VM's range stays an int, anything larger becomes a float */
static lvm_value int_or_float(long long v)
{
    return lvm_fits_int((int64_t)v) ? lvm_int((int64_t)v) : lvm_float((double)v);
}

/* reads the next value for a conversion; returns 0 when the input does not match */
static int scan_number(lvm_std *s, int base, int is_float, lvm_value *out, int *eof)
{
    char buf[80];
    size_t n = 0;
    int c;

    skip_ws(s, eof);
    if (*eof) return 0;

    c = in_get(s);
    if (c == '+' || c == '-') {
        buf[n++] = (char)c;
        c = in_get(s);
    }
    if (is_float) {
        int digits = 0;
        while (is_dig(c) && n < sizeof buf - 2) { buf[n++] = (char)c; c = in_get(s); digits = 1; }
        if (c == '.') {
            buf[n++] = '.';
            c = in_get(s);
            while (is_dig(c) && n < sizeof buf - 2) { buf[n++] = (char)c; c = in_get(s); digits = 1; }
        }
        if (!digits) {
            if (c == -1) *eof = 1;
            else in_unget(s, c);
            return 0;
        }
        if (c == 'e' || c == 'E') {
            buf[n++] = 'e';
            c = in_get(s);
            if (c == '+' || c == '-') { buf[n++] = (char)c; c = in_get(s); }
            while (is_dig(c) && n < sizeof buf - 1) { buf[n++] = (char)c; c = in_get(s); }
        }
        if (c == -1) *eof = 1;
        else in_unget(s, c);
        buf[n] = '\0';
        *out = lvm_float(strtod(buf, NULL));
        return 1;
    } else {
        int digits = 0;
        for (;;) {
            int ok = base == 16 ? (is_dig(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) : is_dig(c);
            if (!ok || n >= sizeof buf - 1) break;
            buf[n++] = (char)c;
            c = in_get(s);
            digits = 1;
        }
        if (c == -1) *eof = 1;
        else in_unget(s, c);
        if (!digits) return 0;
        buf[n] = '\0';
        *out = int_or_float(strtoll(buf, NULL, base));
        return 1;
    }
}

/*
 * scan(fmt): the engine behind scanf. Understands %d %i %u %x %f %e %g %s %c
 * and %%; white space in the format skips white space in the input. Returns
 * the array of converted values, or nil if the input ended before the first.
 */
static lvm_status host_scan(void *ud, lvm_value *args, int argc)
{
    lvm_std *s = ud;
    const char *f;
    char *fmt;
    size_t fl, i;
    lvm_value arr;
    uint32_t n = 0;
    int eof = 0;

    f = argc == 1 ? lvm_string_data(args[0], &fl) : NULL;
    if (!f) return LVM_ERR_TYPE;
    fmt = malloc(fl + 1); /* the format is overwritten by the result below */
    if (!fmt) return LVM_ERR_MEM;
    memcpy(fmt, f, fl);

    arr = lvm_obj_array_new(s->vm, 4);
    if (lvm_is_nil(arr)) {
        free(fmt);
        return LVM_ERR_MEM;
    }
    args[0] = arr;

    for (i = 0; i < fl; i++) {
        lvm_value v = lvm_nil();
        int c, ok = 1;

        if (is_ws((unsigned char)fmt[i])) {
            skip_ws(s, &eof);
            continue;
        }
        if (fmt[i] != '%') { /* a literal must match */
            c = in_get(s);
            if (c != (unsigned char)fmt[i]) {
                if (c == -1) eof = 1;
                else in_unget(s, c);
                break;
            }
            continue;
        }
        i++;
        while (i < fl && strchr("hlLzjt", fmt[i])) i++;
        if (i >= fl) break;

        switch (fmt[i]) {
        case '%':
            skip_ws(s, &eof);
            c = eof ? -1 : in_get(s);
            ok = c == '%';
            if (!ok && c != -1) in_unget(s, c);
            continue;
        case 'd': case 'i': case 'u':
            ok = scan_number(s, 10, 0, &v, &eof);
            break;
        case 'x': case 'X':
            ok = scan_number(s, 16, 0, &v, &eof);
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            ok = scan_number(s, 10, 1, &v, &eof);
            break;
        case 's': {
            tb b = { NULL, 0, 0, 0 };
            skip_ws(s, &eof);
            if (!eof) {
                while ((c = in_get(s)) != -1 && !is_ws(c)) {
                    char ch = (char)c;
                    tb_put(&b, &ch, 1);
                }
                if (c == -1) eof = 1;
                else in_unget(s, c);
            }
            ok = b.n > 0;
            if (ok) {
                v = lvm_new_string(s->vm, b.p, b.n);
                if (lvm_is_nil(v)) ok = 0, eof = 0;
            }
            tb_free(&b);
            break;
        }
        case 'c':
            c = in_get(s);
            ok = c != -1;
            if (ok) v = lvm_int(c);
            else eof = 1;
            break;
        default:
            ok = 0;
            break;
        }
        if (!ok) break;
        if (lvm_obj_set(s->vm, arr, lvm_int((int64_t)n), v) != LVM_OK) {
            free(fmt);
            return LVM_ERR_MEM;
        }
        n++;
    }
    free(fmt);
    if (n == 0 && eof) args[0] = lvm_nil();
    return LVM_OK;
}

static lvm_status host_readline(void *ud, lvm_value *args, int argc)
{
    lvm_std *s = ud;
    tb b = { NULL, 0, 0, 0 };
    int c, any = 0;
    lvm_value v;

    (void)argc;
    while ((c = in_get(s)) != -1) {
        char ch = (char)c;
        any = 1;
        if (c == '\n') break;
        tb_put(&b, &ch, 1);
    }
    if (!any) {
        tb_free(&b);
        args[0] = lvm_nil(); /* end of input */
        return LVM_OK;
    }
    if (b.n && b.p[b.n - 1] == '\r') b.n--;
    v = b.bad ? lvm_nil() : lvm_new_string(s->vm, b.p ? b.p : "", b.n);
    tb_free(&b);
    if (lvm_is_nil(v)) return LVM_ERR_MEM;
    args[0] = v;
    return LVM_OK;
}

static lvm_status host_getchar(void *ud, lvm_value *args, int argc)
{
    (void)argc;
    args[0] = lvm_int(in_get((lvm_std *)ud));
    return LVM_OK;
}

static lvm_status host_putchar(void *ud, lvm_value *args, int argc)
{
    long long v;
    char ch;

    if (argc != 1 || !arg_ll(args[0], &v)) return LVM_ERR_TYPE;
    ch = (char)v;
    out_bytes((lvm_std *)ud, &ch, 1);
    args[0] = lvm_int((unsigned char)ch);
    return LVM_OK;
}
