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
    size_t scope_base;      /* first local of the innermost block */

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

/* ---- parser ---- */

static tok *cur(cc *c)
{
    return &c->toks[c->pos];
}

static tok *peek_at(cc *c, size_t k)
{
    size_t i = c->pos + k;

    if (i >= c->ntoks) i = c->ntoks - 1;
    return &c->toks[i];
}

static int at(cc *c, int kind)
{
    return cur(c)->kind == kind;
}

static tok *advance(cc *c)
{
    tok *t = cur(c);

    if (t->kind != TK_EOF) c->pos++;
    c->line = t->line;
    return t;
}

static int accept(cc *c, int kind)
{
    if (!at(c, kind)) return 0;
    advance(c);
    return 1;
}

static void expect(cc *c, int kind, const char *what)
{
    if (!at(c, kind)) {
        c->line = cur(c)->line;
        die(c, "expected %s", what);
    }
    advance(c);
}

static node *mk(cc *c, int kind)
{
    node *n = zalloc(c, sizeof *n);

    n->kind = kind;
    n->line = c->line;
    return n;
}

static int is_lvalue(const node *n)
{
    return n->kind == N_VAR || n->kind == N_INDEX || n->kind == N_MEMBER;
}

static node *parse_assign(cc *c);
static node *parse_expr(cc *c);
static node *parse_stmt(cc *c);
static node *parse_block(cc *c);

/* the argument list of a call; the '(' has been consumed */
static node *parse_args(cc *c)
{
    node *head = NULL, *tail = NULL;

    if (accept(c, ')')) return NULL;
    do {
        node *a = parse_assign(c);
        if (tail) tail->next = a;
        else head = a;
        tail = a;
    } while (accept(c, ','));
    expect(c, ')', "')' after the arguments");
    return head;
}

static node *parse_primary(cc *c)
{
    tok *t = cur(c);
    node *n;

    c->line = t->line;
    switch (t->kind) {
    case TK_INT:
        advance(c);
        n = mk(c, N_INT);
        n->ival = t->ival;
        return n;
    case TK_FLOAT:
        advance(c);
        n = mk(c, N_FLOAT);
        n->fval = t->fval;
        return n;
    case TK_STR:
        advance(c);
        n = mk(c, N_STR);
        n->str = t->text;
        n->slen = t->len;
        return n;
    case KW_TRUE: case KW_FALSE:
        advance(c);
        n = mk(c, N_BOOL);
        n->ival = t->kind == KW_TRUE;
        return n;
    case KW_NIL:
        advance(c);
        return mk(c, N_NIL);
    case TK_IDENT:
        advance(c);
        if (accept(c, '(')) {
            n = mk(c, N_CALL);
            n->str = t->text;
            n->a = parse_args(c);
            return n;
        }
        n = mk(c, N_VAR);
        n->str = t->text;
        return n;
    case KW_TYPE: { /* int(x), float(x), string(x), array(n), map() */
        const char *name = NULL;
        if (peek_at(c, 1)->kind != '(') die(c, "unexpected type name");
        switch (t->ival) {
        case T_INT: name = "int"; break;
        case T_FLOAT: name = "float"; break;
        case T_STRING: name = "str"; break;
        case T_ARRAY: name = "array"; break;
        case T_MAP: name = "map"; break;
        default: die(c, "this type cannot be used like a function");
        }
        advance(c);
        advance(c);
        n = mk(c, N_CALL);
        n->str = (char *)name;
        n->a = parse_args(c);
        return n;
    }
    case '(':
        advance(c);
        n = parse_expr(c);
        expect(c, ')', "')'");
        return n;
    case '[': { /* array literal */
        node *tail = NULL;
        advance(c);
        n = mk(c, N_ARRAY);
        while (!at(c, ']')) {
            node *e = parse_assign(c);
            if (tail) tail->next = e;
            else n->a = e;
            tail = e;
            if (!accept(c, ',')) break;
        }
        expect(c, ']', "']'");
        return n;
    }
    case '{': { /* {1, 2, 3} is an array, {"k": v} a map, {} an empty map */
        node *first, *tail = NULL;
        advance(c);
        if (accept(c, '}')) return mk(c, N_MAP);
        first = parse_assign(c);
        if (at(c, ':')) {
            node *e;
            n = mk(c, N_MAP);
            for (;;) {
                e = mk(c, N_ENTRY);
                e->a = first;
                expect(c, ':', "':' after the key");
                e->b = parse_assign(c);
                if (tail) tail->next = e;
                else n->a = e;
                tail = e;
                if (!accept(c, ',') || at(c, '}')) break;
                first = parse_assign(c);
            }
        } else {
            n = mk(c, N_ARRAY);
            n->a = first;
            tail = first;
            while (accept(c, ',') && !at(c, '}')) {
                node *e = parse_assign(c);
                tail->next = e;
                tail = e;
            }
        }
        expect(c, '}', "'}'");
        return n;
    }
    default:
        die(c, "expected an expression");
    }
}

static node *parse_postfix(cc *c)
{
    node *n = parse_primary(c);

    for (;;) {
        if (at(c, '[')) {
            node *i;
            advance(c);
            i = mk(c, N_INDEX);
            i->a = n;
            i->b = parse_expr(c);
            expect(c, ']', "']'");
            n = i;
        } else if (at(c, '.')) {
            node *m;
            advance(c);
            if (!at(c, TK_IDENT)) die(c, "expected a field name after '.'");
            m = mk(c, N_MEMBER);
            m->a = n;
            m->str = advance(c)->text;
            n = m;
        } else if (at(c, TK_INC) || at(c, TK_DEC)) {
            node *p;
            if (!is_lvalue(n)) die(c, "++ and -- need a variable, element or field");
            p = mk(c, N_INCDEC);
            p->op = advance(c)->kind;
            p->a = n;
            n = p;
        } else {
            return n;
        }
    }
}

