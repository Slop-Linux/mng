{
  description = "MacNativeGames on Linux (aarch64, Vulkan)";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  outputs = { self, nixpkgs }:
    let
      system = "aarch64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in {
      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [
          python314 clang lld llvm git rsync gnumake pkg-config
          libopus libvpx sdl3 vulkan-headers vulkan-loader spirv-tools glslang
          dotnetCorePackages.sdk_10_0 zlib openssl
        ];
        DOTNET_CLI_TELEMETRY_OPTOUT = "1";
        DOTNET_ROOT = "${pkgs.dotnetCorePackages.sdk_10_0}/share/dotnet";
      };
    };
}
