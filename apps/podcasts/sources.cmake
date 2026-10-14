# The Podcasts app's sources beyond podcasts_app.c and podcasts_ui.c,
# relative to this directory. Named once, here, for the firmware component
# (CMakeLists.txt) and both simulators (cmake/thistle_apps.cmake).

# minimp3, built as upstream wrote it: without warnings, since it is not this
# project's to fix (third_party/minimp3/README.md).
set(THISTLE_PODCASTS_THIRD_PARTY_SRCS
    podcasts/pod_mp3.c
)

set(THISTLE_PODCASTS_SRCS
    podcasts/pod_api.c
    podcasts/pod_cred.c
    podcasts/pod_download.c
    podcasts/pod_json.c
    podcasts/pod_library.c
    podcasts/pod_net.c
    podcasts/pod_player.c
    podcasts/pod_sha1.c
    podcasts/pod_symbols_16.c
    podcasts/pod_symbols_16_mono.c
    podcasts/pod_text.c
    podcasts/pod_ui_player.c
    ${THISTLE_PODCASTS_THIRD_PARTY_SRCS}
)