static node *parse_unary(cc *c)
{
    tok *t = cur(c);
    node *n, *a;

    c->line = t->line;
    switch (t->kind) {
    case '-':
        advance(c);
        a = parse_unary(c);
        if (a->kind == N_INT) { a->ival = -a->ival; return a; }
        if (a->kind == N_FLOAT) { a->fval = -a->fval; return a; }
        n = mk(c, N_UNARY);
        n->op = '-';
        n->a = a;
        return n;
    case '+':
        advance(c);
        return parse_unary(c);
    case '!': case '~':
        advance(c);
        n = mk(c, N_UNARY);
        n->op = t->kind;
        n->a = parse_unary(c);
        return n;
    case '&':
        advance(c);
        n = mk(c, N_ADDR);
        n->a = parse_unary(c);
        if (!is_lvalue(n->a)) die(c, "& needs a variable, element or field");
        return n;
    case TK_INC: case TK_DEC:
        advance(c);
        n = mk(c, N_INCDEC);
        n->op = t->kind;
        n->ival = 1; /* prefix */
        n->a = parse_unary(c);
        if (!is_lvalue(n->a)) die(c, "++ and -- need a variable, element or field");
        return n;
    case '(':
        if (peek_at(c, 1)->kind == KW_TYPE) { /* a cast */
            int type = 0;
            advance(c);
            while (at(c, KW_TYPE)) type = (int)advance(c)->ival;
            expect(c, ')', "')' after the type");
            n = mk(c, N_CAST);
            n->op = type;
            n->a = parse_unary(c);
            return n;
        }
        return parse_postfix(c);
    default:
        return parse_postfix(c);
    }
}

static int binprec(int k)
{
    switch (k) {
    case TK_OROR: return 1;
    case TK_ANDAND: return 2;
    case '|': return 3;
    case '^': return 4;
    case '&': return 5;
    case TK_EQ: case TK_NE: return 6;
    case '<': case '>': case TK_LE: case TK_GE: return 7;
    case TK_SHL: case TK_SHR: return 8;
    case '+': case '-': return 9;
    case '*': case '/': case '%': return 10;
    default: return 0;
    }
}

static node *parse_binary(cc *c, int min)
{
    node *left = parse_unary(c);

    for (;;) {
        int op = cur(c)->kind, p = binprec(op);
        node *n, *right;

        if (p == 0 || p < min) return left;
        advance(c);
        right = parse_binary(c, p + 1);
        n = mk(c, op == TK_ANDAND ? N_AND : (op == TK_OROR ? N_OR : N_BINARY));
        n->op = op;
        n->a = left;
        n->b = right;
        left = n;
    }
}

static node *parse_ternary(cc *c)
{
    node *cond = parse_binary(c, 1), *n;

    if (!at(c, '?')) return cond;
    advance(c);
    n = mk(c, N_COND);
    n->a = cond;
    n->b = parse_assign(c);
    expect(c, ':', "':' in the conditional expression");
    n->c = parse_ternary(c);
    return n;
}

static int assign_op(int kind)
{
    switch (kind) {
    case '=': return -1;
    case TK_ADDEQ: return '+';
    case TK_SUBEQ: return '-';
    case TK_MULEQ: return '*';
    case TK_DIVEQ: return '/';
    case TK_MODEQ: return '%';
    case TK_ANDEQ: return '&';
    case TK_OREQ: return '|';
    case TK_XOREQ: return '^';
    case TK_SHLEQ: return TK_SHL;
    case TK_SHREQ: return TK_SHR;
    default: return -2;
    }
}

static node *parse_assign(cc *c)
{
    node *lhs = parse_ternary(c), *n;
    int op = assign_op(cur(c)->kind);

    if (op == -2) return lhs;
    if (!is_lvalue(lhs)) die(c, "the left side of an assignment must be a variable, element or field");
    advance(c);
    n = mk(c, N_ASSIGN);
    n->op = op == -1 ? 0 : op;
    n->a = lhs;
    n->b = parse_assign(c);
    return n;
}

static node *parse_expr(cc *c)
{
    return parse_assign(c);
}

/* ---- declarations and statements ---- */

static int parse_type(cc *c)
{
    int type = 0;

    if (!at(c, KW_TYPE)) die(c, "expected a type");
    while (at(c, KW_TYPE)) type = (int)advance(c)->ival; /* "long long", "unsigned int" */
    return type;
}

/* name [ '[' size ']' ] [ '=' init ] { ',' ... } ';'  -- the first name is already read */
static node *parse_declarators(cc *c, int type, tok *name)
{
    node *head = NULL, *tail = NULL;

    for (;;) {
        node *d = mk(c, N_DECL);
        d->op = type;
        d->str = name->text;
        d->line = name->line;
        if (type == T_VOID) die(c, "a variable cannot have type void");
        if (accept(c, '[')) {
            d->ival = 1; /* array declarator */
            if (!at(c, ']')) d->b = parse_expr(c);
            expect(c, ']', "']'");
            if (at(c, '[')) die(c, "multi-dimensional arrays are not supported; use an array of arrays");
        }
        if (accept(c, '=')) d->a = parse_assign(c);
        if (tail) tail->next = d;
        else head = d;
        tail = d;
        if (!accept(c, ',')) break;
        if (!at(c, TK_IDENT)) die(c, "expected a variable name");
        name = advance(c);
    }
    expect(c, ';', "';' after the declaration");
    return head;
}

