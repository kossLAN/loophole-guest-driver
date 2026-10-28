/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "agent.h"
#include "clipboard.h"

#define PORT_ID L"ACPI\\PNP0501\\2"
#define PORT_QUEUE 65536
#define WRITE_MS 5000
#define OPEN_TRIES 10
#define OPEN_RETRY_MS 20

struct link {
   HWND hwnd;
   wchar_t name[64];
   HANDLE port;
   OVERLAPPED read;
   HANDLE write_event;
   uint8_t chunk[4096];
   uint8_t head[sizeof(struct loophole_clipboard_header)];
   struct loophole_clipboard_header header;
   size_t have;
   char *payload;
   char *last;
   int last_size;
};

static BOOL
find_port(wchar_t *name, size_t len)
{
   HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, NULL, NULL, DIGCF_PRESENT);
   if (set == INVALID_HANDLE_VALUE)
      return FALSE;
   BOOL found = FALSE;
   SP_DEVINFO_DATA dev = {.cbSize = sizeof(dev)};
   for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &dev); i++) {
      wchar_t id[256];
      if (!SetupDiGetDeviceInstanceIdW(set, &dev, id, ARRAYSIZE(id), NULL) || _wcsicmp(id, PORT_ID))
         continue;
      HKEY k = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
      if (k == INVALID_HANDLE_VALUE)
         continue;
      wchar_t port[32] = {0};
      DWORD size = sizeof(port) - sizeof(wchar_t);
      if (RegQueryValueExW(k, L"PortName", NULL, NULL, (BYTE *)port, &size) == ERROR_SUCCESS && port[0]) {
         _snwprintf(name, len, L"\\\\.\\%ls", port);
         name[len - 1] = 0;
         found = TRUE;
      }
      RegCloseKey(k);
   }
   SetupDiDestroyDeviceInfoList(set);
   return found;
}

static HANDLE
open_port(const wchar_t *name)
{
   HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
   if (h == INVALID_HANDLE_VALUE)
      return h;
   DCB dcb = {.DCBlength = sizeof(dcb)};
   COMMTIMEOUTS timeouts = {
      .ReadIntervalTimeout = MAXDWORD,
      .ReadTotalTimeoutMultiplier = MAXDWORD,
      .ReadTotalTimeoutConstant = MAXDWORD - 1,
   };
   BOOL ok = GetCommState(h, &dcb);
   if (ok) {
      dcb.BaudRate = CBR_115200;
      dcb.ByteSize = 8;
      dcb.Parity = NOPARITY;
      dcb.StopBits = ONESTOPBIT;
      dcb.fBinary = TRUE;
      dcb.fOutxCtsFlow = FALSE;
      dcb.fOutxDsrFlow = FALSE;
      dcb.fDtrControl = DTR_CONTROL_ENABLE;
      dcb.fRtsControl = RTS_CONTROL_ENABLE;
      dcb.fOutX = FALSE;
      dcb.fInX = FALSE;
      dcb.fAbortOnError = FALSE;
      ok = SetCommState(h, &dcb) && SetupComm(h, PORT_QUEUE, PORT_QUEUE) && SetCommTimeouts(h, &timeouts);
   }
   if (!ok) {
      DWORD error = GetLastError();
      CloseHandle(h);
      SetLastError(error);
      return INVALID_HANDLE_VALUE;
   }
   return h;
}

static BOOL
start_read(struct link *l)
{
   if (ReadFile(l->port, l->chunk, sizeof(l->chunk), NULL, &l->read) || GetLastError() == ERROR_IO_PENDING)
      return TRUE;
   say(L"clipboard: reading %ls failed (%lu)\n", l->name, GetLastError());
   return FALSE;
}

static void
reset_frame(struct link *l)
{
   free(l->payload);
   l->payload = NULL;
   l->have = 0;
}

static void
close_link(struct link *l)
{
   DWORD n;
   CancelIoEx(l->port, NULL);
   GetOverlappedResult(l->port, &l->read, &n, TRUE);
   CloseHandle(l->port);
   l->port = INVALID_HANDLE_VALUE;
   reset_frame(l);
}

static BOOL
open_link(struct link *l, BOOL report)
{
   if (!find_port(l->name, ARRAYSIZE(l->name))) {
      if (report)
         say(L"clipboard: no " PORT_ID L" port; looking again every %u s\n", POLL_MS / 1000);
      return FALSE;
   }
   l->port = open_port(l->name);
   if (l->port == INVALID_HANDLE_VALUE) {
      if (report)
         say(L"clipboard: cannot open %ls (%lu); trying again every %u s\n", l->name, GetLastError(),
             POLL_MS / 1000);
      return FALSE;
   }
   if (!start_read(l)) {
      close_link(l);
      return FALSE;
   }
   say(L"clipboard: on %ls\n", l->name);
   return TRUE;
}

