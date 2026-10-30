{ pkgs ? import <nixpkgs> { } }:

let
  inherit (pkgs) lib;
in
(pkgs.mkShell.override { stdenv = pkgs.clangStdenv; }) {
  name = "jpeg-nitro";

  # Keep the upstream sources/patches and static dependency layout supplied by
  # scripts/get-deps.sh; system packages alone cannot replace third_party/.
  nativeBuildInputs = with pkgs; [
    bash
    gnumake
    cmake
    meson
    ninja
    nasm
    pkg-config
    curl
    cacert
    git
    patch
    gnutar
    gzip
    bzip2
    xz
    unzip
    zip
    xxd
    (python3.withPackages (ps: [ ps.pip ]))
  ] ++ lib.optionals pkgs.stdenv.isLinux (with pkgs; [
    glib # glib-compile-resources for make gnome
    libxml2 # xmllint for GtkBuilder resource preprocessing
    desktop-file-utils
    flatpak
    flatpak-builder
    wayland-scanner
  ]);

  buildInputs = with pkgs; [
    zlib
    libiconv
  ] ++ lib.optionals pkgs.stdenv.isLinux (with pkgs; [
    gtk4
    libadwaita
    libGL
    libGLU
    wayland
    wayland-protocols
    libxkbcommon
    libdecor
    dbus
    xorg.libX11
    xorg.libXext
    xorg.libXrandr
    xorg.libXcursor
    xorg.libXi
    xorg.libXfixes
    xorg.libXScrnSaver
    xorg.libXtst
  ]);

  # The Makefile deliberately uses -march=native and builds ASan/UBSan tools.
  hardeningDisable = [ "fortify" ];
  NIX_ENFORCE_NO_NATIVE = "0";
}
