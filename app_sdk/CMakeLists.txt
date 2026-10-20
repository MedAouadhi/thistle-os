# ThistleOS App SDK -- CMake helpers for building .app.elf
#
# Usage in app CMakeLists.txt:
#   set(THISTLE_SDK_PATH "/path/to/app_sdk")
#   include(${THISTLE_SDK_PATH}/CMakeLists.txt)
#   thistle_app(my_app SRCS main.c)

cmake_minimum_required(VERSION 3.16)

function(thistle_app APP_NAME)
    cmake_parse_arguments(ARG "" "" "SRCS" ${ARGN})

    if(THISTLE_HOST_APP)
        add_library(${APP_NAME} MODULE ${ARG_SRCS})
    else()
        add_executable(${APP_NAME} ${ARG_SRCS})
    endif()

    target_include_directories(${APP_NAME} PRIVATE
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/include
    )

    if(THISTLE_HOST_APP)
        target_compile_definitions(${APP_NAME} PRIVATE THISTLE_HOST_APP=1)
        target_compile_options(${APP_NAME} PRIVATE -fPIC)
        if(APPLE)
            target_link_options(${APP_NAME} PRIVATE -Wl,-undefined,dynamic_lookup)
        endif()
        set_target_properties(${APP_NAME} PROPERTIES PREFIX "")
    else()
        # A shared object with thistle_app_main as its entry, as Espressif's
        # ELF loader takes them (its elf_loader.cmake): the loader keeps only
        # .text, .data, .rodata, .data.rel.ro and .bss, which a shared link
        # merges every input section into, and relocates what a shared object
        # leaves (RELATIVE, GLOB_DAT, JMP_SLOT). A relocatable (-r) link keeps
        # each section at address 0 and the loader cannot place them.
        # Hidden visibility makes every reference inside the app RELATIVE;
        # only the kernel's syscalls and the libc it lends are looked up.
        # libgcc goes in whole, for double arithmetic and the like.
        target_compile_options(${APP_NAME} PRIVATE
            -Os
            -fPIC
            -fvisibility=hidden
            -ffunction-sections
            -fdata-sections
            -nostdlib
            -fno-exceptions
        )

        target_link_options(${APP_NAME} PRIVATE
            -nostartfiles
            -nostdlib
            -shared
            -fPIC
            -e thistle_app_main
            -Wl,--gc-sections
            -Wl,--undefined=thistle_app_main
        )
        target_link_libraries(${APP_NAME} PRIVATE gcc)

        string(REPLACE "-elf-gcc" "-elf-strip" THISTLE_APP_STRIP "${CMAKE_C_COMPILER}")
        set(strip_flags --strip-unneeded --remove-section=.comment
                        --remove-section=.got.loc --remove-section=.dynamic)
        if(CMAKE_C_COMPILER MATCHES "xtensa")
            list(APPEND strip_flags --remove-section=.xt.lit --remove-section=.xt.prop
                                    --remove-section=.xtensa.info)
        else()
            list(APPEND strip_flags --remove-section=.riscv.attributes)
        endif()
        add_custom_command(TARGET ${APP_NAME} POST_BUILD
            COMMAND "${THISTLE_APP_STRIP}" ${strip_flags} "$<TARGET_FILE:${APP_NAME}>"
            VERBATIM
        )
    endif()

    set_target_properties(${APP_NAME} PROPERTIES
        SUFFIX ".app.elf"
    )
endfunction()
