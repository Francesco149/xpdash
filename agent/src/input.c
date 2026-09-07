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
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw > 0 && sh > 0) {
        int px = (int)(((uint32_t)x * (uint32_t)sw + 32768) / 65535);
        int py = (int)(((uint32_t)y * (uint32_t)sh + 32768) / 65535);
        if (px >= sw) px = sw - 1;
        if (py >= sh) py = sh - 1;
        SetCursorPos(px, py);
    }
    INPUT inp;
    memset(&inp, 0, sizeof(inp));
    inp.type = INPUT_MOUSE;
    inp.mi.dx = (LONG)x;
    inp.mi.dy = (LONG)y;
    inp.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &inp, sizeof(INPUT));
}

void input_reset_buttons(void) {
    INPUT inp;
    memset(&inp, 0, sizeof(inp));
    inp.type = INPUT_MOUSE;
    inp.mi.dwFlags = MOUSEEVENTF_LEFTUP | MOUSEEVENTF_RIGHTUP | MOUSEEVENTF_MIDDLEUP;
    SendInput(1, &inp, sizeof(INPUT));
}

void input_inject_mouse_btn(uint16_t button, int is_down) {
    DWORD flags = 0;
    switch (button) {
        case 1: /* Primary / Left */
            flags = is_down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            break;
        case 2: /* Secondary / Right */
            flags = is_down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            break;
        case 3: /* Middle */
        case 4:
            flags = is_down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
        default:
            flags = button;
            break;
    }
    if (flags) {
        INPUT inp;
        memset(&inp, 0, sizeof(inp));
        inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = flags;
        SendInput(1, &inp, sizeof(INPUT));
    }
}

void input_inject_mouse_wheel(int16_t delta) {
    INPUT inp;
    memset(&inp, 0, sizeof(inp));
    inp.type = INPUT_MOUSE;
    inp.mi.mouseData = delta;
    inp.mi.dwFlags = MOUSEEVENTF_WHEEL;
    SendInput(1, &inp, sizeof(INPUT));
}
