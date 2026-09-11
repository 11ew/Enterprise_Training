/*****************************************************************************
 * 文件名称：server.c
 * 运行平台：电脑 WSL（Ubuntu Linux，x86_64）
 * 功能描述：AI 语音助手【服务器端】
 *      1. TCP 监听 8888 端口，等待 GEC6818 开发板连接
 *      2. 按协议接收板端上传的录音文件 cmd.wav（可靠收发，严格按长度收齐）
 *      3. popen 调用讯飞在线语音听写程序 iat/iat_online_sample，把语音转成文字
 *      4. popen 调用 python3 zhipu_ai.py，让智谱 GLM 大模型对文字生成回答
 *      5. 把 AI 回答通过同一条 TCP 连接回传给板端显示
 *      6. 断开连接，继续等待下一次对话（服务器常驻运行）
 *
 * 依赖：
 *      iat/iat_online_sample   讯飞 IAT 可执行文件（先在 iat 目录 make 生成）
 *      iat/lib/libmsc.so       讯飞 MSC 动态库（编译时已写 rpath，运行自动加载）
 *      zhipu_ai.py             智谱 AI 调用脚本（需要 pip install zhipuai）
 *
 * 编译（WSL 中，在本目录执行）：
 *      make                    # 会先编译讯飞程序，再编译本服务器
 * 运行：
 *      ./server                # 常驻，Ctrl+C 退出
 *
 * 通信协议（与 board/client.c 严格对应）：
 *      收：文件名长度(4B,网络序) + 文件大小(4B,网络序) + 文件名
 *      发："OK"
 *      收：文件内容（严格按文件大小收齐）
 *      发："OK"
 *      发：回答长度(4B,网络序) + 回答文本(UTF-8)
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>

/*===========================  用户配置区（按需修改）  ===========================*/
#define SERVER_PORT     8888            /* 监听端口，与板端 SERVER_PORT 一致 */
#define LISTEN_BACKLOG  5               /* 等待连接队列长度 */
#define BUF_SIZE        4096            /* 收发缓冲区大小 */
#define MAX_NAME        255             /* 协议允许的最大文件名长度 */
#define MAX_TEXT        4096            /* 讯飞识别文本最大长度 */
#define MAX_ANSWER      4096            /* 智谱回答最大长度（与板端一致） */
#define ACK_TEXT        "OK"
#define ACK_SIZE        2

/* 讯飞语音听写可执行文件路径（相对于本程序的运行目录 server/） */
#define IAT_PROGRAM     "./iat/iat_online_sample"
/* 智谱 AI 脚本路径 */
#define ZHIPU_SCRIPT    "python3 zhipu_ai.py"
/* 接收到的录音保存文件名 */
#define RECV_WAV        "cmd.wav"
/*===========================  用户配置区结束  ==================================*/

/*
 * send_all / recv_all：TCP 可靠收发，循环到全部完成，与板端实现一一对应。
 */
static int send_all(int fd, const void *buffer, size_t length)
{
    const char *data = (const char *)buffer;
    while (length > 0) {
        ssize_t sent = send(fd, data, length, 0);
        if (sent < 0) {
            if (errno == EINTR) continue;
            perror("send");
            return -1;
        }
        data += sent;
        length -= (size_t)sent;
    }
    return 0;
}

static int recv_all(int fd, void *buffer, size_t length)
{
    char *data = (char *)buffer;
    while (length > 0) {
        ssize_t received = recv(fd, data, length, 0);
        if (received == 0)  return -1;
        if (received < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            return -1;
        }
        data += received;
        length -= (size_t)received;
    }
    return 0;
}

/*
 * receive_wav：按协议接收板端上传的文件并写入磁盘
 * 返回 0 成功，-1 失败。
 */
