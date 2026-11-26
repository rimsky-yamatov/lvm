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
            if (c != '%') { /* the literal did not match */
                if (c == -1) eof = 1;
                else in_unget(s, c);
                goto done;
            }
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
done:
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

/* ---- conversions ---- */

static int fits_double_int(double d)
{
    return d >= -140737488355328.0 && d < 140737488355328.0;
}

/* int(x): truncates floats, parses the leading number of a string */
static lvm_status host_int(void *ud, lvm_value *args, int argc)
{
    lvm_value v;

    (void)ud;
    if (argc != 1) return LVM_ERR_ARGS;
    v = args[0];
    if (lvm_is_int(v)) return LVM_OK;
    if (lvm_is_bool(v)) {
        args[0] = lvm_int(lvm_as_bool(v));
    } else if (lvm_is_float(v)) {
        double d = lvm_as_float(v);
        if (d != d || !fits_double_int(d)) return LVM_ERR_TYPE;
        args[0] = lvm_int((int64_t)d);
    } else if (lvm_is_string(v)) {
        args[0] = int_or_float(strtoll(lvm_string_data(v, NULL), NULL, 10));
    } else {
        return LVM_ERR_TYPE;
    }
    return LVM_OK;
}

static lvm_status host_float(void *ud, lvm_value *args, int argc)
{
    lvm_value v;

    (void)ud;
    if (argc != 1) return LVM_ERR_ARGS;
    v = args[0];
    if (lvm_is_float(v)) return LVM_OK;
    if (lvm_is_int(v)) args[0] = lvm_float((double)lvm_as_int(v));
    else if (lvm_is_bool(v)) args[0] = lvm_float(lvm_as_bool(v));
    else if (lvm_is_string(v)) args[0] = lvm_float(strtod(lvm_string_data(v, NULL), NULL));
    else return LVM_ERR_TYPE;
    return LVM_OK;
}

static lvm_status host_str(void *ud, lvm_value *args, int argc)
{
    lvm_std *s = ud;
    tb b = { NULL, 0, 0, 0 };
    lvm_value v;

    if (argc != 1) return LVM_ERR_ARGS;
    if (lvm_is_string(args[0])) return LVM_OK;
    fmt_value(&b, args[0], 0, 0);
    v = b.bad ? lvm_nil() : lvm_new_string(s->vm, b.p ? b.p : "", b.n);
    tb_free(&b);
    if (lvm_is_nil(v)) return LVM_ERR_MEM;
    args[0] = v;
    return LVM_OK;
}

/* ---- strings ---- */

static lvm_status host_substr(void *ud, lvm_value *args, int argc)
{
    size_t len;
    const char *d;
    long long start, n;
    lvm_value v;

    if (argc < 2 || argc > 3) return LVM_ERR_ARGS;
    d = lvm_string_data(args[0], &len);
    if (!d || !arg_ll(args[1], &start)) return LVM_ERR_TYPE;
    if (start < 0) start = 0;
    if (start > (long long)len) start = (long long)len;
    n = (long long)len - start;
    if (argc == 3) {
        long long want;
        if (!arg_ll(args[2], &want)) return LVM_ERR_TYPE;
        if (want < 0) want = 0;
        if (want < n) n = want;
    }
    v = lvm_new_string(((lvm_std *)ud)->vm, d + start, (size_t)n);
    if (lvm_is_nil(v)) return LVM_ERR_MEM;
    args[0] = v;
    return LVM_OK;
}

static lvm_status host_strcmp(void *ud, lvm_value *args, int argc)
{
    size_t la, lb, n;
    const char *a, *b;
    int r;

    (void)ud;
    if (argc != 2) return LVM_ERR_ARGS;
    a = lvm_string_data(args[0], &la);
    b = lvm_string_data(args[1], &lb);
    if (!a || !b) return LVM_ERR_TYPE;
    n = la < lb ? la : lb;
    r = memcmp(a, b, n);
    if (r == 0) r = la < lb ? -1 : (la > lb ? 1 : 0);
    args[0] = lvm_int(r < 0 ? -1 : (r > 0 ? 1 : 0));
    return LVM_OK;
}

static lvm_status host_chr(void *ud, lvm_value *args, int argc)
{
    long long v;
    char ch;
    lvm_value s;

    if (argc != 1 || !arg_ll(args[0], &v)) return LVM_ERR_TYPE;
    if (v < 0 || v > 255) return LVM_ERR_BOUNDS;
    ch = (char)v;
    s = lvm_new_string(((lvm_std *)ud)->vm, &ch, 1);
    if (lvm_is_nil(s)) return LVM_ERR_MEM;
    args[0] = s;
    return LVM_OK;
}

