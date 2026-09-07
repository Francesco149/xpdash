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

/* Inject mouse button state */
void input_inject_mouse_btn(uint16_t btn_flags);

/* Inject mouse wheel */
void input_inject_mouse_wheel(int16_t delta);

#endif /* XPDASH_INPUT_H */
