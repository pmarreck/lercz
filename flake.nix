{
  description = "lercz - Zig wrap of Esri's Limited Error Raster Compression (LERC) C++ library";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    zig-overlay = {
      url = "github:mitchellh/zig-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, flake-utils, zig-overlay }:
    flake-utils.lib.eachSystem [ "x86_64-linux" "aarch64-linux" "aarch64-darwin" ] (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        zig = zig-overlay.packages.${system}."0.16.0";

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
            mkdir -p $out/lib $out/include
            cp -v zig-out/lib/liblerc.a $out/lib/
            cp -v zig-out/include/Lerc_c_api.h zig-out/include/Lerc_types.h $out/include/
          '';
          dontFixup = true;
        };

        lerczTest = pkgs.stdenv.mkDerivation {
          pname = "${pname}-test";
          inherit version;
          src = ./.;
          nativeBuildInputs = [ zig pkgs.bash pkgs.jq pkgs.coreutils pkgs.gnugrep ];
          buildPhase = ''
            export ZIG_GLOBAL_CACHE_DIR=$TMPDIR/zig-cache
            export ZIG_LOCAL_CACHE_DIR=$TMPDIR/zig-local-cache
            mkdir -p $ZIG_GLOBAL_CACHE_DIR
            patchShebangs test build-checked scripts tests
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
      in {
        packages.default = lerczPackage;
        packages.${pname} = lerczPackage;

        checks = {
          build = lerczPackage;
          test  = lerczTest;
        };

        devShells.default = pkgs.mkShell {
          packages = [ zig pkgs.bash pkgs.curl pkgs.jq pkgs.coreutils pkgs.gnugrep pkgs.shellcheck ];
          shellHook = ''
            echo "lercz dev shell (Zig ${zig.version}, LERC ${version})"
          '';
        };
      });
}
