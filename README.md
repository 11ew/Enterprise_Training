# AI 语音助手（GEC6818 + 讯飞 IAT + 智谱 GLM）

板端说一句话，屏幕上显示大模型回答。整条链路：

```
 GEC6818 开发板(板端)                         电脑 WSL(服务器端)
┌─────────────────────┐      TCP/8888       ┌──────────────────────────────┐
│ 1.点屏幕按钮，录音   │ ───上传cmd.wav────▶ │ 3.讯飞 iat_online_sample     │
│   arecord 3秒       │                    │   语音转文字                  │
│ 2.字库显示状态/结果   │                    │ 4.智谱 GLM-4-Flash 生成回答   │
│   libfont.a+TTF      │                    │ 5.回答文本回传板端            │
│   /dev/fb0 LCD       │ ◀────回传回答─────  │                              │
│   gslX680 电容触摸   │                    │                              │
└─────────────────────┘                     └──────────────────────────────┘
```

- **板端**：GEC6818（ARM Linux，800x480 电容触摸 LCD，触摸芯片 gslX680，节点 `/dev/input/event0`），点屏幕按钮触发，ALSA `arecord` 录音，`libfont.a` + `DroidSansFallback.ttf` 显示汉字。
- **服务器端**：电脑 WSL（Ubuntu x86_64），C 写的 TCP 服务器，popen 依次调用讯飞语音听写、智谱大模型。

---

## 一、目录结构

```
succeed/
├── old/                         # 整合前的原始文件（备份，未改动）
│   ├── 07testcline.c            #   旧：板端文件传输
│   ├── 07testserver.c           #   旧：电脑端文件传输
│   └── 08testzhipu.py           #   旧：智谱AI最小示例
│
└── new/                         # ★整合后的完整工程
    ├── README.md                #   本文件
    ├── board/                   # -------- 板端（传到 GEC6818）--------
    │   ├── client.c             #   板端主程序（触摸界面+录音+上传+收结果+LCD显示）
    │   ├── touch_ui.c/.h        #   触摸屏读取、坐标映射、按钮绘制与交互事件循环
    │   ├── touch_test.c         #   （可选）触摸标定小工具，make touch_test 编译
    │   ├── font.h               #   字库头文件
    │   ├── libfont.a            #   字库静态库（ARM）
    │   └── Makefile             #   交叉编译脚本
    │
    └── server/                  # -------- 服务器端（WSL 里运行）--------
        ├── server.c             #   服务器主程序（收wav→讯飞→智谱→回传）
        ├── zhipu_ai.py          #   智谱AI调用脚本
        ├── Makefile             #   一键编译（自动连带编译讯飞程序）
        └── iat/                 #   讯飞在线语音听写
            ├── iat_online_sample.c  # 讯飞识别源码（只输出纯文本结果）
            ├── Makefile
            ├── include/             # 讯飞 MSC 头文件
            └── lib/libmsc.so        # 讯飞 MSC 64位动态库（与appid绑定）
```

---

## 二、通信协议（板端与服务器严格对应）

同一条 TCP 连接，按顺序走：

| 顺序 | 方向 | 内容 | 说明 |
|----|----|----|----|
| 1 | 板→服 | 文件名长度(4字节) + 文件大小(4字节) | 均为网络字节序(htonl) |
| 2 | 板→服 | 文件名字符串 | 如 `cmd.wav`，不带 `\0` |
| 3 | 服→板 | `OK`(2字节) | 确认文件信息，允许开始传内容 |
| 4 | 板→服 | WAV 二进制内容 | 严格按文件大小发完 |
| 5 | 服→板 | `OK`(2字节) | 确认文件完整落盘 |
| 6 | 服内部 | 讯飞听写→智谱AI | 板端此时等待 |
| 7 | 服→板 | 回答长度(4字节) + 回答文本(UTF-8) | 长度为网络字节序 |

> 所有收发都用 `send_all/recv_all` 循环处理，解决 TCP 粘包/半包问题。

---

## 三、服务器端：编译与运行（WSL）

