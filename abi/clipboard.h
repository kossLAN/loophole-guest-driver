/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>

#define LOOPHOLE_CLIPBOARD_MAGIC 0x6263686cu
#define LOOPHOLE_CLIPBOARD_TEXT 1u
#define LOOPHOLE_CLIPBOARD_MAX (1u << 20)

struct loophole_clipboard_header {
   uint32_t magic;
   uint32_t type;
   uint32_t size;
};
