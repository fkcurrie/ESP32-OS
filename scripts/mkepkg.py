#!/usr/bin/env python3
"""Build a C6-MicroLinux .epkg package with an EPK1 SHA-256 header."""

import argparse
import hashlib
import pathlib
import struct

MAGIC = b"EPK1"
FORMAT_VER = 1
ABI_VER = 1


def pad_bytes(text: str, length: int) -> bytes:
    raw = text.encode("utf-8")[: length - 1]
    return raw + b"\x00" * (length - len(raw))


def build_epkg(
    name: str,
    version: str,
    arch: str,
    description: str,
    mappings: list[tuple[pathlib.Path, str, int]],
    output: pathlib.Path,
) -> None:
    payload = bytearray()
    for src_path, dest_vpath, mode in mappings:
        if not dest_vpath.startswith("/") or ".." in dest_vpath:
            raise ValueError(f"Invalid destination path: {dest_vpath}")
        data = src_path.read_bytes()
        vpath_bytes = dest_vpath.encode("utf-8")
        payload += struct.pack(">HHI", len(vpath_bytes), mode, len(data))
        payload += vpath_bytes
        payload += data

    digest = hashlib.sha256(payload).digest()
    header = bytearray()
    header += MAGIC
    header += struct.pack(">HH", FORMAT_VER, ABI_VER)
    header += pad_bytes(name, 32)
    header += pad_bytes(version, 16)
    header += pad_bytes(arch, 24)
    header += pad_bytes(description, 64)
    header += digest
    header += struct.pack(">H", len(mappings))
    assert len(header) == 178

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(bytes(header + payload))
    print(f"[mkepkg] Wrote {output} ({len(header) + len(payload)} bytes, sha256={digest.hex()[:16]}...)")


def main() -> int:
    ap = argparse.ArgumentParser(description="Package RV32IMAC ELF binaries, Lua scripts, or shell scripts into an .epkg archive")
    ap.add_argument("-n", "--name", required=True)
    ap.add_argument("-v", "--version", default="0.1.0")
    ap.add_argument("-a", "--arch", default="rv32imac", help="rv32imac, riscv32, or noarch")
    ap.add_argument("-d", "--description", default="C6-MicroLinux installable package")
    ap.add_argument("-f", "--file", action="append", required=True,
                    help="Mapping in format <local_src_path>:<target_c6os_vpath>[:octal_mode]")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    mappings: list[tuple[pathlib.Path, str, int]] = []
    for item in args.file:
        parts = item.split(":")
        if len(parts) < 2:
            raise SystemExit(f"Invalid --file mapping '{item}', expected src:dst[:mode]")
        src = pathlib.Path(parts[0])
        dst = parts[1]
        mode = int(parts[2], 8) if len(parts) >= 3 else 0o755
        mappings.append((src, dst, mode))

    build_epkg(args.name, args.version, args.arch, args.description, mappings, pathlib.Path(args.output))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
