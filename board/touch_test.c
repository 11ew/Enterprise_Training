/*****************************************************************************
 * 文件名称：touch_test.c
 * 运行平台：GEC6818 开发板（ARM Linux）
 * 功能描述：触摸屏标定/自测小工具（独立于主程序 client，不依赖字库和屏幕）
 *      1.打开 /dev/input/event0，打印 X/Y 轴的原始坐标范围；
 *      2.手指点屏幕、滑动时，实时打印每个输入事件的 type/code/value；
 *      3.用来确认：触摸节点是否正确、坐标方向是否和屏幕一致。
 *
 * 交叉编译（WSL 中）：
 *      make touch_test           # 只编译本工具，生成 ARM 可执行文件 touch_test
 * 板端运行：
 *      ./touch_test              # Ctrl+C 退出
 * 使用方法：
 *      依次点屏幕【左上角、右上角、左下角、右下角】，看打印的原始坐标：
 *        - 左上应最小、右下应最大；哪个轴反了，就去 touch_ui.c 把对应的
 *          TOUCH_INVERT_X / TOUCH_INVERT_Y / TOUCH_SWAP_XY 改成 1，重新 make。
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#define TOUCH_DEV   "/dev/input/event0"   /* gslX680 触摸节点 */

int main(void)
{
    int fd;
    struct input_absinfo abs;
    struct input_event ev;

    /* 1.打开触摸设备 */
    fd = open(TOUCH_DEV, O_RDONLY);
    if (fd < 0) {
        perror("打开 " TOUCH_DEV " 失败");
        return -1;
    }

    /* 2.打印 X/Y 轴范围（minimum/maximum），用于核对映射区间 */
    if (ioctl(fd, EVIOCGABS(ABS_X), &abs) == 0)
        printf("X 轴范围：[%d, %d]\n", abs.minimum, abs.maximum);
    if (ioctl(fd, EVIOCGABS(ABS_Y), &abs) == 0)
        printf("Y 轴范围：[%d, %d]\n", abs.minimum, abs.maximum);

    printf("请点按/滑动屏幕（Ctrl+C 退出）：\n");

    /* 3.逐个读取并打印输入事件 */
    while (1) {
        if (read(fd, &ev, sizeof(ev)) != sizeof(ev)) continue;

        if (ev.type == EV_ABS) {
            if (ev.code == ABS_X)      printf("ABS_X = %d\n", ev.value);
            else if (ev.code == ABS_Y) printf("ABS_Y = %d\n", ev.value);
            else                       printf("EV_ABS code=%d value=%d\n",
                                              ev.code, ev.value);
        } else if (ev.type == EV_KEY) {
            printf("EV_KEY code=%d value=%d（%s）\n",
                   ev.code, ev.value, ev.value ? "按下" : "抬起");
        } else if (ev.type == EV_SYN) {
            /* 一帧事件结束，打印分隔线便于观察 */
            printf("------------------------------\n");
        } else {
            printf("其他事件 type=%d code=%d value=%d\n",
                   ev.type, ev.code, ev.value);
        }
    }

    close(fd);
    return 0;
}
