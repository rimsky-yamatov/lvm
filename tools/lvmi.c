/* lvmi: load a bytecode file and run its entry function. */
#include "lvm.h"

#include <stdio.h>
#include <stdlib.h>

static void print_value(lvm_value v)
{
    if (lvm_is_float(v))     printf("%.17g", lvm_as_float(v));
    else if (lvm_is_int(v))  printf("%lld", (long long)lvm_as_int(v));
    else if (lvm_is_bool(v)) fputs(lvm_as_bool(v) ? "true" : "false", stdout);
    else                     fputs("nil", stdout);
}

/* host function 0: print arguments separated by spaces */
static lvm_status host_print(void *ud, lvm_value *args, int argc)
{
    int k;
    (void)ud;
    for (k = 0; k < argc; k++) {
        if (k) putchar(' ');
        print_value(args[k]);
    }
    putchar('\n');
    args[0] = lvm_nil();
    return LVM_OK;
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

int main(int argc, char **argv)
{
    uint8_t *buf;
    size_t len = 0;
    lvm_module m;
    lvm_vm *vm;
    lvm_value result;
    lvm_status st;

    if (argc != 2) {
        fprintf(stderr, "usage: %s file.lvmb\n", argv[0]);
        return 1;
    }
    buf = read_file(argv[1], &len);
    if (!buf) {
        fprintf(stderr, "lvmi: cannot read %s\n", argv[1]);
        return 2;
    }
    st = lvm_load(NULL, buf, len, &m);
    free(buf);
    if (st != LVM_OK) {
        fprintf(stderr, "lvmi: %s: %s\n", argv[1], lvm_strerror(st));
        return 3;
    }
    vm = lvm_new(NULL, 1u << 16, 4096);
    if (!vm) {
        lvm_module_free(&m);
        fprintf(stderr, "lvmi: out of memory\n");
        return 4;
    }
    lvm_set_host(vm, 0, host_print, NULL);
    st = lvm_run(vm, &m, NULL, 0, &result);
    if (st == LVM_OK) {
        print_value(result);
        putchar('\n');
    } else {
        fprintf(stderr, "lvmi: runtime error: %s\n", lvm_strerror(st));
    }
    lvm_free(vm);
    lvm_module_free(&m);
    return st == LVM_OK ? 0 : 4;
}
