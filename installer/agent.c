/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

/* NTSTATUS, which d3dkmthk.h needs. */
#include <winternl.h>
#include <d3dkmthk.h>

#include "agent.h"
#include "viogpum.h"

#define RESOLUTION_EVENT L"Global\\VioGpuResolutionEvent"

#define VIRTIO_GPU_ID L"PCI\\VEN_1AF4&DEV_1050"
#define MAX_DISPLAYS 8
/* Windows takes a moment to re-read the monitor after an event. */
#define SETTLE_MS 1000
#define SETTLE_CHECKS 3

static PFND3DKMT_OPENADAPTERFROMHDC open_adapter;
static PFND3DKMT_CLOSEADAPTER close_adapter;
static PFND3DKMT_ESCAPE escape;
static FILE *logf;
static SRWLOCK log_lock = SRWLOCK_INIT;

struct display {
   wchar_t name[CCHDEVICENAME];
   D3DKMT_HANDLE adapter;
   HANDLE event;
   USHORT width, height;
   unsigned settle;
};

void
say(const wchar_t *fmt, ...)
{
   if (!logf)
      return;
   SYSTEMTIME t;
   GetLocalTime(&t);
   AcquireSRWLockExclusive(&log_lock);
   fwprintf(logf, L"%02u:%02u:%02u ", t.wHour, t.wMinute, t.wSecond);
   va_list ap;
   va_start(ap, fmt);
   vfwprintf(logf, fmt, ap);
   va_end(ap);
   fflush(logf);
   ReleaseSRWLockExclusive(&log_lock);
}

static LONG
viogpu_escape(D3DKMT_HANDLE adapter, VIOGPU_ESCAPE *e)
{
   D3DKMT_ESCAPE esc = {
      .hAdapter = adapter,
      .pPrivateDriverData = e,
      .PrivateDriverDataSize = sizeof(*e),
   };
   return escape(&esc);
}

static void
close_display(struct display *d)
{
   if (d->event)
      CloseHandle(d->event);
   if (d->adapter) {
      D3DKMT_CLOSEADAPTER c = {d->adapter};
      NTSTATUS closed = close_adapter(&c);
      (void)closed;
   }
   memset(d, 0, sizeof(*d));
}

static BOOL
open_display(struct display *d, const wchar_t *name)
{
   wcsncpy(d->name, name, ARRAYSIZE(d->name) - 1);
   HDC dc = CreateDCW(NULL, name, NULL, NULL);
   if (!dc)
      return say(L"%ls: no device context (%lu)\n", name, GetLastError()), FALSE;
   D3DKMT_OPENADAPTERFROMHDC open = {.hDc = dc};
   LONG st = open_adapter(&open);
   DeleteDC(dc);
   if (st < 0)
      return say(L"%ls: D3DKMTOpenAdapterFromHdc 0x%08lx\n", name, st), FALSE;
   d->adapter = open.hAdapter;

   /* Asking for the id makes the driver create the event. */
   VIOGPU_ESCAPE e = {.Type = VIOGPU_GET_DEVICE_ID, .DataLength = sizeof(ULONG)};
   if ((st = viogpu_escape(d->adapter, &e)) < 0)
      return say(L"%ls: not a loophole display (0x%08lx)\n", name, st), close_display(d), FALSE;
   wchar_t event[64];
   _snwprintf(event, ARRAYSIZE(event), RESOLUTION_EVENT L"%lu", e.Id);
   event[ARRAYSIZE(event) - 1] = 0;
   d->event = OpenEventW(SYNCHRONIZE, FALSE, event);
   if (!d->event)
      return say(L"%ls: cannot open %ls (%lu)\n", name, event, GetLastError()), close_display(d), FALSE;
   say(L"%ls: display %lu\n", name, e.Id);
   return TRUE;
}

static unsigned
open_displays(struct display *d)
{
   unsigned n = 0;
   DISPLAY_DEVICEW dev = {.cb = sizeof(dev)};
   for (DWORD i = 0; n < MAX_DISPLAYS && EnumDisplayDevicesW(NULL, i, &dev, 0); i++) {
      if ((dev.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) &&
          !_wcsnicmp(dev.DeviceID, VIRTIO_GPU_ID, wcslen(VIRTIO_GPU_ID)) && open_display(&d[n], dev.DeviceName))
         n++;
   }
   return n;
}

