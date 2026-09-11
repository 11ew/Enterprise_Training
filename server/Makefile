# ==========================================================================
# 服务器端总 Makefile（在电脑 WSL 的本目录 new/server 下执行 make）
#   make         先编译讯飞 IAT 程序，再编译服务器主程序
#   make clean   清理所有编译产物和接收到的录音
# ==========================================================================

CC      := gcc
CFLAGS  := -g -Wall

.PHONY: all clean

all: iat/iat_online_sample server

# 递归进入 iat 目录编译讯飞语音听写程序
iat/iat_online_sample:
	make -C iat

# 服务器主程序（纯 socket + popen，不依赖额外库）
server: server.c
	$(CC) $(CFLAGS) server.c -o server
	@echo "服务器编译完成，运行方式：./server"

clean:
	make -C iat clean
	rm -f server cmd.wav
