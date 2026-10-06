/* rvsim: run a program on the golden model alone.
 *
 *   rvsim [--fpu] [--trace FILE] [--max-insns N] [--hex FILE] [--quiet] prog.elf
 *
 * --fpu enables the F and D extensions (the model is RV32IM without it).
 * Exit status: the program's exit code (0 = pass), 3 = instruction limit,
 * 4 = model error (access outside RAM and I/O), 5 = usage / load error. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rv_iss.h"

int main(int argc, char **argv)
{
    const char *trace = NULL, *hex = NULL, *elf = NULL;
    unsigned long long max = 100000000ull;
    int quiet = 0, fpu = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace = argv[++i];
        else if (!strcmp(argv[i], "--hex") && i + 1 < argc) hex = argv[++i];
        else if (!strcmp(argv[i], "--max-insns") && i + 1 < argc) max = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "--fpu")) fpu = 1;
        else if (argv[i][0] != '-' && !elf) elf = argv[i];
        else { fprintf(stderr, "usage: rvsim [--fpu] [--trace FILE] [--max-insns N] [--hex FILE] [--quiet] prog.elf\n"); return 5; }
    }
    if (!elf) { fprintf(stderr, "rvsim: no program given\n"); return 5; }

    rv_iss s;
    if (rv_iss_init(&s) || rv_iss_load_elf(&s, elf)) { fprintf(stderr, "rvsim: %s\n", s.errmsg); return 5; }
    if (hex) {
        if (rv_iss_write_hex(&s, hex)) { fprintf(stderr, "rvsim: cannot write %s\n", hex); return 5; }
        return 0;
    }
    s.echo = !quiet;
    s.has_fpu = fpu;
    FILE *tf = trace ? fopen(trace, "w") : NULL;
    if (trace && !tf) { fprintf(stderr, "rvsim: cannot write %s\n", trace); return 5; }

    rv_commit c;
    char line[256];
    unsigned long long n = 0;
    while (!s.exited && !s.error && n < max) {
        rv_step(&s, &c);
        n++;
        if (tf) { rv_format_commit(&c, line, sizeof line); fprintf(tf, "%s\n", line); }
    }
    if (tf) fclose(tf);
    fflush(stdout);
    if (s.error) { fprintf(stderr, "rvsim: %s\n", s.errmsg); return 4; }
    if (!s.exited) { fprintf(stderr, "rvsim: no exit after %llu instructions\n", n); return 3; }
    if (!quiet)
        fprintf(stderr, "rvsim: exit %d after %llu instructions (%llu retired)\n", rv_exit_code(s.exit_value), n,
                (unsigned long long)s.minstret);
    return rv_exit_code(s.exit_value);
}
