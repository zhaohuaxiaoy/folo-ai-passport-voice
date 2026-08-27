#!/usr/bin/env python3
"""AI Passport 打包脚本(pyinstaller)。

Mac:  dist/AI Passport.app (ad-hoc 签名) + dist/AI Passport.dmg (可选)
Win:  dist/AI Passport.exe

用法(在 companion/ 目录):
  python3 build/pack.py                 # 当前平台默认产物
  python3 build/pack.py --dmg           # Mac 额外产出 dmg
  python3 build/pack.py --name "AI Passport 1.0"

依赖: pip install pyinstaller(构建机, 非运行时)。
注意: bleak 后端懒加载(winrt/CoreBluetooth)必须 collect-submodules;
pyobjc 框架必须 collect-all, 否则运行期 from Quartz import 失败。
"""
import argparse
import os
import plistlib
import shutil
import subprocess
import sys

APP_NAME = "AI Passport"
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENTRY = os.path.join(HERE, "fre_app.py")
DIST = os.path.join(HERE, "dist")

# pyinstaller 需要收集的懒加载/框架模块
COLLECT_SUBMODULES = ("bleak", "websockets", "zeroconf", "pystray",
                      "ttkbootstrap")
# ttkbootstrap 无 hooks-contrib hook: 主题/资产数据(元素图、app 图标)
# 必须 collect-data, 否则运行时查资源失败
COLLECT_DATA = ("ttkbootstrap",)
COLLECT_ALL_DARWIN = ("pyobjc-framework-Quartz",
                      "pyobjc-framework-ApplicationServices",
                      "pyobjc-framework-Cocoa")   # 悬浮窗 NSPanel 需要 AppKit


def _run(cmd, **kw):
    print("$", " ".join(cmd))
    return subprocess.run(cmd, cwd=HERE, **kw)


BUNDLE_ID = "com.folotoy.aipassport"


def _patch_info_plist(app_path):
    """PyInstaller 生成的 Info.plist 缺 macOS 隐私用途声明:
    无 NSBluetoothAlwaysUsageDescription 时 bleak 的 CoreBluetooth
    后端(_probe_worker)一启动就被 TCC abort。必须在签名前补上。

    同时修正 CFBundleIdentifier:PyInstaller 默认用 app 名(带空格,
    非 reverse-DNS),macOS 13+ 的 TCC 无法归因这种 id,用途声明
    检查会一直走 abort 路径(真机验证过:只有 key 仍崩溃)。
    """
    plist_path = os.path.join(app_path, "Contents", "Info.plist")
    with open(plist_path, "rb") as f:
        plist = plistlib.load(f)
    usage = "AI Passport 通过蓝牙连接设备以传输语音与状态。"
    plist["CFBundleIdentifier"] = BUNDLE_ID
    plist.setdefault("NSBluetoothAlwaysUsageDescription", usage)
    # 老系统(10.11-10.12)的 peripheral 用途声明,新系统忽略
    plist.setdefault("NSBluetoothPeripheralUsageDescription", usage)
    with open(plist_path, "wb") as f:
        plistlib.dump(plist, f)
    print(f"✓ Info.plist 已补蓝牙用途声明+bundle id: {plist_path}")


def build(app_name):
    cmd = [sys.executable, "-m", "PyInstaller",
           "--noconfirm", "--clean", "--windowed",
           "--name", app_name,
           "--distpath", DIST,
           "--workpath", os.path.join(HERE, "build", "pyinstaller-work"),
           "--specpath", os.path.join(HERE, "build")]
    for m in COLLECT_SUBMODULES:
        cmd += ["--collect-submodules", m]
    for m in COLLECT_DATA:
        cmd += ["--collect-data", m]
    if sys.platform == "darwin":
        for m in COLLECT_ALL_DARWIN:
            cmd += ["--collect-all", m]
    cmd.append(ENTRY)
    p = _run(cmd)
    if p.returncode != 0:
        sys.exit(f"pyinstaller 失败(rc={p.returncode})")

    if sys.platform == "darwin":
        app = os.path.join(DIST, f"{app_name}.app")
        _patch_info_plist(app)  # 先补隐私用途声明再签名
        # pyinstaller 会把 companion 包目录里的 config.local.json 当包数据
        # 拷进 Frameworks —— 既是密钥外泄风险,又会破坏签名封套(codesign
        # 无法对 JSON 数据文件签名,verify 报 invalid subcomponent → TCC
        # SecCode 校验失败 → 真机 BLE 一启动就 abort)。必须在签名前清掉。
        leaked = os.path.join(app, "Contents", "Frameworks", "config.local.json")
        if os.path.exists(leaked):
            os.remove(leaked)
            print(f"✗ 已剔除误入包的 config.local.json(密钥不得进产物): {leaked}")
        p = _run(["codesign", "--force", "--deep", "-s", "-", app])
        if p.returncode != 0:
            print("警告: ad-hoc 签名失败(不影响本地运行, Gatekeeper 会提示)",
                  file=sys.stderr)


def make_dmg(app_name):
    """hdiutil 制作 dmg(需要 .app 已产出)。"""
    if sys.platform != "darwin":
        sys.exit("dmg 仅支持 macOS")
    app = os.path.join(DIST, f"{app_name}.app")
    if not os.path.isdir(app):
        sys.exit(f"缺少 {app}, 先运行 python3 build/pack.py")
    dmg = os.path.join(DIST, f"{app_name}.dmg")
    staging = os.path.join(DIST, "dmg-staging")
    if os.path.exists(staging):
        shutil.rmtree(staging)
    os.makedirs(staging)
    shutil.copytree(app, os.path.join(staging, f"{app_name}.app"))
    os.symlink("/Applications", os.path.join(staging, "Applications"))
    if os.path.exists(dmg):
        os.remove(dmg)
    p = _run(["hdiutil", "create", "-volname", app_name, "-srcfolder",
              staging, "-ov", "-format", "UDZO", dmg])
    shutil.rmtree(staging)
    if p.returncode != 0:
        sys.exit("dmg 创建失败")
    print(f"✓ {dmg}")


def main():
    ap = argparse.ArgumentParser(description="打包 AI Passport 向导")
    ap.add_argument("--dmg", action="store_true", help="Mac 额外产出 dmg")
    ap.add_argument("--name", default=APP_NAME, help="产物名(默认 AI Passport)")
    args = ap.parse_args()
    build(args.name)
    if args.dmg:
        make_dmg(args.name)
    print("✓ 产物目录:", DIST)


if __name__ == "__main__":
    main()
