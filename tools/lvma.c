/* lvma: assemble a .lvma source file into a .lvmb bytecode file. */
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

/* "dir/prog.lvma" -> "dir/prog.lvmb" */
static char *default_output(const char *in)
{
    size_t n = strlen(in);
    const char *dot = strrchr(in, '.');
    const char *slash = strrchr(in, '/');
    char *out;

    if (!dot || (slash && dot < slash)) dot = in + n;
    out = malloc((size_t)(dot - in) + 6);
    if (!out) return NULL;
    memcpy(out, in, (size_t)(dot - in));
    strcpy(out + (dot - in), ".lvmb");
    return out;
}

int main(int argc, char **argv)
{
    const char *in = NULL, *outpath = NULL;
    char *src, *dflt = NULL, err[256];
    size_t len = 0, outlen = 0;
    uint8_t *bc;
    lvm_status st;
    FILE *f;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outpath = argv[++i];
        else if (argv[i][0] == '-' && argv[i][1]) in = NULL, i = argc;
        else if (!in) in = argv[i];
        else in = NULL, i = argc;
    }
    if (!in) {
        fprintf(stderr, "usage: %s [-o out.lvmb] input.lvma\n", argv[0]);
        return 1;
    }

    src = read_file(in, &len);
    if (!src) {
        fprintf(stderr, "lvma: cannot read %s\n", in);
        return 2;
    }
    st = lvm_assemble(NULL, src, len, &bc, &outlen, err, sizeof err);
    free(src);
    if (st != LVM_OK) {
        fprintf(stderr, "%s: %s\n", in, err[0] ? err : lvm_strerror(st));
        return 1;
    }

    if (!outpath) outpath = dflt = default_output(in);
    f = outpath ? fopen(outpath, "wb") : NULL;
    if (!f || fwrite(bc, 1, outlen, f) != outlen || fclose(f) != 0) {
        fprintf(stderr, "lvma: cannot write %s\n", outpath ? outpath : "output");
        lvm_assemble_free(NULL, bc, outlen);
        free(dflt);
        return 2;
    }
    lvm_assemble_free(NULL, bc, outlen);
    free(dflt);
    return 0;
}
