cmake_minimum_required(VERSION 3.16)

project(dsp56kJitTest LANGUAGES C CXX)

enable_testing()

set(ASMJIT_STATIC TRUE CACHE BOOL "Build asmjit as a static library" FORCE)

get_filename_component(TIRUSWIND_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(DSP56300_SOURCE_DIR "${TIRUSWIND_REPO_ROOT}/gearmulator/source/dsp56300/source")

if(NOT EXISTS "${DSP56300_SOURCE_DIR}/dsp56kEmu/CMakeLists.txt")
	message(FATAL_ERROR "gearmulator dsp56300 sources were not found at: ${DSP56300_SOURCE_DIR}")
endif()

include("${DSP56300_SOURCE_DIR}/base.cmake")

add_subdirectory("${DSP56300_SOURCE_DIR}/asmjit" "${CMAKE_BINARY_DIR}/gearmulator/asmjit" EXCLUDE_FROM_ALL)

if(WIN32 OR (UNIX AND NOT APPLE))
	add_subdirectory("${DSP56300_SOURCE_DIR}/vtuneSdk" "${CMAKE_BINARY_DIR}/gearmulator/vtuneSdk" EXCLUDE_FROM_ALL)
endif()

add_subdirectory("${DSP56300_SOURCE_DIR}/dsp56kBase" "${CMAKE_BINARY_DIR}/gearmulator/dsp56kBase" EXCLUDE_FROM_ALL)
add_subdirectory("${DSP56300_SOURCE_DIR}/dsp56kEmu" "${CMAKE_BINARY_DIR}/gearmulator/dsp56kEmu" EXCLUDE_FROM_ALL)

if(MSVC)
	target_compile_options(dsp56kEmu PRIVATE /permissive-)
endif()

add_subdirectory("${DSP56300_SOURCE_DIR}/dsp56kTestRunner" "${CMAKE_BINARY_DIR}/gearmulator/dsp56kTestRunner")

add_executable(dsp56k_jit_perf dsp56k_jit_perf.cpp)
target_link_libraries(dsp56k_jit_perf PRIVATE dsp56kEmu)

add_custom_target(run_dsp56k_jit_tests
	COMMAND $<TARGET_FILE:dsp56kTestRunner>
	DEPENDS dsp56kTestRunner
	USES_TERMINAL
)

add_custom_target(run_dsp56k_jit_perf
	COMMAND $<TARGET_FILE:dsp56k_jit_perf>
	DEPENDS dsp56k_jit_perf
	USES_TERMINAL
)

add_test(NAME dsp56k_jit_perf COMMAND dsp56k_jit_perf 100000)
set_tests_properties(dsp56k_jit_perf PROPERTIES LABELS "Performance;dsp56300")
