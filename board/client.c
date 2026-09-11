/*****************************************************************************
 * 文件名称：client.c
 * 运行平台：GEC6818 开发板（ARM Linux，板载 800x480 LCD）
 * 功能描述：AI 语音助手【板端客户端】
 *      1. 键盘输入 1 后，调用板端 ALSA 工具 arecord 录制 3 秒语音
 *         （16kHz、单声道、16bit、WAV 格式，讯飞 IAT 要求的格式）
 *      2. 通过 TCP 把录好的 cmd.wav 上传给电脑（WSL）上的服务器
 *      3. 等待服务器返回：讯飞语音听写 + 智谱大模型处理后的 AI 回答
 *      4. 调用 libfont.a 字库，把 AI 回答显示到 LCD 屏幕上（自动换行）
 *      5. 循环对话，输入 0 退出
 *
 * 通信协议（与 server.c 严格对应，一条 TCP 连接完成全部流程）：
 *      板端 -> 服务器 : 文件名长度(4字节,网络字节序)
 *                      文件大小(4字节,网络字节序)
 *                      文件名字符串(不含'\0')
 *      服务器 -> 板端 : "OK"（2字节，确认文件信息，可以开始传内容）
 *      板端 -> 服务器 : WAV 文件二进制内容（按文件大小严格发完）
 *      服务器 -> 板端 : "OK"（2字节，确认文件已完整落盘）
 *      .............. 服务器内部：讯飞 IAT 转文字 -> 智谱 AI 生成回答 .........
 *      服务器 -> 板端 : 回答文本长度(4字节,网络字节序)
 *                      回答文本内容(UTF-8 编码)
 *
 * 交叉编译（在电脑 WSL 中执行，生成 ARM 可执行文件后用 sftp/ftp 传到板子）：
 *      make                # 默认调用 arm-linux-gcc，若工具链名字不同请改 Makefile
 * 板端运行：
 *      ./client
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>

#include "font.h"   /* 板端 TTF 字库接口（libfont.a），LCD 显示汉字用 */

/*===========================  用户配置区（按需修改）  ===========================*/
#define SERVER_IP       "169.254.68.229"   /* 电脑(WSL服务器)的 IP：实测 169.254.68.229 */
#define SERVER_PORT     8888                /* 与服务器监听端口保持一致 */
#define WAV_FILE        "cmd.wav"           /* 录音生成、并上传给服务器的文件名 */

/* arecord 是 ALSA-utils 提供的录音命令，板端已验证可用：
 * -d 3      录音 3 秒
 * -c 1      单声道（讯飞要求）
 * -r 16000  采样率 16kHz（讯飞要求）
 * -t wav    输出 wav 封装
 * -f S16_LE 采样格式 有符号16位小端（讯飞要求）
 */
#define RECORD_CMD      "arecord -d 3 -c 1 -r 16000 -t wav -f S16_LE cmd.wav"

#define FONT_PATH       "/usr/share/fonts/DroidSansFallback.ttf" /* 板端中文字体路径 */
#define LCD_DEV         "/dev/fb0"          /* LCD 帧缓冲设备节点 */
#define LCD_WIDTH       800                 /* LCD 宽（像素） */
#define LCD_HEIGHT      480                 /* LCD 高（像素） */
#define FONT_PIXELS     32                  /* 字号（像素） */
#define LINE_HEIGHT     40                  /* 每行文字的行高（字号+行距） */
#define MARGIN          20                  /* 屏幕边距 */
#define BUF_SIZE        4096                /* 收发缓冲区大小 */
#define MAX_NAME        255                 /* 协议允许的最大文件名长度 */
#define MAX_ANSWER      4096                /* AI 回答最大长度 */
#define ACK_TEXT        "OK"                /* 与服务器约定的应答字符串 */
#define ACK_SIZE        2                   /* "OK" 长度，不含字符串结束符 */
/*===========================  用户配置区结束  ==================================*/

