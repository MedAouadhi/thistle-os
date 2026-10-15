# The File Transfer app's sources beyond filexfer_app.c and filexfer_ui.c,
# relative to this directory. Named once, here, for the firmware component
# (CMakeLists.txt) and both simulators (cmake/thistle_apps.cmake).
set(THISTLE_FILEXFER_SRCS
    filexfer/xfer_auth.c
    filexfer/xfer_fs.c
    filexfer/xfer_path.c
    filexfer/xfer_server.c
    filexfer/xfer_web.c
)
