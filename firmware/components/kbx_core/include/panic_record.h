/*
 * kombify box - the last panic, kept in RTC memory across the panic reset.
 *
 * The console is USB CDC, so the panic handler's output never reaches a
 * host, and boxes updated over CDC OTA have no coredump partition. A wrapper
 * around esp_panic_handler stores PC, EXCCAUSE, EXCVADDR, a0-a15, the reason
 * and a short backtrace; the next boot logs it (KBX coredump? repeats it,
 * KBX coredump clear forgets it). Decode the PCs with addr2line on the ELF.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool panic_record_valid(void);
void panic_record_log(void);
void panic_record_clear(void);

#ifdef __cplusplus
}
#endif