/*--------------------------- 全局 LCD / 字库对象  -----------------------------*/
static int              g_lcd_fd = -1;      /* LCD 设备文件描述符 */
static unsigned int    *g_lcd_mp = NULL;    /* LCD 显存映射首地址 */
static font            *g_font  = NULL;     /* 字库对象 */

/*
 * send_all：可靠发送
 * TCP 是字节流协议，一次 send 不保证把数据全部发出去，
 * 所以必须循环发送，直到 length 个字节全部发完为止。
 * 返回 0 成功，-1 失败。
 */
static int send_all(int fd, const void *buffer, size_t length)
{
    const char *data = (const char *)buffer;
    while (length > 0) {
        ssize_t sent = send(fd, data, length, 0);
        if (sent < 0) {
            if (errno == EINTR) continue;   /* 被信号中断则重试 */
            perror("send");
            return -1;
        }
        data += sent;
        length -= (size_t)sent;
    }
    return 0;
}

/*
 * recv_all：可靠接收（必须收满 length 个字节才返回）
 * TCP 一次 recv 可能只收到一部分数据，循环收满指定长度。
 * 返回 0 成功，-1 失败（对端关闭或出错）。
 */
static int recv_all(int fd, void *buffer, size_t length)
{
    char *data = (char *)buffer;
    while (length > 0) {
        ssize_t received = recv(fd, data, length, 0);
        if (received == 0)  return -1;      /* 对端关闭了连接 */
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
 * lcd_init：打开 /dev/fb0 并把显存映射到用户空间，同时加载 TTF 字库。
 * 字库加载失败不影响主流程（只是没有画面，终端仍会打印）。
 */
static void lcd_init(void)
{
    /* 1.打开 LCD 帧缓冲设备 */
    g_lcd_fd = open(LCD_DEV, O_RDWR);
    if (g_lcd_fd < 0) {
        perror("打开 LCD 设备失败");
        return;
    }
    /* 2.把整块 800x480x4 字节的显存映射到进程地址空间，
     *      之后向 g_lcd_mp 写像素就等于直接写屏幕。 */
    g_lcd_mp = mmap(NULL, LCD_WIDTH * LCD_HEIGHT * 4,
                    PROT_READ | PROT_WRITE, MAP_SHARED, g_lcd_fd, 0);
    if (g_lcd_mp == MAP_FAILED) {
        perror("映射 LCD 显存失败");
        g_lcd_mp = NULL;
        close(g_lcd_fd);
        g_lcd_fd = -1;
        return;
    }
    /* 3.加载中文 TTF 字库并设置字号 */
    g_font = fontLoad((char *)FONT_PATH);
    if (g_font == NULL) {
        printf("警告：字库加载失败（%s），将只在终端输出\n", FONT_PATH);
        return;
    }
    fontSetSize(g_font, FONT_PIXELS);
}

/*
 * lcd_release：释放字库、解除显存映射并关闭设备
 */
static void lcd_release(void)
{
    if (g_font) {
        fontUnload(g_font);
        g_font = NULL;
    }
    if (g_lcd_mp) {
        munmap(g_lcd_mp, LCD_WIDTH * LCD_HEIGHT * 4);
        g_lcd_mp = NULL;
    }
    if (g_lcd_fd >= 0) {
        close(g_lcd_fd);
        g_lcd_fd = -1;
    }
}

/*
 * utf8_first_char_len：根据 UTF-8 首字节判断该字符占几个字节
 * UTF-8 编码规则：
 *      0xxxxxxx                1 字节（英文/数字/符号）
 *      110xxxxx 10xxxxxx       2 字节
 *      1110xxxx 10xxxxxx ...   3 字节（汉字通常是 3 字节）
 *      11110xxx ...            4 字节（emoji 等）
 */
static int utf8_first_char_len(unsigned char c)
{
    if (c < 0x80)        return 1;
    if ((c >> 5) == 0x6) return 2;   /* 110xxxxx */
    if ((c >> 4) == 0xE) return 3;   /* 1110xxxx */
    if ((c >> 3) == 0x1E) return 4;  /* 11110xxx */
    return 1;                        /* 异常字节按 1 处理，避免死循环 */
}

/*
 * utf8_char_width：估算一个 UTF-8 字符的显示宽度（像素）
 * 汉字/全角字符宽度≈字号，英文/数字宽度≈字号的一半。
 */
static int utf8_char_width(unsigned char c)
{
    if (c < 0x80) return FONT_PIXELS / 2;  /* 半角英文 */
    return FONT_PIXELS;                     /* 全角（含中文） */
}

/*
 * lcd_draw_lines：在白色画布上按自动换行绘制多行 UTF-8 文本，
 *                 画完一次性刷到 LCD。
 * 参数：lines[] 文本行数组（每行内部仍会按屏幕宽度自动折行），
 *       line_num 行数；colors[] 每行颜色（如黑色 0x00000000）。
 */
static void lcd_draw_lines(const char *lines[], const unsigned int colors[], int line_num)
{
    int i;
    if (g_lcd_mp == NULL || g_font == NULL) return;  /* 没有 LCD/字库时直接跳过 */

    /* 1.创建一张 800x480、32位色、初始化为白色(0xFFFFFFFF)的画布 */
    bitmap *screen = createBitmapWithInit(LCD_WIDTH, LCD_HEIGHT, 4, 0xFFFFFFFF);
    if (screen == NULL) {
        printf("创建画布失败\n");
        return;
    }

    int y = MARGIN;   /* 当前绘制行的 y 坐标 */
    for (i = 0; i < line_num; i++) {
        const char *p = lines[i];
        int x = MARGIN;
        while (p != NULL && *p != '\0') {
            int bytes = utf8_first_char_len((unsigned char)*p);
            int w = utf8_char_width((unsigned char)*p);
            char one[5] = {0};    /* 单个 UTF-8 字符，最多 4 字节 + '\0' */
            memcpy(one, p, bytes);

            /* 遇到显式换行符：光标移动到下一行行首 */
            if (one[0] == '\n') {
                x = MARGIN;
                y += LINE_HEIGHT;
                p += bytes;
                continue;
            }

            /* 当前行剩余宽度放不下该字符时，自动折行 */
            if (x + w > LCD_WIDTH - MARGIN) {
                x = MARGIN;
                y += LINE_HEIGHT;
            }
            /* 超出屏幕高度就不再绘制（防止写爆画布） */
            if (y + LINE_HEIGHT > LCD_HEIGHT) break;

            /* 把这一个字符画到画布的 (x,y) 位置，最后一个参数是允许的最大宽度 */
            fontPrint(g_font, screen, x, y, one, colors[i], w + 2);
            x += w;
            p += bytes;
        }
        y += LINE_HEIGHT;  /* 一个逻辑行结束，额外换行 */
    }

    /* 2.把整张画布一次性搬到 LCD 显存，画面即更新 */
    show_font_to_lcd(g_lcd_mp, 0, 0, screen);

    /* 3.画布用完即销毁，避免内存泄漏 */
    destroyBitmap(screen);
}

/* 便捷封装：只显示一到两行提示信息（如“正在录音...”） */
static void lcd_show_status(const char *line1, const char *line2)
{
    const char *lines[2];
    unsigned int colors[2] = {0xFF000000, 0xFF333333};
    lines[0] = line1 ? line1 : "";
    lines[1] = line2 ? line2 : "";
    lcd_draw_lines(lines, colors, line2 ? 2 : 1);
}

/* 显示 AI 回答：第一行是标题，第二行开始是自动换行的回答正文 */
static void lcd_show_answer(const char *answer)
{
    /* 回答本身可能很长，内部会自动折行，这里整体作为一个逻辑行传入 */
    const char *title = "AI 回答：";
    char body[MAX_ANSWER];
    snprintf(body, sizeof(body), "%s", answer ? answer : "（空）");

    const char *lines[2];
    unsigned int colors[2] = {0xFF0000AA, 0xFF000000};  /* 标题深蓝，正文黑色 */
    lines[0] = title;
    lines[1] = body;
    lcd_draw_lines(lines, colors, 2);
}

/*
 * record_audio：调用 ALSA 的 arecord 录制 3 秒语音
 * system() 会阻塞直到录音结束；返回 0 表示命令执行成功。
 */
static int record_audio(void)
{
    printf(">>> 开始录音（3秒）...\n");
    lcd_show_status("正在录音（3秒）...", "请对着麦克风说话");
    int ret = system(RECORD_CMD);
    if (ret != 0) {
        printf("录音失败，arecord 返回值：%d\n", ret);
        lcd_show_status("录音失败", "请检查麦克风/ALSA配置");
        return -1;
    }
    printf(">>> 录音结束，已生成 %s\n", WAV_FILE);
    return 0;
}

/*
 * upload_wav：连接服务器并按通信协议上传 WAV 文件，
 *             上传完成后保持连接不断开（后面还要用它接收 AI 回答）。
 * 返回：成功返回套接字 fd（>=0），失败返回 -1。
 */
static int upload_wav(const char *file_path)
{
    FILE *file = NULL;
    struct stat file_info;
    struct sockaddr_in server_addr;
    uint32_t name_len, file_size;
    char ack[ACK_SIZE];
    char buf[BUF_SIZE];
    const char *file_name;
    int sock_fd, ret;

    /* 1.以二进制只读方式打开录音文件 */
    file = fopen(file_path, "rb");
    if (file == NULL) {
        perror("打开录音文件失败");
        return -1;
    }
    /* 取文件大小（stat 比 fseek/ftell 更直接） */
    if (stat(file_path, &file_info) < 0 || file_info.st_size <= 0) {
        perror("获取录音文件大小失败");
        fclose(file);
        return -1;
    }

    /* 只取纯文件名（去掉路径），服务器只按文件名保存 */
    file_name = strrchr(file_path, '/');
    file_name = (file_name == NULL) ? file_path : file_name + 1;
    name_len  = (uint32_t)strlen(file_name);
    file_size = (uint32_t)file_info.st_size;
    if (name_len == 0 || name_len > MAX_NAME) {
        printf("文件名不合法\n");
        fclose(file);
        return -1;
    }

    /* 2.创建 IPv4 TCP 套接字 */
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("创建套接字失败");
        fclose(file);
        return -1;
    }

    /* 3.填写服务器地址并发起连接 */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) != 1) {
        printf("服务器 IP 配置错误：%s\n", SERVER_IP);
        close(sock_fd);
        fclose(file);
        return -1;
    }
    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("连接服务器失败");
        close(sock_fd);
        fclose(file);
        return -1;
    }
    printf(">>> 已连接服务器 %s:%d\n", SERVER_IP, SERVER_PORT);

    /* 4.发送协议头：文件名长度 + 文件大小（均转成网络字节序，保证跨平台一致） */
    uint32_t name_len_net  = htonl(name_len);
    uint32_t file_size_net = htonl(file_size);
    if (send_all(sock_fd, &name_len_net, 4) < 0 ||
        send_all(sock_fd, &file_size_net, 4) < 0 ||
        send_all(sock_fd, file_name, name_len) < 0) {
        printf("发送文件信息失败\n");
        close(sock_fd);
        fclose(file);
        return -1;
    }

    /* 5.等待服务器回复 "OK"，确认后才开始发文件内容 */
    if (recv_all(sock_fd, ack, ACK_SIZE) < 0 ||
        memcmp(ack, ACK_TEXT, ACK_SIZE) != 0) {
        printf("服务器未确认文件信息\n");
        close(sock_fd);
        fclose(file);
        return -1;
    }
    printf(">>> 开始上传 %s（%u 字节）\n", file_name, file_size);

    /* 6.循环读取本地文件并发送，严格发完 file_size 个字节 */
    while ((ret = (int)fread(buf, 1, sizeof(buf), file)) > 0) {
        if (send_all(sock_fd, buf, (size_t)ret) < 0) {
            perror("发送文件内容失败");
            close(sock_fd);
            fclose(file);
            return -1;
        }
    }
    if (ferror(file)) {
        printf("读取录音文件出错\n");
        close(sock_fd);
        fclose(file);
        return -1;
    }

    /* 7.等待服务器第二次回复 "OK"，表示文件已完整写入磁盘 */
    if (recv_all(sock_fd, ack, ACK_SIZE) < 0 ||
        memcmp(ack, ACK_TEXT, ACK_SIZE) != 0) {
        printf("服务器未确认文件接收完成\n");
        close(sock_fd);
        fclose(file);
        return -1;
    }
    printf(">>> 录音上传完成，等待 AI 处理...\n");
    fclose(file);
    return sock_fd;   /* 注意：套接字不关闭，留给接收回答用 */
}

