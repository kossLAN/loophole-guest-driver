/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cfgmgr32.h>
#include <initguid.h>
#include <wincrypt.h>
#include <devguid.h>
#include <newdev.h>
#include <setupapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define REBOOT_REQUIRED 3010
#define PROVIDER L"loophole"
#define AGENT L"loophole-agent.exe"

static wchar_t root[MAX_PATH];
static BOOL notify;
static FILE *logf;

static void usage(FILE *f);

static void
say(const wchar_t *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vfwprintf(stdout, fmt, ap);
   va_end(ap);
   if (logf) {
      va_start(ap, fmt);
      vfwprintf(logf, fmt, ap);
      va_end(ap);
   }
}

static void
report_v(BOOL error, const wchar_t *fmt, va_list ap)
{
   wchar_t text[2048];
   _vsnwprintf(text, ARRAYSIZE(text), fmt, ap);
   text[ARRAYSIZE(text) - 1] = 0;
   if (notify)
      MessageBoxW(NULL, text, L"loophole", MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
   else
      fwprintf(error ? stderr : stdout, error ? L"loophole: %ls\n" : L"%ls\n", text);
   if (logf)
      fwprintf(logf, error ? L"error: %ls\n" : L"%ls\n", text);
}

static void
report(const wchar_t *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   report_v(FALSE, fmt, ap);
   va_end(ap);
}

static int
error(const wchar_t *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   report_v(TRUE, fmt, ap);
   va_end(ap);
   return 1;
}

static int
fail(const wchar_t *what, DWORD err)
{
   wchar_t msg[512] = L"";
   FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, msg,
                  ARRAYSIZE(msg), NULL);
   for (size_t n = wcslen(msg); n && (msg[n - 1] == L'\n' || msg[n - 1] == L'\r'); n--)
      msg[n - 1] = 0;
   return error(L"%ls: %ls (0x%08lx)", what, msg, err);
}

static void
join(wchar_t *out, size_t n, const wchar_t *a, const wchar_t *b)
{
   _snwprintf(out, n, L"%ls\\%ls", a, b);
   out[n - 1] = 0;
}

static BOOL
exists(const wchar_t *path)
{
   return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static BOOL
reg_dword(const wchar_t *key, const wchar_t *value, DWORD *out)
{
   DWORD size = sizeof(*out);
   return RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, NULL, out,
                       &size) == ERROR_SUCCESS;
}

static BOOL
testsigning_on(void)
{
   wchar_t opts[1024] = L"";
   DWORD size = sizeof(opts);
   RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control", L"SystemStartOptions",
                RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, NULL, opts, &size);
   _wcsupr(opts);
   return wcsstr(opts, L"TESTSIGNING") != NULL;
}

static BOOL
secure_boot_on(void)
{
   DWORD on = 0;
   return reg_dword(L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State", L"UEFISecureBootEnabled", &on) && on;
}

static DWORD
run_system(const wchar_t *exe, const wchar_t *args)
{
   wchar_t sys[MAX_PATH], cmd[1024];
   GetSystemDirectoryW(sys, MAX_PATH);
   _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\\%ls\" %ls", sys, exe, args);
   cmd[ARRAYSIZE(cmd) - 1] = 0;
   STARTUPINFOW si = {.cb = sizeof(si)};
   PROCESS_INFORMATION pi;
   if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
      return (DWORD)-1;
   WaitForSingleObject(pi.hProcess, INFINITE);
   DWORD code = (DWORD)-1;
   GetExitCodeProcess(pi.hProcess, &code);
   CloseHandle(pi.hThread);
   CloseHandle(pi.hProcess);
   return code;
}

/* Windows refuses test signing while Secure Boot is on, and applies it only
 * after a restart. */
