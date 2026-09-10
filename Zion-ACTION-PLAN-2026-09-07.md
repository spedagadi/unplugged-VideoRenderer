# Zion ML — Phase-2 Action Plan (2026-09-07)

> Priority-ordered implementation plan. All work is in `VideoRenderer` (MPCVR-Zion)
> unless marked **[shell]** (mpc-hc `s3_shell.cpp` / engine).
> Guiding contract: **runtime is metadata-free** (models eat renderer-computed
> features; HDR10/SDR/VP identical); DV P5 intent (150K-scene corpus) is used
> **offline at training time only**. See `Zion-ML-Integration-Plan.md` §Phase 2.

---

## P0 — Black bars (projector haze) — start here

**Goal.** Letterbox bars get lifted by the grade and look faded on a projector.
Grade the full frame as today, then **repaint the bars to black post-grade**.

**Approach (SAT).** The libplacebo-port extractor already computes a summed-area
table for the 3×3/5×5 zone features. Reuse it for a **horizontal pass**:
- per-row max from SAT row-queries (O(rows) per frame);
- top/bottom runs of rows whose rowMax < ε (ε in source-PQ units ≈ 0.05–1.0% of
  peak, tunable; covers both near-zero bars and slightly-lifted black);
- temporal median over ~1–3 s so bar bounds don't flicker on scene changes or a
  one-off bright bar region.

**Repaint.** Post-grade mask pass (after the ML chain, before encode): fill
`[0,barTop)` and `[barBottom,h)` with display black (constant from P4/P5), with a
2–4 px feather on the seam to avoid a visible banding line. Bars are excluded
from model features by removing their rows from the SAT/feature accumulation,
BUT only via the histogram/zone masking — model contract must stay consistent
with training (default: keep features as trained full-frame; repaint only).

**Linked feature slots.** `[85]/[86] top/bottom bar norm` already visible to
training — keep, repurpose as one mask source during inference.

**Files**
- `Source/ml_features.h/.cpp` — expose SAT row-max + bar-bounds helper;
- `Source/DX11VideoProcessor.*`, `ps_ml_*.hlsl` (+ compile.cmd, rc2 ID) — mask
  pass + cbuffer fields `uBarTop/uBarBottom/uBarFeather/uBarBlack`;
- stats line: `bars: top=N bot=M`.

**Tasks (ordered).**
1. SAT row-max helper + epsilon detection in the extractor; unit-check vs known
   bars (letterboxed 2.35:1 + 4:3-pillarbox samples).
2. Temporal median of bounds; expose in stats for tuning.
3. Mask repaint pass (post-grade) with feather; cbuffer wiring.
4. Demo default ON; optional shell row later.
5. HTPC projector validation (below).

**Validation.** Letterboxed movie on the projector: bars indistinguishable from
hardware black, no seam, stable across scene cuts, subtitles-in-bars unaffected.

**Estimate.** 2–3 days.

---

## P1 — Strength modulation

**Goal.** Single user dial (0→1→2, **default 1.0 = identity** post-inference)
scaling gamma / shadow / highlight. Post-inference, memoryless — models and
temporal filtering (IIR/scene-cut) untouched; strength applies after inference,
before `params` packing/publication.

**Math**
- `k(s) = tanh(s^α) / tanh(1)` (pass-through at s=1, self-limiting, no blowout).
- `α_mid ≈ 1.0` (anchor/linear), `α_shd ≈ 0.65` (sublinear — shadows need push
  but crush fast), `α_hlt ≈ 0.45` (most conservative — rolloff over-drive kills
  specular detail).
- Gamma treated as curve exponent (multiplicative semantics), hat(mid)/shadow/
  rolloff as corrections. **Highlight knee must be derived from modulated
  gamma** (decouple the current gamma→knee coupling at params pack).

**Surface.** **[shell]** System → ZION row `ML Strength` slider 0…2 default 1.0
→ engine `MlSettings` → renderer settings `fMLStrength` (demo default 1.0,
no persistence). `1.0` keeps bytes identical to today (identity).

**Files**
- `Source/IVideoRenderer.h`, `VideoRenderer.cpp` — setting + read;
- `Source/DX11VideoProcessor.cpp` `SetMLParams` — modulation + knee decouple;
- cbuffer (params) if new intermediate fields are needed;
- **[shell]** `s3_shell.cpp` row + `MpcPlayer.{h,cpp}` bridge.

**Tasks (ordered).**
1. `tanh` strength helper + three α constants; apply to gamma/shadow/rolloff.
2. Decouple highlight knee to modulated gamma.
3. Shell row + engine bridge; default 1.0.
4. Regression: at 1.0, output identical to today's build (A/B, not just eyeball).
5. Validate step linearity + no clipping at 1.8 on the clip set.

**Estimate.** 2–3 days (incl. shell row).

---

## P2 — PQ micro-contrast (madVR-style crispness)

**Goal.** CR runs in log-space today; do local contrast in **PQ** to be
perceptually uniform, targeting the 4–16 px mid/acuity band — the
"crispness = brightness" cue. Land a **single mid-band DoG (difference of
gaussians)** first; escalate to a full 3-band Laplacian pyramid only if needed.

**Approach**
- At the SDR-range linear stage (after tone-map, **before** final BT.1886
  encode): `pq = linear_to_pq(image)`; `band = blurσ2(pq) − blurσ4(pq)`;
  `pq' = pq + band · crScale · (ML crStr)`;
  back to linear, continue to encode. Two separable passes for the pair of
  blurs (reuse intermediate RT). Leaves the fine (noise/grain) band alone.
