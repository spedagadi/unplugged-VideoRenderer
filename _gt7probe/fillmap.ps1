# fillmap.ps1 -- one-pass comparison of a matched renderer pair: per-band RGB, gamut
# FILL and per-channel clip. Written because band_tint.ps1 called its band function once
# per band -- 20 full pixel scans across two 4K frames, in a PowerShell loop, which takes
# minutes and looks like a hang. This compiles the scan and does it in ONE pass over each
# image (histogram for the band edges, then accumulate), so it returns in seconds.
param(
    [string]$Mad = 'C:\Users\satee\Pictures\Benchmarks\Dune\12\Dune.2021.German.TrueHD.Atmos.DL.2160p.UHD.BluRay.DV.HDR.HEVC.Remux-NIMA4K.mkv_20261011_135314.678.png',
    [string]$Unp = 'C:\Users\satee\Pictures\Benchmarks\Dune\12\unplugged_frame_2.png',
    [int]$Step = 6
)
$ErrorActionPreference = 'Stop'

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Text;
using System.Runtime.InteropServices;

public class FillMap
{
    struct Px { public byte[] buf; public int stride; public int w; public int h; }

    static Px Load(string path)
    {
        Bitmap bm = new Bitmap(path);
        int w = bm.Width, h = bm.Height;
        BitmapData d = bm.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
        byte[] buf = new byte[d.Stride * h];
        Marshal.Copy(d.Scan0, buf, 0, buf.Length);
        bm.UnlockBits(d);
        bm.Dispose();
        Px p; p.buf = buf; p.stride = d.Stride; p.w = w; p.h = h;
        return p;
    }

    // BT.709 weights on the ENCODED values -- the same luma the bands have always been cut
    // on in this project, so the numbers stay comparable to the earlier tables.
    static double Lum(byte[] b, int o)
    {
        return 0.2126 * (double)b[o + 2] + 0.7152 * (double)b[o + 1] + 0.0722 * (double)b[o];
    }

    // distance from this pixel's own neutral point to its bound, per channel
    static double EdgeScale(double c, double y)
    {
        if (c < y) { double d = y - c; return (d > 1e-6) ? y / d : 1e6; }
        double d2 = c - y;
        if (d2 < 1e-6) return 1e6;
        return (1.0 - y) / d2;
    }

    class Acc
    {
        public double n, sL, sR, sG, sB, sSat, sFill, nFill;
        public double cR, cG, cB;
    }

