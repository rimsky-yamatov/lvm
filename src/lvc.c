/*
 * Compiler for lvc, a small C-like language. Output is assembly text for
 * lvm_assemble. Everything is allocated from an arena, so errors longjmp.
 */
#include "lvm.h"
#include "lvm_std.h"
#include "internal.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#  define CC_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#  define CC_NORETURN __declspec(noreturn)
#else
#  define CC_NORETURN
#endif

#define MAXR 254 /* highest register */

typedef union block {
    struct { union block *next; size_t size; } h;
    double d;
    void *p;
    long long ll;
} block;

typedef struct node node;

/* token kinds: single characters stand for themselves */
enum {
    TK_EOF = 256, TK_INT, TK_FLOAT, TK_STR, TK_IDENT,
    TK_EQ, TK_NE, TK_LE, TK_GE, TK_ANDAND, TK_OROR, TK_SHL, TK_SHR, TK_INC, TK_DEC,
    TK_ADDEQ, TK_SUBEQ, TK_MULEQ, TK_DIVEQ, TK_MODEQ, TK_ANDEQ, TK_OREQ, TK_XOREQ,
    TK_SHLEQ, TK_SHREQ,
    KW_IF, KW_ELSE, KW_WHILE, KW_DO, KW_FOR, KW_BREAK, KW_CONTINUE, KW_RETURN,
    KW_TRY, KW_CATCH, KW_THROW, KW_TRUE, KW_FALSE, KW_NIL, KW_TYPE
};

enum { T_INT = 1, T_FLOAT, T_BOOL, T_STRING, T_VAR, T_VOID, T_ARRAY, T_MAP };

typedef struct {
    int kind, line;
    int64_t ival;
    double fval;
    char *text;
    size_t len;
} tok;

enum {
    N_INT, N_FLOAT, N_STR, N_BOOL, N_NIL, N_VAR, N_INDEX, N_MEMBER, N_CALL, N_UNARY,
    N_BINARY, N_AND, N_OR, N_COND, N_ASSIGN, N_INCDEC, N_CAST, N_ARRAY, N_MAP, N_ENTRY,
    N_ADDR,
    N_BLOCK, N_DECL, N_DECLS, N_EXPR, N_IF, N_WHILE, N_DO, N_FOR, N_BREAK, N_CONTINUE,
    N_RETURN, N_TRY, N_THROW, N_EMPTY
};

struct node {
    int kind, line;
    int op;                 /* operator token, type id or flags */
    int64_t ival;
    double fval;
    char *str;
    size_t slen;
    node *a, *b, *c, *d;
    node *next;
};

typedef struct {
    char *name;
    int line, ret, nparams;
    node *params, *body;
} fn;

typedef struct { char *name; int idx; } global;
typedef struct { char *name; int reg; } local;
typedef struct { int brk, cont, tries; } loopctx;

typedef struct {
    lvm_allocator a;
    block *blocks;
    jmp_buf jb;
    lvm_status status;
    char *err;
    size_t errsz;
    int line;

    tok *toks;
    size_t ntoks, captoks, pos;

    fn *fns;
    size_t nfns, capfns;
    node *globals;
    global *gtab;
    size_t ngl, capgl;

    local *locals;
    size_t nloc, caploc;
    loopctx *loops;
    size_t nloops, caploops;
    int top, labels, tries;

    char *buf;              /* text being written */
    size_t nbuf, capbuf;
} cc;

/* ---- errors and memory ---- */

static CC_NORETURN void die(cc *c, const char *fmt, ...)
{
    va_list ap;

    if (c->errsz) {
        size_t n = 0;
        if (c->line > 0) {
            int w = snprintf(c->err, c->errsz, "line %d: ", c->line);
            n = w > 0 ? (size_t)w : 0;
        }
        if (n < c->errsz) {
            va_start(ap, fmt);
            vsnprintf(c->err + n, c->errsz - n, fmt, ap);
            va_end(ap);
        }
    }
    c->status = LVM_ERR_COMPILE;
    longjmp(c->jb, 1);
}

static void *zalloc(cc *c, size_t n)
{
    block *b = LVM_MEM(&c->a, NULL, 0, n + sizeof *b);

    if (!b) {
        c->status = LVM_ERR_MEM;
        longjmp(c->jb, 1);
    }
    b->h.next = c->blocks;
    b->h.size = n + sizeof *b;
    c->blocks = b;
    memset(b + 1, 0, n);
    return b + 1;
}

static void release(cc *c)
{
    block *b = c->blocks, *next;

    while (b) {
        next = b->h.next;
        LVM_MEM(&c->a, b, b->h.size, 0);
        b = next;
    }
    c->blocks = NULL;
}

static void *vgrow(cc *c, void *v, size_t n, size_t *cap, size_t elt)
{
    size_t nc = *cap ? *cap * 2 : 16;
    void *nv = zalloc(c, nc * elt);

    if (n) memcpy(nv, v, n * elt);
    *cap = nc;
    return nv;
}

