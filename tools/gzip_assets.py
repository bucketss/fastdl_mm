#!/usr/bin/env python3
import argparse
import gzip
import os
import sys

DEFAULT_DIRS = "sprites,sound,sounds,overviews,models,maps,gfx"
DEFAULT_TYPES = "bsp,nav,res,wad,mdl,spr,wav,bmp,tga,txt,htm,html"


def split_list(value):
    return {item.strip().lower().lstrip(".") for item in value.split(",") if item.strip()}


def extension(name):
    return os.path.splitext(name)[1].lower().lstrip(".")


def files(root, dirs):
    with os.scandir(root) as entries:
        for entry in entries:
            if entry.is_file():
                yield entry.path, True
            elif entry.is_dir() and entry.name.lower() in dirs:
                for base, _, names in os.walk(entry.path):
                    for name in names:
                        yield os.path.join(base, name), False


def sources(root, dirs, types):
    for path, top in files(root, dirs):
        ext = extension(path)
        if ext in types and (not top or ext == "wad"):
            yield path


def orphans(root, dirs, types):
    for path, top in files(root, dirs):
        if path.endswith(".gz.tmp"):
            yield path
            continue
        if not path.endswith(".gz"):
            continue
        source = path[:-3]
        ext = extension(source)
        if ext in types and (not top or ext == "wad") and not os.path.exists(source):
            yield path


def current(source, target):
    try:
        src = os.stat(source)
        dst = os.stat(target)
    except FileNotFoundError:
        return False
    if dst.st_size < 18 or dst.st_mtime_ns // 10**9 != src.st_mtime_ns // 10**9:
        return False
    with open(target, "rb") as f:
        f.seek(-4, os.SEEK_END)
        return int.from_bytes(f.read(4), "little") == src.st_size & 0xFFFFFFFF


def main():
    parser = argparse.ArgumentParser(description="Write .gz sidecars for fastdl_mm.")
    parser.add_argument("root", help="fastdl_root, e.g. cstrike")
    parser.add_argument("--dirs", default=DEFAULT_DIRS)
    parser.add_argument("--types", default=DEFAULT_TYPES)
    parser.add_argument("--min-saving", type=float, default=5.0,
                        help="percent; smaller savings are not written")
    parser.add_argument("-c", "--cleanup", action="store_true",
                        help="delete .gz files whose source no longer exists")
    args = parser.parse_args()

    dirs = split_list(args.dirs)
    types = split_list(args.types)
    written = kept = skipped = 0
    before = after = 0

    if args.cleanup:
        removed = 0
        for path in list(orphans(args.root, dirs, types)):
            os.remove(path)
            removed += 1
            print(f"removed {path}")
        print(f"removed {removed} orphaned files")

    for source in sources(args.root, dirs, types):
        target = source + ".gz"
        if current(source, target):
            kept += 1
            continue
        stat = os.stat(source)
        with open(source, "rb") as f:
            data = f.read()
        packed = gzip.compress(data, compresslevel=9, mtime=0)
        if not data or len(packed) > len(data) * (1 - args.min_saving / 100):
            skipped += 1
            if os.path.exists(target):
                os.remove(target)
            continue
        temp = target + ".tmp"
        with open(temp, "wb") as f:
            f.write(packed)
        os.utime(temp, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        os.replace(temp, target)
        written += 1
        before += len(data)
        after += len(packed)
        print(f"{source}: {len(data) // 1024} -> {len(packed) // 1024} KiB")

    saved = (before - after) / (1024 * 1024)
    print(f"written {written}, up to date {kept}, not worth it {skipped}, "
          f"saved {saved:.1f} MiB on written files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
