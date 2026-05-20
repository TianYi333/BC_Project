cmake_minimum_required(VERSION 3.22)
enable_language(C ASM)

# 宏
set(MX_Defines_Syms
    USE_PWR_LDO_SUPPLY
    USE_HAL_DRIVER
    STM32H743xx
)

# 头文件路径（完整、稳定、永不报错）
set(MX_Include_Dirs
    ${STM32CUBEMX_ROOT}/Core/Inc
    ${STM32CUBEMX_ROOT}/Drivers/STM32H7xx_HAL_Driver/Inc
    ${STM32CUBEMX_ROOT}/Drivers/STM32H7xx_HAL_Driver/Inc/Legacy
    ${STM32CUBEMX_ROOT}/Drivers/CMSIS/Device/ST/STM32H7xx/Include
    ${STM32CUBEMX_ROOT}/Drivers/CMSIS/Include

    # FreeRTOS
    ${STM32CUBEMX_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/include
    ${STM32CUBEMX_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2
    ${STM32CUBEMX_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F

    # LWIP 完整路径（永不报错）
    ${STM32CUBEMX_ROOT}/Middlewares/Third_Party/LwIP/src/include
    ${STM32CUBEMX_ROOT}/Middlewares/Third_Party/LwIP/src/include/ipv4
    ${STM32CUBEMX_ROOT}/Middlewares/Third_Party/LwIP/src/include/lwip
    ${STM32CUBEMX_ROOT}/Middlewares/Third_Party/LwIP/src/include/netif
    ${STM32CUBEMX_ROOT}/Middlewares/Third_Party/LwIP/system
    ${STM32CUBEMX_ROOT}/LWIP/App
    ${STM32CUBEMX_ROOT}/LWIP/Target

    # ==============================
    # 你所有的自定义路径（全部补齐）
    # ==============================
    ${STM32CUBEMX_ROOT}/UserCode
    ${STM32CUBEMX_ROOT}/UserCode/Drivers/Buzzer
    ${STM32CUBEMX_ROOT}/UserCode/Drivers/Clock
    ${STM32CUBEMX_ROOT}/UserCode/Drivers/Encoder
    ${STM32CUBEMX_ROOT}/UserCode/Drivers/Motor
    ${STM32CUBEMX_ROOT}/UserCode/Drivers/Oled
    ${STM32CUBEMX_ROOT}/UserCode/Drivers/Sht40
    ${STM32CUBEMX_ROOT}/UserCode/Logic
    ${STM32CUBEMX_ROOT}/UserCode/Drivers/ETH

    # U8G2
    ${STM32CUBEMX_ROOT}/UserCode/Third_Party/U8G2

    # FreeModbus 完整路径（这次永久加上）
    ${STM32CUBEMX_ROOT}/UserCode/Third_Party/FreeModbus
    ${STM32CUBEMX_ROOT}/UserCode/Third_Party/FreeModbus/include
    ${STM32CUBEMX_ROOT}/UserCode/Third_Party/FreeModbus/port
    ${STM32CUBEMX_ROOT}/UserCode/Third_Party/FreeModbus/ascii
    ${STM32CUBEMX_ROOT}/UserCode/Third_Party/FreeModbus/rtu
    ${STM32CUBEMX_ROOT}/UserCode/Third_Party/FreeModbus/functions

    # 其他第三方库
    ${STM32CUBEMX_ROOT}/UserCode/Third_Party/cJSON
    ${STM32CUBEMX_ROOT}/UserCode/Third_Party/Fal
    ${STM32CUBEMX_ROOT}/UserCode/Third_Party/FlashDB
    ${STM32CUBEMX_ROOT}/UserCode/Third_Party/LetterShell
    ${STM32CUBEMX_ROOT}/UserCode/Third_Party/SFUD
)

# 自动扫描所有驱动文件
file(GLOB_RECURSE USER_SRC
    ${STM32CUBEMX_ROOT}/UserCode/*.c
)
list(FILTER USER_SRC EXCLUDE REGEX "LetterShell")

file(GLOB_RECURSE HAL_SRC
    ${STM32CUBEMX_ROOT}/Drivers/STM32H7xx_HAL_Driver/Src/*.c
)

file(GLOB_RECURSE RTOS_SRC
    ${STM32CUBEMX_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/*.c
)

file(GLOB_RECURSE LWIP_SRC
    ${STM32CUBEMX_ROOT}/Middlewares/Third_Party/LwIP/src/*.c
)

# 加入工程
target_sources(${PROJECT_NAME} PRIVATE
    ${STM32CUBEMX_ROOT}/Core/Src/main.c
    ${STM32CUBEMX_ROOT}/Core/Src/gpio.c
    ${STM32CUBEMX_ROOT}/Core/Src/freertos.c
    ${STM32CUBEMX_ROOT}/Core/Src/dma.c
    ${STM32CUBEMX_ROOT}/Core/Src/fdcan.c
    ${STM32CUBEMX_ROOT}/Core/Src/i2c.c
    ${STM32CUBEMX_ROOT}/Core/Src/rtc.c
    ${STM32CUBEMX_ROOT}/Core/Src/spi.c
    ${STM32CUBEMX_ROOT}/Core/Src/usart.c
    ${STM32CUBEMX_ROOT}/Core/Src/stm32h7xx_it.c
    ${STM32CUBEMX_ROOT}/Core/Src/stm32h7xx_hal_msp.c
    ${STM32CUBEMX_ROOT}/Core/Src/stm32h7xx_hal_timebase_tim.c
    ${STM32CUBEMX_ROOT}/Core/Src/sysmem.c
    ${STM32CUBEMX_ROOT}/Core/Src/syscalls.c
    ${STM32CUBEMX_ROOT}/Core/Src/system_stm32h7xx.c

    ${LWIP_SRC}
    ${HAL_SRC}
    ${RTOS_SRC}
    ${LWIP_SRC}
    ${USER_SRC}
)

# 应用配置
target_include_directories(${PROJECT_NAME} PRIVATE ${MX_Include_Dirs})
target_compile_definitions(${PROJECT_NAME} PRIVATE ${MX_Defines_Syms})