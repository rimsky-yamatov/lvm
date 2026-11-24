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

/* ---- functions, operands, emission ---- */

static func *cur(as *s)
{
    if (!s->nfuncs)
        die(s, "instruction or directive outside a function (start one with 'name:')");
    return &s->funcs[s->nfuncs - 1];
}

static void note_reg(as *s, unsigned r)
{
    func *f = cur(s);

    if (r > MAX_REG) die(s, "register R%u is out of range (R0..R%d)", r, MAX_REG);
    if (r + 1 > f->used) f->used = r + 1;
}

/* "R12" -> 12 without side effects; 0 if the text is not a register */
static int reg_number(const char *t, unsigned *out)
{
    unsigned v = 0;
    const char *p = t;

    if (*p != 'R' && *p != 'r') return 0;
    p++;
    if (!is_digit((unsigned char)*p)) return 0;
    while (is_digit((unsigned char)*p)) {
        v = v * 10 + (unsigned)(*p - '0');
        if (v > 999) return 0;
        p++;
    }
    if (*p) return 0;
    *out = v;
    return 1;
}

static unsigned reg(as *s, const char *t)
{
    unsigned v;

    if (!reg_number(t, &v)) die(s, "expected a register (R0..R%d), got '%s'", MAX_REG, t);
    note_reg(s, v);
    return v;
}

static unsigned imm8(as *s, const char *t)
{
    int64_t v;

    if (!parse_int(s, t, &v) || v < 0 || v > 255) die(s, "expected a number 0..255, got '%s'", t);
    return (unsigned)v;
}

static int32_t imm16(as *s, const char *t)
{
    int64_t v;

    if (!parse_int(s, t, &v)) die(s, "expected an integer, got '%s'", t);
    if (v < -32768 || v > 32767) die(s, "%s does not fit LDI (16 bits); use LDK", t);
    return (int32_t)v;
}

static unsigned boolean(as *s, const char *t)
{
    if (ieq(t, "true") || strcmp(t, "1") == 0) return 1;
    if (ieq(t, "false") || strcmp(t, "0") == 0) return 0;
    die(s, "expected true/false (or 1/0), got '%s'", t);
}

static void emit(as *s, uint32_t w)
{
    func *f = cur(s);

    if (f->ncode >= MAX_CODE) die(s, "function '%s' is too large", f->name);
    VPUSH(s, f->code, f->ncode, f->capcode, w);
}

/* remember that the word just emitted needs a local label or function */
static void add_fixup(as *s, int is_jump, const char *name)
{
    func *f = cur(s);
    fixup x;

    x.pc = (uint32_t)(f->ncode - 1);
    x.name = dup_str(s, name);
    x.line = s->line;
    if (is_jump) VPUSH(s, f->jumps, f->njumps, f->capjumps, x);
    else VPUSH(s, f->refs, f->nrefs, f->caprefs, x);
}

typedef struct {
    const char *name;
    int op;
    const char *sig;    /* r register, i 0..255, b bool, S int16, K constant, F function, L label */
} insn_def;

/* Register-like operands fill A, B, C in order. S, K, F and L fill the
 * 16-bit field and must come last. */
static const insn_def insns[] = {
    { "MOV", OP_MOV, "rr" },       { "LDK", OP_LDK, "rK" },
    { "LDI", OP_LDI, "rS" },       { "LDBOOL", OP_LDBOOL, "rb" },
    { "ADD", OP_ADD, "rrr" },      { "SUB", OP_SUB, "rrr" },
    { "MUL", OP_MUL, "rrr" },      { "DIV", OP_DIV, "rrr" },
    { "MOD", OP_MOD, "rrr" },      { "NEG", OP_NEG, "rr" },
    { "AND", OP_AND, "rrr" },      { "OR", OP_OR, "rrr" },
    { "XOR", OP_XOR, "rrr" },      { "NOT", OP_NOT, "rr" },
    { "SHL", OP_SHL, "rrr" },      { "SHR", OP_SHR, "rrr" },
    { "EQ", OP_EQ, "brr" },        { "LT", OP_LT, "brr" },
    { "LE", OP_LE, "brr" },
    { "JMP", OP_JMP, "L" },        { "JT", OP_JT, "rL" },
    { "JF", OP_JF, "rL" },
    { "CALL", OP_CALL, "rF" },     { "RET", OP_RET, "r" },
    { "HOST", OP_HOST, "rii" },
    { "GET", OP_GET, "rrr" },      { "SET", OP_SET, "rrr" },
    { "LEN", OP_LEN, "rr" },
    { "CLOSURE", OP_CLOSURE, "rF" },
    { "GETUP", OP_GETUP, "ri" },   { "SETUP", OP_SETUP, "ri" },
    { "CLOSE", OP_CLOSE, "r" },    { "CALLC", OP_CALLC, "ri" },
    { "TRY", OP_TRY, "rL" },       { "ENDTRY", OP_ENDTRY, "" },
    { "THROW", OP_THROW, "r" }
};