#define VPUSH(c, v, n, cap, x) do {                                       \
    if ((n) == (cap)) (v) = vgrow((c), (v), (n), &(cap), sizeof *(v));    \
    (v)[(n)++] = (x);                                                     \
} while (0)

static char *dup_n(cc *c, const char *s, size_t n)
{
    char *d = zalloc(c, n + 1);

    if (n) memcpy(d, s, n);
    return d;
}

/* ---- lexer ---- */

static int is_digit(int ch) { return ch >= '0' && ch <= '9'; }
static int is_alpha(int ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }
static int is_alnum(int ch) { return is_alpha(ch) || is_digit(ch); }
static int hexv(int ch)
{
    if (is_digit(ch)) return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static const struct { const char *word; int kind, val; } keywords[] = {
    { "if", KW_IF, 0 }, { "else", KW_ELSE, 0 }, { "while", KW_WHILE, 0 }, { "do", KW_DO, 0 },
    { "for", KW_FOR, 0 }, { "break", KW_BREAK, 0 }, { "continue", KW_CONTINUE, 0 },
    { "return", KW_RETURN, 0 }, { "try", KW_TRY, 0 }, { "catch", KW_CATCH, 0 },
    { "throw", KW_THROW, 0 }, { "true", KW_TRUE, 0 }, { "false", KW_FALSE, 0 },
    { "nil", KW_NIL, 0 }, { "NULL", KW_NIL, 0 },
    { "int", KW_TYPE, T_INT }, { "long", KW_TYPE, T_INT }, { "short", KW_TYPE, T_INT },
    { "char", KW_TYPE, T_INT }, { "float", KW_TYPE, T_FLOAT }, { "double", KW_TYPE, T_FLOAT },
    { "bool", KW_TYPE, T_BOOL }, { "string", KW_TYPE, T_STRING }, { "var", KW_TYPE, T_VAR },
    { "auto", KW_TYPE, T_VAR }, { "void", KW_TYPE, T_VOID }, { "array", KW_TYPE, T_ARRAY },
    { "map", KW_TYPE, T_MAP }
};

/* qualifiers that carry no meaning here */
static const char *const ignored_words[] = { "const", "static", "unsigned", "signed", "register", "volatile" };

static tok *add_tok(cc *c, int kind, int line)
{
    tok t;

    memset(&t, 0, sizeof t);
    t.kind = kind;
    t.line = line;
    VPUSH(c, c->toks, c->ntoks, c->captoks, t);
    return &c->toks[c->ntoks - 1];
}

/* reads one (possibly escaped) character of a string or char literal */
static int lex_escape(cc *c, const char *s, size_t n, size_t *i)
{
    int ch = (unsigned char)s[(*i)++];

    if (ch != '\\') return ch;
    if (*i >= n) die(c, "unfinished escape sequence");
    ch = (unsigned char)s[(*i)++];
    switch (ch) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': return 0;
    case 'a': return 7;
    case 'b': return 8;
    case 'f': return 12;
    case 'v': return 11;
    case '\\': return '\\';
    case '\'': return '\'';
    case '"': return '"';
    case 'x': {
        int h = *i + 1 < n ? hexv((unsigned char)s[*i]) : -1;
        int l = *i + 1 < n ? hexv((unsigned char)s[*i + 1]) : -1;
        if (h < 0 || l < 0) die(c, "\\x needs two hex digits");
        *i += 2;
        return h * 16 + l;
    }
    default:
        die(c, "unknown escape '\\%c'", ch);
    }
}

static void lex_number(cc *c, const char *s, size_t n, size_t *ip)
{
    size_t i = *ip, start = i;
    int is_float = 0;
    tok *t;
    char *text;

    if (s[i] == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        unsigned long long v = 0;
        int digits = 0;
        i += 2;
        while (i < n && hexv((unsigned char)s[i]) >= 0) {
            if (v > (UINT64_MAX >> 4)) die(c, "integer literal is too large");
            v = v * 16 + (unsigned)hexv((unsigned char)s[i++]);
            digits = 1;
        }
        if (!digits) die(c, "hex literal has no digits");
        if (v > (unsigned long long)INT64_MAX) die(c, "integer literal is too large");
        t = add_tok(c, TK_INT, c->line);
        t->ival = (int64_t)v;
    } else {
        while (i < n && is_digit((unsigned char)s[i])) i++;
        if (i < n && s[i] == '.' && !(i + 1 < n && s[i + 1] == '.')) {
            is_float = 1;
            i++;
            while (i < n && is_digit((unsigned char)s[i])) i++;
        }
        if (i < n && (s[i] == 'e' || s[i] == 'E')) {
            size_t j = i + 1;
            if (j < n && (s[j] == '+' || s[j] == '-')) j++;
            if (j < n && is_digit((unsigned char)s[j])) {
                is_float = 1;
                i = j;
                while (i < n && is_digit((unsigned char)s[i])) i++;
            }
        }
        text = dup_n(c, s + start, i - start);
        if (is_float) {
            t = add_tok(c, TK_FLOAT, c->line);
            t->fval = strtod(text, NULL);
        } else {
            unsigned long long v = 0;
            size_t k;
            for (k = 0; text[k]; k++) {
                if (v > (UINT64_MAX - 9) / 10) die(c, "integer literal is too large");
                v = v * 10 + (unsigned)(text[k] - '0');
            }
            if (v > (unsigned long long)INT64_MAX) die(c, "integer literal is too large");
            t = add_tok(c, TK_INT, c->line);
            t->ival = (int64_t)v;
        }
    }
    while (i < n && strchr("fFlLuU", s[i])) i++; /* 1.5f, 10L, 7u */
    *ip = i;
}

