/*
 * Assembler: text -> bytecode image. Everything is allocated from an arena
 * that is released in one go, so errors can simply longjmp out.
 */
#include "lvm.h"
#include "lvm_opcode.h"
#include "internal.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#  define ASM_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#  define ASM_NORETURN __declspec(noreturn)
#else
#  define ASM_NORETURN
#endif

#define MAX_REG  254          /* nregs is a byte */
#define MAX_OPS  4
#define MAX_CODE (1u << 24)

typedef union block {
    struct { union block *next; size_t size; } h;
    double d;
    void *p;
    long long ll;             /* forces alignment */
} block;

typedef struct { char *name; uint32_t pc; } label;
typedef struct { uint32_t pc; char *name; int line; } fixup;

typedef struct {
    char *name;
    int line;
    unsigned nparams, regs, used;   /* used: highest register touched + 1 */
    lvm_updesc ups[255];
    unsigned nup;
    uint32_t *code;
    size_t ncode, capcode;
    label *labels;
    size_t nlabels, caplabels;
    fixup *jumps;                    /* local labels to patch */
    size_t njumps, capjumps;
    fixup *refs;                     /* function names to patch */
    size_t nrefs, caprefs;
} func;

typedef struct {
    int kind;                        /* 0 nil, 1 bool, 2 int, 3 float, 4 string */
    int64_t i;
    double d;
    char *s;
    size_t n;
} konst;

typedef struct {
    lvm_allocator a;
    block *blocks;
    jmp_buf jb;
    lvm_status status;
    char *err;
    size_t errsz;
    int line;
    func *funcs;
    size_t nfuncs, capfuncs;
    konst *consts;
    size_t nconsts, capconsts;
    uint8_t *out;
    size_t nout, capout;
} as;

/* ---- errors and memory ---- */

static ASM_NORETURN void die(as *s, const char *fmt, ...)
{
    va_list ap;

    if (s->errsz) {
        size_t n = 0;
        if (s->line > 0) {
            int w = snprintf(s->err, s->errsz, "line %d: ", s->line);
            n = w > 0 ? (size_t)w : 0;
        }
        if (n < s->errsz) {
            va_start(ap, fmt);
            vsnprintf(s->err + n, s->errsz - n, fmt, ap);
            va_end(ap);
        }
    }
    s->status = LVM_ERR_ASM;
    longjmp(s->jb, 1);
}

static void *zalloc(as *s, size_t n)
{
    block *b = LVM_MEM(&s->a, NULL, 0, n + sizeof *b);

    if (!b) {
        s->status = LVM_ERR_MEM;
        longjmp(s->jb, 1);
    }
    b->h.next = s->blocks;
    b->h.size = n + sizeof *b;
    s->blocks = b;
    memset(b + 1, 0, n);
    return b + 1;
}

static void release(as *s)
{
    block *b = s->blocks, *next;

    while (b) {
        next = b->h.next;
        LVM_MEM(&s->a, b, b->h.size, 0);
        b = next;
    }
    s->blocks = NULL;
}

static void *vgrow(as *s, void *v, size_t n, size_t *cap, size_t elt)
{
    size_t nc = *cap ? *cap * 2 : 16;
    void *nv = zalloc(s, nc * elt);

    if (n) memcpy(nv, v, n * elt);
    *cap = nc;
    return nv;
}

#define VPUSH(s, v, n, cap, x) do {                                       \
    if ((n) == (cap)) (v) = vgrow((s), (v), (n), &(cap), sizeof *(v));    \
    (v)[(n)++] = (x);                                                     \
} while (0)

static char *dup_str(as *s, const char *src)
{
    size_t n = strlen(src);
    char *d = zalloc(s, n + 1);

    memcpy(d, src, n);
    return d;
}

/* ---- characters ---- */

static int is_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_ident(int c) { return is_alpha(c) || is_digit(c) || c == '_' || c == '.' || c == '$'; }
static int upper(int c)    { return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c; }

static int hex_val(int c)
{
    if (is_digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (upper((unsigned char)*a) != upper((unsigned char)*b)) return 0;
    }
    return *a == *b;
}

/* ---- scanning a line ---- */

/* cut at ';', '#' or '//' outside string literals */
static void strip_comment(as *s, char *l)
{
    int in = 0;

    for (; *l; l++) {
        if (in) {
            if (*l == '\\' && l[1]) l++;
            else if (*l == '"') in = 0;
        } else if (*l == '"') {
            in = 1;
        } else if (*l == ';' || *l == '#' || (*l == '/' && l[1] == '/')) {
            *l = '\0';
            return;
        }
    }
    if (in) die(s, "unterminated string");
}

