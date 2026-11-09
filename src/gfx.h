// GDI+ include wrapper (GDI+ headers need min/max, which NOMINMAX removes).
#pragma once
#include "app.h"
#include <objidl.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

inline Gdiplus::Color GpColor(COLORREF c, BYTE alpha = 255)
{
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

// Builds a rounded rectangle path. A radius of 0 gives a plain rectangle.
void Gfx_RoundRectPath(Gdiplus::GraphicsPath& path, Gdiplus::REAL x, Gdiplus::REAL y,
                       Gdiplus::REAL w, Gdiplus::REAL h, Gdiplus::REAL r);