static int
cmd_testsigning(int argc, wchar_t **argv)
{
   BOOL on = testsigning_on();
   if (argc == 1 && !wcscmp(argv[0], L"status")) {
      say(L"test signing %ls, Secure Boot %ls\n", on ? L"on" : L"off", secure_boot_on() ? L"on" : L"off");
      return on ? 0 : 1;
   }
   if (argc != 1 || wcscmp(argv[0], L"enable"))
      return fwprintf(stderr, L"usage: loophole testsigning status|enable\n"), 2;
   if (on)
      return 0;
   if (secure_boot_on()) {
      return error(L"Secure Boot is on; turn it off in the VM's firmware settings, then run the installer "
                   L"again");
   }
   /* A changed boot configuration sends a BitLocker drive into recovery. */
   wchar_t drive[8] = L"";
   if (GetEnvironmentVariableW(L"SystemDrive", drive, ARRAYSIZE(drive))) {
      wchar_t args[64];
      _snwprintf(args, ARRAYSIZE(args), L"-protectors -disable %ls -RebootCount 1", drive);
      args[ARRAYSIZE(args) - 1] = 0;
      run_system(L"manage-bde.exe", args);
   }
   DWORD code = run_system(L"bcdedit.exe", L"/set testsigning on");
   if (code != 0)
      return error(L"bcdedit /set testsigning on failed (%ld)", (long)code);
   say(L"test signing enabled; it takes effect after a restart\n");
   return REBOOT_REQUIRED;
}

static int
trust_certificate(const wchar_t *cer, const wchar_t *store_name)
{
   HANDLE f = CreateFileW(cer, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
   if (f == INVALID_HANDLE_VALUE)
      return fail(cer, GetLastError());
   BYTE der[8192];
   DWORD n = 0;
   BOOL read = ReadFile(f, der, sizeof(der), &n, NULL);
   CloseHandle(f);
   if (!read)
      return fail(cer, GetLastError());
   HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, store_name);
   if (!store)
      return fail(store_name, GetLastError());
   BOOL ok = CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, der, n, CERT_STORE_ADD_REPLACE_EXISTING,
                                              NULL);
   DWORD err = GetLastError();
   CertCloseStore(store, 0);
   return ok ? 0 : fail(store_name, err);
}

static BOOL
inf_provider(const wchar_t *inf, wchar_t *out, DWORD n)
{
   HINF h = SetupOpenInfFileW(inf, NULL, INF_STYLE_WIN4, NULL);
   if (h == INVALID_HANDLE_VALUE)
      return FALSE;
   INFCONTEXT ctx;
   BOOL ok = SetupFindFirstLineW(h, L"Version", L"Provider", &ctx) &&
             SetupGetStringFieldW(&ctx, 1, out, n, NULL);
   SetupCloseInfFile(h);
   return ok;
}

static BOOL
inf_original_name(const wchar_t *published, wchar_t *out, DWORD n)
{
   DWORD size = 0;
   SetupGetInfInformationW(published, INFINFO_INF_NAME_IS_ABSOLUTE, NULL, 0, &size);
   SP_INF_INFORMATION *info = malloc(size);
   BOOL ok = info && SetupGetInfInformationW(published, INFINFO_INF_NAME_IS_ABSOLUTE, info, size, NULL);
   SP_ORIGINAL_FILE_INFO_W orig = {.cbSize = sizeof(orig)};
   ok = ok && SetupQueryInfOriginalFileInformationW(info, 0, NULL, &orig);
   if (ok)
      wcsncpy(out, orig.OriginalInfName, n);
   free(info);
   return ok;
}

/* newdev.dll's Di*Driver functions, which mingw's import library lacks. */
typedef BOOL(WINAPI *di_driver_fn)(HWND, LPCWSTR, DWORD, PBOOL);

static di_driver_fn
newdev(const char *name)
{
   static HMODULE lib;
   if (!lib)
      lib = LoadLibraryW(L"newdev.dll");
   return lib ? (di_driver_fn)(void *)GetProcAddress(lib, name) : NULL;
}

static BOOL
ours_named(const wchar_t *path, const void *ctx)
{
   wchar_t orig[MAX_PATH], provider[256];
   return inf_original_name(path, orig, MAX_PATH) && !_wcsicmp(orig, ctx) &&
          inf_provider(path, provider, ARRAYSIZE(provider)) && !_wcsicmp(provider, PROVIDER);
}

