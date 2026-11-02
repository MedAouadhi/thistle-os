# The Chat app's sources beyond tailchat_app.c and tailchat_ui.c, relative to
# this directory. Named once, here, for the firmware component
# (CMakeLists.txt) and both simulators (cmake/thistle_apps.cmake).
set(THISTLE_TAILCHAT_SRCS
    tailchat/tailchat_core.c
    tailchat/tailchat_ogg.c
    tailchat/tailchat_voice.c
)
