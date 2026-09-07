#include "input.h"

void input_inject_key(uint16_t scancode, int is_down, int is_extended) {
    INPUT inp;
    memset(&inp, 0, sizeof(inp));
    inp.type = INPUT_KEYBOARD;
    inp.ki.wScan = scancode;
    inp.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (!is_down) inp.ki.dwFlags |= KEYEVENTF_KEYUP;
    if (is_extended) inp.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    SendInput(1, &inp, sizeof(INPUT));
}

void input_inject_mouse_rel(int16_t dx, int16_t dy) {
    INPUT inp;
    memset(&inp, 0, sizeof(inp));
    inp.type = INPUT_MOUSE;
    inp.mi.dx = dx;
    inp.mi.dy = dy;
    inp.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &inp, sizeof(INPUT));
}

void input_inject_mouse_abs(uint16_t x, uint16_t y) {
    INPUT inp;
    memset(&inp, 0, sizeof(inp));
    inp.type = INPUT_MOUSE;
    inp.mi.dx = (LONG)x;
    inp.mi.dy = (LONG)y;
    inp.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &inp, sizeof(INPUT));
}

void input_inject_mouse_btn(uint16_t btn_flags) {
    INPUT inp;
    memset(&inp, 0, sizeof(inp));
    inp.type = INPUT_MOUSE;
    inp.mi.dwFlags = btn_flags;
    SendInput(1, &inp, sizeof(INPUT));
}

void input_inject_mouse_wheel(int16_t delta) {
    INPUT inp;
    memset(&inp, 0, sizeof(inp));
    inp.type = INPUT_MOUSE;
    inp.mi.mouseData = delta;
    inp.mi.dwFlags = MOUSEEVENTF_WHEEL;
    SendInput(1, &inp, sizeof(INPUT));
}
