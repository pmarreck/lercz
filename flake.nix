{
  description = "lercz - Zig wrap of Esri's Limited Error Raster Compression (LERC) C++ library";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    zig-overlay = {
      url = "github:mitchellh/zig-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    corruption-reader.url = "git+ssh://git@github.com/pmarreck/corruption_reader?rev=ac62a738a17ca61a8c74f57952138972ce66b7b6";
    tsv-to-ansi-table.url = "github:pmarreck/tsv_to_ansi_table/931a1a0c20b4ce062cda34f75e9b4724d56c3b07";
  };

  outputs = { self, nixpkgs, flake-utils, zig-overlay, corruption-reader, tsv-to-ansi-table }:
    flake-utils.lib.eachSystem [ "x86_64-linux" "aarch64-linux" "aarch64-darwin" ] (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        zig = zig-overlay.packages.${system}."0.16.0";
        corruptionReader = corruption-reader.packages.${system}.default;
        tableRenderer = tsv-to-ansi-table.packages.${system}.default;

        # Match tiffz's portfolio convention: on Linux, build a fully
        # static musl artifact. macOS handles its own linker.
        zigTarget =
          if system == "x86_64-linux"  then "x86_64-linux-musl"
          else if system == "aarch64-linux" then "aarch64-linux-musl"
          else null;
        zigTargetFlag = if zigTarget == null then "" else "-Dtarget=${zigTarget}";

        pname = "lercz";
        version = "4.2.0";

        lerczPackage = pkgs.stdenv.mkDerivation {
          inherit pname version;
          src = ./.;
          nativeBuildInputs = [ zig ];
          buildPhase = ''
            export ZIG_GLOBAL_CACHE_DIR=$TMPDIR/zig-cache
            export ZIG_LOCAL_CACHE_DIR=$TMPDIR/zig-local-cache
            mkdir -p $ZIG_GLOBAL_CACHE_DIR
            zig build -Doptimize=ReleaseFast ${zigTargetFlag}
          '';
          installPhase = ''
            mkdir -p $out/lib $out/include $out/bin
            cp -v zig-out/lib/liblerc.a $out/lib/
            cp -v zig-out/include/Lerc_c_api.h zig-out/include/Lerc_types.h $out/include/
            cp -v zig-out/bin/lercz $out/bin/
          '';
          dontFixup = true;
        };

        lerczTest = pkgs.stdenv.mkDerivation {
          pname = "${pname}-test";
          inherit version;
          src = ./.;
          nativeBuildInputs = [ zig pkgs.bash pkgs.jq pkgs.coreutils pkgs.gnugrep pkgs.gawk corruptionReader ];
          buildPhase = ''
            export ZIG_GLOBAL_CACHE_DIR=$TMPDIR/zig-cache
            export ZIG_LOCAL_CACHE_DIR=$TMPDIR/zig-local-cache
            mkdir -p $ZIG_GLOBAL_CACHE_DIR
            patchShebangs test build-checked build-cli build-all run corruption-coverage scripts tests
            timeout 600 bash ./test ${zigTargetFlag} || {
              echo "lercz test suite failed"
              exit 1
            }
          '';
          installPhase = ''
            mkdir -p $out
            echo "lercz tests passed" > $out/result
          '';
          dontFixup = true;
        };
        cliTargets = [ "x86_64-linux-musl" "aarch64-linux-musl" "aarch64-macos" "x86_64-windows-gnu" "aarch64-windows-gnu" ];
        targetPackage = target: profile: lerczPackage.overrideAttrs (old: {
          pname = "lercz-${target}-${profile}";
          buildPhase = ''
            export ZIG_GLOBAL_CACHE_DIR=$TMPDIR/zig-cache
            export ZIG_LOCAL_CACHE_DIR=$TMPDIR/zig-local-cache
            ${pkgs.lib.optionalString pkgs.stdenv.isDarwin "unset NIX_CFLAGS_COMPILE NIX_LDFLAGS"}
            zig build -Dtarget=${target} -Doptimize=${profile} --prefix $out
          '';
          installPhase = "true";
        });
      in {
        packages = {
          default = lerczPackage;
          ${pname} = lerczPackage;
        } // builtins.listToAttrs (map (target: { name = target; value = targetPackage target "ReleaseFast"; }) cliTargets)
          // builtins.listToAttrs (map (target: { name = "${target}-debug"; value = targetPackage target "Debug"; }) cliTargets);

        checks = {
          build = lerczPackage;
          test  = lerczTest;
        };

        devShells.default = pkgs.mkShell {
          packages = [ zig pkgs.bash pkgs.curl pkgs.jq pkgs.coreutils pkgs.gnugrep pkgs.gawk pkgs.shellcheck pkgs.file corruptionReader tableRenderer ];
          shellHook = ''
            echo "lercz dev shell (Zig ${zig.version}, LERC ${version})" >&2
          '';
        };
      });
}
