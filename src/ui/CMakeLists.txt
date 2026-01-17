SET(SOURCES ui_Scanner.c
    ui_ManageBands.c
    ui_Hotspot.c
    ui_StandbyScreen.c
    ui.c
    ui_comp_hook.c
    ui_helpers.c
    ui_events.c
    ui_img_mickey_png.c
    ui_img_white_starsbackground_png.c
    fonts/ui_font_Monsterat38withSymbols.c)

add_library(ui ${SOURCES})