/* split "a, b, \"c,d\"" into operands, in place; returns the count */
static int split_ops(as *s, char *p, char **ops)
{
    int n = 0;

    while (is_space((unsigned char)*p)) p++;
    if (!*p) return 0;
    for (;;) {
        char *start = p, *e;
        int in = 0;

        while (*p && (in || *p != ',')) {
            if (in && *p == '\\' && p[1]) p++;
            else if (*p == '"') in = !in;
            p++;
        }
        e = p;
        while (e > start && is_space((unsigned char)e[-1])) e--;
        if (e == start) die(s, "empty operand");
        if (n == MAX_OPS) die(s, "too many operands");
        ops[n++] = start;
        if (*p == ',') {
            *e = '\0';
            p++;
            while (is_space((unsigned char)*p)) p++;
        } else {
            *e = '\0';
            break;
        }
    }
    return n;
}

/* ---- literals ---- */

/* decimal or 0x hex, optional sign; 0 if the text is not an integer */
static int parse_int(as *s, const char *t, int64_t *out)
{
    const char *p = t;
    int neg = 0, base = 10;
    char *end;
    unsigned long long v;

    if (*p == '+' || *p == '-') neg = (*p++ == '-');
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }
    if (hex_val((unsigned char)*p) < 0 || (base == 10 && !is_digit((unsigned char)*p))) return 0;
    v = strtoull(p, &end, base);
    if (*end) return 0;
    if (v > (unsigned long long)INT64_MAX) die(s, "integer '%s' is out of range", t);
    *out = neg ? -(int64_t)v : (int64_t)v;
    return 1;
}

static int parse_float(const char *t, double *out)
{
    const char *p;
    char *end;

    for (p = t; *p; p++) {
        if (!is_digit((unsigned char)*p) && !strchr("+-.eE", *p)) return 0;
    }
    if (!strpbrk(t, "0123456789")) return 0;
    *out = strtod(t, &end);
    return *end == '\0';
}

static char *parse_string(as *s, const char *t, size_t *len)
{
    size_t n = strlen(t), o = 0, k;
    char *out;

    if (n < 2 || t[n - 1] != '"') die(s, "unterminated string");
    out = zalloc(s, n);
    for (k = 1; k < n - 1; k++) {
        char c = t[k];
        if (c != '\\') {
            out[o++] = c;
            continue;
        }
        if (++k >= n - 1) die(s, "bad escape at the end of a string");
        switch (t[k]) {
        case 'n':  out[o++] = '\n'; break;
        case 't':  out[o++] = '\t'; break;
        case 'r':  out[o++] = '\r'; break;
        case '0':  out[o++] = '\0'; break;
        case '\\': out[o++] = '\\'; break;
        case '"':  out[o++] = '"'; break;
        case 'x': {
            int h = k + 2 < n ? hex_val((unsigned char)t[k + 1]) : -1;
            int l = k + 2 < n ? hex_val((unsigned char)t[k + 2]) : -1;
            if (h < 0 || l < 0) die(s, "\\x needs two hex digits");
            out[o++] = (char)(h * 16 + l);
            k += 2;
            break;
        }
        default:
            die(s, "unknown escape '\\%c'", t[k]);
        }
    }
    *len = o;
    return out;
}

static konst parse_literal(as *s, const char *t)
{
    konst k;

    memset(&k, 0, sizeof k);
    if (*t == '"') {
        k.kind = 4;
        k.s = parse_string(s, t, &k.n);
        if (k.n > (1u << 24)) die(s, "string constant is too long");
    } else if (ieq(t, "nil")) {
        k.kind = 0;
    } else if (ieq(t, "true") || ieq(t, "false")) {
        k.kind = 1;
        k.i = upper((unsigned char)t[0]) == 'T';
    } else if (parse_int(s, t, &k.i)) {
        k.kind = 2;
        if (!lvm_fits_int(k.i)) die(s, "integer constant '%s' does not fit in 48 bits", t);
    } else if (parse_float(t, &k.d)) {
        k.kind = 3;
    } else {
        die(s, "bad constant '%s'", t);
    }
    return k;
}

/* constants are shared: equal values get one pool slot */
static uint32_t intern(as *s, const konst *k)
{
    size_t j;

    for (j = 0; j < s->nconsts; j++) {
        const konst *o = &s->consts[j];
        if (o->kind != k->kind) continue;
        switch (k->kind) {
        case 0: return (uint32_t)j;
        case 1: case 2: if (o->i == k->i) return (uint32_t)j; break;
        case 3: if (memcmp(&o->d, &k->d, sizeof k->d) == 0) return (uint32_t)j; break;
        default: if (o->n == k->n && memcmp(o->s, k->s, k->n) == 0) return (uint32_t)j; break;
        }
    }
    if (s->nconsts >= 65536) die(s, "too many constants (max 65536)");
    VPUSH(s, s->consts, s->nconsts, s->capconsts, *k);
    return (uint32_t)(s->nconsts - 1);
}