    public static string Run(string madPath, string unpPath, int step)
    {
        Px m = Load(madPath);
        Px u = Load(unpPath);

        // pass 1: madVR luma histogram -> decile edges, so BOTH frames are cut on the
        // reference's bands. Cutting each image on its own histogram would compare a
        // shadow against a highlight whenever one renderer delivers the scene brighter.
        int[] hist = new int[256];
        for (int y = 0; y < m.h; y += step)
            for (int x = 0; x < m.w; x += step)
            {
                double L = Lum(m.buf, y * m.stride + x * 3);
                if (L >= 6.0) hist[(int)Math.Min(255.0, L)]++;
            }
        long total = 0;
        for (int i = 0; i < 256; i++) total += hist[i];
        double[] edges = new double[11];
        edges[0] = 0.0; edges[10] = 256.0;
        long run = 0; int next = 1;
        for (int i = 0; i < 256 && next < 10; i++)
        {
            run += hist[i];
            while (next < 10 && run >= (long)(total * next / 10)) { edges[next] = i; next++; }
        }
        for (int i = 1; i < 10; i++) if (edges[i] <= edges[i - 1]) edges[i] = edges[i - 1] + 1;

        // pass 2: one scan per frame, accumulated into the ten bands
        Acc[] ma = new Acc[10]; Acc[] ua = new Acc[10];
        for (int i = 0; i < 10; i++) { ma[i] = new Acc(); ua[i] = new Acc(); }
        Scan(m, step, edges, ma, total);
        Scan(u, step, edges, ua, total);

        StringBuilder sb = new StringBuilder();
        sb.AppendFormat("madvr    {0}x{1}   unplugged {2}x{3}   sampled every {4}px   {5:N0} content px{6}",
            m.w, m.h, u.w, u.h, step, total, Environment.NewLine);
        sb.AppendLine("band  luma-range |   mL    uL |   mR    uR |   mG    uG |   mB    uB |  mFILL  uFILL   dFILL |  mSat  uSat");
        for (int i = 0; i < 10; i++)
        {
            if (ma[i].n < 1 || ua[i].n < 1) continue;
            double mFill = ma[i].nFill > 0 ? ma[i].sFill / ma[i].nFill : 0.0;
            double uFill = ua[i].nFill > 0 ? ua[i].sFill / ua[i].nFill : 0.0;
            sb.AppendFormat("D{0,-3} {1,3:F0}-{2,4:F0} | {3,5:F1} {4,5:F1} | {5,5:F1} {6,5:F1} | {7,5:F1} {8,5:F1} | {9,5:F1} {10,5:F1} | {11,5:F3} {12,5:F3} {13,7:F3} | {14,5:F3} {15,5:F3}{16}",
                i + 1, edges[i], edges[i + 1],
                ma[i].sL / ma[i].n, ua[i].sL / ua[i].n,
                ma[i].sR / ma[i].n, ua[i].sR / ua[i].n,
                ma[i].sG / ma[i].n, ua[i].sG / ua[i].n,
                ma[i].sB / ma[i].n, ua[i].sB / ua[i].n,
                mFill, uFill, uFill - mFill,
                ma[i].sSat / ma[i].n, ua[i].sSat / ua[i].n, Environment.NewLine);
        }
        sb.AppendLine();
        sb.AppendLine("per-channel clip, % of band (>=254):");
        sb.AppendFormat("{0,-5} {1,7} {2,7}   {3,7} {4,7}   {5,7} {6,7}", "band", "madR", "unpR", "madG", "unpG", "madB", "unpB");
        sb.AppendLine();
        for (int i = 0; i < 10; i++)
        {
            if (ma[i].n < 1 || ua[i].n < 1) continue;
            sb.AppendFormat("D{0,-4} {1,6:F3}% {2,6:F3}%   {3,6:F3}% {4,6:F3}%   {5,6:F3}% {6,6:F3}%{7}",
                i + 1, 100.0 * ma[i].cR / ma[i].n, 100.0 * ua[i].cR / ua[i].n,
                100.0 * ma[i].cG / ma[i].n, 100.0 * ua[i].cG / ua[i].n,
                100.0 * ma[i].cB / ma[i].n, 100.0 * ua[i].cB / ua[i].n, Environment.NewLine);
        }
        sb.AppendLine();
        sb.AppendLine("FILL = mean distance from the pixel's own neutral, as a fraction of the");
        sb.AppendLine("distance to the BT.709 bound (1.00 = pinned on the edge). Averaged over");
        sb.AppendLine("pixels with encoded saturation >= 0.20 only, so greys do not dilute it.");
        return sb.ToString();
    }

    static void Scan(Px p, int step, double[] edges, Acc[] acc, long dummy)
    {
        for (int y = 0; y < p.h; y += step)
        {
            int row = y * p.stride;
            for (int x = 0; x < p.w; x += step)
            {
                int o = row + x * 3;
                double bl = (double)p.buf[o] / 255.0;
                double gl = (double)p.buf[o + 1] / 255.0;
                double rl = (double)p.buf[o + 2] / 255.0;
                double L = 0.2126 * p.buf[o + 2] + 0.7152 * p.buf[o + 1] + 0.0722 * p.buf[o];
                if (L < 6.0) continue;
                int b = 0;
                while (b < 9 && L >= edges[b + 1]) b++;
                double mx = Math.Max(rl, Math.Max(gl, bl));
                double mn = Math.Min(rl, Math.Min(gl, bl));
                double sat = mx > 0 ? (mx - mn) / mx : 0.0;

                Acc a = acc[b];
                a.n++; a.sL += L; a.sR += p.buf[o + 2]; a.sG += p.buf[o + 1]; a.sB += p.buf[o];
                a.sSat += sat;
                if (p.buf[o + 2] >= 254) a.cR++;
                if (p.buf[o + 1] >= 254) a.cG++;
                if (p.buf[o] >= 254) a.cB++;

                if (sat >= 0.20)
                {
                    double lr = Math.Pow(rl, 2.2), lg = Math.Pow(gl, 2.2), lb2 = Math.Pow(bl, 2.2);
                    double Y = 0.2126 * lr + 0.7152 * lg + 0.0722 * lb2;
                    if (Y > 1e-4 && Y < 0.9999)
                    {
                        double S = Math.Min(Math.Min(EdgeScale(lr, Y), EdgeScale(lg, Y)), EdgeScale(lb2, Y));
                        if (S > 1e-3 && S < 1e5) { a.sFill += 1.0 / S; a.nFill++; }
                    }
                }
            }
        }
    }
}
'@

[FillMap]::Run($Mad, $Unp, $Step)
