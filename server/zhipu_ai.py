#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
文件名称：zhipu_ai.py
运行平台：电脑 WSL（Ubuntu）
功能描述：被 server.c 通过 popen 调用，把讯飞识别出的文字发给智谱 GLM 大模型，
          并把 AI 回答【只】通过标准输出 print 出来（server.c 会读取 stdout）。

用法：
    python3 zhipu_ai.py "你想问的问题"
不带参数时使用默认问题，方便单独测试：
    python3 zhipu_ai.py

依赖安装（WSL 中执行一次即可）：
    pip3 install zhipuai
"""

import sys

from zhipuai import ZhipuAI

# ====================== 配置区 ======================
# 智谱开放平台 API Key（沿用原 08testzhipu.py 中的 key）
API_KEY = "94e9b98a058a4a83a7533117a8185943.hzZciD26AykZuLv0"
# 使用的模型：glm-4-flash 免费、响应快，适合语音对话
MODEL = "glm-4-flash-250414"
# 系统提示词：约束回答风格。
# LCD 只有 800x480，所以要求简短、口语化、不用 markdown/emoji，避免显示乱码。
SYSTEM_PROMPT = (
    "你是一个嵌入式语音助手，通过语音和用户对话。"
    "请用简体中文口语化回答，控制在80个字以内，直接给出答案，"
    "不要使用markdown符号、列表、代码块和emoji表情。"
)
# ===================================================


def chat(question: str) -> str:
    """向智谱大模型发起一次对话请求，返回回答文本。"""
    client = ZhipuAI(api_key=API_KEY)
    response = client.chat.completions.create(
        model=MODEL,
        messages=[
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": question},
        ],
    )
    # 回答正文在 choices[0].message.content
    return response.choices[0].message.content.strip()


def main() -> int:
    # 命令行第一个参数是问题；没传则用默认问题（便于单独调试）
    question = sys.argv[1] if len(sys.argv) >= 2 else "请用一句话介绍你自己。"
    try:
        answer = chat(question)
    except Exception as error:
        # 错误信息走 stderr（server.c 用 2>/dev/null 屏蔽了它），
        # 并以非 0 状态退出，让 server.c 知道调用失败。
        print(f"请求智谱AI失败：{error}", file=sys.stderr)
        return 1

    # 正常回答走 stdout，server.c 只认这一路输出
    print(answer)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
