/* SPDX-License-Identifier: MIT */
#pragma once

#include <windows.h>

#define POLL_MS 5000

void say(const wchar_t *fmt, ...);
DWORD WINAPI clipboard_thread(void *arg);
