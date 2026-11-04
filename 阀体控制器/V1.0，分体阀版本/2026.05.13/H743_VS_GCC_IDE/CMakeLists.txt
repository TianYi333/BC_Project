cmake_minimum_required(VERSION 3.22)

# 编译器配置
set(TOOLCHAIN "D:/APP/GCC-ARM/bin/")
set(CMAKE_C_COMPILER ${TOOLCHAIN}arm-none-eabi-gcc.exe)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN}arm-none-eabi-gcc.exe)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -x assembler-with-cpp")

# 全局编译选项
add_compile_options(
    -mcpu=cortex-m7
    -mthumb
    -mfpu=fpv5-d16
    -mfloat-abi=hard
    -Wall
    -fdata-sections
    -ffunction-sections
)

set(STM32CUBEMX_ROOT ${CMAKE_SOURCE_DIR})
project(H743_VS_GCC_IDE C ASM)

# 正确添加所有核心源文件（lwip.c路径修正）
add_executable(${PROJECT_NAME}
    Core/Src/main.c
    Core/Src/freertos.c
    Core/Src/stm32h7xx_it.c
    Core/Src/system_stm32h7xx.c
    Core/Src/syscalls.c
    Core/Src/sysmem.c
    LWIP/App/lwip.c
)

# 加载完整驱动与第三方库配置
include(${CMAKE_SOURCE_DIR}/cmake/stm32cubemx/CMakeLists.txt)

# 链接选项
target_link_options(${PROJECT_NAME} PRIVATE
    -mcpu=cortex-m7
    -mthumb
    -mfpu=fpv5-d16
    -mfloat-abi=hard
    --specs=nosys.specs
    -Wl,-Map=${PROJECT_NAME}.map
    -Wl,--gc-sections
    -Wl,--defsym=Reset_Handler=0x08000000
)

# 生成固件文件
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${TOOLCHAIN}arm-none-eabi-objcopy.exe -O ihex    $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.hex
    COMMAND ${TOOLCHAIN}arm-none-eabi-objcopy.exe -O binary  $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.bin
)
