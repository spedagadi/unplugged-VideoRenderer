# Tone mapping — status and handoff (2026-09-20)

Where the ingest tone-map work stands, what is measured rather than assumed, and what
is still open. Written to be picked up cold.

## Where the code is

| repo | branch | committed at |
|---|---|---|
| `c:\code\unplugged-VideoRenderer` | `zion-ml-phase0` | **`e61ca88`** |
| `c:\code\unplugged-mpc-hc` | `zion-s3-shell` | **`b89d392f7`** |

**Uncommitted:** the libplacebo peak-detection port (p99.995, IIR, scene hysteresis,
active-fraction). That is the newest work and the only thing not in a restore point.

`git` is NOT on PATH — it is at
`%LOCALAPPDATA%\GitHubDesktop\app-3.6.4\resources\app\git\cmd\git.exe`.

The MPCVR build writes to `_bin\Filter_x64\` **and** creates a versioned
`_bin\MpcVideoRenderer-<ver>\` folder, and the registry follows the new version. Check
`HKLM\SOFTWARE\Classes\CLSID\{71F080AA-8661-4093-B15E-4F6903E77D0A}\InprocServer32`
before deploying — deploying to a stale folder silently does nothing.

---

## The chain, in the order it was established

### 1. The card DOES report ST 2086 — the old conclusion was a probe bug

For months the note was "the card cannot supply ST 2086 over HDMI". It was wrong
twice over:

- **Wrong accessor.** Per Blackmagic's own `DeckLinkAPI.idl`, only `Colorspace` and
  `EOTF` are Integers; the other twelve frame-metadata IDs — primaries, white point,
  mastering min/max, **MaxCLL, MaxFALL** — are **Floats** and must be read with
  `GetFloat`. The probe called `GetInt` on all fourteen and printed `<unsupported>`
  for any failure, so the twelve float-typed ones all read as unsupported.
- **Wrong frame.** The probe ran once per session on frame 1, which returns
  `E_UNEXPECTED` (`0x8000FFFF`) before the signal settles. It could only ever ask at
  the one moment the answer did not exist.

Measured on an HDR10 disc: **MaxCLL 300, MaxFALL 237, mastering 1000 / 0.0001 nits**,
all `S_OK`, agreeing field-for-field with the HDFury's own OSD.

Two agreeing sources are not independent when the second was read through the first —
the old bench note ("ST 2086 never populates on this card's HDMI path") agreed with
the broken probe and was taken as confirmation.

### 2. Title-level attributes drove the tone curve

`DeckLinkSource` publishes the values through `DeckLinkSourceStats`; the engine maps
the card's MaxCLL to `Settings_t.fHdrTitlePeakNits`. So the curve was normalised by a
**title-level** number — the brightest single frame in the whole disc.

That is what made ACES / Reinhard / Möbius / BT.2390 render dark: they were told the
content peaked at 300 nits while the display white is 84, so the whole picture sat low.

### 3. Frame-level peak

The renderer already computes a per-frame scene peak (`m_MLFeatures[0]` = maxscl),
measured on `m_TexConvertOutput` — which is the VP's **PQ output, before the tone map**
(the tone mapping happens inside the correction shader). So a genuine source-referred
per-frame measurement already existed; it was unused by the curves.

**A domain bug had to be fixed first.** The extractor chose its luma domain from
`m_currentSwapChainColorSpace`, which describes the **swap chain** (SDR → G22), not the
texture being sampled (PQ). So it applied `SdrGammaToPQ` to values that were already PQ
— a double remap that corrupted every feature on exactly the path the tone curve runs
on. Now keyed off the correction actually selected.

**Then the statistic.** `features[0]` is a **max**, and a max is set by a single bright
pixel — noise, grain, ringing. Normalising a curve by it makes the picture shimmer.
Now uses **p99.995**, which on a 256×144 readback discards the top ~2 pixels: a speck,
not a highlight.

**Then the filter.** libplacebo's peak detection is now ported with its own constants:
percentile 99.995, `iir_coeff = 1 - exp(-1/smoothing_period)`, period 20, and scene
hysteresis at **1.0/3.0 dB** with `log10_pq = 1e-2`, the delta scaled by the non-black
pixel fraction.

The hysteresis is why it cannot lag: the IIR only wins for deviations **below** 1 dB;
above 3 dB the mix coefficient reaches 1 and the value **jumps in one frame**. An
earlier version used a single hard threshold at 0.12 PQ — roughly 4× libplacebo's high
threshold — so ordinary scene changes fell into the filter's bucket and ramped. That
was the lag; the filter was innocent.

### 4. Hable, on the I channel

Hable was per-channel RGB. It now tone-maps the **I channel of ICtCp**, so hue is the
Ct/Cp relationship and cannot rotate with level. The curve itself is unchanged — same
`hable(x)/hable(4.8)` family, same display-referred asymptote — only its argument is.

- **Normalised by the measured per-scene peak** rather than the fixed 4.8.
- **`ictcp_to_linear_gamut`**: Ct/Cp are *absolute* chroma, so reducing I while holding
  them over-saturates and can leave the gamut, where the inverse clamps negative L'M'S'
  and shifts hue. This desaturates **only as far as the gamut requires** (bisection on
  the chroma scale; early-out when in gamut), keeping hue and intensity. Pass the
  display ceiling in **nits** — `ictcp_to_linear` returns absolute nits, and testing
  against 1.0 means "1 nit" and turns the picture black and white.
- **Hable knee**: identity below, a **solved mobius** above, 0 = pure Hable.

### 5. The knee — and why Hable cannot have a smooth one

Hable is **concave everywhere** (derivative below secant slope at every point; max
slope 0.278 at the origin). Identity has slope 1. So they meet **only at the origin**,
and **no positive knee can be both value- and slope-continuous with Hable**. That is
Hable's shape, not a tuning problem. It is also why libplacebo's `mobius` has a
closed-form solve for exactly this and Hable does not — mobius was *constructed* to be
joinable.

The knee therefore uses the mobius solve above it (`M(j)=j, M(peak)=1, M'(j)=1`) and
Hable when the knee is 0.

