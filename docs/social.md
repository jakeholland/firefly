# Build-log video at milestones

**Standing policy:** every logical milestone produces a short **9:16 vertical clip for TikTok** (1080×1920), rendered from the real firmware — not a mockup. The pipeline is `firmware/tools/social/render_scene.py` (renders app-state fixtures through `ffsim`, stitches with ffmpeg).

**9:16 is the default and the only output unless another is asked for.** The puck sits above centre in the frame so the platform's caption/username overlay — which covers roughly the bottom quarter on a real phone — never lands on the distance readout. Other cuts are opt-in: `--formats gif` (README/embed material) and `--formats square` (Instagram feed); combine with commas.

## What counts as a milestone

| Trigger | Scene to render | The hook |
|---|---|---|
| A **face** first renders (Radar, Now, Signals, Map) | that face in motion | "this screen didn't exist yesterday" |
| A **capability works end-to-end** (message delivered over mesh, position from a real node, flare received) | the state change it causes | "no service. still works." |
| **Honest-state work lands** (stale/lost, no-fix, delivery failure) | `honest_arrow` | "most apps lie when they don't know. this one admits it." |
| **Hardware bring-up** (first boot, first arrow on glass, enclosure fit) | filmed on the bench, cut against the same sim scene | side-by-side: render vs. real |
| **Field test** (Lost Lands) | real footage + the numbers | the field report |
| **Release** (kits, v1.0) | best-of montage | the drop |

Not every merge is a milestone. A refactor, a test-coverage PR, or a spec amendment is not a post.

## Caption formula

1. **Line 1 is the hook and it is concrete** — what the thing does, in the viewer's words, no jargon. ("it points at your friends. no service, no wifi, no app.")
2. **Line 2 is the proof** — what they're looking at. ("that's the actual firmware, not a mockup.")
3. **Line 3 is the thread** — where it's going, or the ask. ("first field test: Lost Lands. following along?")
4. Tags: `#lostlands #festivaltok #edm #dubstep #ravetok #diytech #opensource #buildinpublic` + whatever the scene earns.

First 1.5 seconds decide everything: lead with the moving screen, never with a title card.

## Rendering a clip

```bash
cd firmware
cmake -S . -B build -DFF_TARGET=sim && cmake --build build -j8
python3 tools/social/render_scene.py --list
python3 tools/social/render_scene.py --scene find_dana --out /tmp/clip   # add --ffsim ../build/ffsim if you built from the repo root
```

Adding a scene: write a function returning app-state frames, register it in `SCENES`. Keep clips 6–10 s — long enough to tell one story, short enough to loop.

## Keep the README current

The repo's front page is the other half of this job — it is where everyone who clicks through from a video lands.

**At every milestone, alongside the clip:**
- Refresh `docs/screens/radar-find.gif` (or add a new hero animation if a better story exists) so the top of the README shows the current firmware, not an old build.
- Update the **Status** table: move rows from *in review* to *on `main`*, add newly-started work, and keep the hardware row honest.
- When a new face lands on `main`, add its screenshots to the README's screenshot section — but only once it is actually merged. The README must never show a screen that doesn't exist yet; same honesty rule as below, applied to the shopfront.

Screens live in `docs/screens/` (committed PNG/GIF, rendered by `ffsim`). Keep the total under a couple of megabytes so clones stay light — replace stale images rather than accumulating them.

## Honesty rule

The same rule the device follows applies to its marketing: **never show a state the firmware can't actually produce.** If a scene needs a capability that isn't built yet, either build it or don't post it. Renders are labeled as renders until hardware exists; once it does, prefer real footage.