static int receive_wav(int client_fd)
{
    uint32_t name_len_net = 0, file_size_net = 0;
    uint32_t name_len = 0, remaining = 0;
    char file_name[MAX_NAME + 1];
    char buf[BUF_SIZE];
    FILE *file;

    /* 1.收协议头：文件名长度、文件大小（网络字节序转主机字节序） */
    if (recv_all(client_fd, &name_len_net, 4) < 0 ||
        recv_all(client_fd, &file_size_net, 4) < 0) {
        printf("接收文件信息失败\n");
        return -1;
    }
    name_len  = ntohl(name_len_net);
    remaining = ntohl(file_size_net);
    if (name_len == 0 || name_len > MAX_NAME || remaining == 0) {
        printf("文件信息不合法（name_len=%u, size=%u）\n", name_len, remaining);
        return -1;
    }

    /* 2.收文件名 */
    if (recv_all(client_fd, file_name, name_len) < 0) {
        printf("接收文件名失败\n");
        return -1;
    }
    file_name[name_len] = '\0';

    /* 安全检查：拒绝路径穿越，服务器只在当前目录落盘 */
    if (strstr(file_name, "..") != NULL ||
        strchr(file_name, '/') != NULL ||
        strchr(file_name, '\\') != NULL) {
        printf("拒绝非法文件名：%s\n", file_name);
        return -1;
    }
    printf(">>> 板端上传文件：%s（%u 字节）\n", file_name, remaining);

    /* 3.文件信息无误，回复 "OK" 让板端开始发内容 */
    if (send_all(client_fd, ACK_TEXT, ACK_SIZE) < 0) return -1;

    /* 4.创建本地文件，严格按协议长度收齐（不依赖 read 返回 0 判断结束） */
    file = fopen(RECV_WAV, "wb");
    if (file == NULL) {
        perror("创建录音文件失败");
        return -1;
    }
    while (remaining > 0) {
        size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
        if (recv_all(client_fd, buf, chunk) < 0) {
            printf("接收文件内容失败\n");
            fclose(file);
            return -1;
        }
        if (fwrite(buf, 1, chunk, file) != chunk) {
            printf("写入录音文件失败\n");
            fclose(file);
            return -1;
        }
        remaining -= (uint32_t)chunk;
    }
    fclose(file);

    /* 5.文件完整落盘，第二次回复 "OK" */
    if (send_all(client_fd, ACK_TEXT, ACK_SIZE) < 0) return -1;
    printf(">>> 录音文件已保存：%s\n", RECV_WAV);
    return 0;
}

/*
 * run_command：执行一条 shell 命令，把标准输出全部收集到 out 中。
 * 2>/dev/null 由调用方拼接到命令里，用于屏蔽子进程的调试日志。
 * 返回 0 成功，-1 失败。
 */
static int run_command(const char *command, char *out, size_t out_size)
{
    FILE *pipe = popen(command, "r");
    size_t total = 0;
    char line[BUF_SIZE];

    if (pipe == NULL) {
        perror("popen 失败");
        return -1;
    }
    out[0] = '\0';
    while (fgets(line, sizeof(line), pipe) != NULL) {
        size_t len = strlen(line);
        if (total + len >= out_size) {       /* 防止输出缓冲区溢出 */
            printf("警告：子进程输出超过 %zu 字节，已截断\n", out_size);
            break;
        }
        memcpy(out + total, line, len);
        total += len;
        out[total] = '\0';
    }
    if (pclose(pipe) != 0) {
        printf("命令执行失败：%s\n", command);
        return -1;
    }
    return 0;
}

/*
 * trim：去掉字符串首尾的空白字符（空格、换行、回车、制表符），
 *       返回处理后的字符串指针（原地修改）。
 */
static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s == '\0') return s;
    end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) end--;
    *(end + 1) = '\0';
    return s;
}

/*
 * shell_single_quote：把任意文本安全地包进 shell 单引号参数。
 * shell 单引号内只有单引号本身需要特殊处理：
 *      don't  ->  'don'\''t'
 * 即：先关闭单引号，插入转义的单引号 \' ，再重新开单引号。
 * 这样语音识别出的中文/标点都不会打断 shell 命令。
 */