static long find_bytes(const char *h, size_t hl, const char *n, size_t nl, size_t from)
{
    size_t i;

    if (nl > hl) return -1;
    for (i = from; i + nl <= hl; i++) {
        if (nl == 0 || memcmp(h + i, n, nl) == 0) return (long)i;
    }
    return -1;
}

static lvm_status host_find(void *ud, lvm_value *args, int argc)
{
    size_t hl, nl;
    const char *h, *n;
    long long from = 0;

    (void)ud;
    if (argc < 2 || argc > 3) return LVM_ERR_ARGS;
    h = lvm_string_data(args[0], &hl);
    n = lvm_string_data(args[1], &nl);
    if (!h || !n) return LVM_ERR_TYPE;
    if (argc == 3 && !arg_ll(args[2], &from)) return LVM_ERR_TYPE;
    if (from < 0) from = 0;
    if (from > (long long)hl) {
        args[0] = lvm_int(-1);
        return LVM_OK;
    }
    args[0] = lvm_int(find_bytes(h, hl, n, nl, (size_t)from));
    return LVM_OK;
}

/* split(s, sep): the pieces between occurrences of sep, as an array */
static lvm_status host_split(void *ud, lvm_value *args, int argc)
{
    lvm_vm *vm = ((lvm_std *)ud)->vm;
    const char *s, *sep;
    char *sepcopy;
    size_t sl, pl, start = 0;
    lvm_value arr;
    uint32_t n = 0;
    lvm_status st = LVM_OK;

    if (argc != 2) return LVM_ERR_ARGS;
    s = lvm_string_data(args[0], &sl);
    sep = lvm_string_data(args[1], &pl);
    if (!s || !sep) return LVM_ERR_TYPE;
    if (pl == 0) return LVM_ERR_BOUNDS;
    sepcopy = malloc(pl);
    if (!sepcopy) return LVM_ERR_MEM;
    memcpy(sepcopy, sep, pl);

    arr = lvm_obj_array_new(vm, 8);
    if (lvm_is_nil(arr)) {
        free(sepcopy);
        return LVM_ERR_MEM;
    }
    args[1] = arr; /* keeps the array reachable; args[0] still holds the string */

    for (;;) {
        long at = find_bytes(s, sl, sepcopy, pl, start);
        size_t end = at < 0 ? sl : (size_t)at;
        lvm_value piece = lvm_new_string(vm, s + start, end - start);
        if (lvm_is_nil(piece) || lvm_obj_set(vm, arr, lvm_int((int64_t)n), piece) != LVM_OK) {
            st = LVM_ERR_MEM;
            break;
        }
        n++;
        if (at < 0) break;
        start = end + pl;
    }
    free(sepcopy);
    if (st == LVM_OK) args[0] = arr;
    return st;
}

/* ---- containers ---- */

static lvm_status host_array(void *ud, lvm_value *args, int argc)
{
    lvm_vm *vm = ((lvm_std *)ud)->vm;
    long long n, i;
    lvm_value fill = lvm_nil(), arr;

    if (argc < 1 || argc > 2) return LVM_ERR_ARGS;
    if (!lvm_is_int(args[0])) return LVM_ERR_TYPE;
    n = lvm_as_int(args[0]);
    if (n < 0 || n > (1 << 24)) return LVM_ERR_BOUNDS;
    if (argc == 2) fill = args[1];
    arr = lvm_obj_array_new(vm, (uint32_t)n);
    if (lvm_is_nil(arr)) return LVM_ERR_MEM;
    for (i = 0; i < n; i++) { /* no collection can run here */
        if (lvm_obj_set(vm, arr, lvm_int(i), fill) != LVM_OK) return LVM_ERR_MEM;
    }
    args[0] = arr;
    return LVM_OK;
}

static lvm_status host_map(void *ud, lvm_value *args, int argc)
{
    lvm_value m;

    (void)argc;
    m = lvm_obj_map_new(((lvm_std *)ud)->vm, 0);
    if (lvm_is_nil(m)) return LVM_ERR_MEM;
    args[0] = m;
    return LVM_OK;
}

static lvm_status host_pop(void *ud, lvm_value *args, int argc)
{
    lvm_array *a;

    (void)ud;
    if (argc != 1) return LVM_ERR_ARGS;
    if (!lvm_is_obj(args[0], LVM_OBJ_ARRAY)) return LVM_ERR_TYPE;
    a = (lvm_array *)lvm_to_obj(args[0]);
    if (a->len == 0) return LVM_ERR_BOUNDS;
    a->len--;
    args[0] = a->items[a->len];
    return LVM_OK;
}

