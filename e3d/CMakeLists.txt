# e3d: a software 3D engine for the ESP32-P4, as an ESP-IDF component.
# (PAPP builds compile the same sources with examples/papp/papp_common.py.)
idf_component_register(
    SRCS "src/e3d.c" "src/e3d_render.c" "src/e3d_simd_pie.c" "src/e3d_simd_espv.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES esp_timer heap
)

# The engine is speed-bound: -O2 instead of the project's -Os
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

# The renderer uses the P4's 128-bit integer SIMD for tile fills, copies and
# batched lighting. v1.x chips have it as PIE (xesppie), v3.x re-encoded
# (xespv): code for one faults on the other. Both sets are built and the engine
# picks one by the chip revision when it starts (src/e3d_simd.h).
if(CONFIG_IDF_TARGET_ESP32P4)
    set_source_files_properties(src/e3d_simd_pie.c PROPERTIES
        COMPILE_OPTIONS "-march=rv32imafc_zicsr_zifencei_xesppie")
    set_source_files_properties(src/e3d_simd_espv.c PROPERTIES
        COMPILE_OPTIONS "-march=rv32imafc_zicsr_zifencei_xespv")
endif()
