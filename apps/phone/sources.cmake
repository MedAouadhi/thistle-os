# The Phone app's sources beyond phone_app.c and phone_ui.c, relative to this
# directory. Named once, here, for the firmware component (CMakeLists.txt) and
# both simulators (cmake/thistle_apps.cmake).
set(THISTLE_PHONE_SRCS
    phone/phone_contact.c
    phone/phone_icons_18.c
    phone/phone_icons_18_mono.c
    phone/phone_keypad.c
    phone/phone_lists.c
    phone/phone_symbols_16.c
    phone/phone_symbols_16_mono.c
    phone/phone_tab_12.c
    phone/phone_tab_12_mono.c
    phone/phone_text.c
)