static BOOL
is_last(const struct link *l, const char *text, int size)
{
   return l->last && l->last_size == size && !memcmp(l->last, text, size);
}

static void
remember(struct link *l, const char *text, int size)
{
   char *copy = malloc(size + 1);
   if (!copy)
      return;
   memcpy(copy, text, size);
   free(l->last);
   l->last = copy;
   l->last_size = size;
}

static BOOL
open_clipboard(HWND hwnd)
{
   for (int i = 0; i < OPEN_TRIES; i++) {
      if (OpenClipboard(hwnd))
         return TRUE;
      Sleep(OPEN_RETRY_MS);
   }
   return FALSE;
}

static BOOL
lone_lf(const wchar_t *s, int i)
{
   return s[i] == L'\n' && (!i || s[i - 1] != L'\r');
}

static HGLOBAL
windows_text(const char *text, int size)
{
   int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, size, NULL, 0);
   if (size && !n)
      return NULL;
   wchar_t *wide = malloc((n + 1) * sizeof(*wide));
   if (!wide)
      return NULL;
   MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, size, wide, n);
   int lone = 0;
   for (int i = 0; i < n; i++)
      lone += lone_lf(wide, i);
   HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (n + lone + 1) * sizeof(wchar_t));
   wchar_t *out = mem ? GlobalLock(mem) : NULL;
   if (out) {
      int m = 0;
      for (int i = 0; i < n; i++) {
         if (lone_lf(wide, i))
            out[m++] = L'\r';
         out[m++] = wide[i];
      }
      out[m] = 0;
      GlobalUnlock(mem);
   } else if (mem) {
      GlobalFree(mem);
      mem = NULL;
   }
   free(wide);
   return mem;
}

static char *
host_text(const wchar_t *wide, int *size)
{
   int n = (int)wcslen(wide);
   wchar_t *lf = malloc((n + 1) * sizeof(*lf));
   if (!lf)
      return NULL;
   int m = 0;
   for (int i = 0; i < n; i++) {
      if (wide[i] != L'\r' || wide[i + 1] != L'\n')
         lf[m++] = wide[i];
   }
   int bytes = WideCharToMultiByte(CP_UTF8, 0, lf, m, NULL, 0, NULL, NULL);
   char *text = malloc(bytes + 1);
   if (text)
      *size = WideCharToMultiByte(CP_UTF8, 0, lf, m, text, bytes, NULL, NULL);
   free(lf);
   return text;
}

static void
set_clipboard(struct link *l, const char *text, int size)
{
   if (is_last(l, text, size))
      return;
   HGLOBAL mem = windows_text(text, size);
   if (!mem) {
      say(L"clipboard: dropped %d bytes from the host\n", size);
      return;
   }
   if (!open_clipboard(l->hwnd)) {
      say(L"clipboard: cannot open the clipboard (%lu)\n", GetLastError());
      GlobalFree(mem);
      return;
   }
   EmptyClipboard();
   if (SetClipboardData(CF_UNICODETEXT, mem))
      remember(l, text, size);
   else
      GlobalFree(mem);
   CloseClipboard();
}

static void
start_frame(struct link *l)
{
   memcpy(&l->header, l->head, sizeof(l->header));
   if (l->header.magic == LOOPHOLE_CLIPBOARD_MAGIC && l->header.size <= LOOPHOLE_CLIPBOARD_MAX &&
       (l->payload = malloc(l->header.size + 1))) {
      l->have = 0;
      return;
   }
   memmove(l->head, l->head + 1, sizeof(l->head) - 1);
   l->have = sizeof(l->head) - 1;
}

static void
feed(struct link *l, const uint8_t *data, size_t n)
{
   for (;;) {
      if (l->payload && l->have == l->header.size) {
         if (l->header.type == LOOPHOLE_CLIPBOARD_TEXT)
            set_clipboard(l, l->payload, (int)l->header.size);
         reset_frame(l);
      }
      if (!n)
         return;
      if (l->payload) {
         size_t take = min(l->header.size - l->have, n);
         memcpy(l->payload + l->have, data, take);
         l->have += take;
         data += take;
         n -= take;
      } else {
         l->head[l->have++] = *data++;
         n--;
         if (l->have == sizeof(l->head))
            start_frame(l);
      }
   }
}

