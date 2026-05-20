@echo off
chcp 65001
echo 正在导出所有工程源码到 all_code.txt
type nul > all_code.txt
:: 导出核心编译配置、启动文件、链接脚本
echo ========== CMakeLists.txt ========== >> all_code.txt
type CMakeLists.txt >> all_code.txt
echo ========== toolchain-arm.cmake ========== >> all_code.txt
type toolchain-arm.cmake >> all_code.txt
echo ========== STM32H743XG_FLASH.ld ========== >> all_code.txt
type STM32H743XG_FLASH.ld >> all_code.txt
echo ========== startup_stm32h743xx.s ========== >> all_code.txt
type startup_stm32h743xx.s >> all_code.txt
echo ========== Makefile ========== >> all_code.txt
type Makefile >> all_code.txt
:: 递归导出所有.c .h文件
for /r %%f in (*.c,*.h) do (
echo. >> all_code.txt
echo ====================================== >> all_code.txt
echo 文件：%%f >> all_code.txt
echo ====================================== >> all_code.txt
type %%f >> all_code.txt
)
echo 导出完成！文件：all_code.txt
pause