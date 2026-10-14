# Apps that live as their own components under apps/<dir>/<dir>/.
#
# The firmware finds these through EXTRA_COMPONENT_DIRS in the top-level
# CMakeLists.txt. Neither simulator has a component system, so each lists the
# sources itself — which is how the two of them came to look for these apps
# under components/apps_builtin long after they had moved, and to build with
# nine of the fourteen apps silently compiled out. Naming each app once, here,
# is what stops that happening again.
#
# Entries are "<dir>|<THISTLE_HAVE_ suffix>|<source stem>". The stem differs
# from the directory only for file_manager.

set(THISTLE_OPTIONAL_APPS
    "appstore|APPSTORE|appstore"
    "file_manager|FILEMGR|filemgr"
    "flashlight|FLASHLIGHT|flashlight"
    "navigator|NAVIGATOR|navigator"
    "notes|NOTES|notes"
    "phone|PHONE|phone"
    "podcasts|PODCASTS|podcasts"
    "reader|READER|reader"
    "vault|VAULT|vault"
    "weather|WEATHER|weather"
    "wifiscanner|WIFISCANNER|wifiscanner"
)

# Appends each app's sources to THISTLE_SRCS and its component root to
# THISTLE_APP_INCLUDE_DIRS, and sets THISTLE_HAVE_<suffix>. A macro rather
# than a function so those reach the caller's scope.
#
# An app whose sources are missing is a hard error: every path here is built
# from this list, so an absent one means the list is stale, which is the
# failure this file exists to prevent. Migrating an app out means deleting its
# entry above, not letting the build quietly drop it.
macro(thistle_collect_optional_apps)
    foreach(_entry IN LISTS THISTLE_OPTIONAL_APPS)
        string(REPLACE "|" ";" _parts "${_entry}")
        list(GET _parts 0 _dir)
        list(GET _parts 1 _flag)
        list(GET _parts 2 _stem)
        set(_root "${THISTLE_DIR}/apps/${_dir}")

        if(NOT EXISTS "${_root}/${_dir}/${_stem}_app.c")
            message(FATAL_ERROR
                "App '${_dir}' is listed in cmake/thistle_apps.cmake but "
                "${_root}/${_dir}/${_stem}_app.c does not exist. Fix the path, "
                "or remove the entry if the app has been migrated out.")
        endif()

        set(THISTLE_HAVE_${_flag} 1)
        list(APPEND THISTLE_SRCS
            "${_root}/${_dir}/${_stem}_app.c"
            "${_root}/${_dir}/${_stem}_ui.c"
        )
        # An app with more sources than those two names them in its
        # sources.cmake, as THISTLE_<suffix>_SRCS relative to its root, which
        # its own CMakeLists.txt reads too.
        if(EXISTS "${_root}/sources.cmake")
            include("${_root}/sources.cmake")
            foreach(_src IN LISTS THISTLE_${_flag}_SRCS)
                list(APPEND THISTLE_SRCS "${_root}/${_src}")
            endforeach()
        endif()
        # The apps include one another's headers as "<dir>/<file>.h", so the
        # component root goes on the include path.
        list(APPEND THISTLE_APP_INCLUDE_DIRS "${_root}")
    endforeach()

    unset(_entry)
    unset(_src)
    unset(_parts)
    unset(_dir)
    unset(_flag)
    unset(_stem)
    unset(_root)
endmacro()
