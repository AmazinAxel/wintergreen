{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});
    in {
      # `books`: review EPUBs, bake them to WGB and upload (tools/books). The
      # firmware itself is built with PlatformIO and is not packaged here.
      packages = forAll (pkgs: rec {
        books = pkgs.stdenv.mkDerivation {
          pname = "books";
          version = "0.2.0";
          src = ./.;
          nativeBuildInputs = [ pkgs.cmake pkgs.makeWrapper ];
          # tools/books/CMakeLists.txt reaches up to ../.. for the core
          # sources, so configure from that subdirectory rather than the root.
          cmakeDir = "../tools/books";
          # miniz is vendored. ssh is the only thing exec'd.
          postInstall = ''
            wrapProgram $out/bin/books --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.openssh ]}
          '';
          meta.mainProgram = "books";
        };
        default = books;
      });

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [ cmake platformio esptool ];
        };
      });
    };
}
