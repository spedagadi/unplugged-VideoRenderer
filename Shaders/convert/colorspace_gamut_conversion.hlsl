// using code from zimg - https://github.com/sekrit-twc/zimg

#pragma warning(disable: 4122) // disable warning X4122: sum of ... cannot be represented accurately in double precision.

static const float REC_709_PRIMARIES[3][2]  = { { 0.640, 0.330 }, { 0.300, 0.600 }, { 0.150, 0.060 } };
static const float REC_2020_PRIMARIES[3][2] = { { 0.708, 0.292 }, { 0.170, 0.797 }, { 0.131, 0.046 } };

static const float ILLUMINANT_D65[2] = { 0.3127, 0.3290 };

const float det2(const float a00, const float a01, const float a10, const float a11)
{
    return a00 * a11 - a01 * a10;
}

const float determinant(const float3x3 m)
{
    float det = 0;

    det += m[0][0] * det2(m[1][1], m[1][2], m[2][1], m[2][2]);
    det -= m[0][1] * det2(m[1][0], m[1][2], m[2][0], m[2][2]);
    det += m[0][2] * det2(m[1][0], m[1][1], m[2][0], m[2][1]);

    return det;
}

const float3x3 inverse(const float3x3 m)
{
    float3x3 ret;
    const float det = determinant(m);

    ret[0][0] = det2(m[1][1], m[1][2], m[2][1], m[2][2]) / det;
    ret[0][1] = det2(m[0][2], m[0][1], m[2][2], m[2][1]) / det;
    ret[0][2] = det2(m[0][1], m[0][2], m[1][1], m[1][2]) / det;
    ret[1][0] = det2(m[1][2], m[1][0], m[2][2], m[2][0]) / det;
    ret[1][1] = det2(m[0][0], m[0][2], m[2][0], m[2][2]) / det;
    ret[1][2] = det2(m[0][2], m[0][0], m[1][2], m[1][0]) / det;
    ret[2][0] = det2(m[1][0], m[1][1], m[2][0], m[2][1]) / det;
    ret[2][1] = det2(m[0][1], m[0][0], m[2][1], m[2][0]) / det;
    ret[2][2] = det2(m[0][0], m[0][1], m[1][0], m[1][1]) / det;

    return ret;
}

const float3 xy_to_xyz(const float x, const float y)
{
    float3 ret;

    ret[0] = x / y;
    ret[1] = 1.0;
    ret[2] = (1.0 - x - y) / y;

    return ret;
}

const float3 get_d65_xyz()
{
    return xy_to_xyz(ILLUMINANT_D65[0], ILLUMINANT_D65[1]);
}

const float3x3 get_primaries_xyz(const float primaries_xy[3][2])
{
    // Columns: R G B
    // Rows: X Y Z
    float3x3 ret;

    ret[0] = xy_to_xyz(primaries_xy[0][0], primaries_xy[0][1]);
    ret[1] = xy_to_xyz(primaries_xy[1][0], primaries_xy[1][1]);
    ret[2] = xy_to_xyz(primaries_xy[2][0], primaries_xy[2][1]);

    return transpose(ret);
}

const float3x3 gamut_rgb_to_xyz_matrix(const float primaries_xy[3][2])
{
    const float3x3 xyz_matrix = get_primaries_xyz(primaries_xy);
    const float3 white_xyz = get_d65_xyz();

    const float3 s = mul(inverse(xyz_matrix), white_xyz);
    const float3x3 m = { xyz_matrix[0] * s, xyz_matrix[1] * s, xyz_matrix[2] * s };

    return m;
}

static const float3x3 gamut_xyz_color_matrix_2020 = gamut_rgb_to_xyz_matrix(REC_2020_PRIMARIES);
static const float3x3 gamut_xyz_color_matrix_709  = gamut_rgb_to_xyz_matrix(REC_709_PRIMARIES);
static const float3x3 convert_matrix_2020_to_709  = mul(inverse(gamut_xyz_color_matrix_709), gamut_xyz_color_matrix_2020);

float3 Colorspace_Gamut_Conversion_2020_to_709(const float3 rgb)
{
    return mul(convert_matrix_2020_to_709, rgb);
}

