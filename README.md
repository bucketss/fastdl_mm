# FastDL Metamod

A Metamod plugin that acts as as fastdl server for HLDS.

## Build

HLDS is 32-bit, so the plugin must be built for 32-bit.

Requires CMake and an SDK directory containing `cssdk/` and `metamod/`:

- `cssdk/` — `common/`, `dlls/`, `engine/` and `public/` from either
  [ReGameDLL_CS](https://github.com/rehlds/ReGameDLL_CS) (`regamedll/`) or the
  stock [Half-Life SDK](https://github.com/ValveSoftware/halflife); both build
- `metamod/` — plugin headers from [Metamod-R](https://github.com/rehlds/Metamod-R)
  or any Metamod 1.19+ SDK

Point `FASTDL_MM_SDK_ROOT` at the directory holding both.

Nothing ReGameDLL- or ReHLDS-specific is used at runtime: the plugin hooks only
`GameInit` and `StartFrame` and calls stock engine functions, so it runs on an
unmodified HLDS with standard Metamod.

### Windows

```powershell
cmake -S . -B build -A Win32 `
  -DFASTDL_MM_SDK_ROOT=C:\path\to\sdk
cmake --build build --config Release
```

Output:

```text
build/Release/fastdl_mm.dll
```

### Linux

Requires `g++-multilib` and `libc6-dev-i386`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DFASTDL_MM_SDK_ROOT=/path/to/sdk
cmake --build build -j4
```

Output:

```text
build/fastdl_mm_i386.so
```

## Install

Copy the plugin and configuration file:

```text
cstrike/addons/fastdl/fastdl_mm.dll
cstrike/addons/fastdl/fastdl_mm.cfg
```

Add the appropriate line to `cstrike/addons/metamod/plugins.ini`:

```text
win32 addons/fastdl/fastdl_mm.dll
linux addons/fastdl/fastdl_mm_i386.so
```

Configure the client download URL:

```text
sv_downloadurl "http://203.0.113.10:27015"
```

By default, `fastdl_port "0"` uses the same port number as HLDS.

## Main settings

```text
fastdl_enabled           "1"
fastdl_bind              "0.0.0.0"
fastdl_port              "0"
fastdl_root              "cstrike"
fastdl_serve_dirs        "sprites,sound,sounds,overviews,models,maps,gfx"
fastdl_serve_types       "bsp,nav,res,wad,mdl,spr,wav,mp3,bmp,tga,txt,htm,html,gz,bz2"
fastdl_max_file_mb       "250"
fastdl_threads           "1"
fastdl_log               "logs/fastdl/fastdl.log"
fastdl_log_age           "7"
```

Use `fastdl_restart` to apply changed cvars without restarting HLDS.

## Commands

```text
fastdl_status
fastdl_restart
fastdl_unblock <ip>
```

## Notes

* Only `GET` and `HEAD` requests from the Steam downloader are served.
* Files must match both `fastdl_serve_dirs` and `fastdl_serve_types`.
* `.wad` files directly in `fastdl_root` are also served if `wad` is in `fastdl_serve_types`.
