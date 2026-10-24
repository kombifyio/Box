/*
 * kombify box - optional ESP log mirror over the composite USB CDC port.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void usb_log_install(void);

/* Dispatch one "KBX <cmd>" line exactly like the CDC channel does. The line
 * buffer may be modified. */
void usb_log_exec_line(char *line);

/* Copy every log line into buf (NUL-terminated, truncated at cap) until
 * usb_log_capture_end, which returns the captured length. One capture at a
 * time: begin returns false while another is open. */
bool usb_log_capture_begin(char *buf, size_t cap);
size_t usb_log_capture_end(void);

#ifdef __cplusplus
}
#endif
