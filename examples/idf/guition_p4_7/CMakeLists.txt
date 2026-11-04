# e3d on a bare ESP-IDF app: the Guition JC1060P470C (ESP32-P4, 7" 1024x600 MIPI-DSI)
cmake_minimum_required(VERSION 3.16)

# the engine component, from this repository
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../e3d")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(e3d_guition_p4_7)
