cmake_minimum_required(VERSION 3.22)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(H743_VS_GCC_IDE C ASM)

# 全局宏，CubeMX自动生成的
add_definitions(-DSTM32H743xx)
add_definitions(-DUSE_HAL_DRIVER)

# 编译、链接选项，CubeMX自动生成
add_compile_options(
    -Wno-incompatible-pointer-types
    -mthumb
    -mcpu=cortex-m7
    -mfpu=fpv5-d16
    -mfloat-abi=hard
    -g3 -Og
)
add_link_options(
    -mthumb
    -mcpu=cortex-m7
    -mfpu=fpv5-d16
    -mfloat-abi=hard
    -T ${CMAKE_SOURCE_DIR}/STM32H743XG_FLASH.ld
)

# ========== 核心！直接用CubeMX原生CMake，它已经包含FreeRTOS、所有源码、所有头文件 ==========
include(${CMAKE_SOURCE_DIR}/cmake/stm32cubemx/CMakeLists.txt)