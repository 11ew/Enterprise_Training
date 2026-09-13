/*****************************************************************************
 * 文件名称：touch_ui.h
 * 运行平台：GEC6818 开发板（ARM Linux，800x480 电容触摸屏，触摸芯片 gslX680）
 * 功能描述：触摸屏交互界面模块【头文件】
 *      1. 打开 /dev/input/event0（gslX680 对应的事件节点）读取触摸事件；
 *      2. 用 EVIOCGABS 自动获取触摸原始坐标范围，线性映射到 800x480 屏幕坐标，
 *         并提供 X/Y 交换、镜像的标定宏（见 touch_ui.c 顶部，轴不对时改宏即可）；
 *      3. 提供“按钮”的绘制（实心色块 + 居中文字）与命中判定；
 *      4. ui_run_screen() 统一完成“画界面 -> 等触摸 -> 按下高亮 -> 抬手触发”，
 *         业务代码只需提供一个画图回调和一组按钮，不用关心事件细节。
 *
 * 说明：本模块只依赖板端已有的 libfont.a（font.h）和 Linux 标准输入子系统，
 *       不需要额外移植任何库。
 *****************************************************************************/
#ifndef __TOUCH_UI_H__
#define __TOUCH_UI_H__

#include "font.h"   /* 复用 bitmap / font / fontPrint 等字库与画布接口 */

/*--------------------------- 按钮对象 ---------------------------*/
typedef struct {
    int   x, y, w, h;             /* 按钮在屏幕上的矩形区域（像素） */
    const char *text;             /* 按钮文字（UTF-8，可写中文） */
    u32   color_normal;           /* 正常状态颜色（0xAARRGGBB） */
    u32   color_pressed;          /* 被按下时的颜色（一般取更深的同色） */
    int   fontsize;               /* 按钮文字字号（像素） */
    int   id;                     /* 按钮编号：点击后由 ui_run_screen 返回 */
} UiButton;

/*
 * 界面绘制回调：
 *      screen      ：ui_run_screen 已创建好的 800x480 白色画布，回调只负责往上画
 *      pressed_idx ：当前被按下的按钮下标（-1 表示没有按钮被按下），用于重绘高亮
 *      userdata    ：调用 ui_run_screen 时透传进来的业务数据（可为 NULL）
 * 业务代码在回调里画标题/正文，再对每个按钮调用 ui_paint_button 即可。
 */
typedef void (*ui_paint_fn)(bitmap *screen, int pressed_idx, void *userdata);

/*
 * touch_ui_init：初始化触摸 + UI 模块
 *      lcd_mp ：LCD 显存映射首地址（client.c 里 mmap /dev/fb0 得到）
 *      f      ：已加载并可用的字库对象
 *      w, h   ：屏幕分辨率（本工程固定 800x480）
 * 返回 0 成功，-1 失败（打不开触摸节点等）。
 */
int  touch_ui_init(unsigned int *lcd_mp, font *f, int w, int h);

/* touch_ui_exit：关闭触摸设备节点 */
void touch_ui_exit(void);

/*
 * ui_fill_rect：在画布上画一个实心矩形（带边界裁剪，画到画布外也不会越界）
 *      颜色格式与字库一致：0xAARRGGBB，例如白 0xFFFFFFFF、蓝 0xFF1F6FE0
 */
/* 注意：font.h 里有宏 #define color u32，所以本模块颜色参数一律命名为 clr，
 * 不能用 color 作变量名，否则预处理后会被替换成 u32 导致编译失败。 */
void ui_fill_rect(bitmap *bm, int x, int y, int w, int h, u32 clr);

/* ui_text_width_px：估算一行 UTF-8 文本在指定字号下的显示宽度（像素），
 * 汉字/全角按一个字号宽，英文/数字按半个字号宽，用于文字居中。 */
int  ui_text_width_px(const char *text, int fontsize);

/* ui_draw_centered_text：以(cx,y)为水平中心、y 为顶部，画一行居中文字 */
void ui_draw_centered_text(bitmap *bm, const char *text,
                           int cx, int y, int fontsize, u32 clr);

/* ui_paint_button：画一个按钮（实心矩形 + 居中白字），pressed 决定用哪套颜色 */
void ui_paint_button(bitmap *bm, const UiButton *btn, int pressed);

/*
 * ui_run_screen：运行一屏交互界面（阻塞，直到用户点击某个按钮才返回）
 *      paint  ：本屏的绘制回调
 *      userdata：透传给 paint 的业务数据（不需要就传 NULL）
 *      buttons：本屏的按钮数组
 *      n      ：按钮个数
 * 返回：被点击按钮的 id；触摸设备读取出错时返回 -1。
 */
int  ui_run_screen(ui_paint_fn paint, void *userdata,
                   UiButton buttons[], int n);

#endif /* __TOUCH_UI_H__ */
