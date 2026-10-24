#pragma once

/* The device's one voice control, shared by every input (touch on the
 * 1.85C, the KEY button on the RLCD 4.2). Task context, never ISR. */

/* Tap: stop a conversation or playback, else toggle talk; says why when
 * talk cannot start (offline, signed out). */
void talk_action_tap(void);

/* Hold: toggle the local mute latch; muting also ends an open talk. */
void talk_action_hold_mute(void);