**Result, from testing:** any knee above 0 **eats highlights** — the compression moves
into `[knee, peak]` and steepens the shoulder. **The guarded gamma top-up gave better
results**, because its fence is scene-derived (`MLGuard` from `l1max`) and therefore
**independent of the lift** — you can raise the lift without moving the fence. The knee
has no such independence. Recommendation: leave the knee at **0**.

### 6. Zion gamma top-up — the lift that works

A signed offset **added to** the model's gamma exponent in Auto, clamped to ±0.50 and
to `[0.4, 2.0]` overall, exposed as **Gamma top-up** in the UI.

**Additive, not multiplicative** — deliberately: `predicted × strength` cannot lift a
scene the model judged neutral, because 1.0 is a fixed point. A top-up must work
precisely where the model says "no gamma needed".

It rides the existing gamma block, so the shader's own guards bound it: `fade` over the
shadows and `hi_g` over the highlights. It is **skin-gated** using the trained P(skin)
chroma LUT (`skin_lut`, t1/s1 in the gamma pass), sampled with the *same* derivation the
chroma pass uses — gamma-2.2 Cr/Cb against grid bounds `CR[-0.2009, 0.3295]`,
`CB[-0.2494, 0.1967]` — so the two passes cannot disagree about what counts as skin.

`MLGammaTopUp` is carried as a **separate cbuffer field**, not folded into `MLGamma`:
the shader cannot gate a term it cannot see. Appended tail — `MLParams_t` is now 352
bytes, asserted.

---

### 8. The display floor is REMOVED (2026-09-20) — this was the shadow crush

`max(color.rgb - floorN, 0.0) / scale` is gone from the Hable branch of all three
converters (`ps_convert_pq_to_sdr`, `ps_fixconvert_pq_to_sdr`,
`ps_fixconvert_hlg_to_sdr`). The fallbacks are included deliberately: leaving them
would reintroduce the identical crush on a machine without D3D11 VP PQ support.

`floorN` was the panel's measured black — 0.170 nits over `sdrNits` 85 = **0.002**.
The reasoning for it was sound (the panel's `L = Lb + (Lw-Lb)*v^gamma` has its offset
built in, so encoding `(L-Lb)/(Lw-Lb)` reproduces `L` exactly above the black).

