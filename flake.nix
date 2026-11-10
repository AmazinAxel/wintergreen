{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});
    in {
      # The host-side EPUB → WGB converter and the `books` front end that drives
      # it (preprocess.fish). The firmware itself is built with PlatformIO and
      # is deliberately not packaged here.
      packages = forAll (pkgs: rec {
        books = pkgs.runCommand "books" { nativeBuildInputs = [ pkgs.makeWrapper ]; } ''
          install -Dm755 ${./preprocess.fish} $out/bin/books
          wrapProgram $out/bin/books --prefix PATH : ${pkgs.lib.makeBinPath (with pkgs; [
            fish epub2wgb unzip zip xmlstarlet chafa openssh gnutar coreutils
          ])}
        '';

        epub2wgb = pkgs.stdenv.mkDerivation {
          pname = "epub2wgb";
          version = "0.1.0";
          src = ./.;
          nativeBuildInputs = [ pkgs.cmake ];
          # tools/epub2wgb/CMakeLists.txt reaches up to ../.. for the core
          # sources, so configure from that subdirectory rather than the root.
          cmakeDir = "../tools/epub2wgb";
          # CMakeLists.txt has no install() rule — it is normally driven by
          # tools/convert-books.sh straight out of the build tree.
          installPhase = ''
            runHook preInstall
            install -Dm755 epub2wgb $out/bin/epub2wgb
            runHook postInstall
          '';
          # stb_image and miniz are vendored; nothing else is needed.
          meta.mainProgram = "epub2wgb";
        };
        default = epub2wgb;
      });

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [ cmake platformio esptool ];
        };
      });
    };
}
