/* Release version numbers ("0.3.0", "v1.2", "0.3.0-rc1"). Plain C, host-tested. */
#pragma once

/* <0, 0, >0 like strcmp. Numbers compare numerically part by part (missing parts are 0);
 * with equal numbers, a suffix ("-rc1") is older than none. A leading "v" is ignored. */
int version_cmp(const char *a, const char *b);
