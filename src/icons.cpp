// Vector icons, drawn in code so they stay sharp at every DPI scale.
// All shapes are original and described on a 16 x 16 unit grid.
#include "gfx.h"
#include "icons.h"

using namespace Gdiplus;

namespace {

struct Painter {
    Graphics& g;
    REAL ox, oy, s;      // origin and scale (pixels per grid unit)
    Color col;
    REAL pw;             // stroke width in pixels

    PointF P(REAL x, REAL y) const { return PointF(ox + x * s, oy + y * s); }

    void SetupPen(Pen& pen) const {
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        pen.SetLineJoin(LineJoinRound);
    }
    void Line(REAL x1, REAL y1, REAL x2, REAL y2) const {
        Pen pen(col, pw); SetupPen(pen);
        g.DrawLine(&pen, P(x1, y1), P(x2, y2));
    }
    void Poly(std::initializer_list<PointF> pts, bool closed = false, bool fill = false) const {
        std::vector<PointF> v;
        for (const PointF& p : pts) v.push_back(P(p.X, p.Y));
        if (fill) {
            SolidBrush br(col);
            g.FillPolygon(&br, v.data(), (INT)v.size());
        }
        Pen pen(col, pw); SetupPen(pen);
        if (closed) g.DrawPolygon(&pen, v.data(), (INT)v.size());
        else g.DrawLines(&pen, v.data(), (INT)v.size());
    }
    void RRect(REAL x, REAL y, REAL w, REAL h, REAL r, bool fill = false) const {
        GraphicsPath path;
        Gfx_RoundRectPath(path, ox + x * s, oy + y * s, w * s, h * s, r * s);
        if (fill) { SolidBrush br(col); g.FillPath(&br, &path); }
        Pen pen(col, pw); SetupPen(pen);
        g.DrawPath(&pen, &path);
    }
    void DashRRect(REAL x, REAL y, REAL w, REAL h, REAL r) const {
        GraphicsPath path;
        Gfx_RoundRectPath(path, ox + x * s, oy + y * s, w * s, h * s, r * s);
        Pen pen(col, pw);
        REAL dashes[2] = { 2.0f, 1.6f };
        pen.SetDashPattern(dashes, 2);
        g.DrawPath(&pen, &path);
    }
    void Circle(REAL cx, REAL cy, REAL r, bool fill = false) const {
        RectF rc(ox + (cx - r) * s, oy + (cy - r) * s, 2 * r * s, 2 * r * s);
        if (fill) { SolidBrush br(col); g.FillEllipse(&br, rc); }
        else { Pen pen(col, pw); g.DrawEllipse(&pen, rc); }
    }
    void Bezier(REAL x1, REAL y1, REAL x2, REAL y2, REAL x3, REAL y3, REAL x4, REAL y4) const {
        Pen pen(col, pw); SetupPen(pen);
        g.DrawBezier(&pen, P(x1, y1), P(x2, y2), P(x3, y3), P(x4, y4));
    }
    void FillRect(REAL x, REAL y, REAL w, REAL h, Color c) const {
        SolidBrush br(c);
        g.FillRectangle(&br, RectF(ox + x * s, oy + y * s, w * s, h * s));
    }
    // Draws text as a filled outline, centred horizontally on cx with the glyph box centred on cy.
    void Text(const wchar_t* text, REAL cx, REAL cy, REAL em, INT style) const {
        FontFamily segoe(L"Segoe UI");
        const FontFamily* family = &segoe;
        FontFamily arial(L"Arial");
        if (segoe.GetLastStatus() != Ok || !segoe.IsAvailable()) family = &arial;
        if (!family->IsAvailable()) family = FontFamily::GenericSansSerif();
        if (!family->IsStyleAvailable(style)) style = FontStyleRegular;
        GraphicsPath path;
        StringFormat fmt(StringFormat::GenericTypographic());
        path.AddString(text, -1, family, style, em * s, PointF(0, 0), &fmt);
        RectF b;
        path.GetBounds(&b);
        Matrix m;
        m.Translate(ox + cx * s - (b.X + b.Width / 2), oy + cy * s - (b.Y + b.Height / 2));
        path.Transform(&m);
        SolidBrush br(col);
        g.FillPath(&br, &path);
    }
    RectF Box(REAL cx, REAL cy, REAL r) const { return RectF(ox + (cx - r) * s, oy + (cy - r) * s, 2 * r * s, 2 * r * s); }
    void Stroke(GraphicsPath& path) const {
        Pen pen(col, pw); SetupPen(pen);
        g.DrawPath(&pen, &path);
    }
    // Letter "A" built from strokes: apex at (cx, top), feet on the baseline.
    void LetterA(REAL cx, REAL top, REAL bottom, REAL halfWidth) const {
        Poly({ {cx - halfWidth, bottom}, {cx, top}, {cx + halfWidth, bottom} });
        REAL t = 0.62f;   // height of the crossbar
        REAL y = top + (bottom - top) * t, hw = halfWidth * t;
        Line(cx - hw, y, cx + hw, y);
    }
    // Small digit "2" whose box starts at (x, y) and is about 3 x 4.3 units.
    void Digit2(REAL x, REAL y) const {
        Bezier(x, y + 1.2f, x + 0.1f, y - 0.1f, x + 2.8f, y - 0.1f, x + 2.8f, y + 1.3f);
        Bezier(x + 2.8f, y + 1.3f, x + 2.8f, y + 2.5f, x + 0.7f, y + 3.1f, x, y + 4.3f);
        Line(x, y + 4.3f, x + 3.0f, y + 4.3f);
    }
    // Horizontal text lines used by the paragraph icons.
    void Lines(std::initializer_list<PointF> spans, REAL y0, REAL step) const {
        REAL y = y0;
        for (const PointF& sp : spans) { Line(sp.X, y, sp.Y, y); y += step; }
    }
};

void DrawSwatch(const Painter& p, COLORREF bar, bool dimmed)
{
    // Colour swatch under the "A" / marker icons.
    if (bar == CLR_NONE) {
        Pen pen(p.col, std::max(1.0f, p.pw * 0.8f));
        p.g.DrawRectangle(&pen, RectF(p.ox + 2.5f * p.s, p.oy + 12.5f * p.s, 11 * p.s, 2.2f * p.s));
    } else {
        Color c = GpColor(bar, dimmed ? 110 : 255);
        p.FillRect(2, 12.2f, 12, 3, c);
        // A hairline keeps very light colours visible on a light background.
        Pen pen(Color(60, 128, 128, 128), 1.0f);
        p.g.DrawRectangle(&pen, RectF(p.ox + 2 * p.s, p.oy + 12.2f * p.s, 12 * p.s, 3 * p.s));
    }
}

} // namespace

