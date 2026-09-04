cmake_minimum_required(VERSION 3.25)
project(HackRFTool VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "HackRFTool targets x64 only.")
endif()

set(WINFLUX_ROOT "C:/tjf/github/WinFlux" CACHE PATH "WinFlux 仓库路径")
set(MSYS2_UCRT64_BIN "C:/msys64/ucrt64/bin" CACHE PATH "libhackrf 运行时 DLL 目录")
set(MSYS2_UCRT64_INCLUDE "C:/msys64/ucrt64/include" CACHE PATH "libhackrf 头文件目录")

# 子目录作用域不回传 CMAKE_MODULE_PATH，须在本工程挂 WinFlux 的 cmake/
list(APPEND CMAKE_MODULE_PATH "${WINFLUX_ROOT}/cmake")
add_subdirectory(${WINFLUX_ROOT} winflux EXCLUDE_FROM_ALL)
include(WinFluxCompilerOptions)

add_executable(HackRFTool WIN32
    src/app/main.cpp
    src/dsp/analyzer.cpp
    src/dsp/fft.cpp
    src/dsp/waterfall.cpp
    src/radio/hackrf.cpp
    src/ui/views.cpp)
target_include_directories(HackRFTool PRIVATE src ${MSYS2_UCRT64_INCLUDE})
target_link_libraries(HackRFTool PRIVATE WinFlux::flux)
flux_apply_compiler_options(HackRFTool)

foreach(dll libhackrf.dll libusb-1.0.dll libwinpthread-1.dll)
    add_custom_command(TARGET HackRFTool POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MSYS2_UCRT64_BIN}/${dll}" $<TARGET_FILE_DIR:HackRFTool>
        COMMENT "复制 ${dll}")
endforeach()

add_executable(HackRFSmoke tools/hackrf_smoke.cpp src/radio/hackrf.cpp)
target_include_directories(HackRFSmoke PRIVATE src ${MSYS2_UCRT64_INCLUDE})
flux_apply_compiler_options(HackRFSmoke)
foreach(dll libhackrf.dll libusb-1.0.dll libwinpthread-1.dll)
    add_custom_command(TARGET HackRFSmoke POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MSYS2_UCRT64_BIN}/${dll}" $<TARGET_FILE_DIR:HackRFSmoke>)
endforeach()

enable_testing()
add_executable(HackRFToolTest tests/dsp_test.cpp src/dsp/analyzer.cpp src/dsp/fft.cpp
                              src/dsp/waterfall.cpp)
target_include_directories(HackRFToolTest PRIVATE src)
flux_apply_compiler_options(HackRFToolTest)
add_test(NAME HackRFToolTest COMMAND HackRFToolTest)
