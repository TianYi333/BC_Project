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
set(CMAKE_PROJECT_NAME H743_VS_GCC_IDE)

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

# Add sources to executable
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
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Third_Party/LetterShell/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Third_Party/SFUD/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Third_Party/SFUD/inc/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Third_Party/SFUD/port/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Third_Party/SFUD/src/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Third_Party/cJSON/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Third_Party/Mongoose/*.c        
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Drivers/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Drivers/Encoder/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Drivers/Oled/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Drivers/Clock/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Drivers/Sht40/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Drivers/Motor/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/UserCode/Drivers/Buzzer/*.c
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
    UserCode/Third_Party/LetterShell
    UserCode/Third_Party/SFUD
    UserCode/Third_Party/SFUD/inc
    UserCode/Third_Party/SFUD/port
    UserCode/Third_Party/SFUD/src
    UserCode/Third_Party/cJSON
    UserCode/Third_Party/Mongoose
    UserCode/Drivers/Encoder
    UserCode/Drivers/Oled
    UserCode/Drivers/Clock
    UserCode/Drivers/Sht40
    UserCode/Drivers/Motor
    UserCode/Drivers/Buzzer
    UserCode/Drivers
    UserCode/Logic
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
