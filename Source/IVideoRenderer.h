/*
 * (C) 2018-2026 see Authors.txt
 *
 * This file is part of MPC-BE.
 *
 * MPC-BE is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-BE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include <dxva2api.h>

enum :int {
	TEXFMT_AUTOINT = 0,
	TEXFMT_8INT = 8,
	TEXFMT_10INT = 10,
	TEXFMT_16FLOAT = 16,
};

enum :int {
	DEINT_Disable = 0,
	DEINT_Enable = 1,
	DEINT_HackFutureFrames = 2,
};

enum :int {
	SUPERRES_Disable = 0,
	SUPERRES_SD,
	SUPERRES_720p,
	SUPERRES_1080p,
	SUPERRES_1440p,
	SUPERRES_COUNT
};

enum :int {
	CHROMA_Nearest = 0,
	CHROMA_Bilinear,
	CHROMA_CatmullRom,
	CHROMA_Krig, // Shiandow KrigBilateral (DX11 only; DX9 falls back to Catmull-Rom)
	CHROMA_COUNT
};

enum :int {
	UPSCALE_Nearest = 0,
	UPSCALE_Mitchell,
	UPSCALE_CatmullRom,
	UPSCALE_Lanczos2,
	UPSCALE_Lanczos3,
	UPSCALE_Jinc2,
	UPSCALE_COUNT
};

enum :int {
	DOWNSCALE_Box = 0,
	DOWNSCALE_Bilinear,
	DOWNSCALE_Hamming,
	DOWNSCALE_Bicubic,
	DOWNSCALE_BicubicSharp,
	DOWNSCALE_Lanczos,
	DOWNSCALE_COUNT
};

enum :int {
	SWAPEFFECT_Discard = 0,
	SWAPEFFECT_Flip,
	SWAPEFFECT_COUNT
};

enum :int {
	HDRTD_Disabled = 0,
	HDRTD_On_Fullscreen,
	HDRTD_On,
	HDRTD_OnOff_Fullscreen,
	HDRTD_OnOff
};

// iHdrLocalToneMappingType. 1..6 are the shipped curves.
//
// 7 is the REFERENCE: the upstream VideoRenderer-master converter, kept verbatim
// so a change can be isolated by switching to it rather than by rebuilding an old
// tree. It is per-channel RGB Hable normalised by the fixed 4.8, and it reads NONE
// of the DeckLink title metadata, the measured scene peak, the knee, or ICtCp —
// which is exactly the set of things it exists to rule in or out. The Zion grade
// still runs; set the rows that belong to it (Gamma top-up) to 0 for a clean A/B.
enum :int {
	TONE_CURVE_ACES      = 1,
	TONE_CURVE_REINHARD  = 2,
	TONE_CURVE_HABLE     = 3,
	TONE_CURVE_MOBIUS    = 4,
	TONE_CURVE_BT2390    = 5,
	TONE_CURVE_ST2094_10 = 6,
	TONE_CURVE_REFERENCE = 7,
	// 8 = the GT7 operator (Polyphony Digital, MIT, v1.0 2025-08-10). A game
	// tonemapper: a display-referred ~2:1 limiter that takes luminance from a
	// per-channel pass and chroma from the ORIGINAL colour, fading it out over the
	// highlight band. It is the curve that desaturates a warm head instead of
	// carrying the source's grade to full strength the way the I-channel + uniform
	// gain paths do. Its own branch in ps_convert_pq_to_sdr.hlsl (it shares no
	// convention with the curves above), and its parameters come from
	// fHdrGt7Blend / fHdrGt7FadeStart / fHdrGt7FadeEnd rather than the Hable ones.
	TONE_CURVE_GT7       = 8,
	// 9 = the experimental tone mapper (libplacebo spline). Made a first-class entry
	// in the Tone-curve list so it is mutually exclusive with the named curves by
	// construction: the engine drives iExperimentalToneMapper from this value, which
	// arms the scene-stat extractor + LUT build, and the shader's toneLut branch
	// replaces the curve entirely. (GT7's `> 7.5` test is bounded to `< 8.5` so 9
	// reaches the LUT branch instead of being swallowed.)
	TONE_CURVE_SPLINE    = 9,
	// 10 = GT7 Adapt: the GT7 operator UNCHANGED, with one different input. Where
	// plain GT7 takes the peak it maps FROM from static metadata (maxCLL, falling
	// back to the Title peak), Adapt blends that with the measured per-scene peak:
	//   peak = lerp(maxCLL, scenePeak, fHdrGt7Adapt)
	// which is only safe because the measurement is already IIR-smoothed with a
	// snap on scene cuts (see m_HableScenePeakNits) — so it moves AT a cut, in one
	// frame, and holds inside a scene. A tracker that followed the frame peak
	// continuously is auto-gain, and that is the failure this curve exists to
	// avoid; it is also why plain GT7 stays the default whenever maxCLL is real.
	// fHdrGt7Adapt = 0 makes this curve byte-identical to TONE_CURVE_GT7.
	TONE_CURVE_GT7_ADAPT = 10,
};

#define SDR_NITS_DEF 50
#define SDR_NITS_MIN  25
#define SDR_NITS_MAX 400
#define SDR_NITS_STEP  5

constexpr inline auto HDR_NITS_DEF = 1000;
constexpr inline auto HDR_NITS_MIN = 100;
constexpr inline auto HDR_NITS_MAX = 10000;

struct VPEnableFormats_t {
	bool bNV12;
	bool bP01x;
	bool bYUY2;
	bool bOther;
};

enum : int {
	ML_Disable = 0,
	ML_Manual,
	ML_Auto
};

// Input colour-space override for live HDMI capture (DeckLink). Auto = derive
// from the media type / resolution as today; an explicit value forces the
// video input to be graded as that signal (the source filter does not stamp
// VIH2 colourimetry for DeckLink streams).
enum : int {
	ICS_Auto = 0,
	ICS_BT601,       // Rec.601 / SDR
	ICS_BT709,       // Rec.709 / SDR
	ICS_BT2020_SDR,  // BT.2020 primaries, SDR transfer (DV-on-HDMI heuristic)
	ICS_BT2020_PQ,   // BT.2020 + PQ  (HDR10)
	ICS_BT2020_HLG,  // BT.2020 + HLG
};

struct Settings_t {
	bool bUseD3D11;
	bool bShowStats;
	int  iResizeStats;
	int  iTexFormat;
	VPEnableFormats_t VPFmts;
	int  iVPDeinterlacing;
	bool bDeintDouble;
	bool bVPScaling;
	int iVPSuperRes;
	bool bVPRTXVideoHDR;
	int  iChromaScaling;
	int  iUpscaling;
	int  iDownscaling;
	bool bInterpolateAt50pct;
	bool bUseDither;
	bool bDeintBlend;
	int  iSwapEffect;
	bool bExclusiveFS;
	bool bVBlankBeforePresent;
	bool bAdjustPresentTime;
	bool bReinitByDisplay;
	bool bHdrPreferDoVi;
	bool bHdrPassthrough;
	int  iHdrToggleDisplay;
	int  iHdrOsdBrightness;
	bool bConvertToSdr;
	int  iSDRDisplayNits;
	bool bHdrLocalToneMapping;
	int  iHdrLocalToneMappingType; // TONE_CURVE_* above; 7 = upstream reference
	int iHdrDisplayMaxNits;
	// Display BLACK level in nits — measured, e.g. 0.16 for a projector at bb1.0.
	// The tone mapper builds its destination range as [min, max]; without this it
	// assumes the display's black is absolute zero and compresses the shadow end
	// into a range the panel cannot show. Float, not int: sub-1-nit blacks are the
	// norm and 0.16 would round to 0. 0.0 == previous behaviour.
	float fHdrDisplayMinNits = 0.0f;
	// Output gamut of the tone-mapped SDR signal.
	//   0 = convert BT.2020 -> BT.709 (the standard SDR target, default)
	//   1 = keep BT.2020, so a display whose gamut exceeds Rec.709 shows the
	//       source's real colour instead of having everything outside 709
	//       mapped away. Not a standards target — it is the "use the panel's
	//       full gamut" choice, and the display must be set to match.
	int iOutputGamut = 0;
	// The CONTENT's peak luminance, in nits — the top of the range the tone curves
	// map FROM. Distinct from iHdrDisplayMaxNits, which is the DISPLAY's peak and is
	// only read by the HDR-output path.
	//
	// This exists because the scene-referred curves (ACES, Reinhard, Mobius,
	// BT.2390) normalise the signal by the content peak, and without a real value
	// they fall back to a 1000-nit guess. On the DeckLink capture path that guess is
	// all there is — the card's ST.2086 frame metadata is unsupported on HDMI, so
	// MaxCLL never arrives. Feeding a range 5-10x larger than the content makes
	// those curves render far too dark, which is what a user sees as "ACES looks
	// broken".
	//
	// 1000 is the HDR10 nominal default and reproduces the previous behaviour.
	float fHdrTitlePeakNits = 1000.0f;
	bool bHDRPassthroughRaw = false; // skip tone map + ML: show the raw HDR/DV signal on SDR (demo)
	int iInputColorSpace = ICS_Auto;  // forced input colour space for live HDMI capture (ICS_*)

	// Experimental tone mapping (libplacebo port). A SEPARATE axis from the
	// shipped tone mapping, not a modification of it: 0 leaves every existing
	// curve, control and quality decision untouched, which is what makes this
	// safe to A/B on a calibrated display.
	//   0 = off — the shipped PQ/HLG->SDR converters, bit-identical
	//   1 = the libplacebo spline, built per scene on the CPU into a 1D table
	int   iExperimentalToneMapper = 0;
	// pl_tone_map_spline's "Contrast": higher is steeper (closer to clip),
	// preserving midtones at the cost of shadow/highlight detail. 0.00 .. 1.50.
	float fToneLutContrast = 0.50f;

	// Zion ML grade
	bool  bMLEnabled = false;
	int   iMLMode = ML_Disable; // ML_Disable / ML_Manual / ML_Auto
	bool  bMLToneCurveEnabled = true; // "Tone curve" group master
	bool  bMLGammaEnabled = true;
	float fMLGamma = 1.0f;       // manual gamma [0.5, 1.5]
	bool  bMLShadowEnabled = true;
	float fMLShadow = 2.0f;      // shadow strength: 0=Off 1=Low 2=Med 3=High 4=VHi 5=Saturate
	float fMLShadowDehaze = 0.0f; // 0..1 scale DOWN the model's shadow-toe lift (lifted-black haze control); 0 = model as-is
	float fMLHighlightRolloffCtrl = 0.0f; // model-override: signed offset added to the model's rolloff in Auto (rim highlight softening); 0 = model as-is
	// model-override: signed offset ADDED to the model's gamma exponent in Auto.
	//
	// The model keeps shaping the per-scene gamma; this moves it up or down on top
	// of that judgement without freezing the scene adaptation — the same contract
	// as fMLHighlightRolloffCtrl, and the reason it is additive rather than a
	// multiplier (a multiplier cannot lift a scene the model judged neutral, since
	// 1.0 is a fixed point).
	//
	// It rides the existing gamma block, so the shader's own guards bound where it
	// acts: `fade` over the shadows and `hi_g` over the highlights, which keeps it
	// a midtone move. 0 = model as-is.
	float fMLGammaTopUp = 0.0f;
	// Hable knee — the point, in display-relative light (1.0 = the display's
	// white), below which the Hable curve is REPLACED BY IDENTITY.
	//
	// Hable has no knee: it is a global rational curve and compresses everywhere,
	// which is what makes it highlight-safe and also why the whole picture sits
	// low — the shadows and lower mids pay compression they do not need, because
	// only the top of the range genuinely has to compress.
	//
	// Below `fHdrHableKnee` the value passes through untouched; above it Hable
	// applies unchanged (still normalised by the measured per-scene peak, so the
	// content peak still lands on white and the asymptote still protects). The
	// transition is a smoothstep band, not a corner.
	//
	// 0 = pure Hable, i.e. exactly the behaviour before this existed.
	float fHdrHableKnee = 0.0f;
	// Soft-plus pre-compressor for the Hable path (see the cbuffer comment in
	// ps_convert_pq_to_sdr.hlsl) — re-spreads the highlight band Hable compresses
	// least. fSoftKnee <= 0 disables (identical to before it existed); knee and
	// width are display-relative light (1.0 = the display's white).
	float fSoftKnee      = 0.60f;
	float fSoftKneeRatio = 2.48f;
	float fSoftKneeWidth = 0.42f;
	// The Hable curve's reference axis, decoupled from display white (sdrNits).
	// 1.0 for the curve = this many nits; 0 = use sdrNits (identical to before).
	// libplacebo's SDR diffuse white is 203 — clouds at 150-200 nits are content
	// under a 200-nit reference, but 2x-white under an 85-nit one.
	float fHdrRefWhiteNits = 203.0f;
	// Highlight neutral roll (0..1): pulls the top of the PQ->SDR output toward
	// its own luma, level-dependently, so the head of the range lands near the
	// source's neutral rather than carrying the source's warm grade at full
	// strength (verified on Everest vs madVR 2026-09-23; 0.9 matched madVR's
	// own warm head). 0 disables — byte-identical to before the feature.
	float fHdrHighlightNeutral = 0.9f;
	// User-configured chroma compensation exponent (STAGE 1: DELIVERY ONLY).
	// The value is stored and reported (pipeline dump clE=) but NOT applied in
	// any shader yet — the application lands in a later stage, once delivery is
	// proven in the log. 1.0 = none (semantics for the future stage).
	float fHdrChromaExp = 1.35f;
	bool  bMLHighlightEnabled = true;
	float fMLHighlight = 0.0f;   // manual highlight roll-off [-0.3, 0.4]
	bool  bMLDebugCopy = false;  // grade steps use the plain copy shader (shader-vs-plumbing bisect)
	bool  bMLDebugLog = false;   // per-frame diagnostic CSV next to the filter
	bool  bMLShadowDebugVis = false; // heatmap overlay showing shadow enhancement zones
	float fMLShadowZoneCeil = 0.20f;
	float fMLShadowSkinProtect = 1.0f;
	float fMLShadowRatioCap = 1.40f;
	bool  bMLContrastEnabled = true;
	float fMLContrast = 0.15f; // micro-contrast clarity strength [0, 0.5]
	float fMLContrastZoneLo = 0.15f;
	float fMLContrastSkinProtect = 0.65f; // MC damping on person-skin (stage 4)
	float fMLContrastRatioCap = 1.40f;
	bool  bMLContrastDebugVis = false;
	// Sky semantic segmentation (yolo26n-sem; tone-leg). Default OFF — the sky
	// tone leg produced mask-region artifacts on first live runs; kept for later tuning.
	bool  bMLSkyEnabled = false;
	bool  bMLSkyDebugVis = false;   // cyan mask overlay for validation
	float fMLSkyStrength = 0.6f;    // sky tone-leg blend amount [0, 1]
	// Person skin protection (stage 4; class 11|12 of the same segmentation)
	bool  bMLPersonEnabled = false; // default OFF (pick up stage-5 work later)
	float fMLPersonStrength = 1.0f; // LUT-only-inside-people gate [0,1]; 0 = legacy
	bool  bMLPersonDebugVis = false; // magenta person-mask overlay (validation)
	float fMLSkinPreserve = 0.0f;    // stage-5 skin tone: source-structure keep-scale [0,1] (default OFF)
	bool  bTrayIcon = true;      // system tray icon when the renderer is active (opens the property page)
	// Phase 1b — chroma vector tuning / hunt compensation
	bool  bMLHuntEnabled = true;         // default preset: enabled
	float fMLHunt = 0.80f;  // libplacebo default (ml_render.h)
	bool  bMLChromaEnabled = true;       // master pass gate (also off under DoVi)
	bool  bMLChromaNeutralEnabled = true;
	float fMLChromaNeutral = 1.20f;
	bool  bMLChromaFireEnabled = true;
	float fMLChromaFire = 1.35f;
	bool  bMLChromaKneeEnabled = true;
	float fMLChromaKnee = 0.55f;
	bool  bMLChromaSkinEnabled = true;
	float fMLChromaSkinProtect = 0.95f;
	// Face detection
	bool  bMLFaceProtectEnabled = true;
	float fMLFaceProtect = 0.80f;   // face gamma protection strength [0,1]
	float fMLFaceFeather = 0.04f;   // SDF feather width in UV
	float fMLFaceExpand = 1.50f;    // box expansion factor
	bool  bMLFaceDebugVis = false;  // red rectangle overlay on detected faces
	// P0 black bars
	bool  bMLBars = true;           // repaint detected letterbox bars
	bool  bMLBarsHighlight = false; // red debug overlay
	// diagnostic: monotonic config-change counter shared by s3_shell and MPCVR
	uint32_t configSeq = 0;

	// --- GT7 operator (TONE_CURVE_GT7) -------------------------------------
	// APPENDED, not inserted next to the other hdr fields: Settings_t crosses a
	// binary boundary (the engine links against this header and QIs the registered
	// .ax), so inserting would silently shift every later field against a stale
	// filter. sizeof(Settings_t) is logged on push for exactly that reason.
	//
	// Only the operator's two behaviour knobs are exposed. Its curve constants
	// (alpha .25, grayPoint .538, linearSection .444, toeStrength 1.28) are what
	// make it GT7 and stay fixed; see Shaders/convert/gt7_tonemap.hlsl.
	//   Blend: 0 = the raw per-channel "skewed" curve, 1 = luminance from that pass
	//   with chroma taken from the original colour. GT ships 0.6. This is the dial
	//   that decides hue behaviour — toward 0 de-warms harder, toward 1 preserves
	//   the source's chroma exactly (and with it the warm cast).
	float fHdrGt7Blend      = 0.6f;
	//   The chromaCurve window, in units of the ceiling's ICtCp I. GT ships
	//   0.98/1.16. The ratio is in PQ CODES, so at a 225-nit ceiling that window is
	//   ~201..546 nits — the video highlight band, not the top 2% it reads as.
	float fHdrGt7FadeStart  = 0.98f;
	float fHdrGt7FadeEnd    = 1.16f;

	// Spline shadow-pivot scale (litmus dial for the spline's black floor). The
	// spline places its knee/pivot from the SCENE AVERAGE (St2094PickKnee); a bright
	// average drives the pivot up and lifts the whole shadow band = high black
	// floor. This scales the measured average that feeds the pivot: 1.0 = libplacebo
	// as-is, lower = pivot sits lower = deeper blacks. APPENDED (binary boundary).
	float fToneLutPivotScale = 1.0f;

	// GT7 Adapt (TONE_CURVE_GT7_ADAPT) only: how much of the peak the curve maps
	// FROM is decided by the measured scene peak rather than by static metadata.
	// 0 = pure metadata (identical to plain GT7), 1 = the scene peak alone. The
	// endpoints are two pictures that have both been seen on this box, so the dial
	// is a position between known-good and known-bad rather than an abstract gain.
	// APPENDED (binary boundary).
	float fHdrGt7Adapt = 0.25f;

	// Shadow PEDESTAL (Zion grade): a true offset added in the toe, independent of
	// the pixel's own value. The existing shadow toe is a GAIN (lift proportional to
	// x), so despite being documented as a pedestal it cannot raise a pixel already
	// at code 0-4 — measured, crushFrac was identical (0.454) before and after the
	// entire grade while mean rose 34%. This is the actuator for that. 0 = off, i.e.
	// byte-identical to the previous picture. APPENDED (binary boundary).
	float fMLShadowPedestal = 0.0f;

	Settings_t() {
		SetDefault();
	}

	void SetDefault() {
		if (IsWindows8OrGreater()) {
			bUseD3D11                   = true;
		} else {
			bUseD3D11                   = false;
		}
		bShowStats                      = false;
		iResizeStats                    = 0;
		iTexFormat                      = TEXFMT_AUTOINT;
		VPFmts.bNV12                    = true;
		VPFmts.bP01x                    = true;
		VPFmts.bYUY2                    = true;
		VPFmts.bOther                   = true;
		iVPDeinterlacing                = DEINT_Enable;
		bDeintDouble                    = true;
		bVPScaling                      = true;
		iVPSuperRes                     = SUPERRES_Disable;
		bVPRTXVideoHDR                  = false;
		iChromaScaling                  = CHROMA_Bilinear;
		iUpscaling                      = UPSCALE_CatmullRom;
		iDownscaling                    = DOWNSCALE_Hamming;
		bInterpolateAt50pct             = true;
		bUseDither                      = true;
		bDeintBlend                     = false;
		iSwapEffect                     = SWAPEFFECT_Flip;
		bExclusiveFS                    = false;
		bVBlankBeforePresent            = false;
		bAdjustPresentTime              = true;
		bReinitByDisplay                = false;
		bHdrPreferDoVi                  = false;
		if (IsWindows10OrGreater()) {
			bHdrPassthrough             = true;
			bHdrLocalToneMapping        = false;
			iHdrLocalToneMappingType    = 1;
			iHdrDisplayMaxNits          = 1000;
		} else {
			bHdrPassthrough             = false;
			bHdrLocalToneMapping        = false;
			iHdrLocalToneMappingType    = 1;
			iHdrDisplayMaxNits          = 1000;
		}
		iHdrToggleDisplay               = HDRTD_Disabled;
		bConvertToSdr                   = true;
		iHdrOsdBrightness               = 0;
		iSDRDisplayNits                 = SDR_NITS_DEF;
		fHdrDisplayMinNits              = 0.0f; // 0 == the mapping assumes an ideal black
		iOutputGamut                    = 0;    // 0 == Rec.709 SDR output
		fHdrTitlePeakNits               = 1000.0f; // HDR10 nominal content peak
		iExperimentalToneMapper         = 0;    // 0 == the shipped tone mapping
		fToneLutContrast                = 0.50f; // pl_tone_map_spline default
		iHdrLocalToneMappingType        = 5;   // BT.2390 default
		bHDRPassthroughRaw              = false;
		iInputColorSpace                = ICS_Auto;
		bMLEnabled                      = false;
		iMLMode                         = ML_Disable;
		bMLToneCurveEnabled             = true;
		bMLGammaEnabled                 = true;
		fMLGamma                        = 1.0f;
		bMLShadowEnabled                = true;
		fMLShadow                       = 2.0f;
		fMLShadowDehaze                 = 0.0f;
		fMLHighlightRolloffCtrl         = 0.0f;
		fMLGammaTopUp                   = 0.0f;
		fHdrHableKnee                   = 0.0f;
		fSoftKnee                       = 0.60f;
		fSoftKneeRatio                  = 2.48f;
		fSoftKneeWidth                  = 0.42f;
		fHdrRefWhiteNits                = 203.0f;
		fHdrHighlightNeutral             = 0.9f;
		fHdrChromaExp                   = 1.35f;
		bMLHighlightEnabled             = true;
		fMLHighlight                    = 0.0f;
		bMLDebugCopy                    = false;
		bMLDebugLog                     = false;
		bMLShadowDebugVis               = false;
		fMLShadowZoneCeil               = 0.20f;
		fMLShadowSkinProtect            = 1.0f;
		fMLShadowRatioCap               = 1.40f;
		bMLContrastEnabled              = true;
		fMLContrast                     = 0.15f; // micro-contrast clarity strength [0, 0.5]
		fMLContrastZoneLo               = 0.15f;
		fMLContrastSkinProtect          = 0.65f;
		fMLContrastRatioCap             = 1.40f;
		bMLContrastDebugVis             = false;
		bMLSkyEnabled                   = false;
		bMLSkyDebugVis                  = false;
		fMLSkyStrength                  = 0.6f;
		bMLPersonEnabled                = false;
		fMLPersonStrength               = 1.0f;
		bMLPersonDebugVis               = false;
		bMLHuntEnabled                  = true;
		fMLHunt                         = 0.80f;
		bMLChromaEnabled                = true;
		bMLChromaNeutralEnabled         = true;
		fMLChromaNeutral                = 1.20f;
		bMLChromaFireEnabled            = true;
		fMLChromaFire                   = 1.35f;
		bMLChromaKneeEnabled            = true;
		fMLChromaKnee                   = 0.55f;
		bMLChromaSkinEnabled            = true;
		fMLChromaSkinProtect            = 0.95f;
		bMLFaceProtectEnabled           = true;
		fMLFaceProtect                  = 0.80f;
		fMLFaceFeather                  = 0.04f;
		fMLFaceExpand                   = 1.50f;
		bMLFaceDebugVis                 = false;
		bMLBars                         = true;
		bMLBarsHighlight                = false;
		configSeq                       = 0;
		fHdrGt7Blend                    = 0.6f;
		fHdrGt7FadeStart                = 0.98f;
		fHdrGt7FadeEnd                  = 1.16f;
		fToneLutPivotScale              = 1.0f;
		fHdrGt7Adapt                    = 0.25f;
		fMLShadowPedestal               = 0.0f;
	}
};

// Renderer frame accounting (LIVE panel / PERFORMANCE): receive-side frame
// counts + dropped/skipped/failed tallies from the draw pipeline.
struct RenderStats_t {
	unsigned framesReceived = 0;   // total input frames seen by the renderer
	unsigned framesDrawn = 0;      // frames actually presented
	float    meanLuma    = 1.0f;   // mean luma of the last extracted frame, 0..1
	                               // (0 = black). Measured by the ML feature
	                               // readback; exposed so the INGEST owner can
	                               // see BLACK CONTENT — a present count cannot.
	unsigned dropped = 0;          // receive-side drops (m_DrawStats.m_dropped)
	unsigned skipped = 0;          // double-rate skip path (m_RenderStats.dropped2)
	unsigned failed = 0;           // render failures (m_RenderStats.failed)
	double   drawFps = 0.0;        // presented frames per second (measured)
	double   presentLatencyMs = 0; // how far the frame on screen lags capture time
	                               // (wall-clock delta minus stream-time delta since
	                               // the first presented frame)
};

// ─ Optional host-injected, high-level log sink ─────────────────────────────
//
// A channel SEPARATE from the ML/frame debug log (ml_debug.csv). That one is for
// image-quality work and is untouched by this. This one carries the renderer's
// STATE and ACTIONS — init/deinit, media-type and signal changes, swap-chain
// lifecycle — so a host that owns a capture chain can put the renderer's own
// account on the SAME timeline as its engine and its signal monitor.
//
// Deliberately optional, and it must stay that way:
//   * a host that injects nothing (MPC-HC, MPC-BE, any other player) gets NO
//     logging — no file is opened, and every call site tests the sink first, so
//     the renderer behaves exactly as it did before;
//   * the HOST owns the format, the file and the clock, so these lines land in
//     the host's log, in the host's dialect, with the host's timestamp — rather
//     than in a second log with a second clock that nothing can be aligned to.
//
// A new IID rather than new methods on IVideoRenderer: adding to that vtable
// moves its layout and would break any already-built consumer of the .ax.
using MpcvrLogFn = void(__cdecl*)(void* ctx, const char* line);

interface __declspec(uuid("7C1E9A44-2B8D-4F30-9E52-6A1D3B7F0C11"))
IMpcvrLogSink : public IUnknown {
	// ctx is handed back verbatim. fn may be null to detach — and a null fn at
	// any time means "log nothing", which is the state every other host stays in.
	// line is UTF-8, ONE complete line, no trailing newline (the host adds it).
	STDMETHOD(SetLogSink)(void* ctx, MpcvrLogFn fn) PURE;
};

interface __declspec(uuid("1AB00F10-5F55-42AC-B53F-38649F11BE3E"))
IVideoRenderer : public IUnknown {
	STDMETHOD(GetVideoProcessorInfo) (std::wstring& str) PURE;
	STDMETHOD_(bool, GetActive()) PURE;

	STDMETHOD_(void, GetSettings(Settings_t& setings)) PURE;
	STDMETHOD_(void, SetSettings(const Settings_t& setings)) PURE;

	STDMETHOD(SaveSettings()) PURE;

	STDMETHOD_(void, GetMLStats)(std::wstring& str) PURE;
	STDMETHOD_(void, GetRenderStats)(::RenderStats_t& stats) PURE;
};
