# The Chat app's sources beyond tailchat_app.c and tailchat_ui.c, relative to
# this directory. Named once, here, for the firmware component
# (CMakeLists.txt) and both simulators (cmake/thistle_apps.cmake).
set(THISTLE_TAILCHAT_SRCS
    tailchat/tailchat_core.c
    tailchat/tailchat_ogg.c
    tailchat/tailchat_voice.c
    tailchat/tailchat_icons_14.c
    tailchat/tailchat_icons_14_mono.c
    tailchat/tailchat_icons_20.c
    tailchat/tailchat_icons_20_mono.c
    tailchat/tailchat_emoji_14.c
    tailchat/tailchat_emoji_14_mono.c
    tailchat/tailchat_emoji_20.c
    tailchat/tailchat_emoji_20_mono.c
)
