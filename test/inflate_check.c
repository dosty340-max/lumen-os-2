// Self-check for src/net/inflate.c: gunzip stdin to stdout. Run via test/inflate_check.sh
#include <stdio.h>
#include <stdlib.h>
unsigned char *inflate_any(const unsigned char *in, size_t n, size_t *outn);
int main(void) {
    size_t cap = 1 << 20, n = 0, k;
    unsigned char *b = malloc(cap);
    while ((k = fread(b + n, 1, cap - n, stdin)) > 0) { n += k; if (n == cap) b = realloc(b, cap *= 2); }
    size_t on;
    unsigned char *o = inflate_any(b, n, &on);
    fwrite(o, 1, on, stdout);
    return 0;
}
