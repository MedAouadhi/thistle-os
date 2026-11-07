include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(
    SRCS
        "sms/sms_app.c"
        "sms/sms_ui.c"
        ${THISTLE_SMS_SRCS}
    INCLUDE_DIRS "."
    REQUIRES kernel ui thistle_hal chatkit tailchat
)