static lvm_status host_keys(void *ud, lvm_value *args, int argc)
{
    lvm_vm *vm = ((lvm_std *)ud)->vm;
    const lvm_map *m;
    lvm_value arr;
    uint32_t i, n = 0;

    if (argc != 1) return LVM_ERR_ARGS;
    if (!lvm_is_obj(args[0], LVM_OBJ_MAP)) return LVM_ERR_TYPE;
    m = (const lvm_map *)lvm_to_obj(args[0]);
    arr = lvm_obj_array_new(vm, m->count); /* the map stays rooted in args[0] */
    if (lvm_is_nil(arr)) return LVM_ERR_MEM;
    for (i = 0; i < m->cap; i++) {
        if (m->tab[i].key == LVM_EMPTY_KEY) continue;
        if (lvm_obj_set(vm, arr, lvm_int((int64_t)n++), m->tab[i].key) != LVM_OK) return LVM_ERR_MEM;
    }
    args[0] = arr;
    return LVM_OK;
}

static int cmp_num(const void *x, const void *y)
{
    double a = 0, b = 0;

    arg_double(*(const lvm_value *)x, &a);
    arg_double(*(const lvm_value *)y, &b);
    return a < b ? -1 : (a > b ? 1 : 0);
}

static int cmp_str(const void *x, const void *y)
{
    const lvm_string *a = (const lvm_string *)lvm_to_obj(*(const lvm_value *)x);
    const lvm_string *b = (const lvm_string *)lvm_to_obj(*(const lvm_value *)y);
    size_t n = a->len < b->len ? a->len : b->len;
    int r = memcmp(a->data, b->data, n);

    if (r) return r;
    return a->len < b->len ? -1 : (a->len > b->len ? 1 : 0);
}

/* sort(a): in place, ascending; all numbers or all strings */
static lvm_status host_sort(void *ud, lvm_value *args, int argc)
{
    lvm_array *a;
    uint32_t i;
    int nums = 1, strs = 1;

    (void)ud;
    if (argc != 1) return LVM_ERR_ARGS;
    if (!lvm_is_obj(args[0], LVM_OBJ_ARRAY)) return LVM_ERR_TYPE;
    a = (lvm_array *)lvm_to_obj(args[0]);
    for (i = 0; i < a->len; i++) {
        double d;
        if (!arg_double(a->items[i], &d) || lvm_is_bool(a->items[i])) nums = 0;
        if (!lvm_is_string(a->items[i])) strs = 0;
    }
    if (!nums && !strs) return LVM_ERR_TYPE;
    if (a->len > 1) qsort(a->items, a->len, sizeof(lvm_value), nums ? cmp_num : cmp_str);
    return LVM_OK;
}

/* ---- math ---- */

#define MATH1(NAME, EXPR)                                                  \
static lvm_status NAME(void *ud, lvm_value *args, int argc)                \
{                                                                          \
    double x;                                                              \
    (void)ud;                                                              \
    if (argc != 1) return LVM_ERR_ARGS;                                    \
    if (!arg_double(args[0], &x)) return LVM_ERR_TYPE;                     \
    args[0] = lvm_float(EXPR);                                             \
    return LVM_OK;                                                         \
}

MATH1(host_sqrt, sqrt(x))
MATH1(host_sin, sin(x))
MATH1(host_cos, cos(x))
MATH1(host_tan, tan(x))
MATH1(host_exp, exp(x))
MATH1(host_log, log(x))

#define MATH2(NAME, EXPR)                                                  \
static lvm_status NAME(void *ud, lvm_value *args, int argc)                \
{                                                                          \
    double x, y;                                                           \
    (void)ud;                                                              \
    if (argc != 2) return LVM_ERR_ARGS;                                    \
    if (!arg_double(args[0], &x) || !arg_double(args[1], &y)) return LVM_ERR_TYPE; \
    args[0] = lvm_float(EXPR);                                             \
    return LVM_OK;                                                         \
}

MATH2(host_atan2, atan2(x, y))
MATH2(host_pow, pow(x, y))

/* floor and ceil give an int when the result fits */
static lvm_value round_result(double d)
{
    if (d == d && fits_double_int(d)) return lvm_int((int64_t)d);
    return lvm_float(d);
}

static lvm_status host_floor(void *ud, lvm_value *args, int argc)
{
    double x;

    (void)ud;
    if (argc != 1) return LVM_ERR_ARGS;
    if (!arg_double(args[0], &x)) return LVM_ERR_TYPE;
    args[0] = round_result(floor(x));
    return LVM_OK;
}

static lvm_status host_ceil(void *ud, lvm_value *args, int argc)
{
    double x;

    (void)ud;
    if (argc != 1) return LVM_ERR_ARGS;
    if (!arg_double(args[0], &x)) return LVM_ERR_TYPE;
    args[0] = round_result(ceil(x));
    return LVM_OK;
}

