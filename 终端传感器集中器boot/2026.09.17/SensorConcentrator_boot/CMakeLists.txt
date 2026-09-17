cmake_minimum_required(VERSION 3.22)

# 工具链：若未在外层指定，则使用本工程自带的 gcc-arm-none-eabi 工具链文件。
# 用法：cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE ${CMAKE_SOURCE_DIR}/cmake/gcc-arm-none-eabi.cmake)
endif()

project(SensorConcentrator_boot C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# =====================================================================
# 独立 Bootloader 工程（SensorConcentrator_boot）
# ---------------------------------------------------------------------
# - 烧录在内部 Flash 低端 0x08000000（Bank1 SNB12, 128KB），负责校验 / 烧写
#   App 到 0x08100000（Bank2 SNB20~23, 512KB），并跳转。
# - 本工程完全自包含：CMSIS / 启动文件 / OTA 逻辑 / 分区定义均在本目录内，
#   不依赖原 App 工程 SensorConcentrator。可整体复制复用，修改 boot.ld 的
#   ORIGIN/SIZE 与 Core/Inc/ota_common.h 的分区偏移即可适配其它布局。
# - 与参考工程 D:\Desktop\boot（H7）同处理：独立顶层工程、自带工具链与
#   startup、无 HAL（寄存器级 Flash 操作，仅依赖 CMSIS）。
# =====================================================================

# 芯片级宏定义（寄存器级，不用 HAL）
add_compile_definitions(STM32F767xx)

# 头文件路径
include_directories(
    Core/Inc
    Drivers/CMSIS/Device/ST/STM32F7xx/Include
    Drivers/CMSIS/Include
)

# Bootloader 可执行文件（仅 CMSIS，无 HAL / FreeRTOS / LwIP）
add_executable(SensorConcentrator_boot
    startup_stm32f767xx.s
    Core/Src/main_boot.c
    Core/Src/boot_crc.c
    Core/Src/boot_uart.c
    Core/Src/system_stm32f7xx.c
)

# 链接脚本：本工程使用 boot.ld（0x08000000，128KB）；.RamFunc 使 RAM 段带执行
# 权限 → 关闭 RWX 告警。map 名由工具链统一生成为 SensorConcentrator_boot.map。
target_link_options(SensorConcentrator_boot PRIVATE
    -T "${CMAKE_SOURCE_DIR}/boot.ld"
    -Wl,--no-warn-rwx-segments
)

# 链接 libm（与 CubeMX 生成工程一致；boot_crc 等不强制需要，保留以防裁剪）
target_link_libraries(SensorConcentrator_boot PRIVATE m)

# 生成 bin / hex
add_custom_command(TARGET SensorConcentrator_boot POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:SensorConcentrator_boot>
            ${CMAKE_BINARY_DIR}/SensorConcentrator_boot.bin
    COMMENT "Building BIN : SensorConcentrator_boot.bin"
)
add_custom_command(TARGET SensorConcentrator_boot POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex
            $<TARGET_FILE:SensorConcentrator_boot>
            ${CMAKE_BINARY_DIR}/SensorConcentrator_boot.hex
    COMMENT "Building HEX : SensorConcentrator_boot.hex"
)
