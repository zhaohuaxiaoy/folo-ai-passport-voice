# 执行清单:UP 长按 PTT + 双击 clear + Ctrl+U 注入 + 麦克风图标

## 实施顺序

1. **bsp_button.h** — `BSP_BTN_LONG_UP` 枚举追加(带注释:长按超时后松开,仅 UP 键用到)。
2. **bsp_button.c** — `cb_long_up()` + 注册 `BUTTON_LONG_PRESS_UP`;UP 键 `long_press_time = 500`(OK 键保持 600000 禁用)。
3. **main/app_types.h** — `APP_EV_KEY_LONG_UP` 枚举;按键注释更新(UP=长按说话/双击清空)。
4. **main/main.c** — on_key 映射 `BSP_BTN_LONG_UP → APP_EV_KEY_LONG_UP`。
5. **main/app_state.c** — handle_key:
   - READY 态 `b==UP && LONG` → START 音 + SEND_VOICE_START + LISTENING(与 OK PRESS 同序列)
   - LISTENING 态 `b==UP && LONG_UP` → STREAM_STOP + SEND_VOICE_END + SEND 音 + TRANSCRIBING(与 OK RELEASE 同序列)
   - 全局 `b==UP && DOUBLE` → CLEAR;删 OK DOUBLE 分支(127-130)
   - `APP_EV_KEY_LONG_UP` 并入 app_state_reduce 按键组
6. **main/app_ui.c** — build_listening 麦克风像素画重画(圆头+弧支架+立杆+底座)。
7. **companion/inject.py** — `_TERMINAL_APPS` 清单 + `_frontmost_is_terminal()` + clear 分支(终端→Ctrl+U;非终端→原序列)。
8. **companion/tests** — clear 分支选择测试(伪造探测函数)。检查既有注入测试文件位置与风格,新增用例注册进 main()。
9. **tests/test_app_state.c** — 新增/改用例:UP 长按流、UP 双击 clear、OK 双击不 clear、短按无动作。

## 验证命令

```bash
cd tests && cmake --build build && ctest --test-dir build          # 7/7
cd companion && .venv/bin/python -m pytest tests/ -q -o asyncio_mode=auto   # 全绿
python3 -m py_compile companion/*.py
git diff --check
source ~/esp/esp-idf-v5.5.3/export.sh && idf.py build              # PASS
```

## 交付步骤

10. 打包 App:`cd companion/build && ../.venv/bin/python pack.py`(签名含蓝牙声明 + bundle id 补丁,dist 无密钥)。
11. 烧录:确认 App 退出(lsof /dev/cu.usbmodem1101 为空)→ `idf.py -p /dev/cu.usbmodem1101 flash`。
12. 提交(中文 Conventional Commits,本地,不 push)。
13. 归档任务;NOT RUN 列表告知用户实测项:长按 0.5s 手感、双击清空、Claude Code/CodeX 中 Ctrl+U、麦克风图标观感。

## 回滚点

- 每个文件改动独立可撤;若 iot_button 长按事件实测与设计不符(如 LONG_PRESS_UP 未触发),回退 bsp_button.c 注册行即可恢复 UP 键无功能。
- inject.py 分支失败按"非终端"降级 = 原行为,不恶化。
