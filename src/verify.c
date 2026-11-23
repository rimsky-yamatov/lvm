#include "lvm.h"
#include "lvm_opcode.h"

#define REG(x, n) ((unsigned)(x) < (unsigned)(n))

static int verify_func(const lvm_module *m, const lvm_func *f)
{
    uint32_t pc, n = f->ncode;
    unsigned nr = f->nregs;
    uint32_t last;

    if (n == 0 || nr == 0 || f->nparams > nr) return 0;

    /* control must not fall off the end of the function */
    last = LVM_OP(f->code[n - 1]);
    if (last != OP_RET && last != OP_JMP) return 0;

    for (pc = 0; pc < n; pc++) {
        uint32_t i = f->code[pc];
        unsigned a = LVM_A(i), b = LVM_B(i), c = LVM_C(i);

        switch (LVM_OP(i)) {
        case OP_MOV: case OP_NEG: case OP_NOT:
            if (!REG(a, nr) || !REG(b, nr)) return 0;
            break;
        case OP_LDK:
            if (!REG(a, nr) || LVM_BX(i) >= m->nconsts) return 0;
            break;
        case OP_LDI:
            if (!REG(a, nr)) return 0;
            break;
        case OP_LDNIL:
            if (a + b >= nr) return 0;
            break;
        case OP_LDBOOL:
            if (!REG(a, nr) || b > 1) return 0;
            break;
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
        case OP_AND: case OP_OR: case OP_XOR: case OP_SHL: case OP_SHR:
            if (!REG(a, nr) || !REG(b, nr) || !REG(c, nr)) return 0;
            break;
        case OP_EQ: case OP_LT: case OP_LE:
            if (a > 1 || !REG(b, nr) || !REG(c, nr)) return 0;
            /* the skipped instruction must be a jump */
            if (pc + 1 >= n || LVM_OP(f->code[pc + 1]) != OP_JMP) return 0;
            break;
        case OP_JMP: case OP_JT: case OP_JF: {
            long t = (long)pc + 1 + LVM_SBX(i);
            if (LVM_OP(i) != OP_JMP && !REG(a, nr)) return 0;
            if (t < 0 || t >= (long)n) return 0;
            break;
        }
        case OP_CALL:
            if (!REG(a, nr) || LVM_BX(i) >= m->nfuncs) return 0;
            if (a + m->funcs[LVM_BX(i)].nparams > nr) return 0;
            /* functions with upvalues only run as closures */
            if (m->funcs[LVM_BX(i)].nupvals) return 0;
            break;
        case OP_RET:
            if (!REG(a, nr)) return 0;
            break;
        case OP_HOST:
            if (!REG(a, nr) || a + b > nr) return 0;
            break;
        case OP_NEWARR: case OP_NEWMAP:
            if (!REG(a, nr)) return 0;
            break;
        case OP_GET: case OP_SET:
            if (!REG(a, nr) || !REG(b, nr) || !REG(c, nr)) return 0;
            break;
        case OP_LEN:
            if (!REG(a, nr) || !REG(b, nr)) return 0;
            break;
        case OP_CLOSURE: {
            const lvm_func *p;
            uint32_t k;
            if (!REG(a, nr) || LVM_BX(i) >= m->nfuncs) return 0;
            p = &m->funcs[LVM_BX(i)];
            for (k = 0; k < p->nupvals; k++) {
                unsigned idx = p->upvals[k].index;
                if (p->upvals[k].instack ? !REG(idx, nr) : idx >= f->nupvals) return 0;
            }
            break;
        }
        case OP_GETUP: case OP_SETUP:
            if (!REG(a, nr) || b >= f->nupvals) return 0;
            break;
        case OP_CLOSE:
            if (!REG(a, nr)) return 0;
            break;
        case OP_CALLC:
            /* closure in R[A], arguments in R[A+1..A+B] */
            if (!REG(a, nr) || a + b >= nr) return 0;
            break;
        default:
            return 0;
        }
    }
    return 1;
}

lvm_status lvm_verify(const lvm_module *m)
{
    uint32_t i;

    if (m->nfuncs == 0 || m->entry >= m->nfuncs) return LVM_ERR_VERIFY;
    if (m->funcs[m->entry].nupvals) return LVM_ERR_VERIFY; /* entry has no closure */
    for (i = 0; i < m->nfuncs; i++) {
        if (!verify_func(m, &m->funcs[i])) return LVM_ERR_VERIFY;
    }
    return LVM_OK;
}