struct config {
   UINT32 npaths, nmodes;
   DISPLAYCONFIG_PATH_INFO *paths;
   DISPLAYCONFIG_MODE_INFO *modes;
   DISPLAYCONFIG_PATH_INFO *path;
   DISPLAYCONFIG_SOURCE_MODE *source;
};

static void
free_config(struct config *c)
{
   free(c->paths);
   free(c->modes);
   memset(c, 0, sizeof(*c));
}

static BOOL
query_config(struct config *c, const wchar_t *name)
{
   memset(c, 0, sizeof(*c));
   LONG rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &c->npaths, &c->nmodes);
   if (rc == ERROR_SUCCESS) {
      c->paths = calloc(c->npaths, sizeof(*c->paths));
      c->modes = calloc(c->nmodes, sizeof(*c->modes));
      rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &c->npaths, c->paths, &c->nmodes, c->modes, NULL);
   }
   for (UINT32 i = 0; rc == ERROR_SUCCESS && i < c->npaths && !c->source; i++) {
      DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {
         .header = {.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
                    .size = sizeof(src),
                    .adapterId = c->paths[i].sourceInfo.adapterId,
                    .id = c->paths[i].sourceInfo.id},
      };
      UINT32 mode = c->paths[i].sourceInfo.modeInfoIdx;
      if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS && !wcscmp(src.viewGdiDeviceName, name) &&
          mode < c->nmodes) {
         c->path = &c->paths[i];
         c->source = &c->modes[mode].sourceMode;
      }
   }
   if (!c->source) {
      say(L"%ls: no active display path (%ld)\n", name, rc);
      free_config(c);
   }
   return c->source != NULL;
}

/* For a size Windows does not list yet: Windows picks a target mode that fits,
 * or another size. */
static void
set_desktop_size(const wchar_t *name, UINT32 width, UINT32 height)
{
   struct config c;
   if (!query_config(&c, name))
      return;
   if (c.source->width == width && c.source->height == height) {
      free_config(&c);
      return;
   }
   say(L"%ls: %ux%u -> %ux%u\n", name, c.source->width, c.source->height, width, height);
   c.source->width = width;
   c.source->height = height;
   c.path->targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
   LONG rc = SetDisplayConfig(c.npaths, c.paths, c.nmodes, c.modes,
                              SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES |
                                 SDC_SAVE_TO_DATABASE);
   free_config(&c);
   if (rc != ERROR_SUCCESS)
      say(L"%ls: SetDisplayConfig failed (%ld)\n", name, rc);
   else if (query_config(&c, name)) {
      if (c.source->width != width || c.source->height != height)
         say(L"%ls: Windows chose %ux%u instead\n", name, c.source->width, c.source->height);
      free_config(&c);
   }
}

static void
set_desktop_mode(const wchar_t *name, DWORD width, DWORD height)
{
   DEVMODEW cur = {.dmSize = sizeof(cur)}, m = {.dmSize = sizeof(m)};
   if (!EnumDisplaySettingsW(name, ENUM_CURRENT_SETTINGS, &cur)) {
      say(L"%ls: no current mode\n", name);
      return;
   }
   DWORD rate = 0;
   for (DWORD i = 0; EnumDisplaySettingsW(name, i, &m); i++) {
      if (m.dmPelsWidth == width && m.dmPelsHeight == height && m.dmBitsPerPel == cur.dmBitsPerPel &&
          m.dmDisplayFrequency > rate)
         rate = m.dmDisplayFrequency;
   }
   if (!rate) {
      set_desktop_size(name, width, height);
      return;
   }
   if (cur.dmPelsWidth == width && cur.dmPelsHeight == height && cur.dmDisplayFrequency >= rate)
      return;
   say(L"%ls: %lux%lu@%lu -> %lux%lu@%lu\n", name, cur.dmPelsWidth, cur.dmPelsHeight, cur.dmDisplayFrequency,
       width, height, rate);
   cur.dmPelsWidth = width;
   cur.dmPelsHeight = height;
   cur.dmDisplayFrequency = rate;
   cur.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
   LONG rc = ChangeDisplaySettingsExW(name, &cur, NULL, CDS_UPDATEREGISTRY, NULL);
   if (rc != DISP_CHANGE_SUCCESSFUL)
      say(L"%ls: ChangeDisplaySettingsEx failed (%ld)\n", name, rc);
}

