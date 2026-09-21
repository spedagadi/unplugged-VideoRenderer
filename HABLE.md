# Hable — the display-referred tone map in `ps_convert_pq_to_sdr.hlsl`

This document describes the exact Hable implementation used by the SDI ingest
path: how the pixel travels from PQ to the screen, what every constant means,
where the curve is defined, and — most importantly — what the Hable path
deliberately does **not** read.

**Authoritative references** (read these alongside):

- `Shaders/d3d11/ps_convert_pq_to_sdr.hlsl` — the converter's `main()`, the branch
  structure, the chroma handling.
- `Shaders/convert/hdr_tone_mapping.hlsl` — `hable()`, `ToneMappingHable()`,
  `ToneMappingHablePeak()`.
- `Shaders/convert/tonemap_curves.hlsl` — the scene-referred curves (BT.2390,
  ST.2094-10, ACES, Reinhard, Mobius) plus the otherwise-identical `HabelTonemap`.
- `Shaders/convert/ictcp.hlsl` — `linear_to_ictcp`, `ictcp_to_linear`,
  `ictcp_to_linear_gamut` (the 12-iteration gamut bisection).

---

## 1. The curve itself

```hlsl
// hdr_tone_mapping.hlsl:1-13
inline float3 hable(float3 x)
{
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + (C * B)) + (D * E)) / (x * (A * x + B) + (D * F))) - E / F;
}

float3 ToneMappingHable(const float3 rgb)
{
    static const float3 HABLE_DIV = hable(4.8);     // ≈ 0.5588
    return hable(rgb) / HABLE_DIV;
}
```

Properties that matter for everything below:

- **S-shaped and concave everywhere.** Hable has no knee of its own; it is one
  smooth rational function over the whole input range.
- **Asymptotic, never reaches 1.0.** As `x → ∞`, `hable(x) → E/F = 0.0666…`, so the
  normalised curve asymptotes below 1.0. That is Hable's highlight "protection":
  there is no input that can clip the output.
- **Display-referred.** `hable(x)` is called with `x` being *light relative to the
  display's white* (1.0 = white). It says nothing about the content's peak.

### The two normalisers

| Function | Divides by | When |
|---|---|---|
| `ToneMappingHable` | `hable(4.8)` (fixed ≈ 0.5588) | the REFERENCE branch (selection 7), upstream verbatim |
| `ToneMappingHablePeak` | `hable(peak)`, peak in nits/sdrNits | the production Hable branch (selection 0 or 3) — normalised by the **per-scene content peak** |

`ToneMappingHablePeak` is deliberately not a new curve (`hdr_tone_mapping.hlsl:15-40`):
same `hable()`, same shape, same asymptote — only the scale divisor changes, so the
content's peak lands exactly on 1.0. With the fixed 4.8, content peaking lower than
4.8×sdrNits (e.g. ~400 nits) never reached white and the whole picture sat low, mids
included.

There is also `HabelTonemap` in `tonemap_curves.hlsl:64-68` — the HDR**10-export**
path's variant, *not* normalised. It is a different curve (`white` lands at 93%) and
is kept untouched for the HDR10 path. The SDR converter does **not** use it.

---

## 2. The pixel's journey

Entry (selection 0 or 3 = "Hable") in `main()`:

```hlsl
// ps_convert_pq_to_sdr.hlsl:174-176
float4 color = tex.Sample(samp, input.Tex);   // original pixel, PQ
color = saturate(color);
color = ST2084ToLinear(color, LuminanceScale); // now: 1.0 == sdrNits nits
```

`LuminanceScale = 10000 / sdrNits`, so the decode produces **nits / sdrNits**
(1.0 = display white).

### Step-by-step (ps_convert_pq_to_sdr.hlsl:282-378)

1. **`disp = color.rgb`** — nits/sdrNits, no floor. (The display-black subtraction
   that used to live here was removed on measured evidence — see the long comment at
   `:223-259`. Content 0 → panel black, content sdrNits → panel white.)

2. **ICtCp separation (the chroma path):**
   ```hlsl
   float3 ictcp   = linear_to_ictcp(disp * sdrNits);        // absolute nits
   float  I_rel   = ST2084ToLinear(ictcp.x, 10000).x / sdrNits;
   const float I_origNits = I_rel * sdrNits;                 // original intensity, nits
   ```
   The curve is applied to the **intensity channel only**. Ct/Cp (hue) are carried
   untouched through the map and rescaled in step 6 — per-channel RGB mapping would
   scale R, G and B by different amounts and rotate the hue (warm highlights go
   yellow). Hue is the Ct/Cp relationship; mapping I alone cannot rotate it.

3. **Content-peak normalisation** (`:297-298`):
   ```hlsl
   const float scenePeak = (hableScenePeakNits > 10.0f) ? hableScenePeakNits : maxCLL;
   const float peakSdrUnits = max(scenePeak / max(sdrNits, 1e-3), 1.0);
   ```
   The per-scene measured peak (the IIR-smoothed, cut-snapped anchor from the
   extractor) wins; static `maxCLL` is only the fallback for the first frames or a
   signal the extractor cannot read. This is the quantity that recentres the curve:
   at 85 nits target, a scene peaking ~300 nits arrives as `peakSdrUnits ≈ 3.5`.

