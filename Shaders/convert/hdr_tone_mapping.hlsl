inline float3 hable(float3 x)
{
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;

    return ((x * (A * x + (C * B)) + (D * E)) / (x * (A * x + B) + (D * F))) - E / F;
}

float3 ToneMappingHable(const float3 rgb)
{
    static const float3 HABLE_DIV = hable(4.8);

    return hable(rgb) / HABLE_DIV;
}

// Hable normalised by the CONTENT PEAK instead of that fixed 4.8.
//
// The curve above divides by hable(4.8), which assumes the input white sits at
// 4.8 — i.e. that the content peaks at 4.8 x sdrNits nits (about 400 nits for a
// typical SDR white). When the content peaks LOWER than that assumption the
// curve never reaches 1.0 and the whole picture sits low, mids included:
//
//   content peak 300 nits, display 85  ->  input arrives as 3.53
//     peak    3.53 -> 0.87   (never reaches white)
//     42 nits 0.49 -> 0.22
//
// Dividing by hable(peak) instead maps the content peak exactly to 1.0. The
// SHAPE is untouched — same hable(), same asymptote, same no-clip behaviour
// inside the range — only the scale moves, so this is a normalisation fix and
// not a new curve.
//
// `peak` is in the same units as `rgb`: nits / sdrNits.
float3 ToneMappingHablePeak(const float3 rgb, const float peak)
{
    // Explicit: hable() is a float3 function and is componentwise, so passing
    // the scalar broadcasts and .x is the scalar result. Written out rather than
    // left as an implicit float3 -> float truncation (fxc X3206).
    const float div = max(hable(float3(max(peak, 1e-3), 0.0, 0.0)).x, 1e-4);

    return hable(rgb) / div;
}
