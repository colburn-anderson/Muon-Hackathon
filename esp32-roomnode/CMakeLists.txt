cmake_minimum_required(VERSION 3.16)

# Standalone ESP-IDF project: the C6 half of the room node. It is deliberately
# NOT part of the Pico BSP's CMake build -- different toolchain, different
# target -- and lives in its own tree the way the CSI tool used to.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(roomnode)
