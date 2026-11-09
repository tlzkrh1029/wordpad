// Colour palette popup used by the text colour and highlight buttons.
#pragma once
#include "app.h"

enum ColorPopupResult { COLORPOPUP_CANCEL = 0, COLORPOPUP_COLOR = 1, COLORPOPUP_DEFAULT = 2 };

bool ColorPopup_Register();
// anchor: screen rectangle of the button. highlight: show the highlight palette ("No colour")
// instead of the text palette ("Automatic"). Returns what the user chose; *color receives the colour.
ColorPopupResult ColorPopup_Show(HWND owner, const RECT& anchor, bool highlight, COLORREF* color);