static BOOL
write_all(struct link *l, const void *data, DWORD size)
{
   OVERLAPPED o = {.hEvent = l->write_event};
   DWORD done = 0;
   if (!WriteFile(l->port, data, size, NULL, &o) && GetLastError() != ERROR_IO_PENDING)
      return FALSE;
   if (WaitForSingleObject(o.hEvent, WRITE_MS) != WAIT_OBJECT_0) {
      CancelIoEx(l->port, &o);
      GetOverlappedResult(l->port, &o, &done, TRUE);
      SetLastError(ERROR_TIMEOUT);
      return FALSE;
   }
   return GetOverlappedResult(l->port, &o, &done, FALSE) && done == size;
}

static void
send_text(struct link *l, const char *text, int size)
{
   if (size > (int)LOOPHOLE_CLIPBOARD_MAX) {
      say(L"clipboard: %d bytes is over the %u MiB limit\n", size, LOOPHOLE_CLIPBOARD_MAX >> 20);
      return;
   }
   struct loophole_clipboard_header header = {
      .magic = LOOPHOLE_CLIPBOARD_MAGIC,
      .type = LOOPHOLE_CLIPBOARD_TEXT,
      .size = (uint32_t)size,
   };
   char *frame = malloc(sizeof(header) + size);
   if (!frame)
      return;
   memcpy(frame, &header, sizeof(header));
   memcpy(frame + sizeof(header), text, size);
   if (write_all(l, frame, (DWORD)(sizeof(header) + size)))
      remember(l, text, size);
   else
      say(L"clipboard: writing to %ls failed (%lu)\n", l->name, GetLastError());
   free(frame);
}

static void
send_clipboard(struct link *l)
{
   if (GetClipboardOwner() == l->hwnd || !IsClipboardFormatAvailable(CF_UNICODETEXT) ||
       !open_clipboard(l->hwnd))
      return;
   char *text = NULL;
   int size = 0;
   HANDLE data = GetClipboardData(CF_UNICODETEXT);
   const wchar_t *wide = data ? GlobalLock(data) : NULL;
   if (wide) {
      text = host_text(wide, &size);
      GlobalUnlock(data);
   }
   CloseClipboard();
   if (text && !is_last(l, text, size))
      send_text(l, text, size);
   free(text);
}

DWORD WINAPI
clipboard_thread(void *arg)
{
   (void)arg;
   struct link l = {.port = INVALID_HANDLE_VALUE};
   WNDCLASSW wc = {
      .lpfnWndProc = DefWindowProcW,
      .hInstance = GetModuleHandleW(NULL),
      .lpszClassName = L"loophole-clipboard",
   };
   if (!RegisterClassW(&wc) ||
       !(l.hwnd = CreateWindowExW(0, wc.lpszClassName, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL)) ||
       !AddClipboardFormatListener(l.hwnd))
      return say(L"clipboard: cannot follow the clipboard (%lu)\n", GetLastError()), 1;
   l.read.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
   l.write_event = CreateEventW(NULL, TRUE, FALSE, NULL);
   if (!l.read.hEvent || !l.write_event)
      return say(L"clipboard: no events (%lu)\n", GetLastError()), 1;

   BOOL waiting = FALSE;
   for (;;) {
      if (l.port == INVALID_HANDLE_VALUE)
         waiting = !open_link(&l, !waiting);
      BOOL open = l.port != INVALID_HANDLE_VALUE;
      DWORD count = open ? 1 : 0;
      DWORD r = MsgWaitForMultipleObjects(count, &l.read.hEvent, FALSE, open ? INFINITE : POLL_MS, QS_ALLINPUT);
      if (open && r == WAIT_OBJECT_0) {
         DWORD n = 0;
         if (!GetOverlappedResult(l.port, &l.read, &n, FALSE)) {
            say(L"clipboard: reading %ls failed (%lu)\n", l.name, GetLastError());
            close_link(&l);
            continue;
         }
         feed(&l, l.chunk, n);
         if (!start_read(&l))
            close_link(&l);
      } else if (r == WAIT_OBJECT_0 + count) {
         MSG msg;
         while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message != WM_CLIPBOARDUPDATE)
               DispatchMessageW(&msg);
            else if (open)
               send_clipboard(&l);
         }
      } else if (r == WAIT_FAILED) {
         say(L"clipboard: wait failed (%lu)\n", GetLastError());
         return 1;
      }
   }
}
