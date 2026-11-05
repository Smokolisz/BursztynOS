#!/usr/bin/env python3
import argparse
import stat
import struct
import subprocess
from pathlib import Path

PAGE_SIZE = 4096
MAGIC = b"BRSZRFS1"
HEADER = struct.Struct("<8sQ")
ENTRY = struct.Struct("<QQQII")

LIBRARY_DIRECTORY = Path("/usr/lib")
REAPER_INSTALL_PATH = "opt/REAPER"
SKIPPED_LIBRARIES = {"libGL.so.1", "libdl.so.2", "libpthread.so.0", "linux-vdso.so.1"}
KEPT_GLIBC_STUBS = ["libdl.so.2", "libpthread.so.0", "librt.so.1"]
FONT_DIRECTORY = Path("/usr/share/fonts/liberation")

STATIC_FILES = {
    "etc/passwd": "root:x:0:0:root:/root:/bin/sh\nuser:x:1000:1000:user:/home/user:/bin/sh\n",
    "etc/group": "root:x:0:\nuser:x:1000:\naudio:x:29:user\n",
    "etc/hostname": "bursztyn\n",
    "etc/hosts": "127.0.0.1 localhost bursztyn\n",
    "etc/nsswitch.conf": "passwd: files\ngroup: files\nhosts: files\n",
    "etc/localtime": "",
    "proc/self/status": "Name:\treaper\nState:\tR (running)\nPid:\t1000\nUid:\t1000\t1000\t1000\t1000\n",
    "proc/cpuinfo": "processor\t: 0\nvendor_id\t: BursztynOS\nmodel name\t: x86-64 virtual processor\ncpu MHz\t\t: 3000.000\n\n",
    "proc/meminfo": "MemTotal:\t3145728 kB\nMemFree:\t2097152 kB\nMemAvailable:\t2097152 kB\n",
    "proc/stat": "cpu  0 0 0 0 0 0 0 0 0 0\ncpu0 0 0 0 0 0 0 0 0 0 0\n",
    "sys/devices/system/cpu/online": "0\n",
    "sys/devices/system/cpu/possible": "0\n",
}

SYMLINKS = {
    "lib": "usr/lib",
    "lib64": "usr/lib",
    "proc/self/exe": f"/{REAPER_INSTALL_PATH}/reaper",
}

EMPTY_DIRECTORIES = ["tmp", "root", "home/user/.config", "var/tmp", "dev/input"]


def needed_libraries(path: Path) -> list[str]:
    output = subprocess.run(["readelf", "-d", str(path)], capture_output=True, text=True, check=True).stdout
    names = []
    for line in output.splitlines():
        if "(NEEDED)" in line:
            names.append(line.split("[", 1)[1].split("]", 1)[0])
    return names


def is_elf(path: Path) -> bool:
    with path.open("rb") as handle:
        return handle.read(4) == b"\x7fELF"


def resolve_library_closure(roots: list[Path]) -> dict[str, Path]:
    resolved: dict[str, Path] = {}
    pending = list(roots)
    while pending:
        for name in needed_libraries(pending.pop()):
            if name in resolved or name in SKIPPED_LIBRARIES:
                continue
            candidate = LIBRARY_DIRECTORY / name
            if not candidate.exists():
                raise FileNotFoundError(f"missing shared library {name}")
            resolved[name] = candidate
            pending.append(candidate)
    return resolved


class ImageBuilder:
    def __init__(self) -> None:
        self.entries: list[tuple[str, int, bytes | Path]] = []
        self.seen: set[str] = set()

    def add_directory(self, path: str) -> None:
        self._add(path, stat.S_IFDIR | 0o755, b"")

    def add_file(self, path: str, source: Path) -> None:
        self._add(path, stat.S_IFREG | (source.stat().st_mode & 0o777), source)

    def add_text(self, path: str, text: str) -> None:
        self._add(path, stat.S_IFREG | 0o644, text.encode())

    def add_symlink(self, path: str, target: str) -> None:
        self._add(path, stat.S_IFLNK | 0o777, target.encode())

    def add_tree(self, prefix: str, root: Path, skip: set[str]) -> None:
        for source in sorted(root.rglob("*")):
            relative = source.relative_to(root).as_posix()
            if relative in skip:
                continue
            path = f"{prefix}/{relative}"
            if source.is_symlink():
                self.add_symlink(path, str(source.readlink()))
            elif source.is_dir():
                self.add_directory(path)
            else:
                self.add_file(path, source)

    def _add(self, path: str, mode: int, payload: bytes | Path) -> None:
        if path in self.seen:
            raise ValueError(f"duplicate image path {path}")
        self.seen.add(path)
        self.entries.append((path, mode, payload))

    def write(self, output: Path) -> None:
        table_size = HEADER.size + ENTRY.size * len(self.entries)
        strings = bytearray()
        path_offsets = []
        for path, _, _ in self.entries:
            path_offsets.append(table_size + len(strings))
            strings += path.encode() + b"\0"

        cursor = align_page(table_size + len(strings))
        table = bytearray(HEADER.pack(MAGIC, len(self.entries)))
        layout = []
        for (_, mode, payload), path_offset in zip(self.entries, path_offsets):
            size = payload.stat().st_size if isinstance(payload, Path) else len(payload)
            table += ENTRY.pack(path_offset, cursor, size, mode, 0)
            layout.append((cursor, payload))
            cursor = align_page(cursor + size)

        with output.open("wb") as handle:
            handle.write(table)
            handle.write(strings)
            for offset, payload in layout:
                handle.seek(offset)
                handle.write(payload.read_bytes() if isinstance(payload, Path) else payload)
            handle.truncate(cursor)


def align_page(value: int) -> int:
    return (value + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)


def main() -> None:
    parser = argparse.ArgumentParser(description="Build the root filesystem image for the Linux ABI layer")
    parser.add_argument("--reaper", type=Path, required=True, help="extracted REAPER directory")
    parser.add_argument("--swell", type=Path, required=True, help="libSwell.so built for the fbdev backend")
    parser.add_argument("--config", type=Path, required=True, help="directory seeded as ~/.config/REAPER")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    image = ImageBuilder()
    image.add_tree(REAPER_INSTALL_PATH, args.reaper, skip={"libSwell.so"})
    image.add_file(f"{REAPER_INSTALL_PATH}/libSwell.so", args.swell)
    image.add_tree("home/user/.config/REAPER", args.config, skip=set())

    binaries = [args.reaper / "reaper", args.swell]
    binaries += [path for path in args.reaper.glob("Plugins/*.so") if is_elf(path)]
    libraries = resolve_library_closure(binaries)
    for name, source in sorted(libraries.items()):
        image.add_file(f"usr/lib/{name}", source.resolve())
    for name in KEPT_GLIBC_STUBS:
        image.add_file(f"usr/lib/{name}", (LIBRARY_DIRECTORY / name).resolve())
    if "ld-linux-x86-64.so.2" not in libraries:
        image.add_file("usr/lib/ld-linux-x86-64.so.2", (LIBRARY_DIRECTORY / "ld-linux-x86-64.so.2").resolve())

    for font in sorted(FONT_DIRECTORY.glob("*.ttf")):
        image.add_file(f"usr/share/fonts/{font.name}", font)

    for path, text in STATIC_FILES.items():
        image.add_text(path, text)
    for path, target in SYMLINKS.items():
        image.add_symlink(path, target)
    for path in EMPTY_DIRECTORIES:
        image.add_directory(path)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    image.write(args.output)
    print(f"{args.output}: {len(image.entries)} entries, {args.output.stat().st_size >> 20} MiB")


if __name__ == "__main__":
    main()