**What it cost was never priced.** `color.rgb` is **scene** light, and the content's
black is 0, not `Lb` — so the subtraction removed 0.170 nits of *scene* light from
every pixel and then clipped at zero. In panel code that cut lands at
`(0.170/85)^(1/2.2) = 0.06`, and everything below it died. On dark HDR content the
frame's own **mean** luminance sits *below* the panel's black, so most of the picture
was being deleted.

Measured on one static scene, Hable with the floor vs without:

| | with floor | without |
|---|---|---|
| mean output code | 0.0105 | **0.0162** (+54%) |
| peak output code | 0.7777 | 0.5816 |

The floor bought 34% more peak by deleting the shadows — and the peak is already
above the content's own level either way. Upstream (`VideoRenderer-master`) had no
floor here at all, so reference and shipping now agree.

**The convention now** is the ordinary tone-mapping one: content 0 → panel black,
content `sdrNits` → panel white, so the panel's range is filled rather than trimmed.

**`displayMinNits` is still real** — it feeds the experimental LUT's output range and
`ToneMapSelectedCurve`, where the quantity genuinely is display-absolute. Keep the
setting truthful (0.170); it no longer needs to be zeroed to lift the picture, and
zeroing it now would only misinform the LUT.

### 9. Two domain predicates, not one (2026-09-20)

`MLFeatureDomainIsPq()` and `MLInputIsPq()`, because the two consumers read different
textures and **one flag cannot be right for both**:

- **Extractor** reads `m_TexConvertOutput`, the **pre**-correction texture → domain
  follows the **source** (`TF == 2084`).
- **ML passes** read `pInputTexture`, the **post**-correction texture → PQ survives
  only if the correction did not convert to SDR.

Both were previously keyed off the swap chain, and the extractor additionally ANDed in
`m_bConvertToSdr`. On an SDR display the swap chain is `G22P709` in *every*
configuration, so with Convert to SDR off both were wrong. Proven from the log: for one
static frame `feat0` read **0.3161** on the no-conversion path against **0.3855** on the
tone-mapped one, and `SdrGammaToPQ(0.3855) = 0.31575` — a double remap, to 0.1%. The
Auto model then predicted **gamma 1.4816 vs 1.4446** for that identical frame.

### 10. `mix` is still 0.000 — the hysteresis never fires

`delta = m_MLActiveFrac * |avgPq - peak.avgPq|`, and `m_MLActiveFrac` is a **pixel**
fraction. libplacebo scales by a **workgroup**-active fraction
(`colorspace.c:1231`), which stays near 1.0 because a workgroup counts as active if
*any* pixel of it is. A pixel fraction on a sparse dark frame is small, scales `delta`
below `thresh_low` (0.01), and pins `mix` to 0 — so nothing snaps and the IIR always
wins. Measured: `filt_pq` ramped 0.3202 → 0.3785 over frames 172-252, ~80 frames.
A proper fix means computing a cell-active fraction in `BuildMLFeatures`.

Currently **invisible**, because `peakSdrUnits` clamps to 1.0 for any scene peaking
below `sdrNits` and the smoothed peak is then unread. It becomes visible the moment the
anchor is allowed to follow the content.

---

## Measured facts (do not re-derive)

| | |
|---|---|
| Display SDR white | **84.0 nits** (`LUMINANCE_XYZ_CDM2`, Argyll report 2026-09-17) |
| Display black | **0.172 nits** → ~488:1 |
| App settings | `sdrNits=85`, `blackNits=0.170` — these ARE that measurement |
| Card ST 2086 (HDR10 disc) | MaxCLL 300, MaxFALL 237, mastering 1000/0.0001 |
| HDR InfoFrame decode | verified: primaries land exactly on DCI-P3 D65, CLL/FALL match the OSD |
| Measured scene peaks | ~290–400 nits on the content tested — i.e. close to the title MaxCLL |
| DV disc | **no HDR InfoFrame at all** and no nits anywhere; VSIF carries OUI `00-D0-46` (Dolby) with an empty payload |

**The binding constraint:** content peaks ~300 nits, the display white is 84. That is a
3.5:1 squeeze, and *every* curve pays it somewhere. A source-referenced curve puts the
content peak on white; the knee moves the compression into the highlights; the guarded
gamma lift takes it from the mids without touching the top. There is no version where
the mids come back for free.

