// Resource and command identifiers for RichPad.
#pragma once

#define IDI_APP                 1

// ---- Dialog templates -------------------------------------------------
#define IDD_PARAGRAPH           200
#define IDD_TABS                201
#define IDD_DATETIME            202
#define IDD_ABOUT               203
#define IDD_PICTURESIZE         204
#define IDD_PRINTING            205
#define IDD_TEXTVIEW            206

// ---- Dialog controls --------------------------------------------------
#define IDC_STATIC_1            1000
#define IDC_STATIC_2            1001
#define IDC_STATIC_3            1002
#define IDC_STATIC_4            1003
#define IDC_STATIC_5            1004
#define IDC_STATIC_6            1005
#define IDC_GROUP_1             1010
#define IDC_GROUP_2             1011

#define IDC_PARA_LEFT           1100
#define IDC_PARA_RIGHT          1101
#define IDC_PARA_FIRST          1102
#define IDC_PARA_SPACING        1103
#define IDC_PARA_SPACEAFTER     1104
#define IDC_PARA_ALIGN          1105

#define IDC_TABS_EDIT           1120
#define IDC_TABS_LIST           1121
#define IDC_TABS_SET            1122
#define IDC_TABS_CLEAR          1123
#define IDC_TABS_CLEARALL       1124

#define IDC_DATE_LIST           1140

#define IDC_ABOUT_ICON          1160
#define IDC_ABOUT_TITLE         1161
#define IDC_ABOUT_VERSION       1162
#define IDC_ABOUT_DESC          1163
#define IDC_ABOUT_NOTE          1164

#define IDC_PIC_WIDTH           1180
#define IDC_PIC_HEIGHT          1181
#define IDC_PIC_LOCK            1182

#define IDC_PRINTING_TEXT       1190
#define IDC_TEXTVIEW_EDIT       1195

// ---- Commands ---------------------------------------------------------
#define ID_FILE_NEW             40001
#define ID_FILE_NEWWINDOW       40002
#define ID_FILE_OPEN            40003
#define ID_FILE_SAVE            40004
#define ID_FILE_SAVEAS          40005
#define ID_FILE_PRINT           40006
#define ID_FILE_QUICKPRINT      40007
#define ID_FILE_PRINTPREVIEW    40008
#define ID_FILE_PAGESETUP       40009
#define ID_FILE_EXIT            40010
#define ID_FILE_MRU_FIRST       40020
#define ID_FILE_MRU_LAST        40029

#define ID_EDIT_UNDO            40100
#define ID_EDIT_REDO            40101
#define ID_EDIT_CUT             40102
#define ID_EDIT_COPY            40103
#define ID_EDIT_PASTE           40104
#define ID_EDIT_PASTESPECIAL    40105
#define ID_EDIT_PASTETEXT       40106
#define ID_EDIT_DELETE          40107
#define ID_EDIT_SELECTALL       40108
#define ID_EDIT_FIND            40109
#define ID_EDIT_FINDNEXT        40110
#define ID_EDIT_FINDPREV        40111
#define ID_EDIT_REPLACE         40112
#define ID_EDIT_PICTURESIZE     40113
#define ID_EDIT_PASTE_MENU      40114
#define ID_EDIT_OPENLINK        40115
#define ID_EDIT_COPYLINK        40116

#define ID_VIEW_ZOOMIN          40200
#define ID_VIEW_ZOOMOUT         40201
#define ID_VIEW_ZOOM100         40202
#define ID_VIEW_RULER           40203
#define ID_VIEW_STATUSBAR       40204
#define ID_VIEW_WRAP_NONE       40205
#define ID_VIEW_WRAP_WINDOW     40206
#define ID_VIEW_WRAP_RULER      40207
#define ID_VIEW_UNIT_INCH       40208
#define ID_VIEW_UNIT_CM         40209
#define ID_VIEW_UNIT_PT         40210
#define ID_VIEW_UNIT_PICA       40211
#define ID_VIEW_THEME_SYSTEM    40212
#define ID_VIEW_THEME_LIGHT     40213
#define ID_VIEW_THEME_DARK      40214
#define ID_VIEW_LANG_AUTO       40215
#define ID_VIEW_LANG_KO         40216
#define ID_VIEW_LANG_EN         40217

#define ID_INSERT_PICTURE       40300
#define ID_INSERT_DATETIME      40301
#define ID_INSERT_OBJECT        40302

#define ID_FORMAT_FONT          40400
#define ID_FORMAT_BOLD          40401
#define ID_FORMAT_ITALIC        40402
#define ID_FORMAT_UNDERLINE     40403
#define ID_FORMAT_STRIKE        40404
#define ID_FORMAT_SUBSCRIPT     40405
#define ID_FORMAT_SUPERSCRIPT   40406
#define ID_FORMAT_GROW          40407
#define ID_FORMAT_SHRINK        40408
#define ID_FORMAT_COLOR         40409
#define ID_FORMAT_COLOR_PICK    40410
#define ID_FORMAT_HIGHLIGHT     40411
#define ID_FORMAT_HIGHLIGHT_PICK 40412
#define ID_FORMAT_ALIGN_LEFT    40413
#define ID_FORMAT_ALIGN_CENTER  40414
#define ID_FORMAT_ALIGN_RIGHT   40415
#define ID_FORMAT_ALIGN_JUSTIFY 40416
#define ID_FORMAT_INDENT_INC    40417
#define ID_FORMAT_INDENT_DEC    40418
#define ID_FORMAT_BULLETS       40419
#define ID_FORMAT_LIST_MENU     40420
#define ID_FORMAT_LIST_NONE     40421
#define ID_FORMAT_LIST_BULLET   40422
#define ID_FORMAT_LIST_NUMBER   40423
#define ID_FORMAT_LIST_LALPHA   40424
#define ID_FORMAT_LIST_UALPHA   40425
#define ID_FORMAT_LIST_LROMAN   40426
#define ID_FORMAT_LIST_UROMAN   40427
#define ID_FORMAT_SPACING_MENU  40428
#define ID_FORMAT_SPACING_100   40429
#define ID_FORMAT_SPACING_115   40430
#define ID_FORMAT_SPACING_150   40431
#define ID_FORMAT_SPACING_200   40432
#define ID_FORMAT_SPACE_AFTER   40433
#define ID_FORMAT_PARAGRAPH     40434
#define ID_FORMAT_TABS          40435
#define ID_FORMAT_CLEAR         40436
#define ID_FORMAT_FONTNAME      40437
#define ID_FORMAT_FONTSIZE      40438

#define ID_HELP_ABOUT           40500
#define ID_HELP_ASSOCIATE       40501
#define ID_HELP_SHORTCUTS       40502

// Menu-bar titles (not real commands; used for hit testing/tooltips).
#define ID_MENU_FIRST           40600
