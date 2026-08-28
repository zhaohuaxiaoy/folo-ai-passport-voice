#!/usr/bin/env python3
"""FRE 向导核心逻辑(纯函数, 无 tkinter): config 自动管理 + 权限检测 + 状态机。

与 fre_app.py(tkinter UI)分离, 便于单测与构建机验证(dry-run 不碰 GUI)。
契约与 relay.py 对齐: channel/inject_mode 校验文案一致, config 白名单
只更新向导管理的字段, 绝不覆盖 volcano_api_key 等未知字段(密钥保留)。
"""
import json
import os
import subprocess
import sys

# 向导管理的 config 白名单字段(其他字段原样保留, 含密钥)
WIZARD_FIELDS = ("channel", "inject_mode", "inject_focus_delay", "usb_port")

VALID_CHANNELS = ("ble", "usb")
VALID_INJECT_MODES = ("auto", "unicode", "clipboard")

# 页面状态机: 合法迁移集合(纯函数校验)
FRE_STATE_IDLE = "idle"          # 启动
FRE_STATE_SCANNING = "scanning"  # 扫描中
FRE_STATE_FOUND = "found"        # 发现设备(可连接)
FRE_STATE_CONNECTING = "connecting"
FRE_STATE_CONNECTED = "connected"   # 状态页
FRE_STATE_FAILED = "failed"         # 连接/扫描失败(回 found/idle 重试)
VALID_TRANSITIONS = {
    FRE_STATE_IDLE: {FRE_STATE_SCANNING},
    FRE_STATE_SCANNING: {FRE_STATE_FOUND, FRE_STATE_FAILED},
    FRE_STATE_FOUND: {FRE_STATE_SCANNING, FRE_STATE_CONNECTING},
    FRE_STATE_CONNECTING: {FRE_STATE_CONNECTED, FRE_STATE_FAILED},
    FRE_STATE_CONNECTED: {FRE_STATE_FOUND},       # "重新设置"回发现页
    FRE_STATE_FAILED: {FRE_STATE_SCANNING, FRE_STATE_IDLE},
}


def config_dir():
    """config.local.json 所在目录。

    源码运行: 与模块同目录(与 relay/asr_client 共享)。PyInstaller frozen
    (审查 P1-6): __file__ 指向解包临时目录 _MEIPASS, 每次启动路径变化且
    进程退出即删除 → 写 config 必然丢失。frozen 改到用户数据目录
    (可写、持久): macOS ~/Library/Application Support/AI Passport/,
    其他平台 %APPDATA%/AI Passport/。目录不存在则创建。
    """
    if getattr(sys, "frozen", False):
        if sys.platform == "darwin":
            base = os.path.expanduser("~/Library/Application Support")
        else:
            base = os.environ.get("APPDATA") or os.path.expanduser("~")
        d = os.path.join(base, "AI Passport")
    else:
        d = os.path.dirname(os.path.abspath(__file__))
    os.makedirs(d, exist_ok=True)
    return d


def config_path():
    """config.local.json 绝对路径(frozen 分叉见 config_dir)。"""
    return os.path.join(config_dir(), "config.local.json")


def load_or_default_cfg():
    """读 config.local.json; 缺失/损坏时返回空 dict(写入时才落盘)。"""
    p = config_path()
    if not os.path.exists(p):
        return {}
    try:
        with open(p, "r", encoding="utf-8") as f:
            return json.load(f)
    except (ValueError, OSError):
        return {}


def validate_channel(v):
    if v not in VALID_CHANNELS:
        raise ValueError(
            f"channel 值无效: {v!r}(WiFi 通道已移除, 请改为 \"ble\" 或 \"usb\")")


def validate_inject_mode(v):
    if v not in VALID_INJECT_MODES:
        raise ValueError(
            f"inject_mode 值无效: {v!r}(应为 \"auto\"、\"unicode\" 或 "
            f"\"clipboard\")")


