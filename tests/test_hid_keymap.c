// HID 键码表主机测试:全 ASCII 覆盖 + 特殊键 + 非 ASCII 拒绝。
#include <assert.h>
#include <stdio.h>
#include "hid_keymap.h"

static void test_letters(void) {
    hid_key_t k;
    for (char c = 'a'; c <= 'z'; c++) {
        assert(hid_keymap_lookup(c, &k));
        assert(k.keycode == 0x04 + (c - 'a'));
        assert(k.shift == false);
    }
    for (char c = 'A'; c <= 'Z'; c++) {
        assert(hid_keymap_lookup(c, &k));
        assert(k.keycode == 0x04 + (c - 'A'));
        assert(k.shift == true);
    }
}

static void test_digits(void) {
    hid_key_t k;
    for (char c = '0'; c <= '9'; c++) {
        assert(hid_keymap_lookup(c, &k));
        assert(k.keycode == 0x1E + (c - '0'));
        assert(k.shift == false);
    }
}

static void test_punctuation(void) {
    hid_key_t k;
    struct { char c; uint8_t code; bool shift; } cases[] = {
        { ' ', 0x2C, false }, { '-', 0x2D, false }, { '=', 0x2E, false },
        { '[', 0x2F, false }, { ']', 0x30, false }, { '\\', 0x31, false },
        { ';', 0x33, false }, { '\'', 0x34, false }, { '`', 0x35, false },
        { ',', 0x36, false }, { '.', 0x37, false }, { '/', 0x38, false },
        { '_', 0x2D, true },  { '+', 0x2E, true },  { '{', 0x2F, true },
        { '}', 0x30, true },  { '|', 0x31, true },  { ':', 0x33, true },
        { '"', 0x34, true },  { '~', 0x35, true },  { '<', 0x36, true },
        { '>', 0x37, true },  { '?', 0x38, true },
        { '!', 0x1F, true },  { '@', 0x20, true },  { '#', 0x21, true },
        { '$', 0x22, true },  { '%', 0x23, true },  { '^', 0x24, true },
        { '&', 0x25, true },  { '*', 0x26, true },  { '(', 0x27, true },
        { ')', 0x1E, true },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert(hid_keymap_lookup(cases[i].c, &k));
        assert(k.keycode == cases[i].code);
        assert(k.shift == cases[i].shift);
    }
}

static void test_control(void) {
    hid_key_t k;
    assert(hid_keymap_lookup('\n', &k) && k.keycode == 0x28);   // Enter
    assert(hid_keymap_lookup('\r', &k) && k.keycode == 0x28);
    assert(hid_keymap_lookup('\t', &k) && k.keycode == 0x2B);   // Tab
    assert(hid_keymap_lookup('\b', &k) && k.keycode == 0x2A);   // Backspace
}

static void test_rejects(void) {
    hid_key_t k;
    assert(!hid_keymap_lookup((char)0xE4, &k));                  // CJK 首字节(UTF-8)
    assert(!hid_keymap_lookup((char)0xE4, &k));
    assert(!hid_keymap_lookup(0x01, &k));                        // 控制字符(非映射)
    assert(!hid_keymap_lookup(0x7F, &k));                        // DEL
}

static void test_every_printable_ascii(void) {
    // 0x20..0x7E 全部可键入,且键码落在键盘页合法范围(0x04..0x38)
    hid_key_t k;
    for (int c = 0x20; c <= 0x7E; c++) {
        assert(hid_keymap_lookup((char)c, &k));
        assert(k.keycode >= 0x04 && k.keycode <= 0x38);
    }
}

int main(void) {
    test_letters();
    test_digits();
    test_punctuation();
    test_control();
    test_rejects();
    test_every_printable_ascii();
    printf("test_hid_keymap: all assertions passed\n");
    return 0;
}