---

### 7. The REFERENCE curve — the A/B control (added 2026-09-20)

`iHdrLocalToneMappingType = 7` (`TONE_CURVE_REFERENCE`, IVideoRenderer.h). It is the
upstream `VideoRenderer-master` converter kept **verbatim**, selectable from the
panel's existing **Tone curve** row as `Reference` (6th option; VALUE_MAP sends 7).

It deliberately reads **none** of: the display floor, ICtCp and the chroma bisection,
the measured scene peak, `maxCLL`, the knee, or the mobius coefficients. Per-channel
RGB Hable over a fixed 4.8, exactly as upstream. It must stay **first** in
`main()` — it has to outrank the experimental tone LUT, which is a different axis.

Two traps it walked into, both worth remembering:

- **`discard<int>(m_iHdrLocalToneMappingType, 3, 0, 6)`** in `SetShaderLuminanceParams`
  silently rewrites any out-of-range value to 3 (Hable). A new curve value needs that
  upper bound widened or the row appears to do nothing at all.
- **`ToneLutActive()`** is the single source of truth for the LUT branch; it now also
  excludes the reference, or the predicate and the shader disagree about which branch
  is live and `toneMap: active=` lies.

**What the A/B actually isolates.** For one scene the reference and the current Hable
differ by *exactly* a constant output gain: both are `hable(x)` divided by a constant
(`hable(4.8)` vs `hable(peakSdrUnits)`, and `peakSdrUnits` is clamped to ≥ 1, so the
current divisor is never smaller). They are equal only when a scene peaks at
4.8 × sdrNits ≈ 408 nits. So **the reference is 2.53× DARKER than the current build**
at sdrNits 85 — at every point below the clip.

That makes it a clean experiment: any *shape* difference (crushed shadows, lost detail,
hue) between the two cannot come from the curve — it can only come from the per-channel
RGB → I change, the display floor, or the gamut bisection. Level alone will differ, and
by a lot, so do not read the darker one as "worse".

For a clean comparison also set **Gamma top-up to 0** — that is the Zion stage, which
still runs; the reference only replaces the tone curve.

---

## Open

1. **Verify the uncommitted peak-detection port.** Watch `mix` in the log:
   `HABLE_PEAK ... raw_pq= avg_pq= filt_pq= applied_nits= mix=`. `mix ≈ 1` on a cut,
   `mix ≈ 0` within a shot. If `mix` never reaches 1 on a real change, the thresholds or
   the active-fraction bias are wrong.
2. **The Zion "strength" question.** The original ask was one control adjusting shadow,
   gamma and highlight together. The finding is that **gamma should carry the lift
   alone** — it is the only one of the three whose fence its own value cannot move.
   Shadow and highlight still have no user offset.
3. **Skin tone-preservation is incomplete — do not use it.** The person/skin gate
   machinery exists but its stage-5 leg is unfinished. The **LUT** is the usable piece.
4. **Calibration.** 84 nits is the real number and it is the binding constraint. Adding
   SDR white is the only lever that changes the arithmetic rather than redistributing it.
5. **Committing.** The port needs a commit of its own once validated.

## Traps this session paid for

- **Units.** Three separate bugs came from writing a comparison without checking what
  the thing being compared returns: the gamut ceiling (tested nits against 1.0), the
  transport inference, and a stability claim measured on double-remapped values. Read
  the return units before writing a test against them.
- **`printf` specifiers.** Adding a `%.3f` to the status JSON without its argument
  corrupts the JSON — the panel goes blank *and* the acquiring modal retries forever.
  MSVC's **C4473** catches it; build with warnings visible, not filtered to `error`.
- **PyInstaller.** The app imports Python from the **PYZ inside `Unplugged.exe`**; the
  loose `.py` files in `_internal\` are data and are never read. A UI change needs a
  package rebuild. The tell: every `.pyc` carries the build timestamp and your edited
  module has none.
- **The live MPCVR folder moves with each build.** Deploy to whatever the registry says.
- **The status panel has more than one owner per row.** `on_property`'s mpv path hides
  rows that `on_s3_status` fills; new rows should use new names.
- **Live capture cannot be diffed frame to frame.** Freeze the source before comparing
  settings, or the scene moves more than the setting does.