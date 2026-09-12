cmake_minimum_required(VERSION 3.22)

# 工具链：若未在外层指定，则使用本工程自带的 gcc-arm-none-eabi 工具链文件。
# 用法：cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE ${CMAKE_SOURCE_DIR}/cmake/gcc-arm-none-eabi.cmake)
endif()

project(boot C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# =====================================================================
# 独立 Bootloader 工程（boot）
# ---------------------------------------------------------------------
# - 烧录在内部 Flash 低端 0x08000000（64KB），负责校验/烧写 App 并跳
#   转到 0x08100000。
# - 本工程完全自包含：HAL / CMSIS / 启动文件 / 升级逻辑均在本目录内，
#   不依赖原 App 工程 Oil_Pump_Control。可整体复制到任意 STM32H7 工程，
#   修改 boot.ld 的 ORIGIN/SIZE、spi.c + stm32h7xx_hal_msp.c 的 SPI 引脚、
#   ota_common.h 的分区偏移即可复用。
# =====================================================================

# 芯片级宏定义（与 HAL 驱动一致）
add_compile_definitions(USE_HAL_DRIVER STM32H743xx USE_PWR_LDO_SUPPLY)

# 头文件路径
include_directories(
    Core/Inc
    Drivers/STM32H7xx_HAL_Driver/Inc
    Drivers/STM32H7xx_HAL_Driver/Inc/Legacy
    Drivers/CMSIS/Device/ST/STM32H7xx/Include
    Drivers/CMSIS/Include
)

# HAL 驱动源（仅编 boot 实际用到的模块，减小体积）
set(HAL_DRIVER_SRC
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_cortex.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc_ex.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_gpio.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dma.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_spi.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_spi_ex.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash_ex.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr_ex.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_exti.c
)

# Bootloader 可执行文件
add_executable(boot
    startup_stm32h743xx.s
    Core/Src/main_boot.c
    Core/Src/spi.c
    Core/Src/stm32h7xx_hal_msp.c
    Core/Src/boot_crc.c
    Core/Src/boot_w25q.c
    Core/Src/system_stm32h7xx.c
    ${HAL_DRIVER_SRC}
)

# 链接脚本：本工程使用 boot.ld（0x08000000，64KB）
target_link_options(boot PRIVATE
    -T "${CMAKE_SOURCE_DIR}/boot.ld"
)

# 生成 boot.bin / boot.hex
add_custom_command(TARGET boot POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:boot>
            ${CMAKE_BINARY_DIR}/boot.bin
    COMMENT "Building BIN : boot.bin"
)
add_custom_command(TARGET boot POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex
            $<TARGET_FILE:boot>
            ${CMAKE_BINARY_DIR}/boot.hex
    COMMENT "Building HEX : boot.hex"
)
