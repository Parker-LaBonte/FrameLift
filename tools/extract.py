"""Turn downloaded 60 fps gameplay sections into benchmark clips.

For each section, take 1-second clips (61 frames) at a few offsets, scale 4K -> 1080p (FrameLift's
capture size), and save lossless PNGs in clips/<section>_<offset>/0000.png ...
A clip is rejected if any consecutive frames repeat (the game wasn't really rendering at 60, so
there's no true in-between frame) or if it contains a scene cut.

Usage: python extract.py yt clips
"""
import glob
import os
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image

OFFSETS = [3.0, 12.0]
FRAMES = 61


def luma_small(path):
    im = Image.open(path).convert("L").resize((480, 270), Image.BILINEAR)
    return np.asarray(im, dtype=np.float32)


def main(src, dst):
    os.makedirs(dst, exist_ok=True)
    kept = rejected = 0
    for f in sorted(glob.glob(os.path.join(src, "*.webm")) + glob.glob(os.path.join(src, "*.mp4"))):
        base = os.path.splitext(os.path.basename(f))[0]
        for off in OFFSETS:
            out = os.path.join(dst, f"{base}_{int(off):02d}")
            if os.path.isdir(out) and len(glob.glob(out + "/*.png")) == FRAMES:
                kept += 1
                continue
            os.makedirs(out, exist_ok=True)
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-ss", str(off), "-i", f, "-frames:v", str(FRAMES),
                            "-vf", "scale=1920:1080:flags=lanczos", "-pix_fmt", "rgb24", os.path.join(out, "%04d.png")],
                           check=False)
            pngs = sorted(glob.glob(out + "/*.png"))
            # ffmpeg numbers from 0001; renumber from 0000 for the bench
            reason = None
            if len(pngs) < FRAMES:
                reason = f"only {len(pngs)} frames"
            else:
                diffs = []
                prev = None
                for p in pngs:
                    cur = luma_small(p)
                    if prev is not None:
                        diffs.append(float(np.abs(cur - prev).mean()))
                    prev = cur
                d = np.array(diffs)
                dups = int((d < 0.35).sum())
                med = float(np.median(d))
                if dups > 0:
                    reason = f"{dups} repeated frames (not a true 60 fps game here)"
                elif d.max() > max(25.0, 6 * med):
                    reason = f"scene cut (max diff {d.max():.1f} vs median {med:.1f})"
                elif med < 0.8:
                    reason = f"almost no motion (median diff {med:.2f})"
            if reason:
                print(f"reject {os.path.basename(out)}: {reason}")
                shutil.rmtree(out)
                rejected += 1
                continue
            for i, p in enumerate(pngs):
                os.rename(p, os.path.join(out, f"{i:04d}.tmp"))
            for t in sorted(glob.glob(out + "/*.tmp")):
                os.rename(t, t[:-4] + ".png")
            print(f"keep   {os.path.basename(out)}: median motion diff {med:.2f}")
            kept += 1
    print(f"\n{kept} clips kept, {rejected} rejected")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
