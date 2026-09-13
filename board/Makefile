# ==========================================================================
# 板端客户端 Makefile（在电脑 WSL 中交叉编译，生成 GEC6818(ARM) 可执行文件）
#
# 用法：
#   make            # 用本机 arm-linux-gcc 5.4.0 交叉编译生成 client（触摸屏版）
#   make touch_test # 单独编译触摸屏标定工具 touch_test（不依赖字库）
#   make clean      # 删除生成的文件
#
# 换工具链时覆盖 CROSS_COMPILE 即可，例如：
#   make CROSS_COMPILE=arm-cortex_a8-linux-gnueabi-
#   make CROSS_COMPILE=                      # 留空=用本机 gcc（板上自带 gcc 时）
# ==========================================================================

# 交叉编译工具链前缀（默认指向 WSL 中已安装的 arm 5.4.0 工具链，可用 ?= 覆盖）
CROSS_COMPILE ?= /opt/usr/local/arm/5.4.0/usr/bin/arm-linux-
CC      := $(CROSS_COMPILE)gcc

TARGET  := client
# client.c：主程序（录音+上传+收结果+界面流程）
# touch_ui.c：触摸屏读取、坐标映射、按钮绘制与交互事件循环
SRC     := client.c touch_ui.c
HDR     := font.h touch_ui.h
# -L. 在当前目录找 libfont.a；-lfont 链接字库静态库；-lm 提供数学函数（字库内部用到）
CFLAGS  := -g -Wall
LDFLAGS := -L. -lfont -lm

# 工具链 cc1 运行时依赖自带的 libmpfr.so.4 等，把工具链 lib 目录加入库搜索路径
TOOLCHAIN_LIB := /opt/usr/local/arm/5.4.0/usr/lib

# 主程序：依赖全部源码、头文件和字库静态库
$(TARGET): $(SRC) $(HDR) libfont.a
	LD_LIBRARY_PATH=$(TOOLCHAIN_LIB) $(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)
	@echo "编译完成：$(TARGET)（ARM，触摸屏交互版），用 tftp 传到 GEC6818 开发板运行"

# 触摸屏标定工具：只编译 touch_test.c，不链接 libfont.a，便于单独排查触摸方向
touch_test: touch_test.c
	LD_LIBRARY_PATH=$(TOOLCHAIN_LIB) $(CC) $(CFLAGS) touch_test.c -o touch_test
	@echo "编译完成：touch_test（ARM），传到板子上运行可查看触摸原始坐标"

clean:
	rm -f $(TARGET) touch_test *.o

.PHONY: clean touch_test