### 1. 安装依赖（只需一次）

```bash
sudo apt update
sudo apt install -y gcc make python3 python3-pip
pip3 install zhipuai --break-system-packages   # 若报缺 sniffio，再装：pip3 install sniffio --break-system-packages
```

### 2. 编译

```bash
cd /mnt/e/Enterprise_Training/succeed/new/server
make            # 自动先编译 iat/iat_online_sample，再编译 server
```

生成两个可执行文件：`server` 和 `iat/iat_online_sample`。
`libmsc.so` 的搜索路径已通过 rpath 写进可执行文件，**无需手动 export LD_LIBRARY_PATH**。

### 3. IP 规划（已按实测配置好）

- 电脑（WSL 服务器）：`169.254.68.229`
- 板子（GEC6818）：`169.254.68.230`

`board/client.c` 顶部的 `SERVER_IP` 已填 `169.254.68.229`，服务器 `INADDR_ANY` 监听所有网卡，**正常情况下无需再改 IP**。换网络环境时再同步修改。

### 4. 运行服务器（常驻）

```bash
./server
```

看到 `AI 语音助手服务器已启动，监听端口 8888` 即成功，Ctrl+C 退出。

### 5. 单独自测各环节（可选）

```bash
# 测讯飞识别（需要外网）
./iat/iat_online_sample 某个.wav
# 测智谱AI（需要外网）
python3 zhipu_ai.py "你好"
```

---

## 四、板端：交叉编译、部署与运行（GEC6818）

### 1. 在 WSL 交叉编译

```bash
cd /mnt/e/Enterprise_Training/succeed/new/board
make            # 已固化使用 /opt/usr/local/arm/5.4.0 下的 arm-linux-gcc 5.4.0
```

生成 ARM 可执行文件 `client`（`file client` 应显示 `ELF 32-bit ... ARM`）。
若换其他工具链，可用 `make CROSS_COMPILE=你的前缀-` 覆盖；板子自带 gcc 时用 `make CROSS_COMPILE=`。

如需排查触摸方向，可额外编译标定工具：`make touch_test`（生成 `touch_test`，用法见第六节 FAQ）。

### 2. 部署到板子

用 sftp/ftp/tftp 传到板子，**只需传可执行文件，`font.h`/`touch_ui.h`/源码都不需要传**：
- `client`（主程序，必须）
- `touch_test`（仅标定触摸时需要，可选）

板端前提（你已具备）：
- 字库在 `/usr/share/fonts/DroidSansFallback.ttf`
- ALSA 录音命令 `arecord` 可用
- LCD 设备 `/dev/fb0`
- 触摸屏设备 `/dev/input/event0`（gslX680，`cat /proc/bus/input/devices` 可查）

```bash
# 板子上给执行权限
chmod +x client
```

### 3. 运行（全程点屏幕，无需键盘）

```bash
./client
```

- 待机页点蓝色【开始对话】→ 录 3 秒音 → 自动上传 → LCD 先显示“AI 正在思考” → 回答页显示 AI 回答（自动换行）
- 回答页点绿色【继续提问】回到待机页，可连续多轮对话；任意页面点灰色【退出】结束程序
- 按下按钮时颜色变深表示命中，按在按钮外或滑出后松手不会误触发
- 终端串口仍会打印流程日志（仅用于看日志，不需要输入任何内容）

---

## 五、需要按需修改的配置项

