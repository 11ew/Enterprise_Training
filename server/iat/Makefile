# ==========================================================================
# 讯飞在线语音听写（IAT）程序 Makefile
# 运行平台：电脑 WSL（x86_64）
#
# 目录约定：
#   iat_online_sample.c   讯飞识别源码（只输出识别文本，方便 server.c 读取）
#   include/              讯飞 MSC 头文件
#   lib/libmsc.so         讯飞 MSC 64位动态库（与源码中的 appid 绑定，勿混用）
#
# 编译后生成可执行文件 iat_online_sample
# ==========================================================================

CC      := gcc
TARGET  := iat_online_sample
SRC     := iat_online_sample.c
CFLAGS  := -g -Wall -Iinclude

# -Llib            编译链接时到 lib 目录找 libmsc.so
# -Wl,-rpath,'$ORIGIN/lib'
#                  把库搜索路径写进可执行文件：运行时自动到【本程序所在目录/lib】
#                  下找 libmsc.so，无需手动 export LD_LIBRARY_PATH。
#                  Makefile 中 $ 要写成 $$ 转义，单引号保证链接器收到字面 $ORIGIN。
LDFLAGS := -Llib -lmsc -lrt -ldl -lpthread -lstdc++ -Wl,-rpath,'$$ORIGIN/lib'

$(TARGET): $(SRC) include/*.h lib/libmsc.so
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)
	@echo "讯飞 IAT 程序编译完成：$(TARGET)"

clean:
	rm -f $(TARGET) *.o

.PHONY: clean
