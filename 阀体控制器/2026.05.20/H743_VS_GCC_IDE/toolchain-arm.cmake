# 指定交叉编译器路径
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER D:/APP/GCC-ARM/bin/arm-none-eabi-gcc.exe)
set(CMAKE_CXX_COMPILER D:/APP/GCC-ARM/bin/arm-none-eabi-g++.exe)
set(CMAKE_ASM_COMPILER D:/APP/GCC-ARM/bin/arm-none-eabi-gcc.exe)

# 强制跳过编译器测试，避免它生成 .exe
set(CMAKE_C_COMPILER_WORKS ON CACHE BOOL "" FORCE)
set(CMAKE_CXX_COMPILER_WORKS ON CACHE BOOL "" FORCE)

# 告诉 CMake 这是嵌入式裸机程序，不支持 Windows 链接选项
set(CMAKE_EXE_LINKER_FLAGS_INIT "")