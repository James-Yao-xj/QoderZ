{
  description = "QoderZ — a Qoder to OpenAI-compatible gateway written in C++23";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
        "x86_64-darwin"
      ];
      for_all_systems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      # Darwin uses Clang and libc++ from the same Nix toolchain.
      toolchain = pkgs: if pkgs.stdenv.isDarwin then pkgs.llvmPackages.stdenv else pkgs.gcc14Stdenv;
      mk_qoderz =
        pkgs:
        (toolchain pkgs).mkDerivation {
          pname = "qoderz";
          version = "0.1.1";

          src = pkgs.lib.cleanSourceWith {
            name = "qoderz-source";
            src = self;
            filter =
              path: _type:
              let
                base = baseNameOf (toString path);
              in
              !(builtins.elem base [
                ".git"
                "build"
                "result"
                ".direnv"
                # Defensive: the dev shell keeps ccache in `.git/ccache`, but a
                # hand-set `CCACHE_DIR=$PWD/.ccache` must not drag gigabytes of
                # cache objects into the flake source either.
                ".ccache"
              ]);

          };

          nativeBuildInputs = with pkgs; [
            cmake
            ninja
          ];

          buildInputs = with pkgs; [
            boost
            openssl
            nlohmann_json
            cli11
          ];

          cmakeFlags = [
            "-DBUILD_TESTING=OFF"
            "-DCMAKE_BUILD_TYPE=Release"
          ];

          doCheck = false;

          meta = {
            description = "Qoder to OpenAI-compatible gateway";
            mainProgram = "qoderz";
            platforms = systems;
          };
        };
    in
    {
      packages = for_all_systems (pkgs: {
        default = mk_qoderz pkgs;
        qoderz = mk_qoderz pkgs;
      });

      apps = for_all_systems (pkgs: {
        default = {
          type = "app";
          program = "${mk_qoderz pkgs}/bin/qoderz";
        };
      });

      devShells = for_all_systems (pkgs: {
        default = (pkgs.mkShell.override { stdenv = toolchain pkgs; }) {
          packages = with pkgs; [
            cmake
            ninja
            boost
            openssl
            nlohmann_json
            cli11
            catch2_3
            clang-tools
            ccache

            curl
            jq
          ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.gdb ];

          shellHook = ''
            export CC="${(toolchain pkgs).cc}/bin/cc"
            export CXX="${(toolchain pkgs).cc}/bin/c++"
            echo "qoderz dev shell: $($CXX --version | head -n1) / cmake $(cmake --version | head -n1 | cut -d' ' -f3)"
            # A cold build of the tree takes a few minutes; ccache turns the
            # repeat cost into seconds. Keep the cache bounded so it cannot
            # crowd out the rest of the user's home directory.
            #
            # The cache lives under the XDG cache directory, never in the working
            # tree: Nix copies the tree into the store on every build, so a
            # multi-gigabyte cache there slows each flake operation down and keeps
            # moving the source fingerprint. It deliberately does not live in
            # `.git` either — in a linked worktree `.git` is a file, and version
            # control metadata should stay free of build artifacts.
            export CCACHE_DIR="''${CCACHE_DIR:-$HOME/.cache/qoderz/ccache}"

            export CCACHE_MAXSIZE="''${CCACHE_MAXSIZE:-5G}"
            export CCACHE_COMPRESS="''${CCACHE_COMPRESS:-true}"
            export CCACHE_BASEDIR="''${CCACHE_BASEDIR:-$PWD}"
            export CCACHE_NOHASHDIR=1
          '';
        };
      });
    };
}
