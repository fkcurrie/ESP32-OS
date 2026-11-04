#!/usr/bin/env python3
"""Build or sign C6-MicroLinux .epkg packages (EPK1 unsigned / EPK2 Ed25519-signed)."""

from __future__ import annotations

import argparse
import base64
import hashlib
import os
import pathlib
import secrets
import struct

ABI_VER = 1

# Pure-Python RFC 8032 Ed25519 fallback (used only when `cryptography` is absent)
_P = (1 << 255) - 19
_L = (1 << 252) + 27742317777372353535851937790883648493
_D = (-121665 * pow(121666, _P - 2, _P)) % _P
_I = pow(2, (_P - 1) // 4, _P)
_BY = (4 * pow(5, _P - 2, _P)) % _P


def _recover_x(y: int, sign: int) -> int:
    x2 = ((y * y - 1) * pow(_D * y * y + 1, _P - 2, _P)) % _P
    if x2 == 0:
        return 0
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P != 0:
        x = (x * _I) % _P
    if (x & 1) != sign:
        x = _P - x
    return x


_B = (_recover_x(_BY, 0), _BY, 1, (_recover_x(_BY, 0) * _BY) % _P)


def _ed_add(p: tuple[int, int, int, int], q: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    a = ((p[1] - p[0]) * (q[1] - q[0])) % _P
    b = ((p[1] + p[0]) * (q[1] + q[0])) % _P
    c = (2 * p[3] * q[3] * _D) % _P
    d = (2 * p[2] * q[2]) % _P
    e = (b - a) % _P
    f = (d - c) % _P
    g = (d + c) % _P
    h = (b + a) % _P
    return (e * f) % _P, (g * h) % _P, (f * g) % _P, (e * h) % _P


def _ed_mul(s: int, p: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    acc = (0, 1, 1, 0)
    cur = p
    while s > 0:
        if s & 1:
            acc = _ed_add(acc, cur)
        cur = _ed_add(cur, cur)
        s >>= 1
    return acc


def _ed_encode(pt: tuple[int, int, int, int]) -> bytes:
    inv_z = pow(pt[2], _P - 2, _P)
    x = (pt[0] * inv_z) % _P
    y = (pt[1] * inv_z) % _P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def ed25519_pubkey_from_seed(seed: bytes) -> bytes:
    if len(seed) != 32:
        raise ValueError("Ed25519 seed must be 32 bytes")
    try:
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

        priv = Ed25519PrivateKey.from_private_bytes(seed)
        return priv.public_key().public_bytes(
            encoding=serialization.Encoding.Raw,
            format=serialization.PublicFormat.Raw,
        )
    except ImportError:
        h = bytearray(hashlib.sha512(seed).digest())
        h[0] &= 248
        h[31] = (h[31] & 63) | 64
        a = int.from_bytes(h[:32], "little")
        return _ed_encode(_ed_mul(a, _B))


def ed25519_sign_seed(seed: bytes, msg: bytes) -> bytes:
    if len(seed) != 32:
        raise ValueError("Ed25519 seed must be 32 bytes")
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

        return Ed25519PrivateKey.from_private_bytes(seed).sign(msg)
    except ImportError:
        h = bytearray(hashlib.sha512(seed).digest())
        h[0] &= 248
        h[31] = (h[31] & 63) | 64
        a = int.from_bytes(h[:32], "little")
        pk = _ed_encode(_ed_mul(a, _B))
        r = int.from_bytes(hashlib.sha512(bytes(h[32:]) + msg).digest(), "little") % _L
        r_enc = _ed_encode(_ed_mul(r, _B))
        k = int.from_bytes(hashlib.sha512(r_enc + pk + msg).digest(), "little") % _L
        s = (r + k * a) % _L
        return r_enc + s.to_bytes(32, "little")


def format_openssh_ed25519_pubkey(pk: bytes, comment: str = "epkg@c6-linux") -> str:
    kt = b"ssh-ed25519"
    blob = struct.pack(">I", len(kt)) + kt + struct.pack(">I", len(pk)) + pk
    b64 = base64.b64encode(blob).decode("ascii")
    return f"ssh-ed25519 {b64} {comment}".strip()


def _read_ssh_str(buf: bytes, pos: int) -> tuple[bytes, int]:
    if pos + 4 > len(buf):
        raise ValueError("Truncated OpenSSH key structure")
    (slen,) = struct.unpack_from(">I", buf, pos)
    pos += 4
    if pos + slen > len(buf):
        raise ValueError("Truncated OpenSSH string field")
    return buf[pos : pos + slen], pos + slen


def load_ed25519_seed(spec: str) -> bytes:
    cleaned = spec.strip()
    if len(cleaned) == 64:
        try:
            return bytes.fromhex(cleaned)
        except ValueError:
            pass

    key_path = pathlib.Path(spec)
    text = key_path.read_text(encoding="utf-8").strip()
    if "BEGIN OPENSSH PRIVATE KEY" in text:
        body_lines = [
            ln.strip()
            for ln in text.splitlines()
            if ln.strip() and not ln.strip().startswith("-----")
        ]
        raw = base64.b64decode("".join(body_lines))
        magic = b"openssh-key-v1\x00"
        if not raw.startswith(magic):
            raise ValueError("Unsupported OpenSSH private key header")
        pos = len(magic)
        cipher, pos = _read_ssh_str(raw, pos)
        kdf, pos = _read_ssh_str(raw, pos)
        _kdf_opts, pos = _read_ssh_str(raw, pos)
        if cipher != b"none" or kdf != b"none":
            raise ValueError("Encrypted OpenSSH keys are not supported; generate an unencrypted signing key")
        (num_keys,) = struct.unpack_from(">I", raw, pos)
        pos += 4
        if num_keys < 1:
            raise ValueError("No keys in OpenSSH private key file")
        _pub_blob, pos = _read_ssh_str(raw, pos)
        priv_sec, _ = _read_ssh_str(raw, pos)
        c1, c2 = struct.unpack_from(">II", priv_sec, 0)
        if c1 != c2:
            raise ValueError("Corrupt OpenSSH private key checkints")
        ktype, ppos = _read_ssh_str(priv_sec, 8)
        if ktype != b"ssh-ed25519":
            raise ValueError(f"Expected ssh-ed25519 key, got {ktype!r}")
        _pk, ppos = _read_ssh_str(priv_sec, ppos)
        sk, _ = _read_ssh_str(priv_sec, ppos)
        if len(sk) != 64:
            raise ValueError("Invalid ssh-ed25519 secret key length")
        return sk[:32]

    first_token = text.splitlines()[0].strip()
    if len(first_token) == 64:
        return bytes.fromhex(first_token)
    raise ValueError(f"Unrecognized Ed25519 key format in '{spec}'")


def generate_keypair(prefix_str: str) -> None:
    seed = secrets.token_bytes(32)
    pk = ed25519_pubkey_from_seed(seed)
    key_path = pathlib.Path(f"{prefix_str}.key")
    pub_path = pathlib.Path(f"{prefix_str}.pub")
    key_path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(key_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as fp:
        fp.write(seed.hex() + "\n")
    pub_line = format_openssh_ed25519_pubkey(pk, "epkg@c6-linux")
    pub_path.write_text(pub_line + "\n", encoding="utf-8")
    print(f"[mkepkg] Wrote private seed {key_path} and trusted key {pub_path}")
    print(f"[mkepkg] Add to C6-MicroLinux /etc/epkg/trusted_keys:\n  {pub_line}")


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
    sign_seed: bytes | None = None,
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
    magic = b"EPK2" if sign_seed is not None else b"EPK1"
    fmt_ver = 2 if sign_seed is not None else 1

    header = bytearray()
    header += magic
    header += struct.pack(">HH", fmt_ver, ABI_VER)
    header += pad_bytes(name, 32)
    header += pad_bytes(version, 16)
    header += pad_bytes(arch, 24)
    header += pad_bytes(description, 64)
    header += digest
    header += struct.pack(">H", len(mappings))
    assert len(header) == 178

    if sign_seed is not None:
        sig = ed25519_sign_seed(sign_seed, bytes(header))
        assert len(sig) == 64
        header += sig
        assert len(header) == 242

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(bytes(header + payload))
    kind = "EPK2 Ed25519-signed" if sign_seed is not None else "EPK1 unsigned"
    print(
        f"[mkepkg] Wrote {output} ({len(header) + len(payload)} bytes, "
        f"{kind}, sha256={digest.hex()[:16]}...)"
    )


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Package RV32IMAC ELF binaries, Lua scripts, or shell scripts into an .epkg archive"
    )
    ap.add_argument(
        "--keygen",
        metavar="PREFIX",
        help="Generate an Ed25519 signing keypair (<PREFIX>.key and <PREFIX>.pub for /etc/epkg/trusted_keys)",
    )
    ap.add_argument("-n", "--name", help="Package name")
    ap.add_argument("-v", "--version", default="0.1.0")
    ap.add_argument("-a", "--arch", default="rv32imac", help="rv32imac, riscv32, or noarch")
    ap.add_argument("-d", "--description", default="C6-MicroLinux installable package")
    ap.add_argument(
        "-f",
        "--file",
        action="append",
        help="Mapping in format <local_src_path>:<target_c6os_vpath>[:octal_mode]",
    )
    ap.add_argument(
        "-k",
        "--sign-key",
        help="Ed25519 private key path (OpenSSH unencrypted or 64-hex seed) to emit a signed EPK2 package",
    )
    ap.add_argument("-o", "--output", help="Output .epkg path")
    args = ap.parse_args()

    if args.keygen:
        generate_keypair(args.keygen)
        if not args.name and not args.file and not args.output:
            return 0

    if not args.name or not args.file or not args.output:
        ap.error("--name, --file, and --output are required when building a package")

    sign_seed = load_ed25519_seed(args.sign_key) if args.sign_key else None

    mappings: list[tuple[pathlib.Path, str, int]] = []
    for item in args.file:
        parts = item.split(":")
        if len(parts) < 2:
            raise SystemExit(f"Invalid --file mapping '{item}', expected src:dst[:mode]")
        src = pathlib.Path(parts[0])
        dst = parts[1]
        mode = int(parts[2], 8) if len(parts) >= 3 else 0o755
        mappings.append((src, dst, mode))

    build_epkg(
        args.name,
        args.version,
        args.arch,
        args.description,
        mappings,
        pathlib.Path(args.output),
        sign_seed=sign_seed,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