static node *parse_decl_stmt(cc *c)
{
    int type = parse_type(c);
    node *n;

    if (!at(c, TK_IDENT)) die(c, "expected a variable name");
    n = mk(c, N_DECLS);
    n->a = parse_declarators(c, type, advance(c));
    return n;
}

static node *parse_block(cc *c)
{
    node *b, *tail = NULL;

    expect(c, '{', "'{'");
    b = mk(c, N_BLOCK);
    while (!at(c, '}')) {
        node *s;
        if (at(c, TK_EOF)) die(c, "missing '}' at the end of the block");
        s = parse_stmt(c);
        if (tail) tail->next = s;
        else b->a = s;
        tail = s;
    }
    advance(c);
    return b;
}

static node *parse_stmt(cc *c)
{
    tok *t = cur(c);
    node *n;

    c->line = t->line;
    switch (t->kind) {
    case '{':
        return parse_block(c);
    case ';':
        advance(c);
        return mk(c, N_EMPTY);
    case KW_IF:
        advance(c);
        n = mk(c, N_IF);
        expect(c, '(', "'(' after if");
        n->a = parse_expr(c);
        expect(c, ')', "')'");
        n->b = parse_stmt(c);
        if (accept(c, KW_ELSE)) n->c = parse_stmt(c);
        return n;
    case KW_WHILE:
        advance(c);
        n = mk(c, N_WHILE);
        expect(c, '(', "'(' after while");
        n->a = parse_expr(c);
        expect(c, ')', "')'");
        n->b = parse_stmt(c);
        return n;
    case KW_DO:
        advance(c);
        n = mk(c, N_DO);
        n->b = parse_stmt(c);
        expect(c, KW_WHILE, "'while' after the body of do");
        expect(c, '(', "'('");
        n->a = parse_expr(c);
        expect(c, ')', "')'");
        expect(c, ';', "';'");
        return n;
    case KW_FOR:
        advance(c);
        n = mk(c, N_FOR);
        expect(c, '(', "'(' after for");
        if (accept(c, ';')) {
            n->a = NULL;
        } else if (at(c, KW_TYPE) && (peek_at(c, 1)->kind == TK_IDENT || peek_at(c, 1)->kind == KW_TYPE)) {
            n->a = parse_decl_stmt(c); /* consumes the ';' */
        } else {
            node *e = mk(c, N_EXPR);
            e->a = parse_expr(c);
            expect(c, ';', "';'");
            n->a = e;
        }
        if (!at(c, ';')) n->b = parse_expr(c);
        expect(c, ';', "';' after the condition");
        if (!at(c, ')')) n->c = parse_expr(c);
        expect(c, ')', "')'");
        n->d = parse_stmt(c);
        return n;
    case KW_BREAK:
        advance(c);
        expect(c, ';', "';'");
        return mk(c, N_BREAK);
    case KW_CONTINUE:
        advance(c);
        expect(c, ';', "';'");
        return mk(c, N_CONTINUE);
    case KW_RETURN:
        advance(c);
        n = mk(c, N_RETURN);
        if (!at(c, ';')) n->a = parse_expr(c);
        expect(c, ';', "';' after return");
        return n;
    case KW_THROW:
        advance(c);
        n = mk(c, N_THROW);
        n->a = parse_expr(c);
        expect(c, ';', "';'");
        return n;
    case KW_TRY:
        advance(c);
        n = mk(c, N_TRY);
        n->a = parse_block(c);
        expect(c, KW_CATCH, "'catch' after the try block");
        expect(c, '(', "'(' after catch");
        if (at(c, TK_IDENT)) n->str = advance(c)->text;
        expect(c, ')', "')'");
        n->b = parse_block(c);
        return n;
    case KW_TYPE:
        if (peek_at(c, 1)->kind == TK_IDENT || peek_at(c, 1)->kind == KW_TYPE) return parse_decl_stmt(c);
        break;
    default:
        break;
    }
    n = mk(c, N_EXPR);
    n->a = parse_expr(c);
    expect(c, ';', "';' after the expression");
    return n;
}

/* ---- the program ---- */

static void parse_program(cc *c)
{
    node *gtail = NULL;

    while (!at(c, TK_EOF)) {
        int type;
        tok *name;

        c->line = cur(c)->line;
        if (!at(c, KW_TYPE)) die(c, "expected a function or a global variable declaration");
        type = parse_type(c);
        if (!at(c, TK_IDENT)) die(c, "expected a name after the type");
        name = advance(c);

        if (at(c, '(')) { /* a function */
            fn f;
            node *ptail = NULL;
            advance(c);
            memset(&f, 0, sizeof f);
            f.name = name->text;
            f.line = name->line;
            f.ret = type;
            if (at(c, KW_TYPE) && cur(c)->ival == T_VOID && peek_at(c, 1)->kind == ')') {
                advance(c);
            } else if (!at(c, ')')) {
                do {
                    node *p = mk(c, N_DECL);
                    p->op = parse_type(c);
                    if (!at(c, TK_IDENT)) die(c, "expected a parameter name");
                    p->str = advance(c)->text;
                    if (accept(c, '[')) expect(c, ']', "']'"); /* int a[] */
                    if (ptail) ptail->next = p;
                    else f.params = p;
                    ptail = p;
                    f.nparams++;
                } while (accept(c, ','));
            }
            expect(c, ')', "')' after the parameters");
            if (!at(c, '{')) die(c, "expected the function body");
            f.body = parse_block(c);
            VPUSH(c, c->fns, c->nfns, c->capfns, f);
        } else { /* global variables */
            node *d = parse_declarators(c, type, name);
            if (gtail) gtail->next = d;
            else c->globals = d;
            for (gtail = d; gtail->next; gtail = gtail->next) {}
        }
    }
}

