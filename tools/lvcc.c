/* lvcc: compile an lvc source file. By default to bytecode (.lvmb); -S writes assembly instead. */
#include "lvm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc(n ? (size_t)n : 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* "dir/prog.lvc" -> "dir/prog" + ext */
static char *default_output(const char *in, const char *ext)
{
    size_t n = strlen(in), el = strlen(ext);
    const char *dot = strrchr(in, '.');
    const char *slash = strrchr(in, '/');
    char *out;

    if (!dot || (slash && dot < slash)) dot = in + n;
    out = malloc((size_t)(dot - in) + el + 1);
    if (!out) return NULL;
    memcpy(out, in, (size_t)(dot - in));
    strcpy(out + (dot - in), ext);
    return out;
}

int main(int argc, char **argv)
{
    const char *in = NULL, *outpath = NULL;
    char *src, *text, *dflt = NULL, err[256];
    size_t len = 0, textlen = 0, outlen;
    const void *data;
    uint8_t *bc = NULL;
    int asm_only = 0, i;
    lvm_status st;
    FILE *f;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-S") == 0) asm_only = 1;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outpath = argv[++i];
        else if (argv[i][0] == '-' && argv[i][1]) in = NULL, i = argc;
        else if (!in) in = argv[i];
        else in = NULL, i = argc;
    }
    if (!in) {
        fprintf(stderr, "usage: %s [-S] [-o out] input.lvc\n  -S  write assembly (.lvma) instead of bytecode\n", argv[0]);
        return 1;
    }

    src = read_file(in, &len);
    if (!src) {
        fprintf(stderr, "lvcc: cannot read %s\n", in);
        return 2;
    }
    st = lvm_compile(NULL, src, len, &text, &textlen, err, sizeof err);
    free(src);
    if (st != LVM_OK) {
        fprintf(stderr, "%s: %s\n", in, err[0] ? err : lvm_strerror(st));
        return 1;
    }

    if (asm_only) {
        data = text;
        outlen = textlen;
    } else {
        st = lvm_assemble(NULL, text, textlen, &bc, &outlen, err, sizeof err);
        if (st != LVM_OK) {
            fprintf(stderr, "%s: internal error: %s\n", in, err);
            lvm_compile_free(NULL, text, textlen);
            return 1;
        }
        data = bc;
    }

    if (!outpath) outpath = dflt = default_output(in, asm_only ? ".lvma" : ".lvmb");
    f = outpath ? fopen(outpath, "wb") : NULL;
    if (!f || fwrite(data, 1, outlen, f) != outlen || fclose(f) != 0) {
        fprintf(stderr, "lvcc: cannot write %s\n", outpath ? outpath : "output");
        st = LVM_ERR_MEM;
    }
    if (bc) lvm_assemble_free(NULL, bc, outlen);
    lvm_compile_free(NULL, text, textlen);
    free(dflt);
    return st == LVM_OK ? 0 : 2;
}