4. **Pre-curve gain** (`:317-319`) — a temporary test constant, `kHableGain = 1.00f`
   (exactly the previous picture at 1.0), applied to I before the curve, so it lifts
   mids more than highlights (Hable compresses).

5. **The map, plus the optional knee** (`:319-335`):
   ```hlsl
   const float hMapped = ToneMappingHablePeak(hv, peakSdrUnits).x;
   float hFinal = hMapped;
   if (hableKneS > 0.0) {
       const float3 m = hableKneS * (hv + hableKneA) / (hv + hableKneB);
       hFinal = lerp(hv.x, m.x, step(hableKnee, hv.x));
   }
   I_rel = hFinal;
   ```
   With the knee **off** (`hableKneS == 0`, the default / A-B anchor) this collapses to
   pure Hable. With the knee on, below `hableKnee` Hable is replaced by **identity**
   and above it by a mobius curve whose coefficients are solved CPU-side for
   `M(j) = j, M(peak) = 1, M′(j) = 1` — a join that is smooth by construction, which
   Hable itself cannot provide (Hable is concave everywhere and meets identity only
   at the origin).

6. **Chroma follows the curve — the blotches/anchor-drift fix** (`:337-378`):
   ```hlsl
   const float I_newNits = I_rel * max(sdrNits, 1e-3);
   const float gGain = I_newNits / max(I_origNits, 1e-6);
   const float3 ictcpNew = linear_to_ictcp(ictcp_to_linear(ictcp) * gGain);
   color.rgb = ictcp_to_linear_gamut(ictcpNew, sdrNits) / max(sdrNits, 1e-3);
   ```
   The curve is expressed as one linear gain `gGain` over the **whole colour** —
   applied in linear, then ICtCp is re-derived — so chromaticity cannot move. Handing
   back the *original* absolute Ct/Cp after lowering I over-saturated the pixel, the
   reconstruction left the gamut, and `ictcp_to_linear_gamut`'s per-pixel bisection
   removed saturation as hard-edged patches on smooth gradients (the blotches, worse
   at low sdrNits). Scaling I in linear while scaling chroma in PQ made saturation a
   function of the gain, hence of the moving anchor — the colour-drain. A linear
   scale of R, G and B cannot move hue, so the anchor may move as much as it likes.

7. **Output stages** (`:389-398`): optional 2020→709 gamut conversion on the
   display-relative values (`outputGamut`, defaults to Rec.709; the matrices are
   only valid in linear light), then `saturate` + `pow(x, 1/2.2)` sRGB encode.

---

## 3. What the Hable branch deliberately does NOT read

| Quantity | Used by Hable? | Why |
|---|---|---|
| **`titlePeak`** (panel Title peak, `1016` in the live log) | **No** | Hable is display-referred; the content peak it normalises by is the **measured per-scene peak**, not a title-level row. Title peak feeds the experimental LUT's axis and anchoring stats, never `ToneMappingHablePeak`. |
| **`maxCLL`** | Only as fallback | Used only until the extractor has a per-scene reading (`:297`). |
| **display floor / `displayMinNits`** | **No** | Removed on measurement — subtracting the panel's black from *scene* light clipped most of the picture at dark HDR content. Still used by the experimental LUT and `ToneMapSelectedCurve`, where the value genuinely is display-absolute. |
| **`hableScenePeakNits`** | Yes | The normaliser; measured pre-tone-map, IIR-smoothed with cut-snap. |

## 4. Relationship to the other branches

- `toneCurveSelection 7` = **REFERENCE** — the upstream `VideoRenderer-master`
  converter verbatim (`ToneMappingHable` over RGB, fixed 4.8, no peak, no floor, no
  ICtCp). Kept so a regression is isolated by switching a row. Must stay **first** so
  it outranks the experimental LUT.
- `toneLutEnable` = the **experimental curve** (exptm, 1D PQ-indexed table, already
  display-relative).
- Other selections (1 ACES, 2 Reinhard, 4 Mobius, 5 BT.2390, 6 ST.2094-10) take the
  scene-referred `ToneMapSelectedCurve` path in ICtCp.

## 5. Operational notes from measurement

- Per-scene peaks measured 290–396 nits against a static MaxCLL of 300 →
  normalisation by measured-vs-static differs ~1.5%; the visible difference came from
  `hable(4.8)→hable(3.5)`, the ~1.145x from using the true scene peak.
- `kHableGain = 1.00f` is a **temporary test constant** — the intended user setting
  once the direction is agreed. 1.0 reproduces the picture exactly as before.
- At 85 target nits, a bright sky (200–1000 nits = 2.4×–12× white) sits in Hable's
  least-compressive band — the curve barely bends there, so large bright regions
  read "hot" with almost no rolloff. The levers are headroom (mapping the peak
  below white), display white, or a curve whose rolloff lands in the sky band —
  see the sky-hotspot notes in TONE_MAP_HANDOFF.md.