static lvm_status host_abs(void *ud, lvm_value *args, int argc)
{
    (void)ud;
    if (argc != 1) return LVM_ERR_ARGS;
    if (lvm_is_int(args[0])) {
        int64_t v = lvm_as_int(args[0]);
        args[0] = v < 0 ? int_or_float(-v) : args[0];
    } else if (lvm_is_float(args[0])) {
        args[0] = lvm_float(fabs(lvm_as_float(args[0])));
    } else {
        return LVM_ERR_TYPE;
    }
    return LVM_OK;
}

static lvm_status minmax(lvm_value *args, int argc, int want_max)
{
    double a, b;

    if (argc != 2) return LVM_ERR_ARGS;
    if (!arg_double(args[0], &a) || !arg_double(args[1], &b) ||
        lvm_is_bool(args[0]) || lvm_is_bool(args[1]))
        return LVM_ERR_TYPE;
    if (want_max ? b > a : b < a) args[0] = args[1];
    return LVM_OK;
}

static lvm_status host_min(void *ud, lvm_value *args, int argc)
{
    (void)ud;
    return minmax(args, argc, 0);
}

static lvm_status host_max(void *ud, lvm_value *args, int argc)
{
    (void)ud;
    return minmax(args, argc, 1);
}

static lvm_status host_rand(void *ud, lvm_value *args, int argc)
{
    lvm_std *s = ud;
    uint64_t x = s->rng;

    (void)argc;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    s->rng = x;
    args[0] = lvm_int((int64_t)((x * UINT64_C(0x2545F4914F6CDD1D)) >> 33)); /* 31 bits */
    return LVM_OK;
}

static lvm_status host_srand(void *ud, lvm_value *args, int argc)
{
    lvm_std *s = ud;
    long long v;

    if (argc != 1 || !arg_ll(args[0], &v)) return LVM_ERR_TYPE;
    s->rng = (uint64_t)v * UINT64_C(0x9E3779B97F4A7C15) + 1;
    args[0] = lvm_nil();
    return LVM_OK;
}

static lvm_status host_clock(void *ud, lvm_value *args, int argc)
{
    (void)ud;
    (void)argc;
    args[0] = lvm_float((double)clock() / (double)CLOCKS_PER_SEC);
    return LVM_OK;
}

/* ---- registration ---- */

static const struct { int id; lvm_host_fn fn; } std_table[] = {
    { LVM_STD_PRINT, host_print },   { LVM_STD_PRINTF, host_printf },
    { LVM_STD_SCAN, host_scan },     { LVM_STD_READLINE, host_readline },
    { LVM_STD_GETCHAR, host_getchar }, { LVM_STD_PUTCHAR, host_putchar },
    { LVM_STD_INT, host_int },       { LVM_STD_FLOAT, host_float },
    { LVM_STD_STR, host_str },       { LVM_STD_SUBSTR, host_substr },
    { LVM_STD_STRCMP, host_strcmp }, { LVM_STD_CHR, host_chr },
    { LVM_STD_FIND, host_find },     { LVM_STD_SPLIT, host_split },
    { LVM_STD_ARRAY, host_array },   { LVM_STD_MAP, host_map },
    { LVM_STD_POP, host_pop },       { LVM_STD_KEYS, host_keys },
    { LVM_STD_SORT, host_sort },
    { LVM_STD_SQRT, host_sqrt },     { LVM_STD_SIN, host_sin },
    { LVM_STD_COS, host_cos },       { LVM_STD_TAN, host_tan },
    { LVM_STD_ATAN2, host_atan2 },   { LVM_STD_POW, host_pow },
    { LVM_STD_EXP, host_exp },       { LVM_STD_LOG, host_log },
    { LVM_STD_FLOOR, host_floor },   { LVM_STD_CEIL, host_ceil },
    { LVM_STD_ABS, host_abs },       { LVM_STD_MIN, host_min },
    { LVM_STD_MAX, host_max },       { LVM_STD_RAND, host_rand },
    { LVM_STD_SRAND, host_srand },   { LVM_STD_CLOCK, host_clock }
};

lvm_std *lvm_std_new(lvm_vm *vm, const lvm_std_io *io)
{
    lvm_std *s = LVM_MEM(&vm->alloc, NULL, 0, sizeof *s);
    size_t i;

    if (!s) return NULL;
    memset(s, 0, sizeof *s);
    s->vm = vm;
    if (io) {
        s->io = *io;
    } else {
        s->io.read_char = stdio_read;
        s->io.write = stdio_write;
    }
    s->peeked = -2;
    s->rng = UINT64_C(0x2545F4914F6CDD1D);
    for (i = 0; i < sizeof std_table / sizeof std_table[0]; i++)
        lvm_set_host(vm, (unsigned)std_table[i].id, std_table[i].fn, s);
    return s;
}

void lvm_std_free(lvm_std *s)
{
    lvm_allocator a;

    if (!s) return;
    a = s->vm->alloc;
    LVM_MEM(&a, s, sizeof *s, 0);
}
