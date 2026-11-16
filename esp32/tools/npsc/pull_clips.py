#!/usr/bin/env python3
"""Pull "hey santa" clips from a gadget USB-serial log and write WAV files.

On the gadget, after a recording session (clips stay in PSRAM until reboot):

    >clips

Then feed the serial capture (or a saved log) to this script:

    python3 pull_clips.py --log serial.log --out ./clips
    python3 pull_clips.py --port /dev/ttyACM0 --out ./clips   # sends >clips

These are children's voices. Keep the WAV files private. Do not upload them
or put them in a public repo.
"""
from __future__ import annotations

import argparse
import base64
import re
import sys
import time
from pathlib import Path


BEGIN = re.compile(r"@clips begin count=(\d+)")
CLIP = re.compile(r"@clip (\d+) frames=(\d+) ms=(\d+) bytes=(\d+)")
CLIP_END = re.compile(r"@clip (\d+) end")
DONE = re.compile(r"@clips done count=(\d+)")


def extract(text: str, out_dir: Path) -> list[Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    lines = text.splitlines()
    i = 0
    current = None
    chunks: list[str] = []
    while i < len(lines):
        line = lines[i].rstrip("\r")
        # ESP_LOG prefixes may sit in front of a marker; also accept a bare line.
        bare = line[line.find("@") :] if "@" in line else line
        m = CLIP.match(bare)
        if m:
            if current is not None:
                raise SystemExit(f"clip {current} never ended")
            current = int(m.group(1))
            chunks = []
            i += 1
            continue
        m = CLIP_END.match(bare)
        if m:
            if current != int(m.group(1)):
                raise SystemExit(f"end of clip {m.group(1)} while in {current}")
            raw = base64.b64decode("".join(chunks), validate=False)
            if len(raw) < 44 or raw[:4] != b"RIFF":
                raise SystemExit(f"clip {current}: not a WAV ({len(raw)} bytes)")
            path = out_dir / f"hey-santa-{current:02d}.wav"
            path.write_bytes(raw)
            written.append(path)
            current = None
            chunks = []
            i += 1
            continue
        if current is not None and bare and not bare.startswith("@"):
            chunks.append(bare.strip())
        i += 1
    if current is not None:
        raise SystemExit(f"clip {current} never ended")
    return written


def from_port(port: str, out_dir: Path, baud: int = 115200) -> list[Path]:
    try:
        import serial  # type: ignore
    except ImportError as e:
        raise SystemExit("pyserial is required for --port (pip install pyserial)") from e
    ser = serial.Serial(port, baud, timeout=1)
    try:
        ser.write(b">clips\n")
        ser.flush()
        buf = io_collect(ser, seconds=30)
    finally:
        ser.close()
    Path(out_dir / "serial-capture.log").write_text(buf)
    return extract(buf, out_dir)


def io_collect(ser, seconds: float) -> str:
    deadline = time.time() + seconds
    chunks: list[bytes] = []
    got_done = False
    idle_since = time.time()
    while time.time() < deadline:
        n = ser.in_waiting
        data = ser.read(n or 1)
        if data:
            chunks.append(data)
            idle_since = time.time()
            if b"@clips done" in b"".join(chunks[-8:]):
                got_done = True
        elif got_done and time.time() - idle_since > 0.5:
            break
    return b"".join(chunks).decode("utf-8", "replace")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--log", type=Path, help="serial log already containing a >clips dump")
    ap.add_argument("--port", help="USB serial device; sends >clips and reads the dump")
    ap.add_argument("--out", type=Path, default=Path("hey-santa-clips"))
    args = ap.parse_args()
    if not args.log and not args.port:
        ap.error("pass --log FILE or --port DEVICE")
    if args.port:
        paths = from_port(args.port, args.out)
    else:
        paths = extract(args.log.read_text(errors="replace"), args.out)
    if not paths:
        print("no clips found", file=sys.stderr)
        return 1
    for p in paths:
        print(p, p.stat().st_size, "bytes")
    print(f"wrote {len(paths)} wav(s) under {args.out} (keep private)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
