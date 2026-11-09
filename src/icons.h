// Vector icons, drawn in code so they stay sharp at every DPI scale.
#pragma once
#include "app.h"

enum IconId {
    ICON_NONE = 0,
    ICON_SAVE, ICON_UNDO, ICON_REDO,
    ICON_PASTE, ICON_CUT, ICON_COPY,
    ICON_GROW, ICON_SHRINK,
    ICON_BOLD, ICON_ITALIC, ICON_UNDERLINE, ICON_STRIKE, ICON_SUBSCRIPT, ICON_SUPERSCRIPT,
    ICON_COLOR, ICON_HIGHLIGHT,
    ICON_INDENT_DEC, ICON_INDENT_INC, ICON_LIST, ICON_SPACING,
    ICON_ALIGN_LEFT, ICON_ALIGN_CENTER, ICON_ALIGN_RIGHT, ICON_ALIGN_JUSTIFY, ICON_PARAGRAPH,
    ICON_PICTURE, ICON_DATETIME, ICON_OBJECT,
    ICON_FIND, ICON_REPLACE, ICON_SELECTALL,
    ICON_ZOOM_OUT, ICON_ZOOM_IN,
    ICON_COUNT
};

// Draws an icon into the square (x, y, size, size).
// bar: colour of the swatch under the text-colour / highlight icons (CLR_NONE = hollow swatch).
void Icon_Draw(HDC hdc, int icon, int x, int y, int size, COLORREF color, COLORREF bar = CLR_NONE);

// Small drop-down chevron centred on (cx, cy).
void Icon_DrawChevron(HDC hdc, int cx, int cy, int halfWidth, COLORREF color);
