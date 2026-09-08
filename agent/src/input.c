#include "input.h"
#include "log.h"
#include <stdio.h>

static int s_prev_abs_x = -1;
static int s_prev_abs_y = -1;

int input_init(void) {
    /* Disable Windows XP mouse acceleration curves to provide raw 1:1 linear ballistics for games */
    int mouseParams[3] = { 0, 0, 0 }; // Threshold1=0, Threshold2=0, Acceleration=0 (OFF)
    SystemParametersInfo(SPI_SETMOUSE, 0, mouseParams, 0);
    SystemParametersInfo(SPI_SETMOUSESPEED, 0, (PVOID)10, 0); // 1:1 default notch 6/11
    agent_log("input_init: Windows XP mouse acceleration disabled (1:1 linear ballistics active)");
    return 1;
}

void input_shutdown(void) {
    s_prev_abs_x = -1;
    s_prev_abs_y = -1;
}
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
    /* Check if system cursor is currently shown or hidden */
    CURSORINFO ci;
    memset(&ci, 0, sizeof(ci));
    ci.cbSize = sizeof(CURSORINFO);
    int cursor_showing = 1;
    if (GetCursorInfo(&ci)) {
        cursor_showing = (ci.flags & CURSOR_SHOWING) != 0;
    }

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int px = (int)(((uint32_t)x * (uint32_t)sw + 32768) / 65535);
    int py = (int)(((uint32_t)y * (uint32_t)sh + 32768) / 65535);
    if (px >= sw) px = sw - 1;
    if (py >= sh) py = sh - 1;

    if (cursor_showing) {
        /* Desktop / Menu mode: set cursor position directly */
        SetCursorPos(px, py);
        INPUT inp;
        memset(&inp, 0, sizeof(inp));
        inp.type = INPUT_MOUSE;
        inp.mi.dx = (LONG)x;
        inp.mi.dy = (LONG)y;
        inp.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        SendInput(1, &inp, sizeof(INPUT));
        s_prev_abs_x = px;
        s_prev_abs_y = py;
    } else {
        /* 3D Gameplay mode: cursor is hidden by game!
           Do NOT call SetCursorPos which conflicts with GTA SA's camera re-centering!
           Pass unmodified 1:1 relative mouse movement. */
        if (s_prev_abs_x >= 0 && s_prev_abs_y >= 0) {
            int dx = px - s_prev_abs_x;
            int dy = py - s_prev_abs_y;
            if (dx != 0 || dy != 0) {
                input_inject_mouse_rel((int16_t)dx, (int16_t)dy);
            }
        }
        s_prev_abs_x = px;
        s_prev_abs_y = py;
    }
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
        case 1: flags = is_down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        case 2: flags = is_down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
        case 3:
        case 4: flags = is_down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        default: flags = button; break;
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