/*
 * recv_answer：从同一条连接接收服务器返回的 AI 回答
 * 协议：4 字节网络字节序长度 + 文本内容
 */
static int recv_answer(int sock_fd, char *answer, size_t max_len)
{
    uint32_t len_net = 0, len = 0;
    if (recv_all(sock_fd, &len_net, 4) < 0) {
        printf("接收回答长度失败\n");
        return -1;
    }
    len = ntohl(len_net);
    if (len == 0 || len >= max_len) {
        printf("回答长度异常：%u\n", len);
        return -1;
    }
    if (recv_all(sock_fd, answer, len) < 0) {
        printf("接收回答内容失败\n");
        return -1;
    }
    answer[len] = '\0';
    return 0;
}

int main(void)
{
    int input = 0;

    /* 初始化 LCD 与字库（失败不影响终端流程） */
    lcd_init();
    lcd_show_status("AI 语音助手已就绪", "键盘输入 1 开始录音，输入 0 退出");
    printf("==============================================\n");
    printf(" AI 语音助手板端已启动\n");
    printf(" 输入 1 开始录音对话，输入 0 退出\n");
    printf("==============================================\n");

    while (1) {
        printf("\n请输入指令：");
        if (scanf("%d", &input) != 1) {      /* 输入非数字时清空输入缓冲区，防止死循环 */
            int ch; while ((ch = getchar()) != '\n' && ch != EOF) {}
            printf("请输入数字 1 或 0\n");
            continue;
        }
        if (input == 0) break;               /* 退出主循环 */
        if (input != 1) {
            printf("无效指令，请输入 1 或 0\n");
            continue;
        }

        /* 第一步：板端 ALSA 录音 3 秒 */
        if (record_audio() < 0) continue;

        /* 第二步：连接服务器并上传录音 */
        lcd_show_status("正在连接服务器...", NULL);
        int sock_fd = upload_wav(WAV_FILE);
        if (sock_fd < 0) {
            lcd_show_status("连接/上传失败", "请检查网络与服务器是否启动");
            continue;
        }

        /* 第三步：等待服务器（讯飞听写 + 智谱AI）处理结果 */
        lcd_show_status("AI 正在思考...", "语音识别 + 大模型回答中");
        char answer[MAX_ANSWER] = {0};
        if (recv_answer(sock_fd, answer, sizeof(answer)) < 0) {
            lcd_show_status("接收 AI 回答失败", NULL);
            close(sock_fd);
            continue;
        }
        close(sock_fd);

        /* 第四步：终端 + LCD 同步显示 AI 回答 */
        printf("\n========== AI 回答 ==========\n%s\n=============================\n", answer);
        lcd_show_answer(answer);
    }

    lcd_show_status("程序已退出", NULL);
    lcd_release();
    printf("再见！\n");
    return 0;
}
