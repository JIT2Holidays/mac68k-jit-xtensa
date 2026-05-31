/* C entry points into the FastEPD-based e-paper path (third_party/FastEPD).
 * FastEPD is C++ with C++ symbol linkage, so the C side of the project (app_main.c,
 * touch.c) only ever calls these extern-"C" shims, implemented in the C++ glue
 * eink_fastepd.cpp which owns the single FASTEPD instance. */
#ifndef EINK_FASTEPD_H
#define EINK_FASTEPD_H

#include <stdbool.h>

struct mac_mem;
struct m68k_cpu;

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the PaperS3 panel via FastEPD and start the core-1 render task that
 * scans the guest framebuffer into FastEPD's buffer and pushes partial updates.
 * Mirrors eink_start()'s role. Returns false if the panel failed to init. */
bool eink_fastepd_start(struct mac_mem *mem, struct m68k_cpu *cpu, const char *engine);

/* Request a full de-ghost refresh at the next frame (status-bar tap). */
void eink_fastepd_request_refresh(void);

#ifdef __cplusplus
}
#endif

#endif /* EINK_FASTEPD_H */