// 2020 -> 709 as gamut MAPPING rather than gamut CLIPPING.
//
// The matrix above has a red row with negative coefficients
// (R709 = 1.6605 R - 0.5876 G - 0.0728 B), so a bright cyan-leaning pixel — low red,
// high green and blue — comes out with a NEGATIVE red, which the caller's saturate()
// clamps to zero. Red never comes back. Measured on a snow field: R 209.9 / G 234.2 /
// B 232.4 against madVR's 219.6 / 229.7 / 231.0 — blue identical, red ten codes short,
// saturation doubled. It reads as "the whites are going blue" and it is really a red
// collapse, and it gets WORSE as exposure rises, because brighter highlights sit
// further outside the 709 triangle.
//
// A gamut boundary is not a wall per channel, it is the edge of the triangle, and the
// correct thing to do when a colour is outside it is to travel toward the pixel's own
// neutral point until it is inside. That keeps HUE exactly — the direction from the
// neutral axis is unchanged — and removes only saturation. Clamping per channel does
// the opposite: it changes hue as a side effect of a numerical overflow.
//
// k = 1 keeps the colour; k = 0 is fully neutral. For a channel outside [0,1] the
// limit is where that channel lands exactly on the boundary, and the binding channel
// is the one that needs the smallest k.
// How far a channel can be pushed along the ray from this pixel's neutral point
// before it hits its own bound. Returns the parameter t at which
//     Y + t*(c - Y)  ==  0   (if c < Y)   or   1   (if c > Y).
// t > 1 means the channel still has ROOM (it is inside the triangle); t < 1 means
// it is already OUTSIDE. Unlike a clamp test this is a continuous measure, which is
// the whole point — see Colorspace_Gamut_Compress_2020_to_709.
float GamutEdgeScale(const float c, const float Y)
{
    if (c < Y) {
        const float d = Y - c;
        return (d > 1e-6f) ? (Y / d) : 1e6;         // hits black at t = Y/(Y-c)
    }
    const float d = c - Y;
    if (d < 1e-6f) {
        return 1e6;                                  // achromatic: no bound
    }
    return (1.0f - Y) / d;                           // hits white at t = (1-Y)/(c-Y)
}

// 2020 -> 709 as gamut MAPPING with a capacity target, not as a clamp.
//
// The measured failure this replaces: our sky came out at R=0, G=64, B=119 with a
// gamut fill of exactly 1.00 — pinned ON the 709 edge — while madVR's sat at fill
// 0.48..0.60 with red present at 25..40. A colour with no red is maximally
// saturated for its lightness, so "in gamut" was never the safe state I assumed it
// was: the edge is inside "in gamut", and the edge is exactly where over-saturation
// lives. A clamp-style test cannot see it, because red at 0.0 is not below 0.
//
// So: take the smallest edge scale over the three channels (S), which says where
// this colour's own ray leaves the triangle, and land it at a FRACTION of the way
// there instead of at the boundary.
//
//     fill = 1/S        (1.0 = on the edge, 0.5 = half of capacity used)
//     k    = min(1, cap * S)
//
// cap = 1.0 reproduces pure boundary behaviour (nothing in-gamut is touched at all).
// Lowering it compresses the volume inward: pale colours (large S) stay untouched
// because cap*S is still >= 1, while colours near the edge — the sky — get pulled
// in proportion to how close they are. madVR's measured 0.5-0.6 is what its look
// actually corresponds to, so that is the natural starting point.
//
// Moving along the ray to the pixel's OWN neutral point preserves hue exactly and
// removes only saturation. It cannot overflow: k <= cap*S <= S keeps every channel
// inside the bound it was measured against.
float3 Colorspace_Gamut_Compress_2020_to_709(const float3 rgb, const float cap)
{
    const float3 mapped = Colorspace_Gamut_Conversion_2020_to_709(rgb);

    // Luminance-preserving conversion, so this is the neutral point the compression
    // travels toward.
    const float Y = saturate(dot(mapped, float3(0.2126, 0.7152, 0.0722)));

    // LIGHTNESS-DEPENDENT CAPACITY TARGET.
    //
    // A single constant cap is wrong in both directions, and one table of measurements
    // says so: madVR's SKY sits at fill 0.48..0.63 while its DARK tent fabric sits at
    // 0.87..0.91. A deep navy is legitimately near the edge of the 709 triangle; a
    // bright sky blue is not. madVR's desaturation is therefore lightness-dependent,
    // and a flat cap does the right thing to the sky and the destructive thing to the
    // tent — chroma pulled to 0.58 where madVR keeps 0.91, and gradient energy halved,
    // which is exactly the lost fold structure in the fabric.
    //
    // So the cap relaxes toward 0.95 as lightness falls. The transition band is fitted
    // to madVR's own measured points, not chosen:
    //     tent mid    Y=0.015 -> cap_eff 0.95   (madVR 0.906)
    //     tent upper  Y=0.059 -> cap_eff 0.86   (madVR 0.871)
    //     sky         Y>=0.15 -> cap_eff cap    (madVR 0.48..0.63)
    const float relax  = 1.0 - smoothstep(0.02, 0.15, Y);
    const float capEff = lerp(max(cap, 1e-3f), 0.95, relax);

    const float S = min(min(GamutEdgeScale(mapped.r, Y), GamutEdgeScale(mapped.g, Y)),
                        GamutEdgeScale(mapped.b, Y));
    const float k = min(1.0f, capEff * S);

    return saturate(Y + k * (mapped - Y));
}
