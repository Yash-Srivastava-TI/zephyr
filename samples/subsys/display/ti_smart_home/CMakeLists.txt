# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ti_washing_machine)

set(UI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ui)

# Generated UI source list + hand-written custom sources. Both files append to
# LV_EDITOR_PROJECT_SOURCES using ${CMAKE_CURRENT_LIST_DIR}, which resolves to
# ui/ when included from here.
include(${UI_DIR}/file_list_gen.cmake)
include(${UI_DIR}/user_config.cmake)

target_sources(app PRIVATE
	src/main.c
	${LV_EDITOR_PROJECT_SOURCES}
)

# The UI uses both same-directory ("washing_machine_gen.h") and root-relative
# ("components/base/card/card_gen.h") includes, so the ui/ root is enough.
target_include_directories(app PRIVATE ${UI_DIR})

# Zephyr's LVGL module adds ${LVGL_DIR}/src/ to the include path (where both
# lvgl.h and lvgl_private.h live), but does NOT add ${LVGL_DIR}/ itself.
# The vendored UI headers include either "lvgl.h" or "lvgl/lvgl.h" depending
# on this guard — setting it picks the flat form that matches what's reachable.
target_compile_definitions(app PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
