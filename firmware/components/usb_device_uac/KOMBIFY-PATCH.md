# usb_device_uac - locally patched

This component is the **only** exception to the rule that third-party packages
come through the Component Manager (see `firmware/main/idf_component.yml`).
It lives here because it carries a patch that cannot be expressed through
Kconfig or a build option.

## Base

Unmodified `espressif/usb_device_uac` **1.3.1** from the ESP Component
Registry (`CHANGELOG.md` and `idf_component.yml` are the upstream originals).
Only `test_apps/` was not carried over - that is upstream CI and does not
belong in our build.

## Why the patch

Upstream enumerates the Box as a pure USB audio device. We additionally need a
**CDC-ACM port in the same USB composite**: it carries the `KBX` command
channel used to commission the Box **without a network connection** (WLAN
credentials into NVS, see `firmware/main/usb_log.c` and
`firmware/components/kbx_net/net_wifi.c`). Without this port there is no offline way onto
the device.

TinyUSB decides at **compile time** via `CFG_TUD_CDC` whether the CDC class
exists at all, and the interface/endpoint layout of the composite is hard-coded
in the descriptor sources of this component. Neither can be overridden from
outside - hence the fork instead of a configuration.

## The patch (complete)

All locations are marked `KOMBIFY-PATCH` in the source.

| File | Change |
|---|---|
| `tusb/tusb_config.h` | `CFG_TUD_CDC 1` + RX/TX/EP buffer sizes |
| `tusb_uac/uac_descriptors.h` | `ITF_NUM_CDC_COMM` / `ITF_NUM_CDC_DATA` in the interface enum |
| `tusb/usb_descriptors.c` | CDC descriptor in the composite, endpoints `0x83`/`0x03`/`0x84`, string index `STRID_CDC_LOG` ("logs"), `CONFIG_TOTAL_LEN` extended by `TUD_CDC_DESC_LEN`, `bcdDevice` `0x0100` -> `0x0101` |
| `usb_device_uac.c` | Volume clamp to 0..100 before the `uint32_t` callback (bug fix, independent of the CDC topic) |

## When upgrading to a new upstream version

1. Unpack the new version from the registry and replace these files
   (leave out `test_apps/`).
2. Re-apply the four changes from the table - the
   `KOMBIFY-PATCH` markers show the locations.
3. Check whether upstream now has its own CDC option - if so, this fork goes
   away and the component moves back into the manifest.
4. The volume clamp is an upstream bug: once it is fixed there, keep only
   the CDC part.

Do not maintain a diff against a local vendoring state - always compare against
the registry version with the same number.
