/* C entry points into the FastEPD-based e-paper path (third_party/FastEPD).
 * FastEPD is C++ with C++ symbol linkage, so the C side of the project (eink.c,
 * app_main.c) only ever calls these extern-"C" shims, which are implemented in
 * the C++ glue eink_fastepd.cpp that owns the FASTEPD instance. */
#ifndef EINK_FASTEPD_H
#define EINK_FASTEPD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Step-2 bring-up: init the PaperS3 panel via FastEPD, clear to white, and draw
 * a visible test image (box + text). Returns false if initPanel failed. */
bool eink_fastepd_bringup(void);

#ifdef __cplusplus
}
#endif

#endif /* EINK_FASTEPD_H */
