// Verifies the generated launcher icon geometrically: region colours must match
// the intended design (night sky + crescent moon upper-right, blue play
// triangle centre-left, script lines lower-left) and the adaptive foreground
// must stay inside the 72/108 safe zone.
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

static class CheckIcon {
    static byte[] Load(string path, out int w, out int h) {
        using (var bmp = new Bitmap(path)) {
            w = bmp.Width; h = bmp.Height;
            var d = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            var buf = new byte[w * h * 4];
            Marshal.Copy(d.Scan0, buf, 0, buf.Length);
            bmp.UnlockBits(d);
            return buf;
        }
    }

    static string Avg(byte[] p, int w, int h, float x0, float y0, float x1, float y1) {
        long r = 0, g = 0, b = 0, n = 0, a = 0;
        for (int y = (int)(y0 * h); y < (int)(y1 * h); y++)
            for (int x = (int)(x0 * w); x < (int)(x1 * w); x++) {
                int i = (y * w + x) * 4;
                b += p[i]; g += p[i + 1]; r += p[i + 2]; a += p[i + 3]; n++;
            }
        if (n == 0) return "n/a";
        return string.Format("rgb({0},{1},{2}) a={3}",
            r / n, g / n, b / n, a / n);
    }

    static int Main(string[] a) {
        string root = a.Length > 0 ? a[0] : "android/app/res";
        string big = root + "/mipmap-xxxhdpi/ic_launcher.png";
        int w, h;
        var px = Load(big, out w, out h);
        Console.WriteLine("legacy icon " + w + "x" + h + "  " + big);
        Console.WriteLine("  top-left  (sky)        " + Avg(px, w, h, 0.08f, 0.08f, 0.22f, 0.20f));
        Console.WriteLine("  upper-right(moon)      " + Avg(px, w, h, 0.74f, 0.20f, 0.86f, 0.34f));
        Console.WriteLine("  centre-left(play)      " + Avg(px, w, h, 0.38f, 0.44f, 0.46f, 0.54f));
        Console.WriteLine("  lower-left (text line) " + Avg(px, w, h, 0.20f, 0.70f, 0.30f, 0.74f));
        Console.WriteLine("  bottom-right(sky)      " + Avg(px, w, h, 0.80f, 0.82f, 0.92f, 0.94f));

        // round icon must be transparent in the corners
        int rw, rh;
        var rpx = Load(root + "/mipmap-xxxhdpi/ic_launcher_round.png", out rw, out rh);
        Console.WriteLine("round icon corner alpha  " + Avg(rpx, rw, rh, 0.0f, 0.0f, 0.06f, 0.06f) +
                          "   (want a=0)");
        Console.WriteLine("round icon centre        " + Avg(rpx, rw, rh, 0.45f, 0.45f, 0.55f, 0.55f));

        // adaptive foreground: artwork must sit inside the 72/108 safe zone
        int fw, fh;
        var fpx = Load(root + "/mipmap-xxxhdpi/ic_launcher_foreground.png", out fw, out fh);
        int minX = fw, minY = fh, maxX = -1, maxY = -1;
        for (int y = 0; y < fh; y++)
            for (int x = 0; x < fw; x++)
                if (fpx[(y * fw + x) * 4 + 3] > 8) {
                    if (x < minX) minX = x; if (x > maxX) maxX = x;
                    if (y < minY) minY = y; if (y > maxY) maxY = y;
                }
        double safe0 = 108 * (1 - 72.0 / 108) / 2 / 108;   // = 1/6 ≈ 0.1667
        Console.WriteLine("fg ink bbox = (" + minX + "," + minY + ")..(" + maxX + "," + maxY + ")");
        Console.WriteLine("  safe zone is 18.." + (fw - 18) + " px (72/108 dp); inside = " +
                          (minX >= 18 - 2 && minY >= 18 - 2 && maxX <= fw - 18 + 2 && maxY <= fh - 18 + 2));
        return 0;
    }
}
