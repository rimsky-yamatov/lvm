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

/* ---- instructions ---- */

static const char *const op_names[OP_COUNT] = {
    "MOV", "LDK", "LDI", "LDNIL", "LDBOOL",
    "ADD", "SUB", "MUL", "DIV", "MOD", "NEG",
    "AND", "OR", "XOR", "NOT", "SHL", "SHR",
    "EQ", "LT", "LE",
    "JMP", "JT", "JF",
    "CALL", "RET",
    "HOST",
    "NEWARR", "NEWMAP", "GET", "SET", "LEN",
    "CLOSURE", "GETUP", "SETUP", "CLOSE", "CALLC",
    "TRY", "ENDTRY", "THROW"
};

static int is_jump_op(uint32_t op)
{
    return op == OP_JMP || op == OP_JT || op == OP_JF || op == OP_TRY;
}

static uint32_t jump_target(uint32_t pc, uint32_t word)
{
    return (uint32_t)((long)pc + 1 + LVM_SBX(word));
}

static void put_label(sb *b, uint32_t pc)
{
    sb_fmt(b, ".L%lu", (unsigned long)pc);
}

/* Writes one instruction; returns how many words it consumed (1 or 2). */
static uint32_t put_insn(sb *b, const lvm_module *m, const lvm_func *f,
                         const unsigned char *is_target, uint32_t pc)
{
    uint32_t w = f->code[pc], op = LVM_OP(w);
    unsigned a = LVM_A(w), bb = LVM_B(w), c = LVM_C(w);

    /* a comparison and the jump it guards read better as one line */
    if ((op == OP_EQ || op == OP_LT || op == OP_LE) && pc + 1 < f->ncode &&
        LVM_OP(f->code[pc + 1]) == OP_JMP && !is_target[pc + 1] &&
        (a == 1 || (a == 0 && op == OP_EQ))) {
        const char *name = op == OP_EQ ? (a ? "JEQ" : "JNE") : (op == OP_LT ? "JLT" : "JLE");
        sb_fmt(b, "%s R%u, R%u, ", name, bb, c);
        put_label(b, jump_target(pc + 1, f->code[pc + 1]));
        return 2;
    }

    sb_str(b, op_names[op]);
    switch (op) {
    case OP_MOV: case OP_NEG: case OP_NOT: case OP_LEN:
        sb_fmt(b, " R%u, R%u", a, bb);
        break;
    case OP_LDK:
        sb_fmt(b, " R%u, ", a);
        put_const(b, m->consts[LVM_BX(w)]);
        break;
    case OP_LDI:
        sb_fmt(b, " R%u, %d", a, LVM_SBX(w));
        break;
    case OP_LDNIL:
        sb_fmt(b, " R%u", a);
        if (bb) sb_fmt(b, ", R%u", a + bb);
        break;
    case OP_LDBOOL:
        sb_fmt(b, " R%u, %s", a, bb ? "true" : "false");
        break;
    case OP_EQ: case OP_LT: case OP_LE:
        sb_fmt(b, " %u, R%u, R%u", a, bb, c);
        break;
    case OP_JMP:
        sb_str(b, " ");
        put_label(b, jump_target(pc, w));
        break;
    case OP_JT: case OP_JF: case OP_TRY:
        sb_fmt(b, " R%u, ", a);
        put_label(b, jump_target(pc, w));
        break;
    case OP_CALL: case OP_CLOSURE:
        sb_fmt(b, " R%u, ", a);
        put_func_name(b, m, LVM_BX(w));
        break;
    case OP_RET: case OP_CLOSE: case OP_THROW:
        sb_fmt(b, " R%u", a);
        break;
    case OP_ENDTRY:
        break;
    case OP_HOST:
        sb_fmt(b, " R%u, %u, %u", a, bb, c);
        break;
    case OP_NEWARR: case OP_NEWMAP:
        sb_fmt(b, " R%u", a);
        if (bb) sb_fmt(b, ", %u", bb);
        break;
    case OP_GETUP: case OP_SETUP: case OP_CALLC:
        sb_fmt(b, " R%u, %u", a, bb);
        break;
    default: /* arithmetic, bit ops, GET, SET */
        sb_fmt(b, " R%u, R%u, R%u", a, bb, c);
        break;
    }
    return 1;
}

static void put_func(sb *b, const lvm_module *m, uint32_t idx, int flags)
{
    const lvm_func *f = &m->funcs[idx];
    unsigned char *is_target;
    uint32_t pc, k;

    is_target = LVM_MEM(b->a, NULL, 0, f->ncode);
    if (!is_target) {
        b->failed = 1;
        return;
    }
    memset(is_target, 0, f->ncode);
    for (pc = 0; pc < f->ncode; pc++) {
        if (is_jump_op(LVM_OP(f->code[pc]))) is_target[jump_target(pc, f->code[pc])] = 1;
    }

    put_func_name(b, m, idx);
    sb_str(b, ":\n");
    if (f->nparams) sb_fmt(b, "    .params %u\n", f->nparams);
    sb_fmt(b, "    .regs %u\n", f->nregs);
    for (k = 0; k < f->nupvals; k++) {
        if (f->upvals[k].instack) sb_fmt(b, "    .upval stack R%u\n", f->upvals[k].index);
        else sb_fmt(b, "    .upval up %u\n", f->upvals[k].index);
    }

    for (pc = 0; pc < f->ncode;) {
        uint32_t n, i;
        size_t start;
        if (is_target[pc]) {
            put_label(b, pc);
            sb_str(b, ":\n");
        }
        start = b->len;
        sb_str(b, "    ");
        n = put_insn(b, m, f, is_target, pc);
        if (flags & LVM_DIS_ADDR) {
            size_t col = b->len - start;
            if (col < 32) sb_put(b, "                                ", 32 - col);
            sb_fmt(b, " ; %04lu:", (unsigned long)pc);
            for (i = 0; i < n; i++) sb_fmt(b, " %08lX", (unsigned long)f->code[pc + i]);
        }
        sb_str(b, "\n");
        pc += n;
    }
    LVM_MEM(b->a, is_target, f->ncode, 0);
}

lvm_status lvm_disassemble(const lvm_allocator *a, const lvm_module *m, int flags,
                           char **out, size_t *len)
{
    sb b;
    uint32_t i;

    if (!a) a = lvm_default_allocator();
    memset(&b, 0, sizeof b);
    b.a = a;
    for (i = 0; i < m->nfuncs; i++) {
        if (i) sb_str(&b, "\n");
        put_func(&b, m, i, flags);
    }
    if (b.failed) {
        if (b.buf) LVM_MEM(a, b.buf, b.cap, 0);
        *out = NULL;
        *len = 0;
        return LVM_ERR_MEM;
    }
    *out = b.buf;
    *len = b.len;
    return LVM_OK;
}

void lvm_disassemble_free(const lvm_allocator *a, char *text, size_t len)
{
    if (!a) a = lvm_default_allocator();
    /* the buffer was grown in powers of two from 256 */
    if (text) {
        size_t cap = 256;
        while (cap < len + 1) cap *= 2;
        LVM_MEM(a, text, cap, 0);
    }
}
