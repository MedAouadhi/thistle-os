# The Reader's sources beyond reader_app.c and reader_ui.c, relative to this
# directory. Named once, here, for the firmware component (CMakeLists.txt) and
# both simulators (cmake/thistle_apps.cmake).

# Expat and uzlib, as upstream wrote them: built without warnings, since they
# are not this project's to fix.
set(THISTLE_READER_THIRD_PARTY_SRCS
    third_party/expat/xmlparse.c
    third_party/expat/xmlrole.c
    third_party/expat/xmltok.c
    third_party/uzlib/tinflate.c
    third_party/uzlib/adler32.c
    third_party/uzlib/crc32.c
)

set(THISTLE_READER_SRCS
    reader/arabic_shape.c
    reader/epub.c
    reader/epub_zip.c
    reader/html_entities.c
    reader/reader_font_16.c
    reader/reader_font_16_mono.c
    ${THISTLE_READER_THIRD_PARTY_SRCS}
)