/* compare-and-jump: a comparison followed by the JMP it guards */
static const struct { const char *name; int op; int flag; int swap; } jcc[] = {
    { "JEQ", OP_EQ, 1, 0 }, { "JNE", OP_EQ, 0, 0 },
    { "JLT", OP_LT, 1, 0 }, { "JLE", OP_LE, 1, 0 },
    { "JGT", OP_LT, 1, 1 }, { "JGE", OP_LE, 1, 1 }
};

static void emit_sig(as *s, const insn_def *d, char **ops, int n)
{
    int want = (int)strlen(d->sig), k, nv = 0;
    unsigned v[3] = { 0, 0, 0 };
    char kind = 0;
    const char *name = NULL;
    int32_t sbx = 0;
    uint32_t bx = 0;

    if (n != want) die(s, "%s takes %d operand(s), got %d", d->name, want, n);
    for (k = 0; k < want; k++) {
        switch (d->sig[k]) {
        case 'r': v[nv++] = reg(s, ops[k]); break;
        case 'b': v[nv++] = boolean(s, ops[k]); break;
        case 'i': v[nv++] = imm8(s, ops[k]); break;
        case 'S': kind = 'S'; sbx = imm16(s, ops[k]); break;
        case 'K': {
            konst c = parse_literal(s, ops[k]);
            kind = 'K';
            bx = intern(s, &c);
            break;
        }
        default: kind = d->sig[k]; name = ops[k]; break; /* F, L */
        }
    }

    /* the frame must be big enough for the registers an instruction names */
    if (d->op == OP_HOST) note_reg(s, v[0] + (v[1] ? v[1] - 1 : 0));
    if (d->op == OP_CALLC) note_reg(s, v[0] + v[1]);

    switch (kind) {
    case 0:   emit(s, LVM_ABC(d->op, v[0], v[1], v[2])); break;
    case 'S': emit(s, LVM_ASBX(d->op, v[0], sbx)); break;
    case 'K': emit(s, LVM_ABX(d->op, v[0], bx)); break;
    default:
        emit(s, LVM_ABX(d->op, v[0], 0));
        add_fixup(s, kind == 'L', name);
        break;
    }
}

