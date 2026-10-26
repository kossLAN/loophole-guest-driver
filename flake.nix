{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { nixpkgs, ... }:
    {
      devShells.x86_64-linux.default = import ./shell.nix {
        inherit nixpkgs;
        system = "x86_64-linux";
      };
    };
}
