#!/usr/bin/env bash
#
# get_infinite.sh — fetch everything needed to run the emulator.
#
# Downloads, into ./roms/, the three files a fresh clone needs:
#   roms/MacPlus.ROM            128 KB  Macintosh Plus ROM
#   roms/disks/System6.dsk       10 MB  bootable System 6.0 hard disk
#   roms/disks/InfiniteHD6.dsk    1 GB  the "Infinite HD" software disk
#
# All three come from the Infinite Mac project (https://infinitemac.org,
# github.com/mihaip/infinite-mac): the ROM and System disk are plain
# files in that repo; the Infinite HD is reassembled from the 256 KB
# content-addressed chunks infinitemac.org serves to its in-browser
# emulator. ~580 MB is downloaded in total.
#
# The script is idempotent — files already present are left alone, so it
# is safe to re-run. Once it finishes:
#
#   cmake -B build && cmake --build build
#   ./build/mac68k_host --server --rom --ram-mb 4 \
#       --disk roms/disks/System6.dsk roms/MacPlus.ROM
#
# (the host auto-mounts InfiniteHD6.dsk sitting next to the boot disk).
#
set -euo pipefail
cd "$(dirname "$0")/.."                       # repo root
mkdir -p roms/disks

ROM="roms/MacPlus.ROM"
SYS="roms/disks/System6.dsk"
HD="roms/disks/InfiniteHD6.dsk"
RAW="https://raw.githubusercontent.com/mihaip/infinite-mac/main"

command -v curl   >/dev/null || { echo "error: curl is required"   >&2; exit 1; }
command -v python3>/dev/null || { echo "error: python3 is required">&2; exit 1; }

# --- 1. Macintosh Plus ROM (128 KB) --------------------------------------
if [ -f "$ROM" ]; then
    echo "* $ROM already present — skipping"
else
    echo "* Downloading Macintosh Plus ROM ..."
    curl -fL --progress-bar -o "$ROM.tmp" "$RAW/src/Data/Mac-Plus.rom"
    mv "$ROM.tmp" "$ROM"
fi

# --- 2. System 6.0 boot disk (10 MB, a bootable HFS hard-disk image) -----
if [ -f "$SYS" ]; then
    echo "* $SYS already present — skipping"
else
    echo "* Downloading System 6.0 boot disk ..."
    curl -fL --progress-bar -o "$SYS.tmp" "$RAW/Images/System%206.0%20HD.dsk"
    mv "$SYS.tmp" "$SYS"
fi

# --- 3. Infinite HD (1 GB) — reassembled from the chunk CDN --------------
if [ -f "$HD" ]; then
    echo "* $HD already present — skipping"
else
    echo "* Fetching the Infinite HD (~571 MB download, 1 GB image) ..."
    python3 - "$HD" <<'PY'
import os, re, sys, time, tempfile, shutil, hashlib, urllib.request
from concurrent.futures import ThreadPoolExecutor

out  = sys.argv[1]
SITE = "https://infinitemac.org"

def fetch(url, binary=False):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    data = urllib.request.urlopen(req, timeout=60).read()
    return data if binary else data.decode("utf-8", "replace")

# Locate the Infinite HD6 manifest: index.html -> main-*.js -> the JS
# chunks it references -> the "Infinite HD6.dsk-*.js" module.
print("  locating Infinite HD6 manifest ...", flush=True)
index = fetch(SITE + "/")
main = re.search(r'/assets/main-[A-Za-z0-9_]+\.js', index)
if not main:
    sys.exit("could not find main bundle on infinitemac.org")
seen, manifest_url = set(), None
queue = [SITE + main.group(0)]
while queue and not manifest_url:
    url = queue.pop(0)
    if url in seen:
        continue
    seen.add(url)
    js = fetch(url)
    m = re.search(r'Infinite HD6\.dsk-[A-Za-z0-9_]+\.js', js)
    if m:
        manifest_url = SITE + "/assets/" + m.group(0).replace(" ", "%20")
        break
    for ref in re.findall(r'assets/[A-Za-z0-9 ._%-]+\.js', js):
        queue.append(SITE + "/" + ref.replace(" ", "%20"))
if not manifest_url:
    sys.exit("could not find the Infinite HD6 manifest")

# The manifest module exports a `.`-joined string of per-chunk hashes
# ("" = an all-zero chunk). Chunk size is infinitemac's fixed 256 KB;
# every downloaded chunk is checked against it below.
mjs    = fetch(manifest_url)
chunks = re.search(r'`([0-9a-f.]*)`\.split', mjs).group(1).split(".")
csize  = 262144
uniq   = sorted({c for c in chunks if c})
if not uniq:
    sys.exit("manifest had no chunks — infinitemac.org format changed?")
print(f"  {len(chunks)} chunks ({len(chunks)*csize//(1024*1024)} MB image), "
      f"{len(uniq)} to download", flush=True)

tmp = tempfile.mkdtemp(prefix="infhd-", dir=os.path.dirname(out) or ".")
try:
    def get(h):
        p = os.path.join(tmp, h)
        for attempt in range(5):
            try:
                d = fetch(f"{SITE}/Disk/{h}.chunk", binary=True)
                if len(d) != csize:
                    raise ValueError(f"size {len(d)}")
                if hashlib.blake2b(d, digest_size=16,
                                   salt=b"raw").hexdigest() != h:
                    raise ValueError("hash mismatch")
                open(p, "wb").write(d)
                return
            except Exception as e:
                if attempt == 4:
                    raise SystemExit(f"failed to fetch chunk {h}: {e}")
                time.sleep(1 + attempt)

    t0, done = time.time(), 0
    with ThreadPoolExecutor(max_workers=32) as ex:
        for _ in ex.map(get, uniq):
            done += 1
            if done % 250 == 0 or done == len(uniq):
                print(f"  downloaded {done}/{len(uniq)} chunks "
                      f"({time.time()-t0:.0f}s)", flush=True)

    print("  assembling image ...", flush=True)
    zero = b"\0" * csize
    with open(out + ".tmp", "wb") as f:
        for c in chunks:
            f.write(zero if c == "" else open(os.path.join(tmp, c), "rb").read())
    os.rename(out + ".tmp", out)
    print(f"  wrote {out} ({os.path.getsize(out)} bytes)", flush=True)
finally:
    shutil.rmtree(tmp, ignore_errors=True)
PY
fi

echo
echo "All set. Build and run:"
echo "  cmake -B build && cmake --build build"
echo "  ./build/mac68k_host --server --rom --ram-mb 4 \\"
echo "      --disk $SYS $ROM"