static BOOL
inf_lists_gpu(HINF h)
{
   INFCONTEXT m;
   if (!SetupFindFirstLineW(h, L"Manufacturer", NULL, &m))
      return FALSE;
   do {
      wchar_t models[256];
      if (!SetupGetStringFieldW(&m, 1, models, ARRAYSIZE(models), NULL))
         continue;
      DWORD fields = SetupGetFieldCount(&m);
      for (DWORD f = 1; f <= fields; f++) {
         wchar_t section[512], decoration[256];
         if (f == 1)
            _snwprintf(section, ARRAYSIZE(section), L"%ls", models);
         else if (SetupGetStringFieldW(&m, f, decoration, ARRAYSIZE(decoration), NULL))
            _snwprintf(section, ARRAYSIZE(section), L"%ls.%ls", models, decoration);
         else
            continue;
         section[ARRAYSIZE(section) - 1] = 0;
         INFCONTEXT line;
         if (!SetupFindFirstLineW(h, section, NULL, &line))
            continue;
         do {
            DWORD n = SetupGetFieldCount(&line);
            for (DWORD i = 2; i <= n; i++) {
               wchar_t id[512];
               if (SetupGetStringFieldW(&line, i, id, ARRAYSIZE(id), NULL) &&
                   wcsstr(_wcsupr(id), L"PCI\\VEN_1AF4&DEV_1050"))
                  return TRUE;
            }
         } while (SetupFindNextLine(&line, &line));
      }
   } while (SetupFindNextLine(&m, &m));
   return FALSE;
}

/* Another provider's display driver for the virtio GPU (viogpudo, Windows
 * Update). */
static BOOL
competes(const wchar_t *path, const void *ctx)
{
   (void)ctx;
   wchar_t provider[256], class[64];
   if (!inf_provider(path, provider, ARRAYSIZE(provider)) || !_wcsicmp(provider, PROVIDER))
      return FALSE;
   HINF h = SetupOpenInfFileW(path, NULL, INF_STYLE_WIN4, NULL);
   if (h == INVALID_HANDLE_VALUE)
      return FALSE;
   INFCONTEXT ctx_line;
   BOOL display = SetupFindFirstLineW(h, L"Version", L"Class", &ctx_line) &&
                  SetupGetStringFieldW(&ctx_line, 1, class, ARRAYSIZE(class), NULL) &&
                  !_wcsicmp(class, L"Display");
   BOOL lists = display && inf_lists_gpu(h);
   SetupCloseInfFile(h);
   return lists;
}

static int
uninstall_packages(BOOL (*want)(const wchar_t *, const void *), const void *ctx, const wchar_t *keep,
                   BOOL *reboot)
{
   wchar_t dir[MAX_PATH], pattern[MAX_PATH];
   GetWindowsDirectoryW(dir, MAX_PATH);
   wcsncat(dir, L"\\INF", MAX_PATH - wcslen(dir) - 1);
   join(pattern, MAX_PATH, dir, L"oem*.inf");

   /* DiUninstallDriverW (Windows 10 1703+) also takes the driver off the
    * devices. */
   di_driver_fn uninstall = newdev("DiUninstallDriverW");

   WIN32_FIND_DATAW fd;
   HANDLE f = FindFirstFileW(pattern, &fd);
   if (f == INVALID_HANDLE_VALUE)
      return 0;
   int rc = 0;
   do {
      if (keep && !_wcsicmp(fd.cFileName, keep))
         continue;
      wchar_t path[MAX_PATH], orig[MAX_PATH] = L"?";
      join(path, MAX_PATH, dir, fd.cFileName);
      if (!want(path, ctx))
         continue;
      inf_original_name(path, orig, MAX_PATH);
      BOOL r = FALSE;
      BOOL ok = uninstall ? uninstall(NULL, path, 0, &r)
                          : SetupUninstallOEMInfW(fd.cFileName, SUOI_FORCEDELETE, NULL);
      if (!ok) {
         rc = fail(fd.cFileName, GetLastError());
         continue;
      }
      *reboot |= r;
      say(L"removed %ls (%ls)\n", fd.cFileName, orig);
   } while (FindNextFileW(f, &fd));
   FindClose(f);
   return rc;
}

