// Icon generator for the openartemis Android launcher icon.
//
// Draws an ORIGINAL mark (no krkrsdl3 assets involved) and writes every density
// plus the adaptive-icon foreground/background layers. Run it from the repo
// root: it emits into android/app/res/.
//
// Design: a night sky in the app's own blue palette, carrying a crescent moon
// (Artemis) and a play triangle (the engine reads/plays PACKED SCENARIOS), over
// a few scenario lines suggesting the script text the engine interprets.
//
//   csc /out:tools\gen_icon.exe tools\gen_icon.cs
//   tools\gen_icon.exe
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;

static class GenIcon {
    // --- palette (keeps the app's blue identity, deep enough for a launcher) ---
    static Color Rgb(int r, int g, int b, int a = 255) { return Color.FromArgb(a, r, g, b); }
    static readonly Color SkyTop    = Rgb(0x14, 0x18, 0x3A);  // deep indigo
    static readonly Color SkyBottom = Rgb(0x24, 0x3C, 0x8E);  // brand blue
    static readonly Color Moon      = Rgb(0xF4, 0xE9, 0xC8);  // warm parchment
    static readonly Color MoonEdge  = Rgb(0xD8, 0xC8, 0x96);
    static readonly Color Play      = Rgb(0x2E, 0x8B, 0xF0);  // accent blue
    static readonly Color PlayEdge  = Rgb(0x0D, 0x2B, 0x55);
    static readonly Color LineLight = Rgb(0xBF, 0xD4, 0xF5, 0xD0);

    static GraphicsPath Rounded(RectangleF r, float rad) {
        var p = new GraphicsPath();
        float d = rad * 2;
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }

    static GraphicsPath Triangle(PointF a, PointF b, PointF c) {
        var p = new GraphicsPath();
        p.AddPolygon(new[] { a, b, c });
        p.CloseFigure();
        return p;
    }

