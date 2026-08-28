#!/usr/bin/env python3
"""fre_state 单测: config 管理(保留密钥)/校验/权限检测/状态机。

config 写入测试用临时目录 mock config_path, 不触碰真实 config.local.json
(含密钥, git 忽略)。运行: companion/.venv/bin/python tests/test_fre_state.py
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import fre_state  # noqa: E402

FAILURES = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILURES.append(f"{name}: got {got!r}, want {want!r}")
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")


def _patch_config_path(tmpdir):
    """把 config_path 重定向到临时目录(测完恢复)。"""
    real = fre_state.config_path
    fre_state.config_path = lambda: os.path.join(tmpdir, "config.local.json")
    return real


def test_write_cfg_keeps_secret():
    """写向导字段必须保留已有密钥与未知字段(不覆盖)。"""
    with tempfile.TemporaryDirectory() as td:
        real = _patch_config_path(td)
        try:
            # 预置"已有配置"(模拟用户已填 volcano_api_key)
            p = os.path.join(td, "config.local.json")
            with open(p, "w", encoding="utf-8") as f:
                f.write('{"volcano_api_key": "SECRET-KEY", "channel": "ble"}')
            merged = fre_state.write_cfg(
                {"channel": "usb", "inject_mode": "unicode"})
            check("保留密钥", merged.get("volcano_api_key"), "SECRET-KEY")
            check("channel 更新", merged.get("channel"), "usb")
            check("inject_mode 写入", merged.get("inject_mode"), "unicode")
            # 落盘后再读, 确认持久化
            with open(p, "r", encoding="utf-8") as f:
                again = __import__("json").load(f)
            check("落盘保留密钥", again.get("volcano_api_key"), "SECRET-KEY")
        finally:
            fre_state.config_path = real


def test_write_cfg_from_empty():
    """无现有 config: 全字段写入, 产物可被 json 解析。"""
    with tempfile.TemporaryDirectory() as td:
        real = _patch_config_path(td)
        try:
            merged = fre_state.write_cfg(
                {"channel": "ble", "inject_mode": "auto",
                 "inject_focus_delay": 2.0})
            check("无既有文件可写", merged.get("channel"), "ble")
            check("usb_port 缺省不写入", "usb_port" in merged, False)
        finally:
            fre_state.config_path = real


def test_validate_fields():
    """channel/inject_mode 非法值拒绝(文案与 relay 一致)。"""
    for bad in ("BLE", "", "serial", "wifi", 3):
        try:
            fre_state.validate_channel(bad)
            check(f"channel 非法 {bad!r} 拒绝", False, True)
        except ValueError as e:
            check(f"channel 非法 {bad!r} 拒绝", "channel" in str(e), True)
    for bad in ("AUTO", "", None):
        try:
            fre_state.validate_inject_mode(bad)
            check(f"inject_mode 非法 {bad!r} 拒绝", False, True)
        except ValueError as e:
            check(f"inject_mode 非法 {bad!r} 拒绝", "inject_mode" in str(e), True)
    fre_state.validate_channel("usb")
    fre_state.validate_inject_mode("clipboard")
    check("合法值放行", True, True)


def test_state_machine():
    """页面状态机: 合法迁移生效, 非法迁移保持原状态。"""
    cases = [
        ("idle", "scan_started", "scanning"),
        ("scanning", "found", "found"),
        ("scanning", "failed", "failed"),
        ("found", "connect", "connecting"),
        ("connecting", "connected", "connected"),
        ("connecting", "failed", "failed"),
        ("connected", "reset", "found"),
        ("failed", "retry", "scanning"),
        # 非法迁移: 保持原状态
        ("idle", "connected", "idle"),
        ("connected", "connect", "connected"),
        ("found", "reset", "found"),
    ]
    for state, ev, want in cases:
        got = fre_state.next_fre_state(state, ev)
        if got != want:
            FAILURES.append(f"状态机 {state}+{ev}: got {got!r}, want {want!r}")
    print(f"[{'PASS' if not FAILURES else 'FAIL'}] 状态机 {len(cases)} 例")


def test_write_asr_cfg_keeps_others():
    """ASR Key 写入只更新 volcano_api_key, 其他字段(含既有 key)不丢。"""
    with tempfile.TemporaryDirectory() as td:
        real = _patch_config_path(td)
        try:
            p = os.path.join(td, "config.local.json")
            with open(p, "w", encoding="utf-8") as f:
                f.write('{"volcano_api_key": "OLD", "channel": "ble",'
                        ' "inject_mode": "unicode"}')
            merged = fre_state.write_asr_cfg("  NEW-KEY  ")
            check("key 更新(去空格)", merged.get("volcano_api_key"), "NEW-KEY")
            check("channel 保留", merged.get("channel"), "ble")
            check("inject_mode 保留", merged.get("inject_mode"), "unicode")
            with open(p, "r", encoding="utf-8") as f:
                again = __import__("json").load(f)
            check("落盘保留其他字段", again.get("channel"), "ble")
        finally:
            fre_state.config_path = real


def test_write_asr_cfg_from_empty():
    """无既有 config: 从空文件写入 key 可解析。"""
    with tempfile.TemporaryDirectory() as td:
        real = _patch_config_path(td)
        try:
            merged = fre_state.write_asr_cfg("K1")
            check("空起步写入", merged.get("volcano_api_key"), "K1")
        finally:
            fre_state.config_path = real


def test_write_asr_cfg_rejects_empty():
    """空/空白 key 拒绝, 不落盘。"""
    with tempfile.TemporaryDirectory() as td:
        real = _patch_config_path(td)
        try:
            for bad in ("", "   ", None):
                try:
                    fre_state.write_asr_cfg(bad)
                    check(f"空 key {bad!r} 拒绝", False, True)
                except ValueError:
                    check(f"空 key {bad!r} 拒绝", True, True)
            check("拒绝后未落盘", os.path.exists(
                os.path.join(td, "config.local.json")), False)
        finally:
            fre_state.config_path = real


def test_permission_status_non_darwin():
    """非 darwin(如构建机 Linux/CI): 权限列表为空(无 TCC)。"""
    saved = sys.platform
    sys.platform = "linux"
    try:
        check("非 darwin 无权限项", fre_state.mac_permission_status(), [])
    finally:
        sys.platform = saved


def test_open_settings_non_darwin():
    """非 darwin 打开系统设置 → -1(不执行)。"""
    saved = sys.platform
    sys.platform = "linux"
    try:
        check("非 darwin 打开面板拒绝", fre_state.open_system_settings("bluetooth"), -1)
        check("未知面板拒绝", fre_state.open_system_settings("bogus"), -1)
    finally:
        sys.platform = saved


def main():
    test_write_cfg_keeps_secret()
    test_write_cfg_from_empty()
    test_validate_fields()
    test_state_machine()
    test_write_asr_cfg_keeps_others()
    test_write_asr_cfg_from_empty()
    test_write_asr_cfg_rejects_empty()
    test_permission_status_non_darwin()
    test_open_settings_non_darwin()
    if FAILURES:
        print(f"\n{len(FAILURES)} 项失败:")
        for f in FAILURES:
            print("  -", f)
        sys.exit(1)
    print("\n全部通过")


if __name__ == "__main__":
    main()


def test_config_path_frozen_split():
    """审查 P1-6: frozen 打包时 config 落在用户数据目录(可写持久), 非
    frozen 保持模块目录(既有行为回归)。"""
    import sys as _sys
    import tempfile

    with tempfile.TemporaryDirectory() as td:
        # frozen + macOS → ~/Library/Application Support(monkeypatch HOME)
        old_frozen = getattr(_sys, "frozen", None)
        old_platform = fre_state.sys.platform
        old_home = os.environ.get("HOME")
        os.environ["HOME"] = td
        _sys.frozen = True
        fre_state.sys.platform = "darwin"
        try:
            d = fre_state.config_dir()
            assert os.path.exists(d), "目录应自动创建"
            assert d == os.path.join(td, "Library", "Application Support",
                                     "AI Passport"), d
        finally:
            if old_frozen is None:
                delattr(_sys, "frozen")
            else:
                _sys.frozen = old_frozen
            fre_state.sys.platform = old_platform
            if old_home is None:
                os.environ.pop("HOME", None)
            else:
                os.environ["HOME"] = old_home

        # frozen + Windows → %APPDATA%/AI Passport
        _sys.frozen = True
        old_platform2 = fre_state.sys.platform
        fre_state.sys.platform = "win32"
        old_appdata = os.environ.get("APPDATA")
        os.environ["APPDATA"] = os.path.join(td, "appdata")
        try:
            d = fre_state.config_dir()
            assert d == os.path.join(td, "appdata", "AI Passport"), d
        finally:
            if old_frozen is None:
                delattr(_sys, "frozen")
            else:
                _sys.frozen = old_frozen
            fre_state.sys.platform = old_platform2
            if old_appdata is None:
                os.environ.pop("APPDATA", None)
            else:
                os.environ["APPDATA"] = old_appdata

    # 非 frozen: 保持模块目录(既有行为)
    assert os.path.dirname(fre_state.config_path()) == os.path.dirname(
        os.path.abspath(fre_state.__file__))
    print("[PASS] config 路径 frozen 分叉")
