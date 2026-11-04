cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME SensorConcentrator)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

file(GLOB_RECURSE CUSTOM_SOURCES
    # 列出所有需要匹配的目录，用空格分隔
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Third_Party/U8G2/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Third_Party/FreeModbus/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Third_Party/FreeModbus/port/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Third_Party/FreeModbus/ascii/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Third_Party/FreeModbus/rtu/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Third_Party/FreeModbus/include/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Third_Party/FreeModbus/tcp/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Third_Party/FreeModbus/function/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Third_Party/Fal/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Third_Party/FlashDB/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Third_Party/cJSON/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Third_Party/Mongoose/*.c        
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Drivers/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Drivers/flash/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Drivers/RS485/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Drivers/Clock/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Drivers/ETH/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Logic/*.c
)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${CUSTOM_SOURCES}
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    UserCode/Third_Party/U8G2
    UserCode/Third_Party/FreeModbus/port
    UserCode/Third_Party/FreeModbus/ascii
    UserCode/Third_Party/FreeModbus/rtu
    UserCode/Third_Party/FreeModbus/include
    UserCode/Third_Party/FreeModbus/tcp
    UserCode/Third_Party/FreeModbus/function
    UserCode/Third_Party/Fal
    UserCode/Third_Party/FlashDB
    UserCode/Third_Party/cJSON
    UserCode/Third_Party/Mongoose
    UserCode/Drivers/Clock
    UserCode/Drivers/ETH
    UserCode/Drivers/flash
    UserCode/Drivers/RS485
    UserCode/Logic
    UserCode/Drivers/ota
)


# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# ===================== 屏蔽 RWX LOAD 段警告 =====================
# STM32F7 的 .RamFunc（Flash 编程/擦除函数）必须放在 RAM 中执行
# （Flash 操作期间 CPU 需从 RAM 取指，否则整片停摆）。
# 这导致 RAM 的 LOAD segment 同时含代码与数据，被 GNU ld 标记为 RWX 权限。
# 属预期行为、非错误，此处显式关闭该告警（需 binutils >= 2.39）。
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -T "${CMAKE_SOURCE_DIR}/STM32F767xx_FLASH.ld"
    -Wl,--no-warn-rwx-segments
)

# ===================== 自动生成 HEX 文件 =====================
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex
            ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.elf
            ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.hex
    COMMENT "Building HEX file: ${PROJECT_NAME}.hex"
)

# ===================== 可选：自动生成 BIN 文件 =====================
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
            ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.elf
            ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.bin
    COMMENT "Building BIN file: ${PROJECT_NAME}.bin"
)


# ===================== Bootloader 已独立为顶层工程 =====================
# Bootloader 已移出本工程，成为独立顶层工程 D:\Desktop\SensorConcentrator_boot
# （自带 CMakeLists / 工具链 / CMSIS / startup / boot.ld，仿 D:\Desktop\boot 处理）。
# 本工程不再构建它；其产物 SensorConcentrator_boot.bin 由该独立工程产出。
# 详见 .workbuddy/memory/ 中的 OTA 章节。