static int
driver_install(const wchar_t *inf, BOOL *reboot)
{
   wchar_t full[MAX_PATH], published[MAX_PATH], *component = NULL;
   if (!GetFullPathNameW(inf, MAX_PATH, full, NULL))
      return fail(inf, GetLastError());
   /* Windows must trust a test-signed package's certificate as a root and as
    * a publisher. */
   wchar_t cer[MAX_PATH];
   wcsncpy(cer, full, MAX_PATH);
   wcscpy(wcsrchr(cer, L'\\') + 1, L"loophole-test.cer");
   if (exists(cer) && (trust_certificate(cer, L"Root") || trust_certificate(cer, L"TrustedPublisher")))
      return 1;
   if (!SetupCopyOEMInfW(full, NULL, SPOST_PATH, 0, published, MAX_PATH, NULL, &component))
      return fail(L"staging the driver package", GetLastError());
   di_driver_fn install = newdev("DiInstallDriverW");
   if (!install)
      return fail(L"newdev.dll", GetLastError());
   BOOL r = FALSE;
   /* ERROR_NO_MORE_ITEMS means no such device here, and the package is only
    * staged. */
   if (!install(NULL, full, DIIRFLAG_FORCE_INF, &r) && GetLastError() != ERROR_NO_MORE_ITEMS)
      return fail(L"installing the driver", GetLastError());
   *reboot |= r;
   say(L"installed %ls as %ls\n", wcsrchr(full, L'\\') + 1, component);
   return uninstall_packages(ours_named, wcsrchr(full, L'\\') + 1, component, reboot);
}

static int
driver_setup(const wchar_t *dir, BOOL *reboot)
{
   wchar_t inf[MAX_PATH];
   join(inf, MAX_PATH, dir, L"viogpu3d.inf");
   int rc = driver_install(inf, reboot);
   return rc | uninstall_packages(competes, NULL, NULL, reboot);
}

static int
cmd_driver(int argc, wchar_t **argv)
{
   if (argc < 2)
      return fwprintf(stderr, L"usage: loophole driver setup DIR | install|remove INF...\n"), 2;
   BOOL reboot = FALSE;
   int rc = 0;
   if (!wcscmp(argv[0], L"setup")) {
      if (argc != 2)
         return fwprintf(stderr, L"usage: loophole driver setup DIR\n"), 2;
      rc = driver_setup(argv[1], &reboot);
      return rc ? rc : reboot ? REBOOT_REQUIRED : 0;
   }
   for (int i = 1; i < argc; i++) {
      if (!wcscmp(argv[0], L"install"))
         rc |= driver_install(argv[i], &reboot);
      else if (!wcscmp(argv[0], L"remove"))
         rc |= uninstall_packages(ours_named, argv[i], NULL, &reboot);
      else
         return fwprintf(stderr, L"loophole driver: unknown command %ls\n", argv[0]), 2;
   }
   return rc ? rc : reboot ? REBOOT_REQUIRED : 0;
}

static const struct {
   const wchar_t *api;
   const wchar_t *dlls[3];
} apis[] = {
   {L"d3d8", {L"d3d8.dll", L"d3d9.dll"}},
   {L"d3d9", {L"d3d9.dll"}},
   {L"d3d11", {L"d3d10core.dll", L"d3d11.dll", L"dxgi.dll"}},
   {L"d3d12", {L"d3d12.dll", L"d3d12core.dll", L"dxgi.dll"}},
   {L"opengl", {L"opengl32.dll", L"libgallium_wgl.dll"}},
};
#define LOADER L"vulkan-1.dll"
#define LAYER_DIRS_KEY L"SOFTWARE\\loophole\\LayerDirectories"

