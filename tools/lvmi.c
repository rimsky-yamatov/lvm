/*
 * lvmi: run a program.
 *   file.lvc   lvc source (compiled first); the exit code is main's result
 *   file.lvma  assembly source (assembled first)
 *   file.lvmb  bytecode
 * Anything that is not bytecode and not named *.lvc is treated as assembly.
 */
#include "lvm.h"
#include "lvm_std.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_value(FILE *f, lvm_value v)
{
    if (lvm_is_float(v)) {
        fprintf(f, "%.17g", lvm_as_float(v));
    } else if (lvm_is_int(v)) {
        fprintf(f, "%lld", (long long)lvm_as_int(v));
    } else if (lvm_is_bool(v)) {
        fputs(lvm_as_bool(v) ? "true" : "false", f);
    } else if (lvm_is_string(v)) {
        size_t n;
        const char *d = lvm_string_data(v, &n);
        fwrite(d, 1, n, f);
    } else if (lvm_is_nil(v)) {
        fputs("nil", f);
    } else {
        fprintf(f, "<%s>", lvm_type_name(v));
    }
}

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

static int has_suffix(const char *s, const char *suffix)
{
    size_t n = strlen(s), k = strlen(suffix);

    return n >= k && strcmp(s + n - k, suffix) == 0;
}

int main(int argc, char **argv)
{
    uint8_t *buf;
    size_t len = 0;
    lvm_module m;
    lvm_vm *vm;
    lvm_std *std;
    lvm_value result = lvm_nil();
    lvm_status st;
    int is_lvc;
    char err[256];

    if (argc != 2) {
        fprintf(stderr, "usage: %s file.lvc|file.lvma|file.lvmb\n", argv[0]);
        return 1;
    }
    buf = read_file(argv[1], &len);
    if (!buf) {
        fprintf(stderr, "lvmi: cannot read %s\n", argv[1]);
        return 2;
    }

    is_lvc = has_suffix(argv[1], ".lvc");
    if (is_lvc || len < 4 || memcmp(buf, "LVMB", 4) != 0) { /* source: compile and/or assemble */
        uint8_t *bc;
        size_t bclen;
        char *text = NULL;
        size_t textlen = 0;
        const char *asm_src = (const char *)buf;
        size_t asm_len = len;

        if (is_lvc) {
            st = lvm_compile(NULL, (const char *)buf, len, &text, &textlen, err, sizeof err);
            if (st != LVM_OK) {
                fprintf(stderr, "%s: %s\n", argv[1], err[0] ? err : lvm_strerror(st));
                free(buf);
                return 3;
            }
            asm_src = text;
            asm_len = textlen;
        }
        st = lvm_assemble(NULL, asm_src, asm_len, &bc, &bclen, err, sizeof err);
        if (text) lvm_compile_free(NULL, text, textlen);
        free(buf);
        if (st != LVM_OK) {
            fprintf(stderr, "%s: %s\n", argv[1], err[0] ? err : lvm_strerror(st));
            return 3;
        }
        st = lvm_load(NULL, bc, bclen, &m);
        lvm_assemble_free(NULL, bc, bclen);
    } else {
        st = lvm_load(NULL, buf, len, &m);
        free(buf);
    }
    if (st != LVM_OK) {
        fprintf(stderr, "lvmi: %s: %s\n", argv[1], lvm_strerror(st));
        return 3;
    }

    vm = lvm_new(NULL, 1u << 16, 4096);
    std = vm ? lvm_std_new(vm, NULL) : NULL;
    if (!vm || !std) {
        lvm_std_free(std);
        lvm_free(vm);
        lvm_module_free(&m);
        fprintf(stderr, "lvmi: out of memory\n");
        return 4;
    }

    st = lvm_run(vm, &m, NULL, 0, &result);
    fflush(stdout);
    if (st == LVM_OK) {
        if (is_lvc) { /* like a C program: main's value is the exit status */
            int code = lvm_is_int(result) ? (int)(lvm_as_int(result) & 0xFF) : 0;
            lvm_std_free(std);
            lvm_free(vm);
            lvm_module_free(&m);
            return code;
        }
        print_value(stdout, result);
        putchar('\n');
    } else if (st == LVM_ERR_UNCAUGHT) {
        fputs("lvmi: uncaught exception: ", stderr);
        print_value(stderr, result);
        fputc('\n', stderr);
    } else {
        fprintf(stderr, "lvmi: runtime error: %s\n", lvm_strerror(st));
    }
    lvm_std_free(std);
    lvm_free(vm);
    lvm_module_free(&m);
    return st == LVM_OK ? 0 : 4;
}
