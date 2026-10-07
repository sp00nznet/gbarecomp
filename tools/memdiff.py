"""Diff two GBA memory snapshots (from --dump-at in the runtime or
tools/oracle/mgba_oracle) and list the differing ranges per region.

    py -3 tools/memdiff.py oracle.bin recomp.bin [--max 20]

Layout (both tools write it): EWRAM, IWRAM, IO, PAL, VRAM, OAM, raw.
"""
import argparse

REGIONS = [("EWRAM", 0x02000000, 0x40000), ("IWRAM", 0x03000000, 0x8000),
           ("IO", 0x04000000, 0x400), ("PAL", 0x05000000, 0x400),
           ("VRAM", 0x06000000, 0x18000), ("OAM", 0x07000000, 0x400)]


def ranges(a, b, gap=8):
    """Yield (start, end) of differing byte runs, merging runs closer than gap."""
    start = last = None
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            if start is None:
                start = i
            elif i - last > gap:
                yield start, last + 1
                start = i
            last = i
    if start is not None:
        yield start, last + 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a"); ap.add_argument("b")
    ap.add_argument("--max", type=int, default=20, help="ranges shown per region")
    args = ap.parse_args()
    A, B = open(args.a, "rb").read(), open(args.b, "rb").read()
    off = 0
    total = 0
    for name, base, size in REGIONS:
        ra, rb = A[off:off + size], B[off:off + size]
        rs = list(ranges(ra, rb))
        n = sum(e - s for s, e in rs)
        total += n
        print(f"{name:5s} {n:7d} bytes differ in {len(rs)} ranges")
        for s, e in rs[:args.max]:
            print(f"      {base + s:08X}-{base + e:08X}  a={ra[s:min(e, s + 8)].hex(' ')}  b={rb[s:min(e, s + 8)].hex(' ')}")
        off += size
    print(f"total {total} bytes differ")


if __name__ == "__main__":
    main()
