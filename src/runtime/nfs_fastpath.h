/* nfs_fastpath.h - guest functions the host may handle before their lifted body runs (fastpath.c).
 *
 * config/fastpaths.txt lists them; scripts/run_lift.py makes each sub_<VA> call recomp_fastpath first and
 * run the lifted body, renamed sub_<VA>_lifted, only when the host returns 0. A handler sees the guest
 * registers in the globals with [esp] the dummy return address (RECOMP_CALL); when it handles the call it
 * sets eax and pops what the function's own `ret` would, and returns 1. A function nobody registered, or
 * a handler returning 0, leaves the game's own code to run, unchanged.
 *
 * Shared by the Windows host (registers nothing) and the Android runtime.
 */
#ifndef NFS_FASTPATH_H
#define NFS_FASTPATH_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef int (*nfs_fastpath_fn)(void);
#define NFS_FASTPATH_SLOTS 12u

/* The lifted wrappers' entry (also declared by pcrecomp's recomp_types.h, ci/pcrecomp). */
int recomp_fastpath(uint32_t va);

/* Before the guest boots. A null handler unregisters; returns 0 when every slot is taken. */
int nfs_fastpath_register(uint32_t va, nfs_fastpath_fn handler);
/* Calls that reached recomp_fastpath for `va`, and those its handler took (diagnostics). */
uint64_t nfs_fastpath_calls(uint32_t va);
uint64_t nfs_fastpath_handled(uint32_t va);

#ifdef __cplusplus
}
#endif
#endif