/* ---- code generation: output, names, registers ---- */

static void buf_put(cc *c, const char *s, size_t n)
{
    if (c->nbuf + n + 1 > c->capbuf) {
        size_t nc = c->capbuf ? c->capbuf : 4096;
        char *nb;
        while (nc < c->nbuf + n + 1) nc *= 2;
        nb = zalloc(c, nc);
        if (c->nbuf) memcpy(nb, c->buf, c->nbuf);
        c->buf = nb;
        c->capbuf = nc;
    }
    memcpy(c->buf + c->nbuf, s, n);
    c->nbuf += n;
    c->buf[c->nbuf] = '\0';
}

static void buf_printf(cc *c, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) die(c, "internal error: formatting failed");
    if ((size_t)n < sizeof tmp) {
        buf_put(c, tmp, (size_t)n);
    } else {
        char *big = zalloc(c, (size_t)n + 1);
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        buf_put(c, big, (size_t)n);
    }
}

/* one indented instruction line */
static void emit(cc *c, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) die(c, "internal error: instruction text too long");
    buf_put(c, "    ", 4);
    buf_put(c, tmp, (size_t)n);
    buf_put(c, "\n", 1);
}

static int newlabel(cc *c)
{
    return c->labels++;
}

static void place(cc *c, int l)
{
    buf_printf(c, ".L%d:\n", l);
}

static int alloc(cc *c)
{
    if (c->top > MAXR) die(c, "the expression or function needs too many registers");
    return c->top++;
}

static void emit_int(cc *c, int reg, int64_t v)
{
    if (v >= -32768 && v <= 32767) {
        emit(c, "LDI R%d, %lld", reg, (long long)v);
    } else {
        if (!lvm_fits_int(v)) die(c, "integer %lld does not fit in 48 bits", (long long)v);
        emit(c, "LDK R%d, %lld", reg, (long long)v);
    }
}

static void emit_float(cc *c, int reg, double d)
{
    char tmp[40];

    snprintf(tmp, sizeof tmp, "%.17g", d);
    if (!strpbrk(tmp, ".eE")) strcat(tmp, ".0");
    emit(c, "LDK R%d, %s", reg, tmp);
}

static void emit_str(cc *c, int reg, const char *s, size_t n)
{
    size_t i;
    char head[32];

    snprintf(head, sizeof head, "    LDK R%d, \"", reg);
    buf_put(c, head, strlen(head));
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        char tmp[8];
        switch (ch) {
        case '\n': buf_put(c, "\\n", 2); break;
        case '\t': buf_put(c, "\\t", 2); break;
        case '\r': buf_put(c, "\\r", 2); break;
        case '"':  buf_put(c, "\\\"", 2); break;
        case '\\': buf_put(c, "\\\\", 2); break;
        default:
            if (ch < 0x20 || ch > 0x7E) {
                snprintf(tmp, sizeof tmp, "\\x%02X", ch);
                buf_put(c, tmp, 4);
            } else {
                buf_put(c, (const char *)&ch, 1);
            }
        }
    }
    buf_put(c, "\"\n", 2);
}

/* locals: registers; globals: slots of the array in R0 */
static int find_local(cc *c, const char *name)
{
    size_t i;

    for (i = c->nloc; i-- > 0;) {
        if (strcmp(c->locals[i].name, name) == 0) return c->locals[i].reg;
    }
    return -1;
}

static int find_global(cc *c, const char *name)
{
    size_t i;

    for (i = 0; i < c->ngl; i++) {
        if (strcmp(c->gtab[i].name, name) == 0) return c->gtab[i].idx;
    }
    return -1;
}

static void declare_local(cc *c, const char *name, int reg)
{
    local l;
    size_t i;

    for (i = c->scope_base; i < c->nloc; i++) {
        if (strcmp(c->locals[i].name, name) == 0) die(c, "'%s' is already declared in this block", name);
    }
    l.name = (char *)name;
    l.reg = reg;
    VPUSH(c, c->locals, c->nloc, c->caploc, l);
}

static fn *find_fn(cc *c, const char *name)
{
    size_t i;

    for (i = 0; i < c->nfns; i++) {
        if (strcmp(c->fns[i].name, name) == 0) return &c->fns[i];
    }
    return NULL;
}

/* ---- builtins ---- */

enum { B_HOST, B_LEN, B_PUSH, B_REMOVE, B_SCANF };

typedef struct {
    const char *name;
    int id, min, max, kind;
} builtin;

