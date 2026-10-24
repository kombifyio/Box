# Firmware overlays

`esp_websocket_client.c` is Espressif 1.7.0 with three Kombify patches:

- the websocket task stack is allocated in PSRAM
  (`xTaskCreatePinnedToCoreWithCaps` + matching `vTaskDeleteWithCaps`). On
  this board the largest internal-RAM block after LVGL/WiFi/BT is about
  3.5 KB, so the 10 KB task cannot start from internal heap
  (`Error create websocket task`);
- the RX/TX frame buffers are allocated in PSRAM (`kbx_ws_buf_calloc`). As
  plain 4 KB `malloc()`s they hit `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` and
  held 8 KB of internal RAM for as long as the pre-dialed socket idles.
- `esp_websocket_client_kbx_writable()` polls the connected socket for room
  without taking the client lock (dev17), so the uplink keeps audio queued in
  PSRAM instead of blocking in a write while the reader needs the lock.

`firmware/components/kbx_voice/CMakeLists.txt` copies the overlay onto the managed component
at configure time, because `managed_components/` is gitignored and the
Component Manager restores upstream sources.

Upgrade path: bump `espressif/esp_websocket_client` in
`firmware/main/idf_component.yml`, re-apply the `KOMBIFY-PATCH` sites,
replace this overlay.
