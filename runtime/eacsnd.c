/* EA sound library (eacsnd.dll) replacement. Web Audio backend comes later. */
#include "rt.h"
typedef struct { const char *name; hostfn fn; } ModFn;
static uint32_t snd_version(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0x100003u; } /* version the game requires */
const ModFn eacsnd_fns[] = {
    {"_iSNDdllversion@0", snd_version},
    {0},
};
