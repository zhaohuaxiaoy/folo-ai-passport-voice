// main/hid_keymap.c —— HID 键码表实现(标准 USB HID Usage Table,键盘页 0x07)。
#include "hid_keymap.h"

// 标点:ASCII → {usage id, 是否需 Shift}。按键盘布局(US)逐字符列出。
static bool punct(char c, hid_key_t *out) {
    switch (c) {
    case ' ':  out->keycode = 0x2C; out->shift = false; return true;
    case '-':  out->keycode = 0x2D; out->shift = false; return true;
    case '=':  out->keycode = 0x2E; out->shift = false; return true;
    case '[':  out->keycode = 0x2F; out->shift = false; return true;
    case ']':  out->keycode = 0x30; out->shift = false; return true;
    case '\\': out->keycode = 0x31; out->shift = false; return true;
    case ';':  out->keycode = 0x33; out->shift = false; return true;
    case '\'': out->keycode = 0x34; out->shift = false; return true;
    case '`':  out->keycode = 0x35; out->shift = false; return true;
    case ',':  out->keycode = 0x36; out->shift = false; return true;
    case '.':  out->keycode = 0x37; out->shift = false; return true;
    case '/':  out->keycode = 0x38; out->shift = false; return true;

    case '_':  out->keycode = 0x2D; out->shift = true; return true;
    case '+':  out->keycode = 0x2E; out->shift = true; return true;
    case '{':  out->keycode = 0x2F; out->shift = true; return true;
    case '}':  out->keycode = 0x30; out->shift = true; return true;
    case '|':  out->keycode = 0x31; out->shift = true; return true;
    case ':':  out->keycode = 0x33; out->shift = true; return true;
    case '"':  out->keycode = 0x34; out->shift = true; return true;
    case '~':  out->keycode = 0x35; out->shift = true; return true;
    case '<':  out->keycode = 0x36; out->shift = true; return true;
    case '>':  out->keycode = 0x37; out->shift = true; return true;
    case '?':  out->keycode = 0x38; out->shift = true; return true;

    // Shift+数字 上档符号(US 布局)
    case '!':  out->keycode = 0x1F; out->shift = true; return true;
    case '@':  out->keycode = 0x20; out->shift = true; return true;
    case '#':  out->keycode = 0x21; out->shift = true; return true;
    case '$':  out->keycode = 0x22; out->shift = true; return true;
    case '%':  out->keycode = 0x23; out->shift = true; return true;
    case '^':  out->keycode = 0x24; out->shift = true; return true;
    case '&':  out->keycode = 0x25; out->shift = true; return true;
    case '*':  out->keycode = 0x26; out->shift = true; return true;
    case '(':  out->keycode = 0x27; out->shift = true; return true;
    case ')':  out->keycode = 0x1E; out->shift = true; return true;
    default:   return false;
    }
}

bool hid_keymap_lookup(char c, hid_key_t *out) {
    if (!out) return false;
    out->keycode = 0;
    out->shift = false;

    if (c >= 'a' && c <= 'z') {                 // 0x04..0x1D
        out->keycode = (uint8_t)(0x04 + (c - 'a'));
        return true;
    }
    if (c >= 'A' && c <= 'Z') {                 // 大写 = 小写键码 + Shift
        out->keycode = (uint8_t)(0x04 + (c - 'A'));
        out->shift = true;
        return true;
    }
    if (c >= '0' && c <= '9') {                 // 0x1E..0x27
        out->keycode = (uint8_t)(0x1E + (c - '0'));
        return true;
    }
    switch (c) {
    case '\n': case '\r': out->keycode = 0x28; return true;   // Enter
    case '\t':            out->keycode = 0x2B; return true;   // Tab
    case '\b':            out->keycode = 0x2A; return true;   // Backspace
    default: break;
    }
    return punct(c, out);   // 其余可打印 ASCII 标点
}
