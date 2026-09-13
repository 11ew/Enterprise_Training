/*****************************************************************************
 * 文件名称：touch_ui.c
 * 运行平台：GEC6818 开发板（ARM Linux，800x480 电容触摸屏，触摸芯片 gslX680）
 * 功能描述：触摸屏交互界面模块【实现文件】，详见 touch_ui.h
 *
 * 背景知识（Linux 输入子系统 / evdev）：
 *      1. 板上 `cat /proc/bus/input/devices` 可查到：gslX680 的 Handlers 为
 *         mouse0 event0，所以触摸节点固定为 /dev/input/event0；
 *      2. 该节点支持 EV_KEY（按键/触摸按下抬起）和 EV_ABS（ABS_X/ABS_Y 绝对坐标）；
 *      3. 每触摸一次，内核按帧上报若干 struct input_event，顺序通常是：
 *              ABS_X -> ABS_Y -> BTN_TOUCH(=1 按下) -> SYN_REPORT   （按下）
 *              ABS_X -> ABS_Y ...（手指移动，可能多帧）
 *              BTN_TOUCH(=0 抬起) -> SYN_REPORT                     （抬起）
 *      4. gslX680 在 event0 上只上报“触摸”这一种 EV_KEY，因此本模块不硬编码
 *         BTN_TOUCH 的编号，只要收到 EV_KEY 且 value 为 1/0 就认为是按下/抬起，
 *         兼容性更好。
 *
 * 坐标标定：
 *      触摸原始坐标范围用 ioctl(EVIOCGABS) 自动读取并线性映射到屏幕分辨率；
 *      若上机后发现“上下颠倒/左右镜像/X和Y互换”，改下面 3 个标定宏重新 make 即可。
 *      可用配套小工具 touch_test（make touch_test）在板上点四个角看原始坐标。
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/input.h>      /* struct input_event / EV_ABS / EVIOCGABS 等 */

#include "touch_ui.h"

/*===========================  用户配置区（按需修改）  ===========================*/
#define TOUCH_DEV       "/dev/input/event0"  /* gslX680 触摸设备节点 */

/* ---- 触摸方向标定宏：上机发现轴不对时，把对应项改成 1 重新编译即可 ---- */
#define TOUCH_SWAP_XY   0   /* 1=交换 X/Y（触摸坐标相对屏幕转了 90 度时用） */
#define TOUCH_INVERT_X  0   /* 1=左右镜像（点左边响应在右边时用） */
#define TOUCH_INVERT_Y  0   /* 1=上下镜像（点上边响应在下边时用） */
/*===========================  用户配置区结束  ==================================*/

/*----------------------------- 模块内部全局对象 -------------------------------*/
static int              g_touch_fd = -1;    /* 触摸设备文件描述符 */
static unsigned int    *g_lcd_mp   = NULL;  /* LCD 显存映射首地址（用于刷屏） */
static font            *g_font     = NULL;  /* 字库对象 */
static int              g_screen_w = 800;   /* 屏幕宽 */
static int              g_screen_h = 480;   /* 屏幕高 */

/* 触摸原始坐标的最小/最大值（由 EVIOCGABS 读出，用于线性映射） */
static int              g_xmin = 0, g_xmax = 4095;
static int              g_ymin = 0, g_ymax = 4095;

/*
 * linear_map：把 [min,max] 区间内的原始值 v，线性映射到 [0,out_max]。
 * 同时做区间钳制，避免异常坐标算出屏幕外的点。
 */
static int linear_map(int v, int min, int max, int out_max)
{
    if (max <= min) return 0;                       /* 范围非法时防除 0 */
    if (v < min) v = min;
    if (v > max) v = max;
    return (int)(((long)(v - min) * out_max) / (max - min));
}

/*
 * raw_to_screen：原始触摸坐标 -> 屏幕像素坐标
 *      先按 EVIOCGABS 读到的范围线性映射到 800x480，
 *      再按标定宏决定是否交换/镜像，解决个别屏体方向不一致的问题。
 */
static void raw_to_screen(int raw_x, int raw_y, int *out_x, int *out_y)
{
    int x = linear_map(raw_x, g_xmin, g_xmax, g_screen_w - 1);
    int y = linear_map(raw_y, g_ymin, g_ymax, g_screen_h - 1);

    if (TOUCH_SWAP_XY) { int t = x; x = y; y = t; }
    if (TOUCH_INVERT_X) x = g_screen_w - 1 - x;
    if (TOUCH_INVERT_Y) y = g_screen_h - 1 - y;

    *out_x = x;
    *out_y = y;
}

/*
 * touch_ui_init：打开触摸节点、读取坐标范围、保存 LCD/字库对象
 */
