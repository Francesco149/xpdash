#ifndef XPDASH_INPUT_H
#define XPDASH_INPUT_H

#include <windows.h>
#include <stdint.h>

/* Inject a keyboard event using DirectX hardware scancode */
void input_inject_key(uint16_t scancode, int is_down, int is_extended);

/* Inject relative mouse movement (for 3D FPS gaming) */
void input_inject_mouse_rel(int16_t dx, int16_t dy);

/* Inject absolute mouse movement */
void input_inject_mouse_abs(uint16_t x, uint16_t y);

/* Inject mouse button state (button: 1=Left, 2=Right, 3=Middle; is_down: 1=down, 0=up) */
void input_inject_mouse_btn(uint16_t button, int is_down);

/* Release any stuck mouse buttons (sends LEFTUP, RIGHTUP, MIDDLEUP) */
void input_reset_buttons(void);
/* Inject mouse wheel */
void input_inject_mouse_wheel(int16_t delta);

#endif /* XPDASH_INPUT_H */
