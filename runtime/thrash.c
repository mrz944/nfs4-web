/* "thrash" renderer driver replacement. Filled in as the protocol is traced. */
#include "rt.h"
typedef struct { const char *name; hostfn fn; } ModFn;
const ModFn thrash_fns[] = {
    {0},
};
