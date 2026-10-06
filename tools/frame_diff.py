"""PNG evidence for a changed simulator frame: expected | actual | difference."""
import os
import hashlib
from pathlib import Path
import struct
import subprocess
import tarfile
import tempfile
import zlib

WIDTH = HEIGHT = 480
FRAME_BYTES = WIDTH * HEIGHT * 3


def write_png(path, width, height, rgb):
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))
    rows = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


def side_by_side(panels):
    rows = []
    for y in range(HEIGHT):
        rows.append(b"".join(panel[y * WIDTH * 3:(y + 1) * WIDTH * 3] for panel in panels))
    return b"".join(rows)


def difference(expected, actual):
    return bytes(min(255, abs(a - b) * 4) for a, b in zip(expected, actual))


def frame_at(exe, env, command, index):
    process = subprocess.Popen([exe, *command], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env)
    try:
        for _ in range(index):
            process.stdout.read(FRAME_BYTES)
        return process.stdout.read(FRAME_BYTES)
    finally:
        process.kill()
        process.wait()


def committed_simulator(root, sources, temp):
    """Build the simulator from HEAD; None if HEAD cannot produce this scenario."""
    archive = subprocess.run(["git", "-C", str(root), "archive", "HEAD", "firmware/main", "firmware/sim"],
                             capture_output=True, check=True).stdout
    Path(temp, "head.tar").write_bytes(archive)
    with tarfile.open(Path(temp, "head.tar")) as tar:
        tar.extractall(Path(temp, "head"))
    base = Path(temp, "head/firmware")
    (base / "managed_components").symlink_to(root / "firmware/managed_components")
    exe = str(Path(temp, "head-sim"))
    files = [str(base / (source + ".c")) for source in sources if (base / (source + ".c")).exists()]
    build = subprocess.run([os.environ.get("CC", "cc"), "-O2", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-w", "-o", exe, *files,
                            "-I" + str(base / "managed_components/espressif__qrcode"), "-lm"], capture_output=True)
    return exe if build.returncode == 0 else None


def report(root, sources, exe, env, command, index, label, expected_hash):
    """Write a PNG (expected | actual | difference, or actual alone) and return its path."""
    actual = frame_at(exe, env, command, index)
    if len(actual) != FRAME_BYTES:
        raise ValueError('Incomplete actual frame')
    out = Path(tempfile.gettempdir()) / f"kubik-frame-diff-{label}-{index}.png"
    with tempfile.TemporaryDirectory(prefix="kubik-head-") as temp:
        head = committed_simulator(root, sources, temp)
        expected = frame_at(head, env, command, index) if head else b""
    if len(expected) == FRAME_BYTES and hashlib.blake2b(expected, digest_size=16).hexdigest() == expected_hash:
        write_png(out, WIDTH * 3, HEIGHT, side_by_side([expected, actual, difference(expected, actual)]))
    else:
        print(f'{label}: HEAD is not the reviewed reference; PNG shows the actual frame only')
        write_png(out, WIDTH, HEIGHT, actual)
    return out