static const builtin builtins[] = {
    { "print", LVM_STD_PRINT, 0, 250, B_HOST }, { "puts", LVM_STD_PRINT, 1, 1, B_HOST },
    { "printf", LVM_STD_PRINTF, 1, 250, B_HOST }, { "scanf", 0, 1, 250, B_SCANF },
    { "readline", LVM_STD_READLINE, 0, 0, B_HOST }, { "getline", LVM_STD_READLINE, 0, 0, B_HOST },
    { "getchar", LVM_STD_GETCHAR, 0, 0, B_HOST }, { "putchar", LVM_STD_PUTCHAR, 1, 1, B_HOST },
    { "int", LVM_STD_INT, 1, 1, B_HOST }, { "atoi", LVM_STD_INT, 1, 1, B_HOST },
    { "float", LVM_STD_FLOAT, 1, 1, B_HOST }, { "atof", LVM_STD_FLOAT, 1, 1, B_HOST },
    { "str", LVM_STD_STR, 1, 1, B_HOST },
    { "substr", LVM_STD_SUBSTR, 2, 3, B_HOST }, { "strcmp", LVM_STD_STRCMP, 2, 2, B_HOST },
    { "chr", LVM_STD_CHR, 1, 1, B_HOST }, { "find", LVM_STD_FIND, 2, 3, B_HOST },
    { "split", LVM_STD_SPLIT, 2, 2, B_HOST },
    { "array", LVM_STD_ARRAY, 1, 2, B_HOST }, { "map", LVM_STD_MAP, 0, 0, B_HOST },
    { "pop", LVM_STD_POP, 1, 1, B_HOST }, { "keys", LVM_STD_KEYS, 1, 1, B_HOST },
    { "sort", LVM_STD_SORT, 1, 1, B_HOST },
    { "len", 0, 1, 1, B_LEN }, { "strlen", 0, 1, 1, B_LEN },
    { "push", 0, 2, 2, B_PUSH }, { "remove", 0, 2, 2, B_REMOVE },
    { "sqrt", LVM_STD_SQRT, 1, 1, B_HOST }, { "sin", LVM_STD_SIN, 1, 1, B_HOST },
    { "cos", LVM_STD_COS, 1, 1, B_HOST }, { "tan", LVM_STD_TAN, 1, 1, B_HOST },
    { "atan2", LVM_STD_ATAN2, 2, 2, B_HOST }, { "pow", LVM_STD_POW, 2, 2, B_HOST },
    { "exp", LVM_STD_EXP, 1, 1, B_HOST }, { "log", LVM_STD_LOG, 1, 1, B_HOST },
    { "floor", LVM_STD_FLOOR, 1, 1, B_HOST }, { "ceil", LVM_STD_CEIL, 1, 1, B_HOST },
    { "abs", LVM_STD_ABS, 1, 1, B_HOST }, { "min", LVM_STD_MIN, 2, 2, B_HOST },
    { "max", LVM_STD_MAX, 2, 2, B_HOST }, { "rand", LVM_STD_RAND, 0, 0, B_HOST },
    { "srand", LVM_STD_SRAND, 1, 1, B_HOST }, { "clock", LVM_STD_CLOCK, 0, 0, B_HOST }
};

static const builtin *find_builtin(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof builtins / sizeof builtins[0]; i++) {
        if (strcmp(builtins[i].name, name) == 0) return &builtins[i];
    }
    return NULL;
}

static int count_nodes(const node *n)
{
    int k = 0;

    for (; n; n = n->next) k++;
    return k;
}

/* ---- code generation: expressions ---- */

static int gen(cc *c, node *e);
static void gen_into(cc *c, node *e, int dst);
static void gen_jump(cc *c, node *e, int when, int lab);
static int gen_call(cc *c, node *e, int want);

static const char *binop_name(int op)
{
    switch (op) {
    case '+': return "ADD";
    case '-': return "SUB";
    case '*': return "MUL";
    case '/': return "DIV";
    case '%': return "MOD";
    case '&': return "AND";
    case '|': return "OR";
    case '^': return "XOR";
    case TK_SHL: return "SHL";
    case TK_SHR: return "SHR";
    default: return NULL;
    }
}

static int is_cmp(const node *e)
{
    if (e->kind != N_BINARY) return 0;
    return e->op == TK_EQ || e->op == TK_NE || e->op == '<' || e->op == '>' ||
           e->op == TK_LE || e->op == TK_GE;
}

/* a value that is not a plain variable: evaluated to a register */
static void gen_default(cc *c, int type, int reg)
{
    switch (type) {
    case T_INT:    emit(c, "LDI R%d, 0", reg); break;
    case T_FLOAT:  emit(c, "LDK R%d, 0.0", reg); break;
    case T_BOOL:   emit(c, "LDBOOL R%d, false", reg); break;
    case T_STRING: emit(c, "LDK R%d, \"\"", reg); break;
    case T_ARRAY:  emit(c, "NEWARR R%d", reg); break;
    case T_MAP:    emit(c, "NEWMAP R%d", reg); break;
    default:       emit(c, "LDNIL R%d", reg); break;
    }
}

/* assignment targets, evaluated once */
typedef struct {
    int kind;       /* 0 local register, 1 global slot, 2 element */
    int reg, base, key;
} lv;

static lv lv_eval(cc *c, node *n)
{
    lv v;
    int r;

    memset(&v, 0, sizeof v);
    c->line = n->line;
    if (n->kind == N_VAR) {
        r = find_local(c, n->str);
        if (r >= 0) {
            v.kind = 0;
            v.reg = r;
        } else {
            int g = find_global(c, n->str);
            if (g < 0) die(c, "undefined variable '%s'", n->str);
            v.kind = 1;
            v.key = alloc(c);
            emit(c, "LDI R%d, %d", v.key, g);
        }
    } else if (n->kind == N_INDEX) {
        v.kind = 2;
        v.base = gen(c, n->a);
        v.key = gen(c, n->b);
    } else { /* N_MEMBER */
        v.kind = 2;
        v.base = gen(c, n->a);
        v.key = alloc(c);
        emit_str(c, v.key, n->str, strlen(n->str));
    }
    return v;
}

