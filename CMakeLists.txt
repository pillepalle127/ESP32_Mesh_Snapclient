# Top-level CMake for ESP-IDF v5.5.x
# Target: ESP32 (original) WROVER  -> BT Classic (A2DP) + PSRAM required
#
# Build:
#   idf.py set-target esp32
#   idf.py build flash monitor
#
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32_mesh_snapclient)
