# The Hermes app's sources beyond hermes_app.c and hermes_ui.c, relative to
# this directory. Named once, here, for the firmware component
# (CMakeLists.txt) and both simulators (cmake/thistle_apps.cmake).

# md4c 0.5.2, built as upstream wrote it: without warnings, since it is not
# this project's to fix.
set(THISTLE_HERMES_THIRD_PARTY_SRCS
    third_party/md4c/md4c.c
)

set(THISTLE_HERMES_SRCS
    hermes/hermes_bridge.c
    hermes/hermes_chat.c
    hermes/hermes_config.c
    hermes/hermes_glyphs.c
    hermes/hermes_markdown.c
    hermes/hermes_panels.c
    hermes/hermes_proto.c
    hermes/hermes_view.c
    hermes/hermes_ws.c
    hermes/hermes_ws_conn.c
    ${THISTLE_HERMES_THIRD_PARTY_SRCS}
)