/* the current value of a target, in a register (a new one unless it is a local) */
static int lv_load(cc *c, lv v)
{
    int d;

    if (v.kind == 0) return v.reg;
    d = alloc(c);
    if (v.kind == 1) emit(c, "GET R%d, R0, R%d", d, v.key);
    else emit(c, "GET R%d, R%d, R%d", d, v.base, v.key);
    return d;
}

static void lv_store(cc *c, lv v, int src)
{
    if (v.kind == 0) {
        if (v.reg != src) emit(c, "MOV R%d, R%d", v.reg, src);
    } else if (v.kind == 1) {
        emit(c, "SET R0, R%d, R%d", v.key, src);
    } else {
        emit(c, "SET R%d, R%d, R%d", v.base, v.key, src);
    }
}

static int gen_assign(cc *c, node *e)
{
    node *lhs = e->a;
    int r, val;
    lv t;

    c->line = e->line;
    if (lhs->kind == N_VAR && (r = find_local(c, lhs->str)) >= 0) {
        if (e->op == 0) {
            gen_into(c, e->b, r);
        } else {
            int rb = gen(c, e->b);
            emit(c, "%s R%d, R%d, R%d", binop_name(e->op), r, r, rb);
        }
        return r;
    }
    t = lv_eval(c, lhs);
    if (e->op == 0) {
        val = gen(c, e->b);
    } else {
        int cur = lv_load(c, t);
        int rb = gen(c, e->b);
        val = alloc(c);
        emit(c, "%s R%d, R%d, R%d", binop_name(e->op), val, cur, rb);
    }
    lv_store(c, t, val);
    return val;
}

/* ++ and --; returns the register holding the value of the expression */
static int gen_incdec(cc *c, node *e, int want_value)
{
    const char *op = e->op == TK_INC ? "ADD" : "SUB";
    int prefix = (int)e->ival, one, nw;
    lv t;

    c->line = e->line;
    t = lv_eval(c, e->a);
    if (t.kind == 0) {
        one = alloc(c);
        emit(c, "LDI R%d, 1", one);
        if (prefix || !want_value) {
            emit(c, "%s R%d, R%d, R%d", op, t.reg, t.reg, one);
            return t.reg;
        } else {
            int old = alloc(c);
            emit(c, "MOV R%d, R%d", old, t.reg);
            emit(c, "%s R%d, R%d, R%d", op, t.reg, t.reg, one);
            return old;
        }
    } else {
        int cur = lv_load(c, t);
        one = alloc(c);
        nw = alloc(c);
        emit(c, "LDI R%d, 1", one);
        emit(c, "%s R%d, R%d, R%d", op, nw, cur, one);
        lv_store(c, t, nw);
        return (prefix || !want_value) ? nw : cur;
    }
}

/* comparisons and logic as true/false values */
static void gen_bool(cc *c, node *e, int dst)
{
    int lt = newlabel(c), le = newlabel(c);

    gen_jump(c, e, 1, lt);
    emit(c, "LDBOOL R%d, false", dst);
    emit(c, "JMP .L%d", le);
    place(c, lt);
    emit(c, "LDBOOL R%d, true", dst);
    place(c, le);
}

/* jumps to lab when e is truthy (when = 1) or falsy (when = 0) */
static void gen_jump(cc *c, node *e, int when, int lab)
{
    int mark = c->top;

    c->line = e->line;
    switch (e->kind) {
    case N_BOOL:
        if ((e->ival != 0) == (when != 0)) emit(c, "JMP .L%d", lab);
        return;
    case N_UNARY:
        if (e->op == '!') {
            gen_jump(c, e->a, !when, lab);
            return;
        }
        break;
    case N_AND:
        if (!when) {
            gen_jump(c, e->a, 0, lab);
            gen_jump(c, e->b, 0, lab);
        } else {
            int skip = newlabel(c);
            gen_jump(c, e->a, 0, skip);
            gen_jump(c, e->b, 1, lab);
            place(c, skip);
        }
        return;
    case N_OR:
        if (when) {
            gen_jump(c, e->a, 1, lab);
            gen_jump(c, e->b, 1, lab);
        } else {
            int skip = newlabel(c);
            gen_jump(c, e->a, 1, skip);
            gen_jump(c, e->b, 0, lab);
            place(c, skip);
        }
        return;
    case N_BINARY:
        if (is_cmp(e)) {
            int a = gen(c, e->a), b = gen(c, e->b);
            switch (e->op) {
            case TK_EQ: emit(c, "%s R%d, R%d, .L%d", when ? "JEQ" : "JNE", a, b, lab); break;
            case TK_NE: emit(c, "%s R%d, R%d, .L%d", when ? "JNE" : "JEQ", a, b, lab); break;
            case '<':
                if (when) emit(c, "JLT R%d, R%d, .L%d", a, b, lab);
                else { emit(c, "LT 0, R%d, R%d", a, b); emit(c, "JMP .L%d", lab); }
                break;
            case '>':
                if (when) emit(c, "JGT R%d, R%d, .L%d", a, b, lab);
                else { emit(c, "LT 0, R%d, R%d", b, a); emit(c, "JMP .L%d", lab); }
                break;
            case TK_LE:
                if (when) emit(c, "JLE R%d, R%d, .L%d", a, b, lab);
                else { emit(c, "LE 0, R%d, R%d", a, b); emit(c, "JMP .L%d", lab); }
                break;
            default: /* TK_GE */
                if (when) emit(c, "JGE R%d, R%d, .L%d", a, b, lab);
                else { emit(c, "LE 0, R%d, R%d", b, a); emit(c, "JMP .L%d", lab); }
                break;
            }
            c->top = mark;
            return;
        }
        break;
    default:
        break;
    }

    {   /* any other value: nil, false and 0 are false, everything else is true */
        int r = gen(c, e);
        if (when) {
            int skip = newlabel(c), z = alloc(c);
            emit(c, "JF R%d, .L%d", r, skip);
            emit(c, "LDI R%d, 0", z);
            emit(c, "JEQ R%d, R%d, .L%d", r, z, skip);
            emit(c, "JMP .L%d", lab);
            place(c, skip);
        } else {
            int z = alloc(c);
            emit(c, "JF R%d, .L%d", r, lab);
            emit(c, "LDI R%d, 0", z);
            emit(c, "JEQ R%d, R%d, .L%d", r, z, lab);
        }
    }
    c->top = mark;
}

