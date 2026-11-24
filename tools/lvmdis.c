/* lvmdis: print a bytecode file (or assembly source) as assembly text. */
#include "lvm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
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

int main(int argc, char **argv)
{
    const char *path = NULL;
    int flags = 0, i;
    uint8_t *buf, *bc = NULL;
    size_t len = 0, bclen = 0, textlen;
    char err[256], *text;
    lvm_module m;
    lvm_status st;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) flags |= LVM_DIS_ADDR;
        else if (!path && argv[i][0] != '-') path = argv[i];
        else path = NULL, i = argc;
    }
    if (!path) {
        fprintf(stderr, "usage: %s [-a] file.lvmb|file.lvma\n  -a  show pc and instruction words\n", argv[0]);
        return 1;
    }

    buf = read_file(path, &len);
    if (!buf) {
        fprintf(stderr, "lvmdis: cannot read %s\n", path);
        return 2;
    }
    if (len < 4 || memcmp(buf, "LVMB", 4) != 0) { /* assembly source */
        st = lvm_assemble(NULL, (const char *)buf, len, &bc, &bclen, err, sizeof err);
        free(buf);
        if (st != LVM_OK) {
            fprintf(stderr, "%s: %s\n", path, err[0] ? err : lvm_strerror(st));
            return 3;
        }
        buf = bc;
        len = bclen;
    }
    st = lvm_load(NULL, buf, len, &m);
    if (bc) lvm_assemble_free(NULL, bc, bclen);
    else free(buf);
    if (st != LVM_OK) {
        fprintf(stderr, "lvmdis: %s: %s\n", path, lvm_strerror(st));
        return 3;
    }

    st = lvm_disassemble(NULL, &m, flags, &text, &textlen);
    lvm_module_free(&m);
    if (st != LVM_OK) {
        fprintf(stderr, "lvmdis: %s\n", lvm_strerror(st));
        return 4;
    }
    fwrite(text, 1, textlen, stdout);
    lvm_disassemble_free(NULL, text, textlen);
    return 0;
}
