{
  description = "Saadin: ESP-IDF development shell for the ESP32 firmware in ESP/";

  inputs = {
    # ESP-IDF and Espressif's prebuilt toolchains, packaged for Nix (macOS
    # included). Its flake.lock pins the ESP-IDF version (v5.5.2 at the time of
    # writing); `nix flake update esp-dev` moves it.
    # https://github.com/mirrexagon/nixpkgs-esp-dev
    esp-dev.url = "github:mirrexagon/nixpkgs-esp-dev";
  };

  outputs =
    { esp-dev, ... }:
    let
      # Reuse esp-dev's own nixpkgs rather than ours: it is the combination
      # upstream builds and tests against.
      inherit (esp-dev.inputs) nixpkgs;

      systems = [
        "aarch64-darwin"
        "x86_64-darwin"
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f system nixpkgs.legacyPackages.${system});
    in
    {
      devShells = forAllSystems (
        system: pkgs: {
          default = pkgs.mkShell {
            name = "saadin";

            packages = [
              # ESP-IDF with the Xtensa toolchain only: the ESP32 and ESP32-S3
              # (the `make pico` and `make s3` boards). The RISC-V chips (C3,
              # C5, C6) need `esp-idf-full` instead.
              # Taken already built from esp-dev, which permits the insecure
              # Python `ecdsa` that esptool needs (upstream issue #109).
              esp-dev.packages.${system}.esp-idf-xtensa

              # ESP/Makefile builds the host tool revk_settings with `-lpopt`.
              pkgs.popt
            ]
            # ...and calls the compiler `gcc`. On macOS the shell's compiler is
            # clang, installed as `cc` only, so `gcc` would fall through to
            # Apple's /usr/bin/gcc stub, which fails inside a Nix shell
            # ("tool 'gcc' not found").
            ++ pkgs.lib.optional pkgs.stdenv.hostPlatform.isDarwin (
              pkgs.writeShellScriptBin "gcc" ''exec cc "$@"''
            );
          };
        }
      );
    };
}
