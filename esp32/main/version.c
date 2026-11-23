/* See version.h. */
#include "version.h"

#include <ctype.h>
#include <stdlib.h>

/* the numbers of "1.2.3-rc1"; *rest points at the suffix ("" if none) */
static int parse(const char *s, long out[4], const char **rest)
{
    if (*s == 'v' || *s == 'V') {
        s++;
    }
    int n = 0;
    while (n < 4 && isdigit((unsigned char)*s)) {
        char *end;
        out[n++] = strtol(s, &end, 10);
        s = end;
        if (*s == '.' && isdigit((unsigned char)s[1])) {
            s++;
        } else {
            break;
        }
    }
    for (int i = n; i < 4; i++) {
        out[i] = 0;
    }
    *rest = s;
    return n;
}

int version_cmp(const char *a, const char *b)
{
    long x[4], y[4];
    const char *ra, *rb;
    parse(a, x, &ra);
    parse(b, y, &rb);
    for (int i = 0; i < 4; i++) {
        if (x[i] != y[i]) {
            return x[i] < y[i] ? -1 : 1;
        }
    }
    if (!*ra != !*rb) {
        return *ra ? -1 : 1; /* "1.0-rc1" < "1.0" */
    }
    return 0;
}
