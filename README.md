# loophole-guest

The Windows gpu driver for [loophole](https://github.com/kossLAN/loophole) 

```
 application / DWM
   │ Direct3D 8-12, OpenGL
   ▼
 loophole_umd.dll      umd/           Triton on DXVK (D3D11) and vkd3d-proton (D3D12), D3D9On12 (D3D8/9)
 loophole_wgl.dll      third_party/   Mesa's Zink (OpenGL)
   │ Vulkan
   ▼
 vulkan_radeon.dll     third_party/   RADV, its amdgpu winsys over viogpu3d's escapes
   │ D3DKMTEscape (abi/viogpum.h)
   ▼
 viogpu3d.sys          kmd/           WDDM display and render driver ══▶ virtio-gpu
```

## Building

```sh
make
```

That builds both user-mode halves into `out/x64` and `out/x86`, checking out the submodules and
fetching the WDK headers and LunarG's Vulkan loader on the way. Without Nix, MinGW-w64 GCC, meson
1.4 or later, ninja, glslang, widl, bison, flex, Python with mako, packaging and PyYAML, curl and
unzip will do.

Then on Windows, with Visual Studio 2022, the WDK and NSIS, in the same tree:

```bat
msbuild installer\package.proj
```

Will build `viogpu3d.sys` and puts the driver package, the per-game layers and
`loophole-setup.exe` in `out\package`. `-p:Configuration=Debug` builds a kernel driver that logs
to COM1

To try a build, run `loophole-setup.exe` in the VM, or update just the driver with
`pnputil /add-driver drivers\viogpu3d.inf /install`.
