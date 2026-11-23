#include "lvm.h"
#include "lvm_opcode.h"
#include "internal.h"

#include <string.h>

#define MAX_CONSTS 65536u
#define MAX_FUNCS  65536u
#define MAX_CODE   (1u << 24)
#define MAX_STRING (1u << 24)

typedef struct {
    const uint8_t *p, *end;
    int bad;
} reader;

static int need(reader *r, size_t n)
{
    if (!r->bad && (size_t)(r->end - r->p) < n) r->bad = 1;
    return !r->bad;
}

static size_t left(const reader *r)
{
    return (size_t)(r->end - r->p);
}

static uint32_t rd8(reader *r)
{
    return need(r, 1) ? *r->p++ : 0u;
}

static uint32_t rd16(reader *r)
{
    uint32_t v = 0;
    if (need(r, 2)) {
        v = (uint32_t)r->p[0] | (uint32_t)r->p[1] << 8;
        r->p += 2;
    }
    return v;
}

static uint32_t rd32(reader *r)
{
    uint32_t v = 0;
    if (need(r, 4)) {
        v = (uint32_t)r->p[0] | (uint32_t)r->p[1] << 8 |
            (uint32_t)r->p[2] << 16 | (uint32_t)r->p[3] << 24;
        r->p += 4;
    }
    return v;
}

static uint64_t rd64(reader *r)
{
    uint64_t lo = rd32(r);
    uint64_t hi = rd32(r);
    return lo | hi << 32;
}

static lvm_status read_const(reader *r, const lvm_allocator *a, lvm_value *out)
{
    switch (rd8(r)) {
    case 0:
        *out = lvm_nil();
        return LVM_OK;
    case 1: {
        uint32_t b = rd8(r);
        if (b > 1) return LVM_ERR_FORMAT;
        *out = lvm_bool((int)b);
        return LVM_OK;
    }
    case 2: {
        int64_t v = (int64_t)rd64(r);
        if (!lvm_fits_int(v)) return LVM_ERR_FORMAT;
        *out = lvm_int(v);
        return LVM_OK;
    }
    case 3: {
        uint64_t u = rd64(r);
        double d;
        memcpy(&d, &u, sizeof d);
        *out = lvm_float(d);
        return LVM_OK;
    }
    case 4: {
        uint32_t n = rd32(r);
        lvm_string *s;
        if (r->bad || n > MAX_STRING || n > left(r)) return LVM_ERR_FORMAT;
        s = lvm_string_perm(a, (const char *)r->p, n);
        if (!s) return LVM_ERR_MEM;
        r->p += n;
        *out = lvm_from_obj(&s->o);
        return LVM_OK;
    }
    default:
        return LVM_ERR_FORMAT;
    }
}

void lvm_module_free(lvm_module *m)
{
    uint32_t i;
    const lvm_allocator *a = &m->alloc;

    if (m->funcs) {
        for (i = 0; i < m->nfuncs; i++) {
            if (m->funcs[i].code)
                LVM_MEM(a, m->funcs[i].code, (size_t)m->funcs[i].ncode * sizeof(uint32_t), 0);
        }
        LVM_MEM(a, m->funcs, (size_t)m->nfuncs * sizeof(lvm_func), 0);
    }
    if (m->consts) {
        for (i = 0; i < m->nconsts; i++) {
            if (lvm_is_ptr(m->consts[i]))
                lvm_string_free_perm(a, (lvm_string *)lvm_to_obj(m->consts[i]));
        }
        LVM_MEM(a, m->consts, (size_t)m->nconsts * sizeof(lvm_value), 0);
    }
    memset(m, 0, sizeof *m);
}

lvm_status lvm_load(const lvm_allocator *a, const uint8_t *buf, size_t len, lvm_module *m)
{
    reader r;
    lvm_status st;
    uint32_t nk, nf, i, j;

    if (!a) a = lvm_default_allocator();
    memset(m, 0, sizeof *m);
    m->alloc = *a;

    r.p = buf;
    r.end = buf + len;
    r.bad = 0;

    if (len < 4 || memcmp(buf, "LVMB", 4) != 0) return LVM_ERR_FORMAT;
    r.p += 4;
    if (rd16(&r) != LVM_VERSION) return LVM_ERR_FORMAT;
    if (rd16(&r) != 0) return LVM_ERR_FORMAT; /* flags: reserved */
    nk = rd32(&r);
    nf = rd32(&r);
    m->entry = rd32(&r);
    if (r.bad || nk > MAX_CONSTS || nf == 0 || nf > MAX_FUNCS || m->entry >= nf)
        return LVM_ERR_FORMAT;

    if (nk) {
        m->consts = LVM_MEM(a, NULL, 0, (size_t)nk * sizeof(lvm_value));
        if (!m->consts) return LVM_ERR_MEM;
        memset(m->consts, 0, (size_t)nk * sizeof(lvm_value));
        m->nconsts = nk;
    }
    m->funcs = LVM_MEM(a, NULL, 0, (size_t)nf * sizeof(lvm_func));
    if (!m->funcs) {
        lvm_module_free(m);
        return LVM_ERR_MEM;
    }
    memset(m->funcs, 0, (size_t)nf * sizeof(lvm_func));
    m->nfuncs = nf;

    for (i = 0; i < nk; i++) {
        st = read_const(&r, a, &m->consts[i]);
        if (st == LVM_OK && r.bad) st = LVM_ERR_FORMAT;
        if (st != LVM_OK) goto fail;
    }

    for (i = 0; i < nf; i++) {
        lvm_func *f = &m->funcs[i];
        uint32_t np = rd8(&r);
        uint32_t nr = rd8(&r);
        uint32_t nc = rd32(&r);

        if (r.bad || nc == 0 || nc > MAX_CODE || nc > left(&r) / 4) {
            st = LVM_ERR_FORMAT;
            goto fail;
        }
        f->code = LVM_MEM(a, NULL, 0, (size_t)nc * sizeof(uint32_t));
        if (!f->code) {
            st = LVM_ERR_MEM;
            goto fail;
        }
        f->nparams = (uint8_t)np;
        f->nregs = (uint8_t)nr;
        f->ncode = nc;
        for (j = 0; j < nc; j++) f->code[j] = rd32(&r);
    }

    if (left(&r) != 0) {
        st = LVM_ERR_FORMAT;
        goto fail;
    }

    st = lvm_verify(m);
    if (st != LVM_OK) goto fail;
    return LVM_OK;

fail:
    lvm_module_free(m);
    return st;
}
