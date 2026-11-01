# M4 Linux / Vulkan

`Slop-Linux/mng` is a direct fork of
[`ly05010419/MacNativeGames`](https://github.com/ly05010419/MacNativeGames).
The `m4/linux` branch preserves the Linux port through `ba1ea1d`, including
the Vulkan changes used by the existing M4 build. The upstream `main` branch
is retained separately.

The runtime compiles from `games/totk/Makefile.linux` for aarch64 Linux. It
loads rewritten ARM64 game modules, provides the host system services, and
translates NVN operations to Vulkan. `totk_xlate` translates Maxwell shaders
to SPIR-V using the vendored Ryujinx translator and the maintained patches.
SDL3 supplies the Wayland window, input and audio. No microVM is required.

For a development build on the M4:

```sh
nix develop
cd games/totk
make -f Makefile.linux VERSION=botw190 GFX=vk -j"$(nproc)"
```

This produces `build/linux-botw190/botw-native`. The executable contains the
runtime; game modules, RomFS, DLC and shared fonts are separate inputs.
The Linux shader translator can be built with
`games/totk/tools/totk_xlate/publish_linux.sh` from the repository root.

Deployment packages are maintained in `Slop-Linux/nixos-apple`. Its Nix
definitions build the runtime and translator from this repository and prepare
game data from separately supplied original files. A host supplies those
private inputs; they are not distributed with this source fork. Keep profile,
saves, mods and writable shader caches in the user's data directory.
