# Linux maintenance

- This repository is a direct fork of `ly05010419/MacNativeGames`. Keep the
  upstream history and maintain the M4 Linux/Vulkan port on `m4/linux`.
- Use hardware and platform names in shared code; deployment node names belong
  in the consumer's host configuration.
- Maintain Linux runtime source here. Nix packages and reusable NixOS modules
  live in `Slop-Linux/nixos-apple`; the host repository pins that integration.
- Build and validate on the M4. Use the consumer's Colmena hive for NixOS node
  builds and deployments. Preserve working system generations and user saves.
- Do not write or run unit tests, mocks, self-tests or test frameworks for this
  maintenance work. Validate builds and actual hardware behavior.
- Keep game files, decryption keys, firmware, saves, caches and runtime captures
  out of this public source repository.
