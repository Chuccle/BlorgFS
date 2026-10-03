#!/usr/bin/env python3
"""Writes the deterministic file tree server-rs serves to the driver during a
guest run, plus corpus-manifest.json describing it.

    make-corpus.py OUT_DIR [--extra NAME=DIR ...]

OUT_DIR/corpus is the served root; OUT_DIR/corpus-manifest.json lists every
file (Windows-style relative path, size, SHA-256) and every directory, which
the in-guest listing check compares the mounted volume against.

Generated on the host, so the same tree serves the Linux server on the host
and, pushed into the guest, the Windows server's fallback mode -- one
generator, so the two can't drift. The same seed always produces the same
bytes, so a failure on one run replays byte for byte on the next.

The shape is chosen for what tends to break a network filesystem:
  boundaries/  sizes either side of sector, page, 64 KiB, the read-ahead
               granule (128 KiB) and ceiling (2 MiB)
  large/       one file past Test-BlorgCorrectness's whole-file hash limit
  many/        enough entries that a listing spans more than one buffer
  names/       spaces, brackets, dots, no extension, non-ASCII, a long name
  nested/      a deep path
  empty/       an empty directory and a zero-byte file

--extra NAME=DIR copies a suite's fixture directory in as NAME/ (see the suite
contract in in-guest/Invoke-GuestTests.ps1), so fixtures are served by the
same server and appear in the manifest like everything else.
"""

import argparse
import hashlib
import json
import os
import random
import shutil
import sys

KIB = 1024
MIB = 1024 * KIB
SEED = 20261003


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--extra", action="append", default=[], metavar="NAME=DIR")
    args = ap.parse_args()

    root = os.path.join(args.out, "corpus")
    if os.path.exists(root):
        shutil.rmtree(root)
    os.makedirs(root)
    rng = random.Random(SEED)
    files = []

    def add(rel, size):
        full = os.path.join(root, *rel.split("/"))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        h = hashlib.sha256()
        with open(full, "wb") as f:
            left = size
            while left:
                n = min(left, MIB)
                chunk = rng.randbytes(n)
                f.write(chunk)
                h.update(chunk)
                left -= n
        files.append({"path": rel.replace("/", "\\"), "size": size, "sha256": h.hexdigest()})

    for s in (1, 511, 512, 513, 4095, 4096, 4097, 65535, 65536, 65537,
              128 * KIB - 1, 128 * KIB, 128 * KIB + 1,
              MIB - 1, MIB + 1, 2 * MIB - 1, 2 * MIB + 1, 2 * MIB + 4097, 8 * MIB + 3):
        add(f"boundaries/size-{s:09d}.bin", s)
    add("large/past-hash-limit.bin", 40 * MIB + 5)
    for i in range(300):
        add(f"many/entry-{i:04d}.dat", (i * 37) % 3000 + 1)
    for name in ("with spaces.bin",
                 "[bracketed] (parens) {braces}.bin",
                 "many.dots.in.the.name.bin",
                 "no-extension",
                 "café über 日本.bin",
                 "long" * 50 + ".bin"):
        add(f"names/{name}", 4099)
    add("nested/a/b/c/d/e/f/g/deep.bin", 70001)
    os.makedirs(os.path.join(root, "empty", "nothing-here"))
    add("empty/zero-bytes.bin", 0)

    for spec in args.extra:
        name, _, src = spec.partition("=")
        if not name or not os.path.isdir(src):
            sys.exit(f"make-corpus: bad --extra '{spec}' (want NAME=DIR)")
        for dirpath, _, names in os.walk(src):
            for n in sorted(names):
                full = os.path.join(dirpath, n)
                rel = name + "/" + os.path.relpath(full, src).replace(os.sep, "/")
                dest = os.path.join(root, *rel.split("/"))
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                shutil.copyfile(full, dest)
                with open(dest, "rb") as f:
                    digest = hashlib.sha256(f.read()).hexdigest()
                files.append({"path": rel.replace("/", "\\"), "size": os.path.getsize(dest), "sha256": digest})

    dirs = sorted(os.path.relpath(d, root).replace(os.sep, "\\")
                  for d, _, _ in os.walk(root) if d != root)
    with open(os.path.join(args.out, "corpus-manifest.json"), "w", encoding="utf-8") as f:
        json.dump({"seed": SEED, "files": files, "directories": dirs}, f, ensure_ascii=False, indent=1)
    total = sum(x["size"] for x in files)
    print(f"Corpus: {len(files)} files, {len(dirs)} directories, {total / MIB:.1f} MiB in {root}")


if __name__ == "__main__":
    main()
