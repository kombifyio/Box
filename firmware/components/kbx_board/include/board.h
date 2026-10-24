// kombify box - Kconfig board profile selects pins and numeric tuning.
// Feature switches and the OTA board id come from Kconfig capabilities.
#pragma once
#include "sdkconfig.h"

#if CONFIG_KBX_BOARD_WAVESHARE_RLCD42
#include "board_rlcd42.h"
#else
#include "board_185c.h"
#endif