static void
remember_dir(const wchar_t *dir, BOOL add)
{
   HKEY k;
   if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, LAYER_DIRS_KEY, 0, NULL, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, NULL, &k,
                       NULL) != ERROR_SUCCESS)
      return;
   if (add)
      RegSetValueExW(k, dir, 0, REG_SZ, (const BYTE *)L"", sizeof(wchar_t));
   else
      RegDeleteValueW(k, dir);
   RegCloseKey(k);
}

static const wchar_t *
pe_arch(const wchar_t *path)
{
   HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
   if (f == INVALID_HANDLE_VALUE)
      return NULL;
   BYTE buf[4096];
   DWORD n = 0;
   ReadFile(f, buf, sizeof(buf), &n, NULL);
   CloseHandle(f);
   if (n < 0x40 || buf[0] != 'M' || buf[1] != 'Z')
      return NULL;
   DWORD pe = *(DWORD *)(buf + 0x3c);
   if (pe + 6 > n || memcmp(buf + pe, "PE\0\0", 4))
      return NULL;
   WORD machine = *(WORD *)(buf + pe + 4);
   return machine == IMAGE_FILE_MACHINE_AMD64 ? L"x64" : machine == IMAGE_FILE_MACHINE_I386 ? L"x86" : NULL;
}

static const wchar_t *
app_arch(const wchar_t *dir)
{
   wchar_t pattern[MAX_PATH];
   join(pattern, MAX_PATH, dir, L"*.exe");
   WIN32_FIND_DATAW fd;
   HANDLE f = FindFirstFileW(pattern, &fd);
   if (f == INVALID_HANDLE_VALUE) {
      error(L"%ls: no program to take the architecture from", dir);
      return NULL;
   }
   const wchar_t *arch = NULL;
   do {
      wchar_t exe[MAX_PATH];
      join(exe, MAX_PATH, dir, fd.cFileName);
      const wchar_t *a = pe_arch(exe);
      if (!a)
         continue;
      if (arch && wcscmp(arch, a)) {
         error(L"%ls: has both x64 and x86 programs", dir);
         arch = NULL;
         break;
      }
      arch = a;
   } while (FindNextFileW(f, &fd));
   FindClose(f);
   return arch;
}