static void lex(cc *c, const char *s, size_t n)
{
    size_t i = 0, k;

    c->line = 1;
    while (i < n) {
        int ch = (unsigned char)s[i];

        if (ch == '\n') { c->line++; i++; continue; }
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\v' || ch == '\f') { i++; continue; }
        if (ch == '#') { /* preprocessor lines such as #include are ignored */
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (ch == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (ch == '/' && i + 1 < n && s[i + 1] == '*') {
            int start_line = c->line;
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) {
                if (s[i] == '\n') c->line++;
                i++;
            }
            if (i + 1 >= n) {
                c->line = start_line;
                die(c, "unterminated comment");
            }
            i += 2;
            continue;
        }
        if (is_digit(ch) || (ch == '.' && i + 1 < n && is_digit((unsigned char)s[i + 1]))) {
            lex_number(c, s, n, &i);
            continue;
        }
        if (is_alpha(ch)) {
            size_t start = i;
            char *word;
            int found = 0;
            while (i < n && is_alnum((unsigned char)s[i])) i++;
            word = dup_n(c, s + start, i - start);
            for (k = 0; k < sizeof ignored_words / sizeof ignored_words[0]; k++) {
                if (strcmp(word, ignored_words[k]) == 0) found = 1;
            }
            if (found) continue;
            for (k = 0; k < sizeof keywords / sizeof keywords[0]; k++) {
                if (strcmp(word, keywords[k].word) == 0) {
                    tok *t = add_tok(c, keywords[k].kind, c->line);
                    t->ival = keywords[k].val;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                tok *t = add_tok(c, TK_IDENT, c->line);
                t->text = word;
                t->len = i - start;
            }
            continue;
        }
        if (ch == '"') {
            char *buf = zalloc(c, n - i + 1);
            size_t o = 0;
            tok *t;
            i++;
            while (i < n && s[i] != '"') {
                if (s[i] == '\n') die(c, "unterminated string");
                buf[o++] = (char)lex_escape(c, s, n, &i);
            }
            if (i >= n) die(c, "unterminated string");
            i++;
            t = add_tok(c, TK_STR, c->line);
            t->text = buf;
            t->len = o;
            continue;
        }
        if (ch == '\'') {
            tok *t;
            int v;
            i++;
            if (i >= n || s[i] == '\'' || s[i] == '\n') die(c, "bad character literal");
            v = lex_escape(c, s, n, &i);
            if (i >= n || s[i] != '\'') die(c, "unterminated character literal");
            i++;
            t = add_tok(c, TK_INT, c->line);
            t->ival = v;
            continue;
        }
        {
            static const struct { const char *op; int kind; } ops[] = {
                { "<<=", TK_SHLEQ }, { ">>=", TK_SHREQ },
                { "==", TK_EQ }, { "!=", TK_NE }, { "<=", TK_LE }, { ">=", TK_GE },
                { "&&", TK_ANDAND }, { "||", TK_OROR }, { "<<", TK_SHL }, { ">>", TK_SHR },
                { "++", TK_INC }, { "--", TK_DEC }, { "+=", TK_ADDEQ }, { "-=", TK_SUBEQ },
                { "*=", TK_MULEQ }, { "/=", TK_DIVEQ }, { "%=", TK_MODEQ }, { "&=", TK_ANDEQ },
                { "|=", TK_OREQ }, { "^=", TK_XOREQ }
            };
            int matched = 0;
            for (k = 0; k < sizeof ops / sizeof ops[0]; k++) {
                size_t l = strlen(ops[k].op);
                if (i + l <= n && memcmp(s + i, ops[k].op, l) == 0) {
                    add_tok(c, ops[k].kind, c->line);
                    i += l;
                    matched = 1;
                    break;
                }
            }
            if (matched) continue;
        }
        if (strchr("+-*/%&|^~!<>=?:;,.()[]{}", ch)) {
            add_tok(c, ch, c->line);
            i++;
            continue;
        }
        die(c, "unexpected character '%c'", ch);
    }
    add_tok(c, TK_EOF, c->line);
}