static void shell_single_quote(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    dst[j++] = '\'';
    for (size_t i = 0; src[i] != '\0' && j + 5 < dst_size; i++) {
        if (src[i] == '\'') {
            dst[j++] = '\'';
            dst[j++] = '\\';
            dst[j++] = '\'';
            dst[j++] = '\'';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j++] = '\'';
    dst[j]   = '\0';
}

/*
 * iat_recognize：调用讯飞 IAT 程序，把 wav 转成文字
 */
static int iat_recognize(const char *wav, char *text, size_t text_size)
{
    char command[512];
    /* 2>/dev/null：屏蔽讯飞 msc 库自身的日志，只保留识别结果文本 */
    snprintf(command, sizeof(command), "%s %s 2>/dev/null", IAT_PROGRAM, wav);
    printf(">>> 调用讯飞语音听写：%s\n", command);
    if (run_command(command, text, text_size) < 0) return -1;

    char *result = trim(text);
    memmove(text, result, strlen(result) + 1);   /* 去掉首尾空白后原地前移 */
    printf(">>> 讯飞识别结果：%s\n", text[0] ? text : "（空）");
    return 0;
}

/*
 * zhipu_chat：调用智谱 AI 脚本，把识别文本变成 AI 回答
 */
static int zhipu_chat(const char *question, char *answer, size_t answer_size)
{
    char quoted[MAX_TEXT * 2];
    char command[512 + sizeof(quoted)];

    shell_single_quote(question, quoted, sizeof(quoted));
    /* 2>/dev/null：屏蔽 python 的警告信息，stdout 只保留 AI 回答正文 */
    snprintf(command, sizeof(command), "%s %s 2>/dev/null", ZHIPU_SCRIPT, quoted);
    printf(">>> 调用智谱大模型，问题：%s\n", question);
    if (run_command(command, answer, answer_size) < 0) return -1;

    char *result = trim(answer);
    memmove(answer, result, strlen(result) + 1);
    return 0;
}

/*
 * send_answer：把 AI 回答按协议发回板端（4字节长度 + 文本）
 */
static int send_answer(int client_fd, const char *answer)
{
    uint32_t len = (uint32_t)strlen(answer);
    uint32_t len_net = htonl(len);
    printf(">>> 回传 AI 回答（%u 字节）：%s\n", len, answer);
    if (send_all(client_fd, &len_net, 4) < 0) return -1;
    if (send_all(client_fd, answer, len) < 0) return -1;
    return 0;
}

int main(void)
{
    int server_fd, client_fd;
    int opt = 1;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len;

    /* 1.创建 TCP 套接字 */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("创建套接字失败");
        return 1;
    }
    /* 端口复用，避免服务器重启时 "Address already in use" */
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* 2.绑定 0.0.0.0:8888（接收任意网卡上的连接） */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("绑定端口失败");
        close(server_fd);
        return 1;
    }

    /* 3.监听 */
    if (listen(server_fd, LISTEN_BACKLOG) < 0) {
        perror("监听失败");
        close(server_fd);
        return 1;
    }
    printf("==============================================\n");
    printf(" AI 语音助手服务器已启动，监听端口 %d\n", SERVER_PORT);
    printf(" 流程：收录音 -> 讯飞听写 -> 智谱AI -> 回传板端\n");
    printf(" Ctrl+C 退出\n");
    printf("==============================================\n");

    /* 4.常驻循环：一次连接处理一次对话，结束后继续等下一次 */
    while (1) {
        client_addr_len = sizeof(client_addr);
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_fd < 0) {
            perror("accept 失败");
            continue;
        }
        printf("\n---------- 新连接：%s ----------\n", inet_ntoa(client_addr.sin_addr));

        /* 第一步：接收板端录音 */
        if (receive_wav(client_fd) < 0) {
            printf("本次文件接收失败，跳过\n");
            close(client_fd);
            continue;
        }

        /* 第二步：讯飞 IAT 语音转文字 */
        char text[MAX_TEXT] = {0};
        char answer[MAX_ANSWER] = {0};
        if (iat_recognize(RECV_WAV, text, sizeof(text)) < 0 || text[0] == '\0') {
            /* 识别失败或没识别到内容，给板端一个友好提示而不是直接断开 */
            snprintf(answer, sizeof(answer), "没有听清，请靠近麦克风再说一次");
            send_answer(client_fd, answer);
            close(client_fd);
            continue;
        }

        /* 第三步：智谱大模型生成回答 */
        if (zhipu_chat(text, answer, sizeof(answer)) < 0 || answer[0] == '\0') {
            snprintf(answer, sizeof(answer), "AI 服务暂时不可用，请稍后再试");
        }

        /* 第四步：回传 AI 回答给板端 */
        if (send_answer(client_fd, answer) < 0) {
            printf("回传回答失败\n");
        }

        close(client_fd);
        printf("---------- 本次对话结束 ----------\n");
    }

    close(server_fd);   /* 正常不会走到这里（Ctrl+C 退出） */
    return 0;
}
