cmake_minimum_required(VERSION 3.20)

project(TirusWind LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ASMJIT_STATIC TRUE CACHE BOOL "" FORCE)
set(ASMJIT_TEST FALSE CACHE BOOL "" FORCE)
set(ASMJIT_NO_INSTALL TRUE CACHE BOOL "" FORCE)
set(DSP56300_DEBUGGER OFF CACHE BOOL "" FORCE)

set(GEARMULATOR_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gearmulator/source")
set(DSP56300_SOURCE_DIR "${GEARMULATOR_SOURCE_DIR}/dsp56300/source")

add_subdirectory("${DSP56300_SOURCE_DIR}/asmjit" "${CMAKE_BINARY_DIR}/gearmulator/asmjit" EXCLUDE_FROM_ALL)

if(WIN32 OR (UNIX AND NOT APPLE))
	add_subdirectory("${DSP56300_SOURCE_DIR}/vtuneSdk" "${CMAKE_BINARY_DIR}/gearmulator/vtuneSdk" EXCLUDE_FROM_ALL)
endif()

add_subdirectory("${DSP56300_SOURCE_DIR}/dsp56kBase" "${CMAKE_BINARY_DIR}/gearmulator/dsp56kBase" EXCLUDE_FROM_ALL)
add_subdirectory("${DSP56300_SOURCE_DIR}/dsp56kEmu" "${CMAKE_BINARY_DIR}/gearmulator/dsp56kEmu" EXCLUDE_FROM_ALL)

add_subdirectory("${GEARMULATOR_SOURCE_DIR}/baseLib" "${CMAKE_BINARY_DIR}/gearmulator/baseLib" EXCLUDE_FROM_ALL)
add_subdirectory("${GEARMULATOR_SOURCE_DIR}/libresample" "${CMAKE_BINARY_DIR}/gearmulator/libresample" EXCLUDE_FROM_ALL)
add_subdirectory("${GEARMULATOR_SOURCE_DIR}/synthLib" "${CMAKE_BINARY_DIR}/gearmulator/synthLib" EXCLUDE_FROM_ALL)
add_subdirectory("${GEARMULATOR_SOURCE_DIR}/virusLib" "${CMAKE_BINARY_DIR}/gearmulator/virusLib" EXCLUDE_FROM_ALL)

add_executable(tirus_minimal source/main.cpp)

file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" TIRUSWIND_PROJECT_ROOT)

target_compile_definitions(tirus_minimal PRIVATE
	TIRUSWIND_PROJECT_ROOT="${TIRUSWIND_PROJECT_ROOT}"
)

target_link_libraries(tirus_minimal PRIVATE virusLib)
