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
`GameInit`, `ServerActivate` and `StartFrame` and calls stock engine functions, so it runs on an
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

A build made this way only loads on systems with the same or newer glibc. For a
build that loads on glibc 2.17 and newer (Debian 8+, CentOS 7+), use the
manylinux2014 container (needs podman or docker):

```sh
tools/build_manylinux.sh
```

Output:

```text
build/manylinux/fastdl_mm_i386.so
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
fastdl_serve_types       "bsp,nav,res,wad,mdl,spr,wav,mp3,bmp,tga,txt,htm,html,bz2"
fastdl_max_file_mb       "250"
fastdl_threads           "1"
fastdl_log               "logs/fastdl/fastdl.log"
fastdl_log_age           "7"
```

`fastdl_restart` reloads `fastdl_mm.cfg` and restarts the HTTP server without restarting HLDS.

## Commands

```text
fastdl_status
fastdl_restart
fastdl_blocked
fastdl_unblock <ip>
```

## Notes

* Only `GET` and `HEAD` requests from the Steam downloader are served.
* Files must match both `fastdl_serve_dirs` and `fastdl_serve_types`.
* `.wad` files directly in `fastdl_root` are also served if `wad` is in `fastdl_serve_types`.
* File names are matched case-insensitively on Linux too.
* `sv_downloadurl` is checked against the fastdl port on start, on map change and in `fastdl_status`.

## Gzip

Clients that accept gzip get `<file>.gz` instead of `<file>` when the `.gz` has the same
modification time (to the second) as the original and its gzip trailer matches the original's size.
Otherwise the original is served. `.gz` files are not served by name unless `gz` is in `fastdl_serve_types`.

Create or refresh the `.gz` files after adding or changing content:

```text
python tools/gzip_assets.py cstrike
perl tools/gzip_assets.pl cstrike
```

Options: `--dirs`, `--types`, `--min-saving` (percent, default 5).
