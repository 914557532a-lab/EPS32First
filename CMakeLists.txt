SET(SOURCES ui_comp_paneloftitle.c
    ui_comp.c
    ui_MainScreen.c
    ui_QRScreen.c
    ui.c
    ui_comp_hook.c
    ui_helpers.c
    ui_img_990005157.c
    ui_img_648949130.c
    ui_img_160084951.c
    fonts/ui_font_IconFont1.c
    fonts/ui_font_IconFont2.c
    fonts/ui_font_IconFont3.c)

add_library(ui ${SOURCES})