static int
install_dll(const wchar_t *src_dir, const wchar_t *dir, const wchar_t *name)
{
   wchar_t src[MAX_PATH], dst[MAX_PATH], orig[MAX_PATH], marker[MAX_PATH];
   join(src, MAX_PATH, src_dir, name);
   join(dst, MAX_PATH, dir, name);
   _snwprintf(orig, MAX_PATH, L"%ls.loophole-orig", dst);
   _snwprintf(marker, MAX_PATH, L"%ls.loophole", dst);
   /* `disable` restores a DLL the application ships itself. */
   if (exists(dst) && !exists(orig) && !exists(marker) && !MoveFileW(dst, orig))
      return fail(dst, GetLastError());
   if (!CopyFileW(src, dst, FALSE))
      return fail(src, GetLastError());
   HANDLE h = CreateFileW(marker, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
   if (h == INVALID_HANDLE_VALUE)
      return fail(marker, GetLastError());
   CloseHandle(h);
   return 0;
}

static int
remove_dll(const wchar_t *dir, const wchar_t *name)
{
   wchar_t dst[MAX_PATH], orig[MAX_PATH], marker[MAX_PATH];
   join(dst, MAX_PATH, dir, name);
   _snwprintf(orig, MAX_PATH, L"%ls.loophole-orig", dst);
   _snwprintf(marker, MAX_PATH, L"%ls.loophole", dst);
   if (!exists(marker))
      return 0;
   if ((exists(dst) && !DeleteFileW(dst)) || !DeleteFileW(marker))
      return fail(dst, GetLastError());
   if (exists(orig) && !MoveFileW(orig, dst))
      return fail(orig, GetLastError());
   return 0;
}

static BOOL
layers_left(const wchar_t *dir)
{
   wchar_t pattern[MAX_PATH];
   join(pattern, MAX_PATH, dir, L"*.dll.loophole");
   WIN32_FIND_DATAW fd;
   HANDLE f = FindFirstFileW(pattern, &fd);
   BOOL left = FALSE;
   if (f == INVALID_HANDLE_VALUE)
      return FALSE;
   do
      left |= _wcsicmp(fd.cFileName, LOADER L".loophole") != 0;
   while (!left && FindNextFileW(f, &fd));
   FindClose(f);
   return left;
}

static int cmd_layers(int argc, wchar_t **argv);

static int
layers_disable_all(void)
{
   HKEY k;
   if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, LAYER_DIRS_KEY, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
      return 0;
   /* Collected first: disabling deletes the values. */
   wchar_t dirs[64][MAX_PATH];
   DWORD n = 0;
   for (DWORD i = 0; n < ARRAYSIZE(dirs); i++) {
      DWORD len = MAX_PATH;
      if (RegEnumValueW(k, i, dirs[n], &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
         break;
      n++;
   }
   RegCloseKey(k);
   int rc = 0;
   for (DWORD i = 0; i < n; i++) {
      wchar_t *args[] = {L"disable", dirs[i]};
      if (exists(dirs[i]))
         rc |= cmd_layers(2, args);
      else
         remember_dir(dirs[i], FALSE);
   }
   RegDeleteKeyExW(HKEY_LOCAL_MACHINE, LAYER_DIRS_KEY, KEY_WOW64_64KEY, 0);
   return rc;
}

static int
cmd_layers(int argc, wchar_t **argv)
{
   if (argc == 1 && !wcscmp(argv[0], L"disable-all"))
      return layers_disable_all();
   if (argc < 2 || (wcscmp(argv[0], L"enable") && wcscmp(argv[0], L"disable")))
      return fwprintf(stderr, L"usage: loophole layers enable|disable DIR [--api %ls]\n",
                      L"d3d8,d3d9,d3d11,d3d12,opengl"),
             2;
   BOOL enable = !wcscmp(argv[0], L"enable");
   wchar_t dir[MAX_PATH];
   if (!GetFullPathNameW(argv[1], MAX_PATH, dir, NULL))
      return fail(argv[1], GetLastError());
   DWORD attrs = GetFileAttributesW(dir);
   if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
      *wcsrchr(dir, L'\\') = 0;
   const wchar_t *api_arg = L"d3d8,d3d9,d3d11,d3d12,opengl";
   for (int i = 2; i < argc; i++) {
      if (!wcscmp(argv[i], L"--api") && i + 1 < argc)
         api_arg = argv[++i];
      else if (!wcscmp(argv[i], L"--notify"))
         notify = TRUE;
      else
         return fwprintf(stderr, L"loophole layers: unexpected %ls\n", argv[i]), 2;
   }
   wchar_t api_list[256];
   wcsncpy(api_list, api_arg, ARRAYSIZE(api_list) - 1);
   api_list[ARRAYSIZE(api_list) - 1] = 0;

   const wchar_t *names[16];
   unsigned n = 0;
   for (wchar_t *ctx = NULL, *a = wcstok(api_list, L",", &ctx); a; a = wcstok(NULL, L",", &ctx)) {
      unsigned i = 0;
      while (i < ARRAYSIZE(apis) && wcscmp(apis[i].api, a))
         i++;
      if (i == ARRAYSIZE(apis))
         return fwprintf(stderr, L"loophole: unknown API %ls (d3d8, d3d9, d3d11, d3d12, opengl)\n", a), 2;
      for (unsigned d = 0; d < ARRAYSIZE(apis[i].dlls) && apis[i].dlls[d]; d++) {
         unsigned j = 0;
         while (j < n && wcscmp(names[j], apis[i].dlls[d]))
            j++;
         if (j == n)
            names[n++] = apis[i].dlls[d];
      }
   }

   const wchar_t *arch = app_arch(dir);
   if (!arch)
      return 1;
   wchar_t layers[MAX_PATH], src[MAX_PATH];
   join(layers, MAX_PATH, root, L"layers");
   join(src, MAX_PATH, layers, arch);

   int rc = 0;
   if (enable) {
      rc |= install_dll(src, dir, LOADER);
      for (unsigned i = 0; i < n; i++)
         rc |= install_dll(src, dir, names[i]);
      remember_dir(dir, TRUE);
   } else {
      for (unsigned i = 0; i < n; i++)
         rc |= remove_dll(dir, names[i]);
      /* The loader stays while any layer still needs it. */
      if (!layers_left(dir)) {
         rc |= remove_dll(dir, LOADER);
         remember_dir(dir, FALSE);
      }
   }
   if (!rc)
      report(L"%ls the %ls translation layers (%ls) %ls %ls", enable ? L"Installed" : L"Removed", arch, api_arg,
             enable ? L"in" : L"from", dir);
   return rc;
}

static BOOL
elevated(void)
{
   HANDLE token = NULL;
   TOKEN_ELEVATION e = {0};
   DWORD size = 0;
   BOOL ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
             GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size);
   if (token)
      CloseHandle(token);
   return ok && e.TokenIsElevated;
}

static void
append_quoted(wchar_t *out, size_t n, const wchar_t *arg)
{
   size_t len = wcslen(out);
   if (len + 1 < n)
      out[len++] = L' ';
   if (len + 1 < n)
      out[len++] = L'"';
   for (const wchar_t *p = arg;; p++) {
      size_t slashes = 0;
      while (*p == L'\\') {
         slashes++;
         p++;
      }
      /* Backslashes before a quote, or before the closing one, double. */
      size_t emit = (*p == L'"' || !*p) ? 2 * slashes : slashes;
      for (size_t i = 0; i < emit && len + 1 < n; i++)
         out[len++] = L'\\';
      if (!*p)
         break;
      if (*p == L'"' && len + 1 < n)
         out[len++] = L'\\';
      if (len + 1 < n)
         out[len++] = *p;
   }
   if (len + 1 < n)
      out[len++] = L'"';
   out[len] = 0;
}

static int
run_elevated(int argc, wchar_t **argv)
{
   wchar_t self[MAX_PATH], params[8192] = L"";
   if (!GetModuleFileNameW(NULL, self, MAX_PATH))
      return fail(L"locating loophole.exe", GetLastError());
   for (int i = 1; i < argc; i++)
      append_quoted(params, ARRAYSIZE(params), argv[i]);
   SHELLEXECUTEINFOW sei = {
      .cbSize = sizeof(sei),
      .fMask = SEE_MASK_NOCLOSEPROCESS,
      .lpVerb = L"runas",
      .lpFile = self,
      .lpParameters = params,
      .nShow = SW_HIDE,
   };
   if (!ShellExecuteExW(&sei))
      return fail(L"running as administrator", GetLastError());
   WaitForSingleObject(sei.hProcess, INFINITE);
   DWORD code = 1;
   GetExitCodeProcess(sei.hProcess, &code);
   CloseHandle(sei.hProcess);
   return (int)code;
}

static int
cmd_agent(int argc, wchar_t **argv)
{
   if (argc != 1 || wcscmp(argv[0], L"stop"))
      return usage(stderr), 2;
   HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
   if (snap == INVALID_HANDLE_VALUE)
      return fail(L"listing processes", GetLastError());
   PROCESSENTRY32W p = {.dwSize = sizeof(p)};
   for (BOOL more = Process32FirstW(snap, &p); more; more = Process32NextW(snap, &p)) {
      if (_wcsicmp(p.szExeFile, AGENT))
         continue;
      HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, p.th32ProcessID);
      if (h && TerminateProcess(h, 0))
         WaitForSingleObject(h, 5000);
      if (h)
         CloseHandle(h);
   }
   CloseHandle(snap);
   return 0;
}

static void
say_vulkan_driver(HDEVINFO set, SP_DEVINFO_DATA *dev, const wchar_t *value, const wchar_t *what)
{
   HKEY k = SetupDiOpenDevRegKey(set, dev, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
   if (k == INVALID_HANDLE_VALUE)
      return;
   wchar_t manifest[MAX_PATH + 2] = {0};
   DWORD size = sizeof(manifest) - 2 * sizeof(wchar_t);
   if (RegQueryValueExW(k, value, NULL, NULL, (BYTE *)manifest, &size) == ERROR_SUCCESS && manifest[0])
      say(L"  %ls  %ls\n", what, manifest);
   else
      say(L"  %ls  not registered\n", what);
   RegCloseKey(k);
}

static int
cmd_status(void)
{
   HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, NULL, NULL, DIGCF_PRESENT);
   if (set == INVALID_HANDLE_VALUE)
      return fail(L"listing display adapters", GetLastError());
   unsigned ours = 0, ours_ok = 0;
   SP_DEVINFO_DATA dev = {.cbSize = sizeof(dev)};
   for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
      wchar_t name[256] = L"?";
      if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_FRIENDLYNAME, NULL, (BYTE *)name, sizeof(name), NULL))
         SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_DEVICEDESC, NULL, (BYTE *)name, sizeof(name), NULL);
      ULONG status = 0, problem = 0;
      CONFIGRET cr = CM_Get_DevNode_Status(&status, &problem, dev.DevInst, 0);
      BOOL ok = cr == CR_SUCCESS && !(status & DN_HAS_PROBLEM);
      say(L"adapter  %ls: %ls", name, ok ? L"working" : L"problem");
      if (!ok)
         say(L" (code %lu)", cr == CR_SUCCESS ? problem : (ULONG)cr);
      say(L"\n");
      if (wcsstr(name, L"loophole")) {
         ours++;
         ours_ok += ok;
         say_vulkan_driver(set, &dev, L"VulkanDriverName", L"vulkan x64");
         say_vulkan_driver(set, &dev, L"VulkanDriverNameWow", L"vulkan x86");
      }
   }
   SetupDiDestroyDeviceInfoList(set);
   return ours == 1 && ours_ok == 1 ? 0 : 1;
}