int touch_ui_init(unsigned int *lcd_mp, font *f, int w, int h)
{
    struct input_absinfo abs;    /* EVIOCGABS 返回的绝对轴信息（含 min/max） */

    if (lcd_mp == NULL || f == NULL) {
        printf("touch_ui_init：LCD 显存或字库为空，无法初始化触摸界面\n");
        return -1;
    }
    g_lcd_mp   = lcd_mp;
    g_font     = f;
    g_screen_w = w;
    g_screen_h = h;

    /* 1.以阻塞只读方式打开触摸设备（后面配合 poll 使用） */
    g_touch_fd = open(TOUCH_DEV, O_RDONLY);
    if (g_touch_fd < 0) {
        perror("打开触摸屏设备失败 " TOUCH_DEV);
        return -1;
    }

    /* 2.读取 X/Y 轴的原始坐标范围（电容屏一般已配成与像素一致，读不到就用默认值） */
    if (ioctl(g_touch_fd, EVIOCGABS(ABS_X), &abs) == 0) {
        g_xmin = abs.minimum;
        g_xmax = abs.maximum;
    }
    if (ioctl(g_touch_fd, EVIOCGABS(ABS_Y), &abs) == 0) {
        g_ymin = abs.minimum;
        g_ymax = abs.maximum;
    }
    printf(">>> 触摸屏已打开：%s，原始坐标 X[%d,%d] Y[%d,%d]\n",
           TOUCH_DEV, g_xmin, g_xmax, g_ymin, g_ymax);
    return 0;
}

void touch_ui_exit(void)
{
    if (g_touch_fd >= 0) {
        close(g_touch_fd);
        g_touch_fd = -1;
    }
}

/*
 * ui_fill_rect：在画布指定矩形内逐像素填色。
 * bitmap 的 map 是字节数组，每像素 byteperpixel(=4) 个字节，
 * 直接按 32 位像素写入即可，颜色数值格式 0xAARRGGBB 与 fontPrint 一致。
 * 四个方向都做裁剪，保证按钮贴边或超出画布时也不会内存越界。
 */
void ui_fill_rect(bitmap *bm, int x, int y, int w, int h, u32 clr)
{
    int xx, yy, x0, y0, x1, y1;

    if (bm == NULL || bm->map == NULL || w <= 0 || h <= 0) return;

    /* 把矩形裁进画布范围 */
    x0 = x > 0 ? x : 0;
    y0 = y > 0 ? y : 0;
    x1 = x + w;  if (x1 > (int)bm->width)  x1 = (int)bm->width;
    y1 = y + h;  if (y1 > (int)bm->height) y1 = (int)bm->height;

    for (yy = y0; yy < y1; yy++) {
        /* 第 yy 行、第 x0 列对应的像素地址（每行 width 个像素，每像素 4 字节） */
        u32 *row = (u32 *)(bm->map +
                           ((yy * (int)bm->width + x0) * (int)bm->byteperpixel));
        for (xx = x0; xx < x1; xx++) {
            *row++ = clr;
        }
    }
}

/*
 * utf8_char_bytes：返回 UTF-8 首字节所属字符占几个字节
 * （与 client.c 中的规则一致：英文1字节、汉字通常3字节、emoji 4字节）
 */
static int utf8_char_bytes(unsigned char c)
{
    if (c < 0x80)        return 1;
    if ((c >> 5) == 0x6) return 2;   /* 110xxxxx */
    if ((c >> 4) == 0xE) return 3;   /* 1110xxxx，中文走这里 */
    if ((c >> 3) == 0x1E) return 4;  /* 11110xxx */
    return 1;
}

/*
 * ui_text_width_px：估算整行文本的像素宽度。
 * 半角（英文/数字）按字号一半宽，全角（中文等）按一个字号宽。
 */
int ui_text_width_px(const char *text, int fontsize)
{
    const unsigned char *p = (const unsigned char *)text;
    int total = 0;

    if (text == NULL) return 0;
    while (*p != '\0') {
        int n = utf8_char_bytes(*p);
        total += (n == 1) ? (fontsize / 2) : fontsize;
        p += n;
    }
    return total;
}

/*
 * ui_draw_centered_text：以 cx 为水平中心点画一行文字（垂直位置由 y 指定）。
 * 每次绘制前显式设置字号，避免和其他字号（标题/正文）互相影响。
 */
void ui_draw_centered_text(bitmap *bm, const char *text,
                           int cx, int y, int fontsize, u32 clr)
{
    int text_w, x;

    if (bm == NULL || g_font == NULL || text == NULL) return;
    fontSetSize(g_font, fontsize);
    text_w = ui_text_width_px(text, fontsize);
    x = cx - text_w / 2;                 /* 左边缘 = 中心 - 半个文字宽 */
    if (x < 0) x = 0;
    fontPrint(g_font, bm, x, y, (char *)text, clr, text_w + 4);
}

/*
 * ui_paint_button：画一个按钮 = 实心色块 + 居中白色文字。
 * pressed 为真时用更深的颜色，给用户“按下去了”的视觉反馈。
 */
void ui_paint_button(bitmap *bm, const UiButton *btn, int pressed)
{
    if (bm == NULL || btn == NULL) return;

    /* 1.画按钮底色 */
    ui_fill_rect(bm, btn->x, btn->y, btn->w, btn->h,
                 pressed ? btn->color_pressed : btn->color_normal);

    /* 2.画居中文字：水平居中于按钮，垂直也大致居中 */
    ui_draw_centered_text(bm, btn->text,
                          btn->x + btn->w / 2,
                          btn->y + (btn->h - btn->fontsize) / 2,
                          btn->fontsize, 0xFFFFFFFF);   /* 白色文字 */
}

