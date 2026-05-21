# This is a general purpose Zephyr flake for working with west workspaces in
# the traditional fashion, i.e., directly with west and venvs.
#
# Using venvs is a lot easier than worrying about west2nix.
#
# For this to work, you'll need to ensure you have some system level deps.
# Below are the relevant parts of my system config.
#
# ```nix
# nixpkgs.config = {
#   allowUnfree = true;
#
#   # JLink
#   segger-jlink.acceptLicense = true;
#   permittedInsecurePackages = [
#     "segger-jlink-qt4-874"
#   ];
# };
#
# ...
#
# -- Adding udev rules so we can access the USB devices as non-root
# services = {
#   udev.packages = [
#     pkgs.segger-jlink
#     pkgs.nrf-udev
#   ];
# ...
#
# users = {
#   users.sam = {
#     packages = with pkgs;
#       # Embedded
#       nrfutil
#       nrfconnect
#       segger-jlink
#     ];
#   };
# };
# ```
{
  description = "Zephyr Flake";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    # Pinned for python310 (EOL, removed from unstable). The Zephyr SDK
    # 0.17.x ships gdb-py linked against libpython3.10 -- can be dropped
    # once sdk-ng 0.18+ relaxes the Python version (already on main).
    nixpkgs-python310.url = "github:NixOS/nixpkgs/nixos-24.05";

    # NOTE: We intentionally do NOT follow zephyr-nix's `zephyr` input.
    # The upstream flake pins zephyr-src for pythonEnv (to read
    # requirements.txt), but we don't use pythonEnv.
    #
    # Instead, we manage Zephyr source and Python deps via a local venv +
    # west. Omitting the follows avoids fetching the full Zephyr repo into the
    # Nix store at eval time.
    #
    # Using a west workspace is more sensible if we don't do it the Nix way,
    # better to use west directly, T2 style.
    zephyr-nix.url = "github:nix-community/zephyr-nix";
    zephyr-nix.inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs = {
    nixpkgs,
    nixpkgs-python310,
    zephyr-nix,
    ...
  }: let
    # WORKAROUND: zephyr-nix's default.nix requires python310, which has been
    # removed from nixpkgs-unstable. Both our nixpkgs AND zephyr-nix's own
    # lock pin a nixpkgs too new for python310.
    #
    # We overlay the real python310 from an older nixpkgs so that:
    # 1. default.nix evaluates (it takes python310 as an argument)
    # 2. autoPatchelfHook finds libpython3.10.so.1.0 for gdb-py
    #
    # We call zephyr-nix's default.nix ourselves (rather than using
    # zephyr-nix.packages) so the overlay actually takes effect.
    pkgs = import nixpkgs {
      system = "x86_64-linux";
      overlays = [
        (_: _: {
          python310 = nixpkgs-python310.legacyPackages.x86_64-linux.python310;
        })
      ];
    };
    zephyr = pkgs.callPackage "${zephyr-nix}" {
      zephyr-src = null; # only needed for pythonEnv, which we don't use
      pyproject-nix = null; # ditto
    };
  in {
    devShells.x86_64-linux.default = pkgs.mkShell {
      packages = [
        (zephyr.sdk.override {
          targets = [
            "arm-zephyr-eabi"
          ];
        })
        # Using zephyr.hosttools-nix to use nixpkgs built tooling instead of
        # official Zephyr binaries
        zephyr.hosttools-nix

        # Again, we omit the zephyr.pythonEnv as we are managing this via
        # a local venv.

        pkgs.cmake
        pkgs.ninja

        pkgs.python313
        pkgs.uv
      ];

      shellHook = ''
        # Source venv if it exists (from west topdir)
        if [ -f "../.venv/bin/activate" ]; then
          source ../.venv/bin/activate
          echo Sourced west venv
        fi
        # Source zephyr env
        if [ -f "../zephyr/zephyr-env.sh" ]; then
          source ../zephyr/zephyr-env.sh
          echo Sourced Zephyr environment
        fi
      '';
    };
  };
}