static BOOL
follow(struct display *d, BOOL now)
{
   VIOGPU_ESCAPE e = {.Type = VIOGPU_GET_CUSTOM_RESOLUTION, .DataLength = 4};
   LONG st = viogpu_escape(d->adapter, &e);
   if (st < 0)
      return say(L"%ls: lost the display driver (0x%08lx)\n", d->name, st), FALSE;
   USHORT w = e.Resolution.XResolution, h = e.Resolution.YResolution;
   if (w && h && (now || w != d->width || h != d->height)) {
      d->width = w;
      d->height = h;
      set_desktop_mode(d->name, w, h);
   }
   return TRUE;
}

static void
open_log(void)
{
   wchar_t dir[MAX_PATH], path[MAX_PATH];
   if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, dir) != S_OK)
      return;
   wcsncat(dir, L"\\loophole", MAX_PATH - wcslen(dir) - 1);
   CreateDirectoryW(dir, NULL);
   _snwprintf(path, MAX_PATH, L"%ls\\agent.log", dir);
   path[MAX_PATH - 1] = 0;
   logf = _wfopen(path, L"w");
}

int WINAPI
wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
   (void)inst, (void)prev, (void)cmdline, (void)show;
   CreateMutexW(NULL, FALSE, L"Local\\loophole-agent");
   if (GetLastError() == ERROR_ALREADY_EXISTS)
      return 0;
   open_log();

   HANDLE clipboard = CreateThread(NULL, 0, clipboard_thread, NULL, 0, NULL);
   if (clipboard)
      CloseHandle(clipboard);
   else
      say(L"no clipboard thread (%lu)\n", GetLastError());

   HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
   open_adapter = (void *)GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc");
   close_adapter = (void *)GetProcAddress(gdi, "D3DKMTCloseAdapter");
   escape = (void *)GetProcAddress(gdi, "D3DKMTEscape");
   if (!open_adapter || !close_adapter || !escape)
      return say(L"gdi32 has no D3DKMT functions\n"), 1;

   struct display d[MAX_DISPLAYS] = {0};
   BOOL found = TRUE;
   for (;;) {
      unsigned n = open_displays(d);
      if (found && !n)
         say(L"no loophole display on the desktop; looking again every %u s\n", POLL_MS / 1000);
      found = n != 0;
      HANDLE events[MAX_DISPLAYS];
      BOOL ok = TRUE;
      for (unsigned i = 0; i < n; i++) {
         events[i] = d[i].event;
         ok = ok && follow(&d[i], FALSE);
      }
      while (ok) {
         DWORD wait = POLL_MS;
         for (unsigned i = 0; i < n; i++) {
            if (d[i].settle)
               wait = SETTLE_MS;
         }
         DWORD r = n ? WaitForMultipleObjects(n, events, FALSE, wait) : (Sleep(wait), WAIT_TIMEOUT);
         if (r < WAIT_OBJECT_0 + n) {
            d[r - WAIT_OBJECT_0].settle = SETTLE_CHECKS;
            ok = follow(&d[r - WAIT_OBJECT_0], TRUE);
         } else if (r == WAIT_TIMEOUT) {
            for (unsigned i = 0; ok && i < n; i++) {
               BOOL settling = d[i].settle != 0;
               if (settling)
                  d[i].settle--;
               ok = follow(&d[i], settling);
            }
            ok = ok && n;
         } else {
            say(L"wait failed (%lu)\n", GetLastError());
            ok = FALSE;
         }
      }
      for (unsigned i = 0; i < n; i++)
         close_display(&d[i]);
   }
}