    /// Draws the mark. `scale` is the full canvas size; `inset` shrinks the art
    /// into the adaptive-icon safe zone (0 = full bleed, legacy icons use ~0.06).
    static Bitmap Render(int size, bool transparentBg, float inset, bool roundMask) {
        var bmp = new Bitmap(size, size, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bmp)) {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.Clear(Color.Transparent);

            float pad = size * inset;
            var outer = new RectangleF(pad, pad, size - 2 * pad, size - 2 * pad);

            // A round icon is the same artwork clipped to a circle: render into a
            // fresh surface whose clip is set before anything is drawn.
            if (roundMask) {
                var out2 = new Bitmap(size, size, PixelFormat.Format32bppArgb);
                using (var mg = Graphics.FromImage(out2))
                using (var circle = new GraphicsPath()) {
                    mg.SmoothingMode = SmoothingMode.AntiAlias;
                    mg.PixelOffsetMode = PixelOffsetMode.HighQuality;
                    mg.Clear(Color.Transparent);
                    circle.AddEllipse(0, 0, size - 1, size - 1);
                    mg.SetClip(circle, CombineMode.Replace);
                    DrawArt(mg, outer, transparentBg);
                }
                bmp.Dispose();
                return out2;
            }

            DrawArt(g, outer, transparentBg);
        }
        return bmp;
    }

    /// Paints sky + mark into the given (possibly clipped) surface.
    static void DrawArt(Graphics g, RectangleF outer, bool transparentBg) {
        float size = Math.Max(outer.Width, outer.Height);

        {
            // --- background: rounded square with a sky gradient ---
            if (!transparentBg) {
                using (var sky = Rounded(outer, size * 0.22f))
                using (var brush = new LinearGradientBrush(
                           new RectangleF(outer.X, outer.Y, outer.Width, outer.Height),
                           SkyTop, SkyBottom, 118f)) {
                    g.FillPath(brush, sky);
                }
            }

            // --- scenario lines (lower left): the script text the engine reads ---
            float lx = outer.X + outer.Width * 0.14f;
            float ly = outer.Y + outer.Height * 0.66f;
            float lh = outer.Height * 0.055f;
            float[] lens = { 0.40f, 0.30f, 0.36f };
            using (var pen = new Pen(LineLight, lh * 0.62f)) {
                pen.StartCap = LineCap.Round; pen.EndCap = LineCap.Round;
                for (int i = 0; i < lens.Length; i++) {
                    float w = outer.Width * lens[i];
                    float y = ly + i * lh * 1.75f;
                    g.DrawLine(pen, lx, y, lx + w, y);
                }
            }

            // --- crescent moon (upper right): Artemis ---
            float md = outer.Width * 0.40f;                 // moon diameter
            float mx = outer.Right - md - outer.Width * 0.13f;
            float my = outer.Y + outer.Height * 0.13f;
            using (var moon = new GraphicsPath(FillMode.Winding)) {
                moon.AddEllipse(mx, my, md, md);
                // subtract a second circle to carve the crescent
                moon.AddEllipse(mx + md * 0.30f, my - md * 0.14f, md * 0.98f, md * 0.98f);
                using (var brush = new LinearGradientBrush(
                           new RectangleF(mx, my, md, md), Moon, MoonEdge, 70f))
                    g.FillPath(brush, moon);
            }

            // --- play triangle (overlapping the moon): the engine plays it ---
            float tx = outer.X + outer.Width * 0.30f;
            float ty = outer.Y + outer.Height * 0.30f;
            float tw = outer.Width * 0.34f;
            float th = outer.Height * 0.38f;
            var tri = Triangle(new PointF(tx + tw * 0.06f, ty),
                               new PointF(tx + tw * 0.06f, ty + th),
                               new PointF(tx + tw, ty + th * 0.5f));
            using (var shadow = (GraphicsPath)tri.Clone())
            using (var brush = new SolidBrush(Rgb(0x08, 0x10, 0x24, 0x9A)))
                g.FillPath(brush, shadow);
            using (var pen = new Pen(PlayEdge, outer.Width * 0.022f) { LineJoin = LineJoin.Round })
                g.DrawPath(pen, tri);
            using (var brush = new LinearGradientBrush(
                       new RectangleF(tx, ty, tw, th), Rgb(0x6F, 0xC2, 0xFF), Play, 60f))
                g.FillPath(brush, tri);
        }
    }

    /// Monochrome layer for Android 13+ themed icons: one flat opaque colour on
    /// transparency, with the crescent carved by an offset circle and the play
    /// head knocked out of the crescent so both shapes stay distinguishable once
    /// the system re-tints the mask.
    static Bitmap RenderMonochrome(int size, float inset) {
        var bmp = new Bitmap(size, size, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bmp)) {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(Color.Transparent);
            float pad = size * inset;
            var outer = new RectangleF(pad, pad, size - 2 * pad, size - 2 * pad);
            float md = outer.Width * 0.40f;
            float mx = outer.Right - md - outer.Width * 0.13f;
            float my = outer.Y + outer.Height * 0.13f;
            float tx = outer.X + outer.Width * 0.30f;
            float ty = outer.Y + outer.Height * 0.30f;
            float tw = outer.Width * 0.34f;
            float th = outer.Height * 0.38f;
            var tri = Triangle(new PointF(tx + tw * 0.06f, ty),
                               new PointF(tx + tw * 0.06f, ty + th),
                               new PointF(tx + tw, ty + th * 0.5f));

            using (var mark = new GraphicsPath(FillMode.Winding)) {
                mark.AddEllipse(mx, my, md, md);
                var hole = new GraphicsPath();
                hole.AddEllipse(mx + md * 0.30f, my - md * 0.14f, md * 0.98f, md * 0.98f);
                mark.AddPath(hole, false);
                mark.AddPath(tri, false);   // winding: triangle cuts out of the moon
                using (var brush = new SolidBrush(Color.White))
                    g.FillPath(brush, mark);
            }
        }
        return bmp;
    }

    static void Save(Bitmap b, string path) {
        Directory.CreateDirectory(Path.GetDirectoryName(path));
        b.Save(path, ImageFormat.Png);
        Console.WriteLine("  " + path.Replace('\\', '/') + "  " + b.Width + "x" + b.Height);
    }

    static int Main() {
        try {
            return Run();
        } catch (Exception ex) {
            Console.Error.WriteLine("FAILED: " + ex.GetType().Name + ": " + ex.Message);
            if (ex.InnerException != null)
                Console.Error.WriteLine("  inner: " + ex.InnerException.GetType().Name + ": " + ex.InnerException.Message);
            return 2;
        }
    }

    static int Run() {
        string res = Path.Combine("android", "app", "res");
        if (!Directory.Exists(Path.Combine("android", "app"))) {
            Console.Error.WriteLine("run from the repo root (android/app/res not found)");
            return 1;
        }
        Console.WriteLine("legacy launcher icons:");

        // Legacy ic_launcher: full-bleed rounded square per density.
        string[] dirs = { "mipmap-mdpi", "mipmap-hdpi", "mipmap-xhdpi", "mipmap-xxhdpi", "mipmap-xxxhdpi" };
        int[] px = { 48, 72, 96, 144, 192 };
        for (int i = 0; i < dirs.Length; i++) {
            using (var b = Render(px[i], false, 0.02f, false))
                Save(b, Path.Combine(res, dirs[i], "ic_launcher.png"));
        }

        // Legacy round icon (API 25 launchers that ask for it): circular mask.
        Console.WriteLine("legacy round icons:");
        for (int i = 0; i < dirs.Length; i++) {
            using (var b = Render(px[i], false, 0.0f, true))
                Save(b, Path.Combine(res, dirs[i], "ic_launcher_round.png"));
        }

        // Adaptive icon (API 26+): 108dp layers; the art must sit inside the
        // inner 72dp safe zone, so the foreground is drawn with an inset and a
        // smaller scale than the legacy full-bleed art.
        Console.WriteLine("adaptive icon layers:");
        int[] apx = { 108, 162, 216, 324, 432 };
        for (int i = 0; i < dirs.Length; i++) {
            using (var fg = Render(apx[i], true, 0.045f, false))
                Save(fg, Path.Combine(res, dirs[i], "ic_launcher_foreground.png"));
            using (var bg = Render(apx[i], false, 0.0f, false))
                Save(bg, Path.Combine(res, dirs[i], "ic_launcher_background.png"));
            using (var mono = RenderMonochrome(apx[i], 0.045f))
                Save(mono, Path.Combine(res, dirs[i], "ic_launcher_monochrome.png"));
        }

        Console.WriteLine("done.");
        return 0;
    }
}
