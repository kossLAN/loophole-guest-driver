# Where the code comes from

## kmd/

[osy/kvm-guest-drivers-windows](https://github.com/osy/kvm-guest-drivers-windows) at `VirtIO/` (without the WDF library),

1. viogpu: drop `[[msvc::forceinline]]` on lambdas
2. viogpu3d: per-ring fence completion page for native contexts
3. viogpu3d: map blobs into user processes with the host's caching mode
4. viogpu3d: render-only driver model (`RENDER_ONLY`)
5. viogpu3d: `VIOGPU_SCANOUT_BLOB` escape
6. viogpu3d: `VIOGPU_SUBMIT_DIRECT` escape
7. viogpu3d: no boot-framebuffer PnP hook in the `RENDER_ONLY` build
8. viogpu3d: describe linear surfaces held in guest blobs
9. viogpu3d: key the KMT map by process and handle
10. viogpu3d: show and copy the host resource a residency-only blob names
11. viogpu3d: offer the monitor again when the host changes its EDID
12. viogpu3d: a hardware pointer, shown by the host
13. viogpu3d: a new allocation takes over its lookup cookie
14. viogpu3d: keep a shared blob mapped until its last device closes
15. viogpu3d: allow monitored-fence gates to complete from a deferred UMD event

and, for this build, its own `viogpu3d.vcxproj`, a self-contained `viogpu3d.rc` (upstream's
includes its `build/` directory) and `viogpum.h` from `abi/` instead of `viogpu/shared/`.

## umd/triton/

[osy/virtio-win-mesa](https://github.com/osy/virtio-win-mesa)

1. triton12: call `ID3D12Resource::GetDesc` through the vtable
2. triton: let the embedding DLL route logs and gate KMD sharing
3. triton: export the adapter entry points through the .def file only
4. triton: log the creation of shared, presentable and primary resources
5. wddm_hw: surface description for residency-only guest blobs
6. triton: cross-adapter resources through embedder hooks
7. triton: shared, presentable and primary textures through embedder hooks
8. triton12: present companions through embedder hooks
9. triton: include `stddef.h` for `offsetof`
10. triton: monitored-fence gates the embedder completes (`VIOGPU_GATE_COMPLETE`)

Their licenses are in `umd/triton/licenses`.

## umd/src/d3d9on12ddi.h

`interface/d3d9on12ddi.h` from [microsoft/D3D9On12](https://github.com/microsoft/D3D9On12) (MIT),
without its byte-order mark and with Unix line endings.

## third_party/

Submodules on loophole's forks, branch `loophole`: `git log <base>..loophole` in each lists its
changes.

- `mesa`: [mesa](https://gitlab.freedesktop.org/mesa/mesa) at `mesa-26.2.4`. Its Windows build
  takes libdrm_amdgpu's VA manager (libdrm 2.4.134) and TRE 0.9.0 as wraps.
- `dxvk`: [DXVK](https://github.com/doitsujin/dxvk) at `v3.1.1`
- `vkd3d-proton`: [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) at `v2.14.1`

The user-mode build also downloads, at versions the `Makefile` pins, the WDK headers (the
Microsoft.Windows.SDK.CPP and Microsoft.Windows.WDK.x64 NuGet packages) and LunarG's Vulkan
Runtime.
