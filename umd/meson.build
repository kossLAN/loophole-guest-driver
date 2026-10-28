project(
  'loophole-umd',
  'c',
  default_options: ['c_std=gnu11', 'optimization=2', 'warning_level=0'],
)

# The parts of osy/virtio-win-mesa the UMD builds on, patched (see UPSTREAM.md).
triton = 'triton'
neptune = triton / 'src/virtio/neptune'
inc = include_directories(
  neptune / 'triton',
  neptune,
  triton / 'src',
  triton / 'include',
  triton / 'src/virtio',
)

add_project_arguments(
  '-DCOBJMACROS',
  '-DWIDL_EXPLICIT_AGGREGATE_RETURNS',
  '-DWIN32_LEAN_AND_MEAN',
  '-DHAVE_STRUCT_TIMESPEC',
  '-D_TIMESPEC_DEFINED',
  language: 'c',
)

triton_lib = static_library(
  'triton',
  files(
    neptune / 'triton/triton.c',
    neptune / 'triton/tritonDDI.c',
    neptune / 'triton/tritonDDI12.c',
    neptune / 'triton/tritonDescriptor12.c',
    neptune / 'triton/tritonDxbc.c',
    neptune / 'triton/tritonDxgi.c',
    neptune / 'triton/tritonDxil.c',
    neptune / 'triton/tritonHostCaps.c',
    neptune / 'triton/tritonKm.c',
    neptune / 'triton/tritonList12.c',
    neptune / 'triton/tritonPipeline12.c',
    neptune / 'triton/tritonPresent.c',
    neptune / 'triton/tritonPresent12.c',
    neptune / 'triton/tritonQuery.c',
    neptune / 'triton/tritonQueue12.c',
    neptune / 'triton/tritonResource.c',
    neptune / 'triton/tritonResource12.c',
    neptune / 'triton/tritonShader.c',
    neptune / 'triton/tritonState.c',
    neptune / 'triton/tritonView.c',
  ),
  include_directories: inc,
)

cc = meson.get_compiler('c')
shared_library(
  'loophole_umd',
  'src/backend.c',
  'src/crossadapter.c',
  'src/d3d9.c',
  'src/shared.c',
  'src/shared12.c',
  name_prefix: '',
  vs_module_defs: 'src/loophole_umd.def',
  include_directories: inc,
  link_whole: triton_lib,
  c_args: ['-Wall', '-Wno-format-truncation', '-Wno-unknown-pragmas'],
  # d3d10umddi.h defines the D3D10 constants as const UINTs in every unit.
  link_args: ['-static-libgcc', '-Wl,--allow-multiple-definition'],
  dependencies: [
    cc.find_library('uuid'),
    cc.find_library('dxguid'),
    cc.find_library('advapi32'),
  ],
  install: true,
)
