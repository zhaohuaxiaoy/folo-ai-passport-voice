// main/hid_keymap.h —— ASCII → HID 键盘 usage id 映射(纯 C,宿主机可测)。
// 非 ASCII(CJK 等)不可键入 —— 由 Mac 端选 paste 模式经 Cmd+V 注入。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// HID 修饰键位
#define HID_MOD_LSHIFT 0x02
#define HID_MOD_LGUI   0x08   // Cmd

typedef struct {
    uint8_t keycode;   // HID usage id(0x04..0x38)
    bool    shift;     // 需按住左 Shift
} hid_key_t;

// 查询字符映射。可键入返回 true 并填 out;不可键入(非 ASCII)返回 false。
bool hid_keymap_lookup(char c, hid_key_t *out);

#ifdef __cplusplus
}
#endif
