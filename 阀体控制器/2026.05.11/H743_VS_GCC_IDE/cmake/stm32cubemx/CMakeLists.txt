cmake_minimum_required(VERSION 3.22)
enable_language(C ASM)

# 宏定义
set(MX_Defines_Syms
    USE_PWR_LDO_SUPPLY
    USE_HAL_DRIVER
    STM32H743xx
    $<$<CONFIG:Debug>:DEBUG>
)

# 所有头文件路径（包含 UserCode + FreeModbus 全部子文件夹）
set(MX_Inc
    ${CMAKE_CURRENT_LIST_DIR}/../../Core/Inc
    ${CMAKE_CURRENT_LIST_DIR}/../../LWIP/App
    ${CMAKE_CURRENT_LIST_DIR}/../../LWIP/Target
    ${CMAKE_CURRENT_LIST_DIR}/../../Middlewares/Third_Party/LwIP/src/include
    ${CMAKE_CURRENT_LIST_DIR}/../../Middlewares/Third_Party/LwIP/system
    ${CMAKE_CURRENT_LIST_DIR}/../../Drivers/STM32H7xx_HAL_Driver/Inc
    ${CMAKE_CURRENT_LIST_DIR}/../../Drivers/STM32H7xx_HAL_Driver/Inc/Legacy
    ${CMAKE_CURRENT_LIST_DIR}/../../Middlewares/Third_Party/FreeRTOS/Source/include
    ${CMAKE_CURRENT_LIST_DIR}/../../Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2
    ${CMAKE_CURRENT_LIST_DIR}/../../Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F
    ${CMAKE_CURRENT_LIST_DIR}/../../Drivers/BSP/Components/lan8742
    ${CMAKE_CURRENT_LIST_DIR}/../../Drivers/CMSIS/Device/ST/STM32H7xx/Include
    ${CMAKE_CURRENT_LIST_DIR}/../../Drivers/CMSIS/Include

    # UserCode 根目录 + 所有驱动子文件夹
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Drivers
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Drivers/Buzzer
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Drivers/Clock
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Drivers/Encoder
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Drivers/Motor       # 电机驱动路径
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Drivers/Sht40
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Logic

    # Third_Party 全量路径
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Third_Party
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Third_Party/cJSON
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Third_Party/Fal
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Third_Party/FlashDB
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Third_Party/LetterShell
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Third_Party/SFUD
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Third_Party/U8G2     # U8G2 完整路径

    # FreeModbus 全部子文件夹
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Third_Party/FreeModbus
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Third_Party/FreeModbus/ascii
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Third_Party/FreeModbus/functions
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Third_Party/FreeModbus/include
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Third_Party/FreeModbus/port
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Third_Party/FreeModbus/rtu
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/Third_Party/FreeModbus/tcp
)

# ===================== 批量自动导入所有 .c 文件 =====================
# HAL 库
file(GLOB_RECURSE DRV_SRC
    ${CMAKE_CURRENT_LIST_DIR}/../../Drivers/STM32H7xx_HAL_Driver/Src/*.c
    ${CMAKE_CURRENT_LIST_DIR}/../../Drivers/BSP/Components/lan8742/*.c
)

# FreeRTOS
file(GLOB_RECURSE RTOS_SRC
    ${CMAKE_CURRENT_LIST_DIR}/../../Middlewares/Third_Party/FreeRTOS/Source/*.c
)

# LwIP
file(GLOB_RECURSE LWIP_SRC
    ${CMAKE_CURRENT_LIST_DIR}/../../Middlewares/Third_Party/LwIP/src/*.c
    ${CMAKE_CURRENT_LIST_DIR}/../../Middlewares/Third_Party/LwIP/system/OS/*.c
)

# 用户代码（包含 FreeModbus 所有 .c）
file(GLOB_RECURSE USER_SRC
    ${CMAKE_CURRENT_LIST_DIR}/../../UserCode/*.c
)

# ===================== 库链接 =====================
add_library(stm32cubemx INTERFACE)
target_include_directories(stm32cubemx INTERFACE ${MX_Inc})
target_compile_definitions(stm32cubemx INTERFACE ${MX_Defines_Syms})

add_library(STM32_Drivers OBJECT ${DRV_SRC})
target_link_libraries(STM32_Drivers PUBLIC stm32cubemx)

add_library(FreeRTOS OBJECT ${RTOS_SRC})
target_link_libraries(FreeRTOS PUBLIC stm32cubemx)

add_library(LwIP OBJECT ${LWIP_SRC})
target_link_libraries(LwIP PUBLIC stm32cubemx)

add_library(User_Code OBJECT ${USER_SRC})
target_link_libraries(User_Code PUBLIC stm32cubemx)