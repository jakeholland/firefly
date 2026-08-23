#!/usr/bin/env python3
"""Render a Firefly "scene" to social video, using the real firmware renderer.

A scene is a list of app-state fixtures. Each is rendered headlessly by ffsim
(the same code path as the golden screenshots — so what you post is literally
what the device draws), then stitched with ffmpeg.

Usage (from firmware/):
    python3 tools/social/render_scene.py --scene find_dana --out /tmp/clip
    python3 tools/social/render_scene.py --list

Outputs into --out: <scene>_9x16.mp4 (TikTok/Reels), <scene>.gif (README/posts),
<scene>_square.mp4 (Instagram feed), plus frames/ for stills.

Adding a scene: write a function returning a list of radar-state dicts and
register it in SCENES. Keep them short — 6-10s reads best on TikTok.
"""
import argparse, json, math, pathlib, shutil, subprocess, sys

FPS = 20
CREW = [(42.0, "D", 0, False), (128.0, "R", 1, False),
        (205.0, "M", 2, True), (301.0, "J", 3, False)]


def _fmt_m(m):
    return f"{int(round(m))} m" if m >= 1 else "~1 m"


def _frame(mode, arrow, dist_str, age, *, trend=0, clock="1:12", batt=78,
           name="DANA", dot_shift=0.0, mesh=True, arrow_valid=None):
    return {
        "face": "radar",
        "radar": {
            "mode": mode,
            "arrow_deg": round(arrow % 360, 2),
            "arrow_valid": (mode not in ("close", "nofix", "nosel")
                            if arrow_valid is None else arrow_valid),
            "name": name, "dist_str": dist_str, "age_str": age, "trend": trend,
            "clock_str": clock, "batt_pct": batt, "mesh_ok": mesh,
            "dots": [{"ring_deg": round((d[0] + dot_shift) % 360, 2),
                      "initial": d[1], "color_idx": d[2], "stale": d[3]}
                     for d in CREW],
        },
    }


def scene_find_dana():
    """You're facing the wrong way -> arrow locks on -> walk in -> close-range handoff."""
    out = []
    n = int(2.0 * FPS)                                    # turn toward her
    for i in range(n):
        e = 1 - (1 - i / (n - 1)) ** 3
        out.append(_frame("live", -132 + e * 132, "320 m", "8 SEC", dot_shift=-132 + e * 132))
    n = int(3.0 * FPS)                                    # walk it down
    for i in range(n):
        t = i / (n - 1)
        d = max(320 * (1 - t) ** 1.35 + 18 * (1 - t), 16)
        out.append(_frame("live", math.sin(t * 11) * 6 * (1 - t * .4), _fmt_m(d), "3 SEC"))
    n = int(1.6 * FPS)                                    # GPS gives up, radio takes over
    for i in range(n):
        t = i / (n - 1)
        out.append(_frame("close", 0, _fmt_m(max(16 - t * 9, 6)), "2 SEC", trend=1))
    return out


def scene_honest_arrow():
    """The trust story: a fix goes fresh -> stale -> lost, and the UI admits it."""
    out = []
    for i in range(int(2.0 * FPS)):                       # fresh and confident
        out.append(_frame("live", math.sin(i / 7) * 4, "320 m", "8 SEC"))
    for i in range(int(2.5 * FPS)):                       # data ages out
        out.append(_frame("stale", math.sin(i / 9) * 3, "320 m", "4 MIN"))
    for i in range(int(2.5 * FPS)):                       # stop trusting it
        out.append(_frame("lost", 0, "~1.1 km", "42 MIN"))
    return out