def write_cfg(cfg):
    """把向导管理的字段合并写回 config.local.json, 保留全部已有字段。

    cfg 为向导收集到的完整字段(dict); 先读现有文件合并(密钥等未知字段
    原样保留), 再整体写回。校验向导字段的合法性, 非法抛 ValueError。
    """
    merged = load_or_default_cfg()
    for k in WIZARD_FIELDS:
        if k in cfg:
            v = cfg[k]
            if k == "channel":
                validate_channel(v)
            elif k == "inject_mode":
                validate_inject_mode(v)
            merged[k] = v
    p = config_path()
    with open(p, "w", encoding="utf-8") as f:
        json.dump(merged, f, indent=2, ensure_ascii=False)
        f.write("\n")
    return merged


def write_asr_cfg(key):
    """写入火山 API Key(白名单合并: 只更新 volcano_api_key, 其余保留)。

    与 write_cfg 的区别: key 属机密, 单字段专用入口(ASR 配置页); 密钥
    只落 git-ignored 的 config.local.json, 绝不入日志/打印。空 key 抛
    ValueError。
    """
    key = (key or "").strip()
    if not key:
        raise ValueError("火山 API Key 不能为空")
    merged = load_or_default_cfg()
    merged["volcano_api_key"] = key
    p = config_path()
    with open(p, "w", encoding="utf-8") as f:
        json.dump(merged, f, indent=2, ensure_ascii=False)
        f.write("\n")
    return merged


def mac_permission_status():
    """Mac 授权状态 → list[("accessibility"|"bluetooth", "ok"|"missing")]。

    - accessibility: AXIsProcessTrusted(ApplicationServices, 懒加载);
      pyobjc 缺失时返回 ("accessibility", "unknown")
    - bluetooth: 系统级查询不可行, 返回 "unknown" —— 实际以扫描结果为准
      (扫描失败/空结果时由 UI 提示检查 隐私-蓝牙)
    Windows 无 TCC 权限: 返回空列表。
    """
    if sys.platform != "darwin":
        return []
    out = []
    try:
        from ApplicationServices import AXIsProcessTrusted
        if not AXIsProcessTrusted():
            out.append(("accessibility", "missing"))
        else:
            out.append(("accessibility", "ok"))
    except ImportError:
        out.append(("accessibility", "unknown"))
    return out


def open_system_settings(panel):
    """打开 macOS 系统设置对应面板(仅 darwin)。返回 subprocess 返回码。"""
    urls = {
        "accessibility": "x-apple.systempreferences:"
                         "com.apple.preference.security?Privacy_Accessibility",
        "bluetooth": "x-apple.systempreferences:"
                     "com.apple.preference.security?Privacy_Bluetooth",
    }
    url = urls.get(panel)
    if sys.platform != "darwin" or url is None:
        return -1
    return subprocess.run(["open", url]).returncode


def next_fre_state(state, event):
    """状态机迁移(纯函数): (当前状态, 事件) → 下一状态。

    event ∈ "scan_started" / "found" / "connect" / "connected" /
             "failed" / "retry" / "reset"; 非法迁移返回原状态。
    """
    events = {
        (FRE_STATE_IDLE, "scan_started"): FRE_STATE_SCANNING,
        (FRE_STATE_SCANNING, "found"): FRE_STATE_FOUND,
        (FRE_STATE_SCANNING, "failed"): FRE_STATE_FAILED,
        (FRE_STATE_FOUND, "scan_started"): FRE_STATE_SCANNING,
        (FRE_STATE_FOUND, "connect"): FRE_STATE_CONNECTING,
        (FRE_STATE_CONNECTING, "connected"): FRE_STATE_CONNECTED,
        (FRE_STATE_CONNECTING, "failed"): FRE_STATE_FAILED,
        (FRE_STATE_CONNECTED, "reset"): FRE_STATE_FOUND,
        (FRE_STATE_FAILED, "retry"): FRE_STATE_SCANNING,
        (FRE_STATE_FAILED, "reset"): FRE_STATE_IDLE,
    }
    return events.get((state, event), state)
