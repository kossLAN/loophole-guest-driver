{
  nixpkgs ? (builtins.getFlake (toString ./.)).inputs.nixpkgs,
  system ? builtins.currentSystem,
}:

let
  pkgs = nixpkgs.legacyPackages.${system};
  inherit (pkgs.pkgsCross) mingwW64 mingw32;

  widl = triple: pkgs.writeShellScriptBin "${triple}-widl" ''exec ${pkgs.wine64}/bin/widl "$@"'';
in
pkgs.mkShellNoCC {
  packages = with pkgs; [
    gnumake
    git
    curl
    unzip
    meson
    ninja
    glslang
    bison
    flex
    xorriso
    (python3.withPackages (p: [
      p.mako
      p.packaging
      p.pyyaml
    ]))
    mingwW64.buildPackages.gcc
    mingw32.buildPackages.gcc
    (widl "x86_64-w64-mingw32")
    (widl "i686-w64-mingw32")
  ];

  env = {
    NIX_LDFLAGS_x86_64_w64_mingw32 = "-L${mingwW64.windows.pthreads}/lib";
    NIX_LDFLAGS_i686_w64_mingw32 = "-L${mingw32.windows.pthreads}/lib";
    NIX_LDFLAGS_AFTER_i686_w64_mingw32 = toString [
      "${mingw32.windows.mcfgthreads}/lib/libmcfgthread.a"
      "-lssp"
      "-ladvapi32"
      "-lmingwex"
      "-lmsvcrt"
      "-lkernel32"
      "-lntdll"
    ];
  };
}
