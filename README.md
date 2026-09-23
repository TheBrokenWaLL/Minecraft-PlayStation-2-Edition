# Building

There are two build entry points, based on the platform profiles in the GitHub
Actions workflow. Run either without arguments to select GCC, Wii or PS2.

## Linux

```sh
bash build_linux.sh
bash build_linux.sh gcc -debug
bash build_linux.sh wii -release --jobs 4
bash build_linux.sh ps2
```

## Windows (PowerShell 7.3+)

```powershell
pwsh -File .\build_windows.ps1
pwsh -File .\build_windows.ps1 gcc -Debug
pwsh -File .\build_windows.ps1 wii -Release -Jobs 4
pwsh -File .\build_windows.ps1 ps2
```

Release is the default. GCC builds native Linux on Linux and Windows MinGW on
Windows. Wii builds the full game. Console builds stage assets by default; PS2
also regenerates `assets.pak`. SDKs and dependencies must already be installed.
No source patches, downloads, package installations, or cleanup run automatically.

The old `build_ps2_*` entry points
and `scr_build_linux` / `scr_build_win` collections have been retired. Extra CMake
options replace the old specialized profiles. Assets, saves and SDKs are unchanged.

---

## Disclaimer!!!

MC:PS2 is a fork of the original [OptiCraft Heritage](https://github.com/OptiJuegos/OptiCraftHeritageEdition) Edition project.

This fork is not intended to replace the original project. Its primary focus is to improve the PlayStation 2 version while preserving the original projects overall direction and architecture whenever practical.

The project focuses on PS2-specific improvements such as performance and memory optimizations, multiplayer support, bug fixes, controller and interface improvements, storage handling, localization, compatibility with real hardware, and other quality-of-life features.

Whenever possible, changes are kept isolated and maintainable so they can coexist with the broader OptiCraft codebase without unnecessarily diverging from the upstream project.

The original [OptiCraft Heritage Edition](https://github.com/OptiJuegos/OptiCraftHeritageEdition) project remains the upstream project, while MC:PS2 serves as a PS2-focused extension and improvement effort.

This project is not affiliated with, endorsed by, or sponsored by Mojang Studios or Microsoft. Minecraft and its associated trademarks and intellectual property belong to their respective owners.

[![Star History Chart](https://api.star-history.com/svg?repos=thebrokenwall/minecraft-playstation-2-edition&type=Date)](https://www.star-history.com/?repos=thebrokenwall%2Fminecraft-playstation-2-edition&type=date&legend=top-left)