/* hit_button：判断屏幕坐标(x,y)落在哪个按钮上，返回下标，都没命中返回 -1 */
static int hit_button(int x, int y, const UiButton buttons[], int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (x >= buttons[i].x && x <= buttons[i].x + buttons[i].w &&
            y >= buttons[i].y && y <= buttons[i].y + buttons[i].h) {
            return i;
        }
    }
    return -1;
}

/*
 * ui_run_screen：一屏交互的完整状态机（阻塞到点击按钮才返回）。
 *
 * 事件处理逻辑：
 *      EV_KEY value=1（按下）：记录当前按中的按钮下标并重绘高亮；
 *      EV_ABS      （移动） ：实时更新高亮按钮，滑出按钮则取消高亮；
 *      EV_KEY value=0（抬起）：若仍按在某个按钮上，视为一次有效“点击”，
 *                              返回该按钮 id；否则恢复常态继续等。
 * 这样可避免“按下去后滑走再松手”造成的误触发。
 */
int ui_run_screen(ui_paint_fn paint, void *userdata,
                  UiButton buttons[], int n)
{
    bitmap *screen;
    int raw_x = 0, raw_y = 0;        /* 最新原始坐标 */
    int cur_x = 0, cur_y = 0;        /* 最新屏幕坐标 */
    int is_down = 0;                 /* 手指当前是否按着屏幕 */
    int pressed = -1;                /* 当前按中的按钮下标 */
    int result_id = -1;

    if (g_touch_fd < 0 || g_lcd_mp == NULL || g_font == NULL ||
        paint == NULL || buttons == NULL || n <= 0) {
        return -1;
    }

    /* 1.创建一张 800x480 的白色画布，整屏交互期间反复复用这一张 */
    screen = createBitmapWithInit(g_screen_w, g_screen_h, 4, 0xFFFFFFFF);
    if (screen == NULL) {
        printf("ui_run_screen：创建画布失败\n");
        return -1;
    }

    /* 2.本屏的统一重绘函数：先铺白底，再调业务回调画内容，最后一次性刷到 LCD */
    #define REPAINT()  do {                                                  \
        ui_fill_rect(screen, 0, 0, g_screen_w, g_screen_h, 0xFFFFFFFF);      \
        paint(screen, pressed, userdata);                                    \
        show_font_to_lcd(g_lcd_mp, 0, 0, screen);                            \
    } while (0)

    REPAINT();   /* 先把界面显示出来 */

    /* 3.事件循环：poll 阻塞等待触摸，有事件就读一个 input_event 处理 */
    while (1) {
        struct pollfd pfd;
        struct input_event ev;
        ssize_t r;

        pfd.fd = g_touch_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        if (poll(&pfd, 1, -1) < 0) {         /* -1 表示无限等待 */
            if (errno == EINTR) continue;    /* 被信号打断则重试 */
            perror("poll 触摸事件失败");
            break;
        }

        r = read(g_touch_fd, &ev, sizeof(ev));
        if (r != (ssize_t)sizeof(ev)) {
            continue;                        /* 半包/信号读残，丢弃继续 */
        }

        if (ev.type == EV_ABS) {
            /* 绝对坐标事件：更新 X 或 Y，重新映射为屏幕坐标 */
            if (ev.code == ABS_X) {
                raw_x = ev.value;
            } else if (ev.code == ABS_Y) {
                raw_y = ev.value;
            } else {
                continue;                    /* 压力等其他轴不关心 */
            }
            raw_to_screen(raw_x, raw_y, &cur_x, &cur_y);

            /* 手指按着移动时，实时切换高亮按钮 */
            if (is_down) {
                int now = hit_button(cur_x, cur_y, buttons, n);
                if (now != pressed) {
                    pressed = now;
                    REPAINT();
                }
            }
        } else if (ev.type == EV_KEY) {
            /* 触摸按下/抬起事件（gslX680 在 event0 只上报触摸键，不校验具体编号） */
            if (ev.value == 1) {
                /* ---- 按下：记录命中的按钮并高亮 ---- */
                is_down = 1;
                raw_to_screen(raw_x, raw_y, &cur_x, &cur_y);
                pressed = hit_button(cur_x, cur_y, buttons, n);
                REPAINT();
            } else if (ev.value == 0) {
                /* ---- 抬起：若仍按在按钮上，就是一次有效点击 ---- */
                int clicked = pressed;
                is_down = 0;
                pressed = -1;
                if (clicked >= 0) {
                    result_id = buttons[clicked].id;
                    break;                   /* 本屏交互结束，返回按钮 id */
                }
                REPAINT();                   /* 误触（按在空白处/滑出）：恢复常态 */
            }
        }
        /* EV_SYN（一帧结束）等其他事件无需处理 */
    }

    #undef REPAINT
    destroyBitmap(screen);
    return result_id;
}