def scene_three_faces():
    """The device you can hold today: radar locks on, set times, a flare arrives."""
    import json as _json
    fx = pathlib.Path("tests/fixtures")

    def load(name):
        return _json.loads((fx / f"{name}.json").read_text())

    out = []
    # Radar: arrow swings around and locks on.
    base = load("radar_live")
    n = int(2.2 * FPS)
    for i in range(n):
        e = 1 - (1 - i / (n - 1)) ** 3
        f = _json.loads(_json.dumps(base))
        f["radar"]["arrow_deg"] = round((-120 + e * 162) % 360, 2)
        for d in f["radar"]["dots"]:
            d["ring_deg"] = round((d["ring_deg"] - 120 + e * 120) % 360, 2)
        out.append(f)
    for _ in range(int(0.8 * FPS)):
        out.append(_json.loads(_json.dumps(base)))
    # Now: the honest TBD lineup, then the live schedule.
    for _ in range(int(2.0 * FPS)):
        out.append(load("now_tbd"))
    for _ in range(int(2.0 * FPS)):
        out.append(load("now_live"))
    # Flare: someone needs finding.
    for _ in range(int(2.4 * FPS)):
        out.append(load("flare_takeover_locked"))
    return out


SCENES = {"find_dana": scene_find_dana, "honest_arrow": scene_honest_arrow,
          "three_faces": scene_three_faces}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", default="find_dana")
    ap.add_argument("--out", default="/tmp/firefly_clip")
    ap.add_argument("--ffsim", default="build/ffsim")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--formats", default="tiktok",
                    help="comma-separated: tiktok (9:16, default), gif, square")
    a = ap.parse_args()
    if a.list:
        print("\n".join(f"{k}: {v.__doc__}" for k, v in SCENES.items()))
        return 0
    if a.scene not in SCENES:
        sys.exit(f"unknown scene {a.scene!r}; try --list")
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg not found (brew install ffmpeg)")
    if not pathlib.Path(a.ffsim).exists():
        sys.exit(f"{a.ffsim} not found — build first: cmake -S firmware -B build "
                 "-DFF_TARGET=sim && cmake --build build")

    out = pathlib.Path(a.out)
    fx, png = out / "fixtures", out / "frames"
    for d in (fx, png):
        shutil.rmtree(d, ignore_errors=True)
        d.mkdir(parents=True)

    frames = SCENES[a.scene]()
    for i, f in enumerate(frames):
        f = dict(f, fixture=f"{a.scene}_{i:04d}")
        (fx / f"{f['fixture']}.json").write_text(json.dumps(f, indent=2))
        subprocess.run([a.ffsim, "--headless", "--mock-clock", "--fixture",
                        str(fx / f"{f['fixture']}.json"), "--screenshot", str(png)],
                       check=True, stdout=subprocess.DEVNULL)
    print(f"rendered {len(frames)} frames ({len(frames)/FPS:.1f}s)")

    glob = str(png / f"{a.scene}_*.png")
    # TikTok/Reels: 1080x1920. The puck is placed ABOVE centre (y=380) so it clears
    # the platform's bottom caption/username overlay, which eats roughly the lower
    # quarter of the frame — a vertically centred puck gets its distance readout
    # covered by the caption on a real phone.
    formats = {
        "tiktok": (["-vf", "scale=912:912:flags=lanczos,pad=1080:1920:84:380:color=0x050507",
                    "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18",
                    "-movflags", "+faststart"], f"{a.scene}_9x16.mp4"),
        "gif": (["-vf", "scale=456:456:flags=lanczos,split[x][y];[x]palettegen=stats_mode=diff[p];"
                 "[y][p]paletteuse=dither=bayer:bayer_scale=3", "-loop", "0"], f"{a.scene}.gif"),
        "square": (["-vf", "scale=912:912:flags=lanczos", "-c:v", "libx264",
                    "-pix_fmt", "yuv420p", "-crf", "18"], f"{a.scene}_square.mp4"),
    }
    want = [f.strip() for f in a.formats.split(",") if f.strip()]
    unknown = [f for f in want if f not in formats]
    if unknown:
        sys.exit(f"unknown format(s): {', '.join(unknown)}; choose from {', '.join(formats)}")
    for key in want:
        args, name = formats[key]
        subprocess.run(["ffmpeg", "-y", "-framerate", str(FPS), "-pattern_type", "glob",
                        "-i", glob, *args, str(out / name), "-loglevel", "error"], check=True)
        print(f"  {out / name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