void Icon_Draw(HDC hdc, int icon, int x, int y, int size, COLORREF color, COLORREF bar)
{
    if (icon <= ICON_NONE || icon >= ICON_COUNT || size <= 0) return;
    Graphics g(hdc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);

    REAL s = size / 16.0f;
    // Thin 1px strokes at 16px, slightly lighter relative weight for the large 32px icons.
    REAL pw = (size >= 28) ? std::max(1.5f, s * 0.75f) : std::max(1.0f, s * 1.0f);
    Painter p{ g, (REAL)x, (REAL)y, s, GpColor(color), pw };
    bool dimmed = false;

    switch (icon) {
    case ICON_SAVE:
        p.Poly({ {2.5f, 2.5f}, {10.5f, 2.5f}, {13.5f, 5.5f}, {13.5f, 13.5f}, {2.5f, 13.5f} }, true);
        p.Poly({ {5.0f, 2.5f}, {5.0f, 5.5f}, {9.5f, 5.5f}, {9.5f, 2.5f} });
        p.Poly({ {4.5f, 13.5f}, {4.5f, 9.0f}, {11.5f, 9.0f}, {11.5f, 13.5f} });
        break;
    case ICON_UNDO:
        p.Poly({ {5.5f, 3.5f}, {2.5f, 6.5f}, {5.5f, 9.5f} });
        p.Line(2.5f, 6.5f, 9.5f, 6.5f);
        p.Bezier(9.5f, 6.5f, 12.0f, 6.5f, 13.5f, 8.0f, 13.5f, 10.0f);
        p.Bezier(13.5f, 10.0f, 13.5f, 12.0f, 12.0f, 13.5f, 9.5f, 13.5f);
        p.Line(9.5f, 13.5f, 6.5f, 13.5f);
        break;
    case ICON_REDO:
        p.Poly({ {10.5f, 3.5f}, {13.5f, 6.5f}, {10.5f, 9.5f} });
        p.Line(13.5f, 6.5f, 6.5f, 6.5f);
        p.Bezier(6.5f, 6.5f, 4.0f, 6.5f, 2.5f, 8.0f, 2.5f, 10.0f);
        p.Bezier(2.5f, 10.0f, 2.5f, 12.0f, 4.0f, 13.5f, 6.5f, 13.5f);
        p.Line(6.5f, 13.5f, 9.5f, 13.5f);
        break;
    case ICON_PASTE:
        p.RRect(3.5f, 3.5f, 9.0f, 11.0f, 1.3f);
        p.RRect(6.0f, 2.0f, 4.0f, 2.6f, 0.8f, true);
        if (size >= 24) {
            p.Line(6.0f, 8.5f, 10.0f, 8.5f);
            p.Line(6.0f, 11.0f, 10.0f, 11.0f);
        }
        break;
    case ICON_CUT:
        p.Circle(4.5f, 12.0f, 1.9f);
        p.Circle(11.5f, 12.0f, 1.9f);
        p.Line(5.7f, 10.5f, 12.0f, 2.5f);
        p.Line(10.3f, 10.5f, 4.0f, 2.5f);
        break;
    case ICON_COPY:
        p.RRect(6.5f, 5.5f, 7.0f, 9.0f, 1.2f);
        p.Poly({ {6.5f, 10.5f}, {2.5f, 10.5f}, {2.5f, 1.5f}, {9.5f, 1.5f}, {9.5f, 5.5f} });
        break;
    case ICON_GROW:
        p.LetterA(6.3f, 3.5f, 13.0f, 4.0f);
        p.Poly({ {11.4f, 5.5f}, {13.2f, 3.5f}, {15.0f, 5.5f} });
        break;
    case ICON_SHRINK:
        p.LetterA(5.8f, 6.0f, 13.0f, 3.1f);
        p.Poly({ {11.4f, 3.5f}, {13.2f, 5.5f}, {15.0f, 3.5f} });
        break;
    case ICON_BOLD: {
        Painter b = p;
        b.pw = p.pw * 1.9f;
        GraphicsPath path;
        path.StartFigure();
        path.AddLine(b.P(5.0f, 3.0f), b.P(8.3f, 3.0f));
        path.AddArc(b.Box(8.3f, 5.5f, 2.5f), 270, 180);
        path.AddLine(b.P(8.3f, 8.0f), b.P(5.0f, 8.0f));
        path.StartFigure();
        path.AddLine(b.P(5.0f, 8.0f), b.P(9.0f, 8.0f));
        path.AddArc(b.Box(9.0f, 10.75f, 2.75f), 270, 180);
        path.AddLine(b.P(9.0f, 13.5f), b.P(5.0f, 13.5f));
        path.StartFigure();
        path.AddLine(b.P(5.0f, 3.0f), b.P(5.0f, 13.5f));
        b.Stroke(path);
        break;
    }
    case ICON_ITALIC:
        p.Line(7.0f, 3.0f, 11.5f, 3.0f);
        p.Line(4.5f, 13.0f, 9.0f, 13.0f);
        p.Line(9.5f, 3.0f, 6.5f, 13.0f);
        break;
    case ICON_UNDERLINE: {
        GraphicsPath path;
        path.AddLine(p.P(4.5f, 2.5f), p.P(4.5f, 7.5f));
        path.AddArc(p.Box(8.0f, 7.5f, 3.5f), 180, -180);
        path.AddLine(p.P(11.5f, 7.5f), p.P(11.5f, 2.5f));
        p.Stroke(path);
        p.Line(3.5f, 14.0f, 12.5f, 14.0f);
        break;
    }
    case ICON_STRIKE: {
        GraphicsPath path;
        path.AddBezier(p.P(11.0f, 4.6f), p.P(10.3f, 3.2f), p.P(9.3f, 2.5f), p.P(8.0f, 2.5f));
        path.AddBezier(p.P(8.0f, 2.5f), p.P(6.2f, 2.5f), p.P(5.0f, 3.6f), p.P(5.0f, 5.2f));
        path.AddBezier(p.P(5.0f, 5.2f), p.P(5.0f, 6.9f), p.P(6.3f, 7.5f), p.P(8.0f, 8.0f));
        path.AddBezier(p.P(8.0f, 8.0f), p.P(9.8f, 8.5f), p.P(11.2f, 9.2f), p.P(11.2f, 10.9f));
        path.AddBezier(p.P(11.2f, 10.9f), p.P(11.2f, 12.5f), p.P(9.9f, 13.5f), p.P(8.0f, 13.5f));
        path.AddBezier(p.P(8.0f, 13.5f), p.P(6.5f, 13.5f), p.P(5.4f, 12.8f), p.P(4.8f, 11.6f));
        p.Stroke(path);
        p.Line(2.0f, 8.0f, 14.0f, 8.0f);
        break;
    }
    case ICON_SUBSCRIPT:
        p.Line(2.5f, 3.5f, 9.0f, 11.5f);
        p.Line(9.0f, 3.5f, 2.5f, 11.5f);
        p.Digit2(11.0f, 9.2f);
        break;
    case ICON_SUPERSCRIPT:
        p.Line(2.5f, 5.5f, 9.0f, 13.5f);
        p.Line(9.0f, 5.5f, 2.5f, 13.5f);
        p.Digit2(11.0f, 2.2f);
        break;
    case ICON_COLOR:
        p.LetterA(8.0f, 1.8f, 10.6f, 3.9f);
        DrawSwatch(p, bar, dimmed);
        break;
    case ICON_HIGHLIGHT:
        p.Poly({ {9.5f, 1.5f}, {13.5f, 5.5f}, {8.5f, 10.5f}, {4.5f, 6.5f} }, true);
        p.Poly({ {4.5f, 6.5f}, {8.5f, 10.5f}, {3.0f, 11.0f} }, true, true);
        DrawSwatch(p, bar, dimmed);
        break;
    case ICON_INDENT_DEC:
        p.Lines({ {2.5f, 13.5f}, {7.5f, 13.5f}, {7.5f, 13.5f}, {2.5f, 13.5f} }, 3.5f, 3.0f);
        p.Poly({ {4.5f, 6.0f}, {2.5f, 8.0f}, {4.5f, 10.0f} });
        break;
    case ICON_INDENT_INC:
        p.Lines({ {2.5f, 13.5f}, {7.5f, 13.5f}, {7.5f, 13.5f}, {2.5f, 13.5f} }, 3.5f, 3.0f);
        p.Poly({ {2.5f, 6.0f}, {4.5f, 8.0f}, {2.5f, 10.0f} });
        break;
    case ICON_LIST:
        p.Circle(3.0f, 4.0f, 1.0f, true);
        p.Circle(3.0f, 8.0f, 1.0f, true);
        p.Circle(3.0f, 12.0f, 1.0f, true);
        p.Lines({ {6.5f, 13.5f}, {6.5f, 13.5f}, {6.5f, 13.5f} }, 4.0f, 4.0f);
        break;
    case ICON_SPACING:
        p.Line(4.0f, 3.0f, 4.0f, 13.0f);
        p.Poly({ {2.0f, 5.0f}, {4.0f, 3.0f}, {6.0f, 5.0f} });
        p.Poly({ {2.0f, 11.0f}, {4.0f, 13.0f}, {6.0f, 11.0f} });
        p.Lines({ {8.5f, 13.5f}, {8.5f, 13.5f}, {8.5f, 13.5f} }, 4.0f, 4.0f);
        break;
    case ICON_ALIGN_LEFT:
        p.Lines({ {2.5f, 13.5f}, {2.5f, 9.5f}, {2.5f, 13.5f}, {2.5f, 8.5f} }, 3.5f, 3.0f);
        break;
    case ICON_ALIGN_CENTER:
        p.Lines({ {2.5f, 13.5f}, {4.5f, 11.5f}, {2.5f, 13.5f}, {5.5f, 10.5f} }, 3.5f, 3.0f);
        break;
    case ICON_ALIGN_RIGHT:
        p.Lines({ {2.5f, 13.5f}, {6.5f, 13.5f}, {2.5f, 13.5f}, {7.5f, 13.5f} }, 3.5f, 3.0f);
        break;
    case ICON_ALIGN_JUSTIFY:
        p.Lines({ {2.5f, 13.5f}, {2.5f, 13.5f}, {2.5f, 13.5f}, {2.5f, 13.5f} }, 3.5f, 3.0f);
        break;
    case ICON_PARAGRAPH: {
        // Pilcrow: a filled bowl with two stems.
        GraphicsPath bowl;
        bowl.AddLine(p.P(8.5f, 2.5f), p.P(6.5f, 2.5f));
        bowl.AddArc(p.Box(6.5f, 5.5f, 3.0f), 270, -180);
        bowl.AddLine(p.P(6.5f, 8.5f), p.P(8.5f, 8.5f));
        bowl.CloseFigure();
        SolidBrush br(p.col);
        g.FillPath(&br, &bowl);
        p.Stroke(bowl);
        p.Line(8.5f, 2.5f, 8.5f, 13.5f);
        p.Line(11.5f, 2.5f, 11.5f, 13.5f);
        p.Line(8.5f, 2.5f, 13.0f, 2.5f);
        break;
    }
    case ICON_PICTURE:
        p.RRect(2.5f, 3.5f, 11.0f, 9.0f, 1.4f);
        p.Circle(10.5f, 6.4f, 1.0f, true);
        p.Poly({ {3.2f, 11.6f}, {6.5f, 8.0f}, {9.0f, 10.6f}, {10.4f, 9.2f}, {12.8f, 11.6f} });
        break;
    case ICON_DATETIME:
        p.RRect(2.5f, 3.5f, 11.0f, 10.0f, 1.4f);
        p.Line(2.5f, 6.5f, 13.5f, 6.5f);
        p.Line(5.5f, 2.0f, 5.5f, 4.5f);
        p.Line(10.5f, 2.0f, 10.5f, 4.5f);
        p.FillRect(4.6f, 8.2f, 1.5f, 1.5f, p.col);
        p.FillRect(7.25f, 8.2f, 1.5f, 1.5f, p.col);
        p.FillRect(9.9f, 8.2f, 1.5f, 1.5f, p.col);
        p.FillRect(4.6f, 10.6f, 1.5f, 1.5f, p.col);
        p.FillRect(7.25f, 10.6f, 1.5f, 1.5f, p.col);
        break;
    case ICON_OBJECT:
        p.RRect(2.5f, 2.5f, 7.5f, 7.5f, 1.0f);
        p.Circle(10.2f, 10.2f, 3.3f);
        break;
    case ICON_FIND:
        p.Circle(7.0f, 7.0f, 4.2f);
        p.Line(10.2f, 10.2f, 13.8f, 13.8f);
        break;
    case ICON_REPLACE:
        p.Line(3.0f, 5.5f, 13.0f, 5.5f);
        p.Poly({ {10.5f, 3.0f}, {13.0f, 5.5f}, {10.5f, 8.0f} });
        p.Line(13.0f, 10.5f, 3.0f, 10.5f);
        p.Poly({ {5.5f, 8.0f}, {3.0f, 10.5f}, {5.5f, 13.0f} });
        break;
    case ICON_SELECTALL:
        p.DashRRect(2.5f, 2.5f, 11.0f, 11.0f, 1.0f);
        p.Line(5.5f, 6.5f, 10.5f, 6.5f);
        p.Line(5.5f, 9.5f, 10.5f, 9.5f);
        break;
    case ICON_ZOOM_OUT:
        p.Line(3.5f, 8.0f, 12.5f, 8.0f);
        break;
    case ICON_ZOOM_IN:
        p.Line(3.5f, 8.0f, 12.5f, 8.0f);
        p.Line(8.0f, 3.5f, 8.0f, 12.5f);
        break;
    }
}

void Icon_DrawChevron(HDC hdc, int cx, int cy, int halfWidth, COLORREF color)
{
    Graphics g(hdc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    Pen pen(GpColor(color), std::max(1.0f, halfWidth / 3.5f));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    REAL hw = (REAL)halfWidth, hh = hw * 0.55f;
    PointF pts[3] = { PointF(cx - hw, cy - hh), PointF((REAL)cx, cy + hh), PointF(cx + hw, cy - hh) };
    g.DrawLines(&pen, pts, 3);
}