static void instruction(as *s, const char *w, char **ops, int n)
{
    size_t i;

    cur(s); /* must be inside a function */

    if (ieq(w, "LDNIL")) { /* LDNIL Ra  or  LDNIL Ra, Rb  (the range a..b) */
        unsigned a, b = 0;
        if (n < 1 || n > 2) die(s, "LDNIL takes 1 or 2 registers");
        a = reg(s, ops[0]);
        if (n == 2) {
            unsigned last = reg(s, ops[1]);
            if (last < a) die(s, "LDNIL range is backwards");
            b = last - a;
        }
        emit(s, LVM_ABC(OP_LDNIL, a, b, 0));
        return;
    }
    if (ieq(w, "NEWARR") || ieq(w, "NEWMAP")) { /* optional size hint */
        unsigned a, hint = 0;
        if (n < 1 || n > 2) die(s, "%s takes a register and an optional size hint", w);
        a = reg(s, ops[0]);
        if (n == 2) hint = imm8(s, ops[1]);
        emit(s, LVM_ABC(ieq(w, "NEWARR") ? OP_NEWARR : OP_NEWMAP, a, hint, 0));
        return;
    }
    for (i = 0; i < sizeof jcc / sizeof jcc[0]; i++) {
        if (ieq(w, jcc[i].name)) {
            unsigned b, c;
            if (n != 3) die(s, "%s takes 2 registers and a label", jcc[i].name);
            b = reg(s, ops[0]);
            c = reg(s, ops[1]);
            emit(s, LVM_ABC(jcc[i].op, jcc[i].flag, jcc[i].swap ? c : b, jcc[i].swap ? b : c));
            emit(s, LVM_ASBX(OP_JMP, 0, 0));
            add_fixup(s, 1, ops[2]);
            return;
        }
    }
    for (i = 0; i < sizeof insns / sizeof insns[0]; i++) {
        if (ieq(w, insns[i].name)) {
            emit_sig(s, &insns[i], ops, n);
            return;
        }
    }
    die(s, "unknown instruction '%s'", w);
}

static void directive(as *s, const char *w, char **ops, int n)
{
    func *f = cur(s);
    int64_t v;

    if (ieq(w, ".params") || ieq(w, ".regs")) {
        if (n != 1 || !parse_int(s, ops[0], &v) || v < 0 || v > 255)
            die(s, "%s takes a number 0..255", w);
        if (ieq(w, ".params")) f->nparams = (unsigned)v;
        else f->regs = (unsigned)v;
    } else if (ieq(w, ".upval")) {
        /* .upval stack Rn: capture register n of the creating function
         * .upval up n:     copy upvalue n of the creating closure */
        unsigned idx;
        int in_stack;
        if (n == 1) { /* "stack R0": split at the first blank */
            char *sp = ops[0];
            while (*sp && !is_space((unsigned char)*sp)) sp++;
            if (*sp) {
                *sp++ = '\0';
                while (is_space((unsigned char)*sp)) sp++;
                ops[1] = sp;
                n = 2;
            }
        }
        if (n != 2) die(s, ".upval takes 'stack <register>' or 'up <index>'");
        in_stack = ieq(ops[0], "stack");
        if (!in_stack && !ieq(ops[0], "up")) die(s, ".upval kind must be 'stack' or 'up'");
        if (!reg_number(ops[1], &idx)) {
            if (!parse_int(s, ops[1], &v) || v < 0 || v > 255)
                die(s, ".upval index must be R0..R%d or 0..255", MAX_REG);
            idx = (unsigned)v;
        }
        if (idx > 255) die(s, ".upval index is out of range");
        if (f->nup == 255) die(s, "too many upvalues");
        f->ups[f->nup].instack = (uint8_t)(in_stack ? 1 : 0);
        f->ups[f->nup].index = (uint8_t)idx;
        f->nup++;
    } else {
        die(s, "unknown directive '%s'", w);
    }
}

/* ---- parsing ---- */

static void define_label(as *s, const char *name)
{
    size_t i;

    if (name[0] == '.') { /* local label */
        func *f = cur(s);
        label l;
        for (i = 0; i < f->nlabels; i++) {
            if (strcmp(f->labels[i].name, name) == 0) die(s, "duplicate label '%s'", name);
        }
        l.name = dup_str(s, name);
        l.pc = (uint32_t)f->ncode;
        VPUSH(s, f->labels, f->nlabels, f->caplabels, l);
    } else { /* new function */
        func f;
        for (i = 0; i < s->nfuncs; i++) {
            if (strcmp(s->funcs[i].name, name) == 0) die(s, "duplicate function '%s'", name);
        }
        memset(&f, 0, sizeof f);
        f.name = dup_str(s, name);
        f.line = s->line;
        VPUSH(s, s->funcs, s->nfuncs, s->capfuncs, f);
    }
}

