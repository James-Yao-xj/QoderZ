{
  description = "QoderZ — a Qoder to OpenAI-compatible gateway written in C++23";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      for_all_systems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      mk_qoderz =
        pkgs:
        pkgs.gcc14Stdenv.mkDerivation {
          pname = "qoderz";
          version = "0.1.0";

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
            platforms = pkgs.lib.platforms.linux;
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
        default = (pkgs.mkShell.override { stdenv = pkgs.gcc14Stdenv; }) {
          packages = with pkgs; [
            cmake
            ninja
            gcc14
            boost
            openssl
            nlohmann_json
            cli11
            catch2_3
            clang-tools
            gdb
            curl
            jq
          ];

          shellHook = ''
            export CC="${pkgs.gcc14}/bin/gcc"
            export CXX="${pkgs.gcc14}/bin/g++"
            echo "qoderz dev shell: $($CXX -dumpfullversion) / cmake $(cmake --version | head -n1 | cut -d' ' -f3)"
          '';
        };
      });
    };
}
