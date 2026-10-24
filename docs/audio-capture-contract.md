# Audio capture broker contract

The capture broker is the firmware-owned source for both USB microphone audio
and future on-device speech consumers. It contains no wake-word model,
transport, provider integration, or credential handling.

## Frame contract

- Hardware input: 48 kHz, stereo, signed 16-bit PCM.
- Broker block: 480 stereo frames (10 ms).
- Speech output: 320 mono samples (640 bytes) at 16 kHz, signed 16-bit PCM,
  every 20 ms.
- Conversion: fixed channel selection per active stream, DC removal, bounded
  fixed gain, and a 63-tap anti-alias FIR before 3:1 decimation.
- A zero-energy startup block does not lock channel selection. The first block
  carrying energy selects left, right, or a balanced mix for the stream.

`audio_capture_start_16k` grants ownership to exactly one consumer.
`audio_capture_read_16k` consumes two consecutive broker blocks. The owner must
call it at least once per 160 ms and normally once per 20 ms. A second start is
rejected until `audio_capture_stop_16k` releases ownership.

Reads are bounded to at most 100 ms. A missing/partial I2S block, timeout,
consumer overflow, mute transition, or DSP contract failure invalidates the
stream. No gap is hidden by joining audio from opposite sides of the fault.
The owner must stop and explicitly start a fresh stream. Telemetry identifies
the fault and reports sequence, gap, overflow, level, and clipping counters.
Mute transitions advance a monotonic epoch that is checked again when a frame
is committed to the caller; a rejected read always leaves a zeroed frame.

The USB UAC microphone is a separate real-time consumer of the same broker. It
may catch up to the newest block and emit silence for a gap because USB audio
must keep its clock. The governed speech stream instead fails closed.

## Ownership and future wake-word use

A future microWakeWord/ESP-TFLM slice may hold the single consumer lease and
process one 20 ms frame at a time. It must release the lease before PTT capture
starts or act as the coordinator that hands the same stream to the PTT state
machine; two independent DSP readers are intentionally unsupported so
backpressure and privacy state cannot diverge. The failed first attempt is
removed from `main`; a retry must not take the lease from `app_main` or run the
interpreter on the LVGL/QSPI core ([device-baseline.md](device-baseline.md)).

## Memory bounds

The 16-block ring retains 160 ms and uses about 30.2 KiB of PSRAM. The governed
consumer copy and 20 ms stereo scratch add about 5.7 KiB of PSRAM, for about
35.9 KiB total before allocator metadata. The broker task has a 4 KiB stack.
Its I2S and meter buffers plus the USB copy buffers use about 5.7 KiB of static
RAM. No per-frame allocation occurs.
