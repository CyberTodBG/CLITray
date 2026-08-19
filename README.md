# CLITray

CLITray is a tiny Windows tray application that launches any command-line program silently in the background, without keeping any window in the taskbar. A system-tray icon is added to control it: **Start**, **Stop**, **Restart**, a live **console** window, config editing.

## Idea
The initial idea for this project came from wanting to run unsloth-studio without keeping a cmd window open. Then I decided to expand it and make it universal for any console app.

## Features
- Runs any CLI command without a cmd window open (configurable via `clitray.ini`)
- Tray menu: Start / Stop / Restart / Console / Edit Config / Exit
- Live console with auto-scroll toggle
- Hover tooltip showing the app name (without the arguments)
- Optional autostart and fresh-log-per-launch (`fresh_log=1`)
- No dependencies — a single static `.exe`

## Installation
Just download [CLITray (Latest Release)](https://github.com/CyberTodBG/CLITray/releases/download/v1.0/CLITray-v1.0.zip)

Unzip and set the path to the program in the .ini file.

## Build
Cross-compiled with MinGW on Linux (`./build.sh`), no installer required.

When running the build script to compile from source it asks for an icon file or generates a default one.

## Footnote
This app was **vibe coded** with [OpenCode](https://opencode.ai). The code and debugging were produced through conversational prompts with **DeepSeek-V4-Flash**.
