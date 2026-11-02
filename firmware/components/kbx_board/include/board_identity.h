// kombify box - which board this hardware is, independent of the image.
//
// The compiled board (Kconfig KBX_BOARD) says what the image was built for;
// the probed board says what the hardware is. Early boot probes the system
// I2C pin pair of every known board for its signature devices before any
// driver touches a pin. A conclusive probe is cached in NVS and only used
// when a later probe is inconclusive. When the probe names another board
// than the compiled one, the box is in board_mismatch: the image never marks
// itself valid, no board driver starts, and the update path selects and
// accepts only images for the probed board.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Probe the hardware. Call first in app_main, before any driver and before
 * system_i2c_get(); later calls return at once. */
void kbx_board_identity_probe(void);

/* Board id this image was built for (CONFIG_KBX_BOARD_ID). */
const char *kbx_board_compiled_id(void);
/* Board id the hardware answered as, or NULL when no probe was conclusive. */
const char *kbx_board_probed_id(void);
/* The board to select and accept updates for: probed, else compiled. */
const char *kbx_board_effective_id(void);
/* "i2c" (probe this boot), "nvs" (cached conclusive probe) or "none". */
const char *kbx_board_probe_source(void);
/* Signature devices each pin pair answered with, for the logs. */
const char *kbx_board_probe_detail(void);
/* True when the probed board is known and differs from the compiled one. */
bool kbx_board_mismatch(void);
/* USB serial string of the effective board (KBX-0001, KBX-RLCD42). */
const char *kbx_board_usb_serial(void);
/* True for a board id of a known S3 Box board. */
bool kbx_board_id_known(const char *id);

/* One "board:" line (compiled, probed, source, mismatch, probe detail). */
void kbx_board_identity_log(void);
/* KBX hw?: "hw: chip= mac= board= image= match=yes|no|unknown via=i2c|nvs|none". */
void kbx_board_hw_log(void);

/* An image that rolled back because of a board mismatch, remembered for the
 * image that runs next ("" when none). */
void kbx_board_note_rejected(const char *version);
const char *kbx_board_rejected(void);

#ifdef __cplusplus
}
#endif
