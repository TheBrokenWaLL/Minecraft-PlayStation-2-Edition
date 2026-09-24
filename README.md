# Overview

## Minecraft: PlayStation 2 Edition (MC:PS2)
is a fork focused exclusively on delivering and improving the Minecraft experience on the PlayStation 2.

![Minecraft PlayStation 2 Edition](https://drive.google.com/uc?export=view&id=1U29xSOC7pMjn4WNkd6KTkb78eKljMzT_)

The project follows a more purist approach to Minecraft, aiming to preserve the identity, presentation, gameplay feel, and simplicity of the original game while adapting it to the limitations and characteristics of PS2 hardware.

Although the project is based on **[OptiCraft Heritage Edition](https://github.com/OptiJuegos/OptiCraftHeritageEdition)** and may incorporate useful fixes and improvements from upstream, MC:PS2 is developed as an independent PS2-focused fork. It is not intended to strictly follow upstream design decisions, platform priorities, interface changes, or development standards.

When an upstream change benefits the PS2 version, it may be integrated and adapted as necessary. Likewise, changes may be implemented exclusively for this fork whenever they better fit the goals of the project, even if they intentionally diverge from upstream.

[![Star History Chart](https://api.star-history.com/svg?repos=thebrokenwall/minecraft-playstation-2-edition&type=Date)](https://www.star-history.com/?repos=thebrokenwall%2Fminecraft-playstation-2-edition&type=date&legend=top-left)

# Building

## Linux

```sh
./build_linux.sh
./build_linux.sh gcc -debug
./build_linux.sh wii -release --jobs 4
./build_linux.sh ps2
```

## Windows (PowerShell 7.3+)

```powershell
pwsh -File .\build_windows.ps1
pwsh -File .\build_windows.ps1 gcc -Debug
pwsh -File .\build_windows.ps1 wii -Release -Jobs 4
pwsh -File .\build_windows.ps1 ps2
```

---

## Disclaimer!!!

MC:PS2 is a fork of the original [OptiCraft Heritage Edition](https://github.com/OptiJuegos/OptiCraftHeritageEdition) project.

This fork is not intended to replace the original project. Its primary focus is to improve the PlayStation 2 version while preserving the original projects overall direction and architecture whenever practical.

The project focuses on PS2-specific improvements such as performance and memory optimizations, multiplayer support, bug fixes, controller and interface improvements, storage handling, localization, compatibility with real hardware, and other quality-of-life features.

Whenever possible, changes are kept isolated and maintainable so they can coexist with the broader OptiCraft codebase without unnecessarily diverging from the upstream project.

The original [OptiCraft Heritage Edition](https://github.com/OptiJuegos/OptiCraftHeritageEdition) project remains the upstream project, while MC:PS2 serves as a PS2-focused extension and improvement effort.

This project is not affiliated with, endorsed by, or sponsored by Mojang Studios or Microsoft. Minecraft and its associated trademarks and intellectual property belong to their respective owners.