static void
usage(FILE *f)
{
   fwprintf(f, L"usage: loophole status\n"
               L"       loophole testsigning status|enable\n"
               L"       loophole driver setup DIR | driver install INF... | driver remove NAME.inf...\n"
               L"       loophole layers enable|disable PATH [--api d3d8,d3d9,d3d11,d3d12,opengl] [--notify]\n"
               L"       loophole agent stop\n");
}

static int
dispatch(int argc, wchar_t **argv)
{
   if (argc < 2)
      return usage(stderr), 2;
   const wchar_t *cmd = argv[1];
   if (!wcscmp(cmd, L"status"))
      return cmd_status();
   if (!wcscmp(cmd, L"testsigning"))
      return cmd_testsigning(argc - 2, argv + 2);
   if (!wcscmp(cmd, L"driver"))
      return cmd_driver(argc - 2, argv + 2);
   if (!wcscmp(cmd, L"layers"))
      return elevated() ? cmd_layers(argc - 2, argv + 2) : run_elevated(argc, argv);
   if (!wcscmp(cmd, L"agent"))
      return cmd_agent(argc - 2, argv + 2);
   if (!wcscmp(cmd, L"help") || !wcscmp(cmd, L"--help"))
      return usage(stdout), 0;
   usage(stderr);
   return 2;
}

int
wmain(int argc, wchar_t **argv)
{
   DWORD n = GetModuleFileNameW(NULL, root, MAX_PATH);
   if (!n || n == MAX_PATH)
      return fail(L"locating loophole.exe", GetLastError());
   *wcsrchr(root, L'\\') = 0;

   if (argc > 2 && !wcscmp(argv[1], L"--log")) {
      logf = _wfopen(argv[2], L"a");
      argv[2] = argv[0];
      argv += 2;
      argc -= 2;
      if (logf) {
         fwprintf(logf, L">");
         for (int i = 1; i < argc; i++)
            fwprintf(logf, L" %ls", argv[i]);
         fwprintf(logf, L"\n");
      }
   }
   int rc = dispatch(argc, argv);
   if (logf) {
      fwprintf(logf, L"exit %d\n", rc);
      fclose(logf);
   }
   return rc;
}