static void gen_array(cc *c, node *e, int t)
{
    node *el;
    int n = count_nodes(e->a), i = 0;

    if (n > 0) emit(c, "NEWARR R%d, %d", t, n > 255 ? 255 : n);
    else emit(c, "NEWARR R%d", t);
    for (el = e->a; el; el = el->next, i++) {
        int mark = c->top, ri = alloc(c), rv;
        emit_int(c, ri, i);
        rv = gen(c, el);
        emit(c, "SET R%d, R%d, R%d", t, ri, rv);
        c->top = mark;
    }
}

static void gen_map(cc *c, node *e, int t)
{
    node *en;

    emit(c, "NEWMAP R%d", t);
    for (en = e->a; en; en = en->next) {
        int mark = c->top, rk = gen(c, en->a), rv = gen(c, en->b);
        emit(c, "SET R%d, R%d, R%d", t, rk, rv);
        c->top = mark;
    }
}

/* scanf(fmt, &a, &b, ...): the host returns the converted values and each
 * one is stored into its target; the result is the count, or -1 at the end of input */
static int gen_scanf(cc *c, node *e)
{
    node *fmt = e->a, *arg;
    int t, res, k = 0, lend = newlabel(c), lfail = newlabel(c);

    for (arg = fmt->next; arg; arg = arg->next) {
        if (arg->kind != N_ADDR) die(c, "the arguments of scanf after the format must be &variable");
    }
    t = alloc(c);
    gen_into(c, fmt, t);
    emit(c, "HOST R%d, 1, %d", t, LVM_STD_SCAN);
    res = alloc(c);
    emit(c, "JF R%d, .L%d", t, lfail);
    emit(c, "LEN R%d, R%d", res, t);
    for (arg = fmt->next; arg; arg = arg->next, k++) {
        int mark = c->top, kk = alloc(c), v = alloc(c);
        lv target;
        emit_int(c, kk, k);
        emit(c, "JLE R%d, R%d, .L%d", res, kk, lend);
        emit(c, "GET R%d, R%d, R%d", v, t, kk);
        target = lv_eval(c, arg->a);
        lv_store(c, target, v);
        c->top = mark;
    }
    emit(c, "JMP .L%d", lend);
    place(c, lfail);
    emit(c, "LDI R%d, -1", res);
    place(c, lend);
    emit(c, "MOV R%d, R%d", t, res);
    c->top = t + 1;
    return t;
}

/* calls; the result is in the returned register (or in `want` when given) */
static int gen_call(cc *c, node *e, int want)
{
    const builtin *bi;
    fn *f = find_fn(c, e->str);
    int argc = count_nodes(e->a), mark = c->top, t, r, i;
    node *arg;

    c->line = e->line;
    if (f) {
        if (argc != f->nparams)
            die(c, "'%s' takes %d argument(s), got %d", f->name, f->nparams, argc);
        t = alloc(c);
        emit(c, "MOV R%d, R0", t); /* every function receives the globals first */
        for (arg = e->a; arg; arg = arg->next) {
            r = alloc(c);
            gen_into(c, arg, r);
            c->top = r + 1;
        }
        emit(c, "CALL R%d, u_%s", t, f->name);
    } else if ((bi = find_builtin(e->str)) != NULL) {
        if (argc < bi->min || argc > bi->max) {
            if (bi->min == bi->max) die(c, "%s takes %d argument(s), got %d", bi->name, bi->min, argc);
            die(c, "%s takes %d to %d arguments, got %d", bi->name, bi->min, bi->max, argc);
        }
        switch (bi->kind) {
        case B_SCANF:
            t = gen_scanf(c, e);
            break;
        case B_LEN: {
            int a = gen(c, e->a);
            c->top = mark;
            t = alloc(c);
            emit(c, "LEN R%d, R%d", t, a);
            break;
        }
        case B_PUSH: {
            int ra = gen(c, e->a), rv = gen(c, e->a->next);
            t = alloc(c);
            emit(c, "LEN R%d, R%d", t, ra);
            emit(c, "SET R%d, R%d, R%d", ra, t, rv);
            emit(c, "LDNIL R%d", t);
            break;
        }
        case B_REMOVE: {
            int ra = gen(c, e->a), rk = gen(c, e->a->next);
            t = alloc(c);
            emit(c, "LDNIL R%d", t);
            emit(c, "SET R%d, R%d, R%d", ra, rk, t);
            break;
        }
        default: /* a host function: arguments in consecutive registers from the base */
            t = c->top;
            for (i = 0, arg = e->a; arg; arg = arg->next, i++) {
                r = alloc(c);
                gen_into(c, arg, r);
                c->top = r + 1;
            }
            if (argc == 0) t = alloc(c);
            emit(c, "HOST R%d, %d, %d", t, argc, bi->id);
            break;
        }
    } else {
        die(c, "undefined function '%s'", e->str);
    }

    if (want >= 0) {
        if (want != t) emit(c, "MOV R%d, R%d", want, t);
        c->top = mark;
        return want;
    }
    c->top = t + 1;
    return t;
}