static void parse(as *s, const char *src, size_t len)
{
    size_t pos = 0, i, run = 0, maxlen = 0;
    char *line;

    for (i = 0; i < len; i++) {
        run = src[i] == '\n' ? 0 : run + 1;
        if (run > maxlen) maxlen = run;
    }
    line = zalloc(s, maxlen + 1);

    while (pos < len) {
        size_t e = pos, n;
        char *p, *q, *w, *ops[MAX_OPS];
        int nops;

        while (e < len && src[e] != '\n') e++;
        n = e - pos;
        memcpy(line, src + pos, n);
        line[n] = '\0';
        pos = e < len ? e + 1 : e;
        s->line++;

        strip_comment(s, line);
        p = line;
        while (is_space((unsigned char)*p)) p++;
        if (!*p) continue;

        q = p;
        while (is_ident((unsigned char)*q)) q++;
        if (q > p && *q == ':') { /* a label, optionally followed by a statement */
            *q = '\0';
            define_label(s, p);
            p = q + 1;
            while (is_space((unsigned char)*p)) p++;
            if (!*p) continue;
        }

        w = p;
        while (*p && !is_space((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
        nops = split_ops(s, p, ops);
        if (w[0] == '.') directive(s, w, ops, nops);
        else instruction(s, w, ops, nops);
    }
    s->line = 0;
}

/* ---- resolving names ---- */

static void patch16(uint32_t *w, uint32_t v)
{
    *w = (*w & 0xFFFFu) | ((v & 0xFFFFu) << 16);
}

static void resolve(as *s)
{
    size_t fi, k, m;

    if (!s->nfuncs) die(s, "no functions");

    for (fi = 0; fi < s->nfuncs; fi++) {
        func *f = &s->funcs[fi];
        unsigned n;

        if (!f->ncode) {
            s->line = f->line;
            die(s, "function '%s' has no instructions", f->name);
        }

        for (k = 0; k < f->njumps; k++) {
            const fixup *j = &f->jumps[k];
            long target = -1, off;

            for (m = 0; m < f->nlabels; m++) {
                if (strcmp(f->labels[m].name, j->name) == 0) {
                    target = (long)f->labels[m].pc;
                    break;
                }
            }
            s->line = j->line;
            if (target < 0) die(s, "undefined label '%s'", j->name);
            if (target >= (long)f->ncode) die(s, "label '%s' is past the last instruction", j->name);
            off = target - ((long)j->pc + 1);
            if (off < -32768 || off > 32767) die(s, "jump to '%s' is too far", j->name);
            patch16(&f->code[j->pc], (uint32_t)off);
        }

        for (k = 0; k < f->nrefs; k++) {
            const fixup *r = &f->refs[k];
            const func *callee = NULL;
            uint32_t word = f->code[r->pc];
            size_t idx = 0;

            for (m = 0; m < s->nfuncs; m++) {
                if (strcmp(s->funcs[m].name, r->name) == 0) {
                    callee = &s->funcs[m];
                    idx = m;
                    break;
                }
            }
            s->line = r->line;
            if (!callee) die(s, "undefined function '%s'", r->name);
            if (idx > 65535) die(s, "too many functions");
            patch16(&f->code[r->pc], (uint32_t)idx);

            if (LVM_OP(word) == OP_CALL) {
                if (callee->nup)
                    die(s, "'%s' has upvalues: create it with CLOSURE and call it with CALLC", r->name);
                /* arguments live in R[A..A+nparams-1] */
                if (callee->nparams && LVM_A(word) + callee->nparams > f->used)
                    f->used = LVM_A(word) + callee->nparams;
            } else { /* CLOSURE: the captured registers must exist */
                for (m = 0; m < callee->nup; m++) {
                    if (callee->ups[m].instack && (unsigned)callee->ups[m].index + 1 > f->used)
                        f->used = (unsigned)callee->ups[m].index + 1;
                }
            }
        }

        n = f->regs;
        if (f->used > n) n = f->used;
        if (f->nparams > n) n = f->nparams;
        if (!n) n = 1;
        if (n > 255) {
            s->line = f->line;
            die(s, "function '%s' needs %u registers (max 255)", f->name, n);
        }
        f->regs = n;
    }
}

/* ---- output ---- */

static void put8(as *s, unsigned v)
{
    VPUSH(s, s->out, s->nout, s->capout, (uint8_t)v);
}

static void put16(as *s, unsigned v) { put8(s, v & 0xFF); put8(s, (v >> 8) & 0xFF); }
static void put32(as *s, uint32_t v) { put16(s, v & 0xFFFFu); put16(s, v >> 16); }
static void put64(as *s, uint64_t v) { put32(s, (uint32_t)v); put32(s, (uint32_t)(v >> 32)); }

static void build(as *s)
{
    size_t i, j;
    uint32_t entry = 0;

    for (i = 0; i < s->nfuncs; i++) {
        if (strcmp(s->funcs[i].name, "main") == 0) entry = (uint32_t)i; /* else the first */
    }

    put8(s, 'L'); put8(s, 'V'); put8(s, 'M'); put8(s, 'B');
    put16(s, LVM_VERSION);
    put16(s, 0);
    put32(s, (uint32_t)s->nconsts);
    put32(s, (uint32_t)s->nfuncs);
    put32(s, entry);

    for (i = 0; i < s->nconsts; i++) {
        const konst *k = &s->consts[i];
        put8(s, (unsigned)k->kind);
        switch (k->kind) {
        case 1: put8(s, (unsigned)k->i); break;
        case 2: put64(s, (uint64_t)k->i); break;
        case 3: {
            uint64_t bits;
            memcpy(&bits, &k->d, sizeof bits);
            put64(s, bits);
            break;
        }
        case 4:
            put32(s, (uint32_t)k->n);
            for (j = 0; j < k->n; j++) put8(s, (unsigned char)k->s[j]);
            break;
        default: break;
        }
    }

    for (i = 0; i < s->nfuncs; i++) {
        const func *f = &s->funcs[i];
        put8(s, f->nparams);
        put8(s, f->regs);
        put8(s, f->nup);
        for (j = 0; j < f->nup; j++) {
            put8(s, f->ups[j].instack);
            put8(s, f->ups[j].index);
        }
        put32(s, (uint32_t)f->ncode);
        for (j = 0; j < f->ncode; j++) put32(s, f->code[j]);
    }
}

void lvm_assemble_free(const lvm_allocator *a, uint8_t *buf, size_t len)
{
    if (!a) a = lvm_default_allocator();
    if (buf) LVM_MEM(a, buf, len, 0);
}

/* errors longjmp back here; nothing else lives in this frame */
static void assemble_all(as *s, const char *src, size_t len)
{
    if (setjmp(s->jb) == 0) {
        parse(s, src, len);
        resolve(s);
        build(s);
    }
}

lvm_status lvm_assemble(const lvm_allocator *a, const char *src, size_t len,
                        uint8_t **out, size_t *out_len, char *err, size_t err_size)
{
    as *s;
    lvm_status st;
    lvm_module m;

    if (!a) a = lvm_default_allocator();
    *out = NULL;
    *out_len = 0;
    if (err_size) err[0] = '\0';

    s = LVM_MEM(a, NULL, 0, sizeof *s);
    if (!s) return LVM_ERR_MEM;
    memset(s, 0, sizeof *s);
    s->a = *a;
    s->err = err;
    s->errsz = err_size;

    assemble_all(s, src, len);
    if (s->status == LVM_OK) {
        uint8_t *buf = LVM_MEM(a, NULL, 0, s->nout);
        if (!buf) {
            s->status = LVM_ERR_MEM;
        } else {
            memcpy(buf, s->out, s->nout);
            *out = buf;
            *out_len = s->nout;
        }
    }
    st = s->status;
    release(s);
    LVM_MEM(a, s, sizeof *s, 0);
    if (st != LVM_OK) return st;

    /* the verifier has the last word */
    st = lvm_load(a, *out, *out_len, &m);
    if (st == LVM_OK) {
        lvm_module_free(&m);
        return LVM_OK;
    }
    if (err_size)
        snprintf(err, err_size, "rejected by the verifier (%s): check register numbers, "
                 "jumps and that every function ends with RET or JMP", lvm_strerror(st));
    lvm_assemble_free(a, *out, *out_len);
    *out = NULL;
    *out_len = 0;
    return st;
}
