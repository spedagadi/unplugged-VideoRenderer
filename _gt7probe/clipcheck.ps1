# clipcheck.ps1 -- is this image CLIPPED, or is it just bright?
#
# A flat white core can be two different failures with two different fixes:
#   * a tone curve running out of shoulder  -> the pixels sit at the SAME code across a
#     region (a plateau), and the value is often below 255 because the clamp is in the
#     curve, not at the quantiser;
#   * a post-scale overshoot (sharpen) or a per-channel clip -> the pixels sit AT 255 on
#     one or two channels only, and the halo/ringing shows up as colour where the source
#     had none -- a green or magenta edge on a white core is that signature.
# So: count per-channel saturation, find the plateau, and look at the ring around the
# core. Report the numbers, then decide which stage owns it.
param([Parameter(Mandatory=$true)][string[]]$Path)
$ErrorActionPreference = 'Stop'
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;

public class ClipCheck
{
    public static string Run(string path)
    {
        Bitmap bm = new Bitmap(path);
        int w = bm.Width, h = bm.Height;
        BitmapData d = bm.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
        byte[] buf = new byte[d.Stride * h];
        Marshal.Copy(d.Scan0, buf, 0, buf.Length);
        bm.UnlockBits(d);
        bm.Dispose();
        int stride = d.Stride;

        long n = 0;
        long clipR = 0, clipG = 0, clipB = 0, clipAll = 0, clipAny = 0;
        // top-code histogram: how many pixels sit on each of the last few codes of the
        // max channel. A curve clamp piles them onto ONE code; a soft shoulder spreads.
        int[] topHist = new int[32];
        long bright = 0; double sumL = 0, sumR = 0, sumG = 0, sumB = 0;
        // ring pixels: bright but not the brightest, where a sharpen halo lives
        long ringN = 0; double ringR = 0, ringG = 0, ringB = 0;
        // how much of the image is on the single most common top code (plateau share)
        int maxTop = 0;

        for (int y = 0; y < h; y++)
        {
            int row = y * stride;
            for (int x = 0; x < w; x++)
            {
                int o = row + x * 3;
                int bi = buf[o], gi = buf[o + 1], ri = buf[o + 2];
                n++;
                bool r255 = ri >= 255, g255 = gi >= 255, b255 = bi >= 255;
                if (r255) clipR++;
                if (g255) clipG++;
                if (b255) clipB++;
                if (r255 && g255 && b255) clipAll++;
                if (r255 || g255 || b255) clipAny++;

                int mx = Math.Max(ri, Math.Max(gi, bi));
                double L = 0.2126 * ri + 0.7152 * gi + 0.0722 * bi;
                if (mx >= 224)
                {
                    int idx = Math.Min(31, mx - 224);
                    topHist[idx]++;
                    if (topHist[idx] > maxTop) maxTop = topHist[idx];
                }
                if (L >= 200) { bright++; sumL += L; sumR += ri; sumG += gi; sumB += bi; }
                if (L >= 150 && L < 200) { ringN++; ringR += ri; ringG += gi; ringB += bi; }
            }
        }

        StringBuilder sb = new StringBuilder();
        sb.AppendFormat("{0}   {1}x{2}   {3:N0} px{4}", System.IO.Path.GetFileName(path), w, h, n, Environment.NewLine);
        sb.AppendFormat("  at 255:   R {0,6:P3}   G {1,6:P3}   B {2,6:P3}   all three {3,6:P3}   any {4,6:P3}{5}",
            clipAny > 0 ? (double)clipR / n : 0.0, (double)clipG / n, (double)clipB / n,
            (double)clipAll / n, (double)clipAny / n, Environment.NewLine);
        sb.AppendFormat("  brightest 2%% of frame (luma>=200): n={0:N0}  mean R{1:F0} G{2:F0} B{3:F0}  (G-R {4:+0;-0;0}, B-R {5:+0;-0;0}){6}",
            bright, bright > 0 ? sumR / bright : 0, bright > 0 ? sumG / bright : 0,
            bright > 0 ? sumB / bright : 0,
            bright > 0 ? (sumG - sumR) / bright : 0, bright > 0 ? (sumB - sumR) / bright : 0, Environment.NewLine);
        sb.AppendFormat("  ring below it (luma 150-200):      n={0:N0}  mean R{1:F0} G{2:F0} B{3:F0}  (G-R {4:+0;-0;0}, B-R {5:+0;-0;0}){6}",
            ringN, ringN > 0 ? ringR / ringN : 0, ringN > 0 ? ringG / ringN : 0,
            ringN > 0 ? ringB / ringN : 0,
            ringN > 0 ? (ringG - ringR) / ringN : 0, ringN > 0 ? (ringB - ringR) / ringN : 0, Environment.NewLine);
        sb.AppendLine("  histogram of the MAX channel, codes 224..255 (a curve clamp piles onto one code):");
        for (int c = 224; c <= 255; c += 4)
        {
            long cnt = 0;
            for (int i = c; i < Math.Min(256, c + 4); i++) cnt += topHist[i - 224];
            sb.AppendFormat("    {0}-{1,3}: {2,8:N0} {3}{4}", c, c + 3, cnt,
                new string('#', (int)Math.Min(48, cnt / Math.Max(1, n / 400))), Environment.NewLine);
        }
        sb.AppendFormat("  plateau: {0:P2} of the frame sits on the single most common top code{1}",
            (double)maxTop / n, Environment.NewLine);
        return sb.ToString();
    }
}
'@
foreach ($p in $Path) { [ClipCheck]::Run($p); '' }