static int gen(cc *c, node *e)
{
    int mark = c->top, a, b, d;

    c->line = e->line;
    switch (e->kind) {
    case N_VAR:
        a = find_local(c, e->str);
        if (a >= 0) return a; /* a local is its own register */
        break;
    case N_CALL:
        return gen_call(c, e, -1);
    case N_BINARY:
        if (!is_cmp(e)) {
            a = gen(c, e->a);
            b = gen(c, e->b);
            c->top = mark;
            d = alloc(c);
            emit(c, "%s R%d, R%d, R%d", binop_name(e->op), d, a, b);
            return d;
        }
        break;
    case N_UNARY:
        if (e->op != '!') {
            a = gen(c, e->a);
            c->top = mark;
            d = alloc(c);
            emit(c, "%s R%d, R%d", e->op == '-' ? "NEG" : "NOT", d, a);
            return d;
        }
        break;
    case N_INDEX:
        a = gen(c, e->a);
        b = gen(c, e->b);
        c->top = mark;
        d = alloc(c);
        emit(c, "GET R%d, R%d, R%d", d, a, b);
        return d;
    default:
        break;
    }
    d = alloc(c);
    gen_into(c, e, d);
    return d;
}

static void gen_into(cc *c, node *e, int dst)
{
    int mark = c->top, a, b, t;

    c->line = e->line;
    switch (e->kind) {
    case N_INT:   emit_int(c, dst, e->ival); break;
    case N_FLOAT: emit_float(c, dst, e->fval); break;
    case N_STR:   emit_str(c, dst, e->str, e->slen); break;
    case N_BOOL:  emit(c, "LDBOOL R%d, %s", dst, e->ival ? "true" : "false"); break;
    case N_NIL:   emit(c, "LDNIL R%d", dst); break;
    case N_VAR:
        a = find_local(c, e->str);
        if (a >= 0) {
            if (a != dst) emit(c, "MOV R%d, R%d", dst, a);
        } else {
            int g = find_global(c, e->str);
            if (g < 0) die(c, "undefined variable '%s'", e->str);
            emit(c, "LDI R%d, %d", dst, g);
            emit(c, "GET R%d, R0, R%d", dst, dst);
        }
        break;
    case N_INDEX:
        a = gen(c, e->a);
        b = gen(c, e->b);
        emit(c, "GET R%d, R%d, R%d", dst, a, b);
        break;
    case N_MEMBER:
        a = gen(c, e->a);
        t = alloc(c);
        emit_str(c, t, e->str, strlen(e->str));
        emit(c, "GET R%d, R%d, R%d", dst, a, t);
        break;
    case N_CALL:
        gen_call(c, e, dst);
        break;
    case N_UNARY:
        if (e->op == '!') {
            gen_bool(c, e, dst);
        } else {
            a = gen(c, e->a);
            emit(c, "%s R%d, R%d", e->op == '-' ? "NEG" : "NOT", dst, a);
        }
        break;
    case N_BINARY:
        if (is_cmp(e)) {
            gen_bool(c, e, dst);
        } else {
            a = gen(c, e->a);
            b = gen(c, e->b);
            emit(c, "%s R%d, R%d, R%d", binop_name(e->op), dst, a, b);
        }
        break;
    case N_AND: case N_OR:
        gen_bool(c, e, dst);
        break;
    case N_COND: {
        int le = newlabel(c), lend = newlabel(c);
        gen_jump(c, e->a, 0, le);
        gen_into(c, e->b, dst);
        emit(c, "JMP .L%d", lend);
        place(c, le);
        gen_into(c, e->c, dst);
        place(c, lend);
        break;
    }
    case N_ASSIGN:
        a = gen_assign(c, e);
        if (a != dst) emit(c, "MOV R%d, R%d", dst, a);
        break;
    case N_INCDEC:
        a = gen_incdec(c, e, 1);
        if (a != dst) emit(c, "MOV R%d, R%d", dst, a);
        break;
    case N_CAST: {
        int id;
        switch (e->op) {
        case T_INT: id = LVM_STD_INT; break;
        case T_FLOAT: id = LVM_STD_FLOAT; break;
        case T_STRING: id = LVM_STD_STR; break;
        default: die(c, "this cast is not supported (use int, float or string)");
        }
        t = alloc(c);
        gen_into(c, e->a, t);
        emit(c, "HOST R%d, 1, %d", t, id);
        if (t != dst) emit(c, "MOV R%d, R%d", dst, t);
        break;
    }
    case N_ARRAY:
        t = alloc(c);
        gen_array(c, e, t);
        emit(c, "MOV R%d, R%d", dst, t); /* built aside: it may refer to dst itself */
        break;
    case N_MAP:
        t = alloc(c);
        gen_map(c, e, t);
        emit(c, "MOV R%d, R%d", dst, t);
        break;
    case N_ADDR:
        die(c, "& can only be used on the arguments of scanf");
    default:
        die(c, "internal error: bad expression");
    }
    c->top = mark;
}
