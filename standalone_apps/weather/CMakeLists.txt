cmake_minimum_required(VERSION 3.16)
project(weather C)

set(THISTLE_APP_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../../app_sdk")
include(${THISTLE_APP_SDK}/CMakeLists.txt)

thistle_app(weather SRCS main.c)