- Strength label comes from the existing `crStr` prediction (scale factory kept
  inside this pass). Later, the local-contrast model (P3) supplies the strength.
- Once micro-contrast lands, **radiance / absolute lift can back off** — track
  as a re-tuning item (brightness from contrast, not mean-lift inflation).

**Files**
- new `ps_ml_dog.hlsl` (+ `Shaders` vcxproj, `compile.cmd`, rc2 ID);
- intermediate RT alloc + two-blur sequence in `GetPostScaleSteps`/`Process`;
- cbuffer: `uMicroContrastStrength/uMicroScale` + PQ helpers (already in-tree for
  sibling shaders).

**Tasks (ordered).**
1. PQ round-trip pass correctness on flat + noisy patches (no shift).
2. σ2/σ4 separable blurs wired; band math; crStr scale.
3. A/B vs current log-space CR (shadow crispness, highlight cleanliness).
4. Optional: 3-band pyramid port (only if banding/insufficient range observed).

**Validation.** HTPC: shadows get acuity without noise pump; specular detail
intact; ≤2-extra-pass GPU budget.

**Estimate.** 3–4 days.

---

## P3 — Feature mapping: dehaze, surround, HK, local-contrast (models)

**Goal.** New computed features + new models, trained offline on the DV P5
corpus (150K scenes) with libplacebo-synthetic fallback. Runtime untouched by
metadata. **Order: dehaze → surround → HK → local-contrast** (each adds features).

**Feature-extractor extensions (`ml_features.*`, contract 88 → ~96, versioned):**
1. **Low-shadow mass**: histogram mass below 5% of peak (+ p0.5/p1 percentiles)
   → ink-blacks (dehaze);
2. **Chroma-stats pass** (frame saturation mean / percentiles / histogram in
   CbCr or ICtCp) → HK chroma boost;
3. **Windowed-luma STD summary** (frame-level, 2 scales) → local-contrast
   strength label.

**Training pipeline (offline; per plan §Phase 2):**
- **RPU/P5 label extractor** (training harness only) → per-scene labels
  (shadow-floor offset, gamma/dcurve, rolloff) for the ~150K DV scenes;
- **dual-render harness** (naive vs libplacebo reference) for non-DV coverage;
- train `dehaze` (shadow floor + contrast gain, two-headed or two models),
  `surround` (multiplicative midtone lift), `HK chroma`, `local-contrast`;
- **validate existing gamma/shadow/highlight models against DV-intent** labels.

**Files**
- `Source/ml_features.h/.cpp` (+ chroma readback texture in DX11VideoProcessor);
- `Training/` harness (new; can lean on `C:\Code\unplugged` tooling);
- model assets (next to `.ax`, like today's `.plxgb`).

**Tasks (ordered).**
1. Low-shadow + percentile extension; sanity vs DV-intent P5 near-black.
2. Dehaze model train + ship (most-visible pain).
3. Surround model (features only — no new extraction needed).
4. Chroma-stats pass; HK model.
5. Local-contrast model (picks up P2's strength).
6. DV-intent validation run for the existing 3 models.

**Estimate.** Feature work ~1 week; training/tooling ~1 week; HK adds a GPU pass.

---

## P4 — Display model

**Goal.** Grade targets the actual display, not a generic 100-nit SDR: **peak**
already exists as a knob (`sdrDisplayNits`); verify it genuinely drives
tone-curve construction (no hardcoded 100 anywhere) and add the **black floor**
(the missing reclaim: content-black maps to `displayBlackNits`, reclaiming the
bottom code range for shadow separation → perceived contrast).

**Approach**
- audit `sdrDisplayNits` wiring → curve; wire if half-done;
- new `displayBlackNits` (default 0.0; 0.0→0.1 slider in **[shell]** HDR sheet);
- curve construction: prescale so content-black → display black;
- **one constant reused**: `displayBlack` ALSO feeds P0's bar repaint.

**Files**
- `Source/DX11VideoProcessor.cpp` tone-map construction + settings;
- `Source/IVideoRenderer.h`, `VideoRenderer.cpp`;
- **[shell]** `s3_shell.cpp` HDR rows + engine bridge.

**Tasks (ordered).**
1. Audit peak wiring; fix if hardcoded.
2. Add black-floor param + curve prescale.
3. Shared display-black constant into P0 repaint.
4. Projector validation: below-floor shadow separation (no wasted code range).

**Estimate.** 2 days (+ P0 re-check).

---

## Dependencies & notes

- **Independent cores**: P0 ↔ P1 ↔ P2 don't block each other (P2 and P3's last
  item link via `crStr`).
- **P5 display model** feeds P0's repaint constant — wire both when P4 lands;
  till then P0 uses output black.
- Demo philosophy: everything defaults to today's behavior (strength 1.0, bars
  ON, curves unchanged); no persistence.
- Training stays gated on nothing now — 150K DV corpus decided (P3-only task).

## Suggested execution order (start of next session)

1. **P0 black-bars** (no deps, highest visible payoff) — SAT row-max probe today,
   mask pass, HTPC projector check.
2. Follow P1 → P2 → P3(features) — P4 whenever the audit is convenient.

## Persons + skin protection (2026-09-10, done)

Person mask (class 11|12) decoded from the SAME yolo26n-sem inference as sky, packed
into the G channel of the R8G8 semseg mask (R=sky unchanged). LUT-only-inside-people
gate: chroma push suppression and micro-contrast damping now concentrate on real
people; `zion.personStrength` 0 = bit-identical legacy. Seg cadence fixed to a true
~10 fps throttle (was every frame in normal playback). Face detection/tracker (YuNet +
ByteTrack) now redundant — **Phase 2 follow-up: remove after HTPC validation.**