| 文件 | 宏/变量 | 默认值 | 含义 |
|----|----|----|----|
| board/client.c | `SERVER_IP` | 169.254.68.229 | WSL 服务器 IP（电脑） |
| board/client.c | `SERVER_PORT` | 8888 | 服务器端口（两端一致） |
| board/client.c | `RECORD_CMD` | arecord -d 3 ... | 录音时长/参数（讯飞要求16k/单声道/S16_LE） |
| board/client.c | `FONT_PATH` | /usr/share/fonts/DroidSansFallback.ttf | 板端字体路径 |
| board/touch_ui.c | `TOUCH_DEV` | /dev/input/event0 | gslX680 触摸设备节点 |
| board/touch_ui.c | `TOUCH_SWAP_XY/INVERT_X/INVERT_Y` | 全 0 | 触摸方向标定宏，轴反了改 1 重新 make |
| board/client.c | `g_idle_btns/g_answer_btns` | 见源码 | 两屏按钮的位置、大小、颜色、文字 |
| server/server.c | `SERVER_PORT` | 8888 | 监听端口 |
| server/iat/iat_online_sample.c | `login_params` 里 appid | 1eb57cf1 | 讯飞 appid，**与 libmsc.so 绑定，换库必须同步换 appid** |
| server/zhipu_ai.py | `API_KEY` | 已填 | 智谱开放平台 Key |
| server/zhipu_ai.py | `MODEL` | glm-4-flash-250414 | 智谱模型 |
| server/zhipu_ai.py | `SYSTEM_PROMPT` | 80字内口语化 | 控制回答风格/长度（适配小屏） |

---

## 六、常见问题

1. **板端提示“连接服务器失败”**
   - 板子能否 ping 通 `SERVER_IP`；服务器 `./server` 是否已启动；
   - WSL 防火墙/网络模式，直连网线推荐 WSL1 或给 WSL 配同网段 IP。

2. **板端“录音失败”**
   - 先在板子手动跑 `arecord -d 3 -c 1 -r 16000 -t wav -f S16_LE t.wav` 与 `aplay t.wav` 验证 ALSA 链路。

3. **服务器收到音频但识别为空**
   - 单独跑 `./iat/iat_online_sample cmd.wav` 看报错码；常见为无外网、appid 与 libmsc.so 不匹配、音频格式不是 16k/16bit/单声道。
   - 错误码含义查 `iat/include/msp_errors.h`。

4. **提示 `No module named zhipuai`（或 sniffio）**
   - `pip3 install zhipuai sniffio --break-system-packages`。

5. **LCD 不显示/花屏**
   - 确认 `/dev/fb0` 存在、分辨率确为 800x480；字号/行高在 client.c 顶部 `FONT_PIXELS/LINE_HEIGHT` 调整。
   - 字库加载失败时程序直接退出（触摸界面必须有屏幕和字库）。

5.1 **点按钮没反应 / 点击位置和响应位置错位（触摸方向不对）**
   - 先 `cat /proc/bus/input/devices` 确认 gslX680 的 Handlers 是 `event0`（不是则改 touch_ui.c 的 `TOUCH_DEV`）；
   - 把 `make touch_test` 编出的 `touch_test` 传上板子运行，依次点屏幕四个角看原始坐标：
     点左/右响应反了 → `TOUCH_INVERT_X=1`；点上/下反了 → `TOUCH_INVERT_Y=1`；
     坐标整体转了 90° → `TOUCH_SWAP_XY=1`；改完重新 `make` 部署即可。
   - 正常情况下坐标范围由 `EVIOCGABS` 自动读取并线性映射，无需手填。

6. **回答太长一屏放不下**
   - 程序已自动按屏宽折行、超高裁剪；也可调 zhipu_ai.py 的 SYSTEM_PROMPT 让回答更短。

---

## 七、整体执行时序

```
板端 client.c                服务器 server.c            iat_online_sample / zhipu_ai.py
   │ 点【开始对话】按钮           │                              │
   │ arecord录音3秒               │                              │
   │ connect ──────────────────▶ accept                         │
   │ 文件名长度/大小/文件名 ────▶ 校验、回OK                      │
   │ ◀──────────── OK            │                              │
   │ 发送wav内容 ──────────────▶ 写cmd.wav、回OK                  │
   │ 等待中...                    │ popen 调用 ────────────────▶ 讯飞语音转文字
   │                             │ ◀──────────── 识别文本        │
   │                             │ popen 调用 ────────────────▶ 智谱生成回答
   │                             │ ◀──────────── 回答文本        │
   │ ◀──── 回答长度+回答文本 ──── │                              │
   │ LCD自动换行显示回答           │ 回到等待下一次连接             │
```
