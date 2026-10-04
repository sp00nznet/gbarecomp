"""Conformance harness: lockstep validation of a recompiled GBA build.

Runs the game headless with GBA_VALIDATE (every recompiled function's first K
calls run natively and in the interpreter from one snapshot, then compared)
and reports the pass count against a committed baseline:

    py -3 tools/conformance.py --exe build/Release/AWRE.exe --rom game/aw.gba \
        --baseline conformance_baseline.txt [--frames 760] [--input script.txt]

    conformance: 154/158 functions agree (details: scratch/conformance.log)
    REGRESSION: 150 < baseline 154            -> exit 1
    --update writes the current count as the new baseline.

Without the ROM (CI, a test VM) it prints "conformance: skipped -- ..." and
exits 0, so data-dependent runs report SKIP rather than PASS. The ROM never
enters the repo. See docs/conformance.md.
"""
import argparse
import os
import re
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--baseline", required=True, help="file holding the passing count")
    ap.add_argument("--frames", type=int, default=760)
    ap.add_argument("--input", help="button script (docs/headless.md)")
    ap.add_argument("--calls", type=int, default=3, help="calls validated per function")
    ap.add_argument("--log", default=os.path.join("scratch", "conformance.log"))
    ap.add_argument("--update", action="store_true", help="store the current count as baseline")
    a = ap.parse_args()

    for what, path in (("ROM", a.rom), ("build", a.exe)):
        if not os.path.exists(path):
            print(f"conformance: skipped -- needs the {what} at {path}")
            return 0

    # A save from an earlier run would change the boot path; start blank
    sav = os.path.splitext(a.rom)[0] + ".sav"
    if os.path.exists(sav):
        os.remove(sav)

    # Absolute: CreateProcess doesn't resolve a relative "build/x.exe"
    cmd = [os.path.abspath(a.exe), a.rom, "--headless", "--frames", str(a.frames), "--log-every", "0"]
    if a.input:
        cmd += ["--input", a.input]
    env = dict(os.environ, GBA_VALIDATE=str(a.calls))
    os.makedirs(os.path.dirname(a.log) or ".", exist_ok=True)
    try:
        p = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=1800)
    except subprocess.TimeoutExpired:
        print("conformance: FAIL -- run timed out")
        return 1
    out = p.stdout + p.stderr
    with open(a.log, "w", encoding="utf-8") as f:
        f.write(out)

    m = re.search(r"validate: (\d+)/(\d+) functions agree", out)
    if p.returncode != 0 or not m:
        print(f"conformance: FAIL -- run exited {p.returncode} without a result (see {a.log})")
        return 1
    passed, total = int(m.group(1)), int(m.group(2))
    print(f"conformance: {passed}/{total} functions agree (details: {a.log})")
    for line in out.splitlines():
        if line.startswith("VALIDATE "):
            print("  " + line)

    if a.update:
        with open(a.baseline, "w") as f:
            f.write(f"{passed}\n")
        print(f"baseline set to {passed}")
        return 0
    base = int(open(a.baseline).read().split()[0]) if os.path.exists(a.baseline) else 0
    if passed < base:
        print(f"REGRESSION: {passed} < baseline {base}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
