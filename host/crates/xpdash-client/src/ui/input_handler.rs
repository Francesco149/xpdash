//! Input capture, pointer confinement, and PS/2 scancode translation.

use egui::{Event, Key, PointerButton, Pos2, Rect};
use xpdash_core::{
    MsgInputEvent, INPUT_TYPE_KEY, INPUT_TYPE_MOUSE_ABS, INPUT_TYPE_MOUSE_BTN,
    INPUT_TYPE_MOUSE_REL, INPUT_TYPE_MOUSE_WHEEL,
};

use crate::network::ClientSession;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConfinementMode {
    Unconfined, // Cyan border, host cursor free
    Confined,   // Amber border, pointer locked & relative delta streaming
}

pub struct InputHandler {
    pub mode: ConfinementMode,
    pub last_cursor_pos: Option<Pos2>,
    pub show_hud: bool,
    pub sensitivity: f32,
    pub accum_x: f32,
    pub accum_y: f32,
    pub is_at_edge: bool,
    pub grab_mode: egui::CursorGrab,
    pub last_applied_grab: Option<egui::CursorGrab>,
    pub last_applied_visible: Option<bool>,
}

impl InputHandler {
    pub fn new() -> Self {
        Self {
            mode: ConfinementMode::Unconfined,
            last_cursor_pos: None,
            show_hud: false,
            sensitivity: 0.15,
            accum_x: 0.0,
            accum_y: 0.0,
            is_at_edge: false,
            grab_mode: egui::CursorGrab::Locked,
            last_applied_grab: None,
            last_applied_visible: None,
        }
    }

    pub fn is_confined(&self) -> bool {
        self.mode == ConfinementMode::Confined
    }

    pub fn toggle_confinement(&mut self) {
        self.mode = match self.mode {
            ConfinementMode::Unconfined => ConfinementMode::Confined,
            ConfinementMode::Confined => ConfinementMode::Unconfined,
        };
        self.last_cursor_pos = None;
        self.accum_x = 0.0;
        self.accum_y = 0.0;
        self.is_at_edge = false;
    }
    pub fn set_confined(&mut self, confined: bool) {
        self.mode = if confined {
            ConfinementMode::Confined
        } else {
            ConfinementMode::Unconfined
        };
        self.last_cursor_pos = None;
        self.accum_x = 0.0;
        self.accum_y = 0.0;
        self.is_at_edge = false;
    }
    /// Process egui input events and send corresponding network input messages
    pub fn handle_events(
        &mut self,
        ctx: &egui::Context,
        viewport_rect: Rect,
        _guest_width: u16,
        _guest_height: u16,
        session: &ClientSession,
    ) {
        // Enforce OS cursor lock mode only when state changes (prevents Wayland socket flooding)
        let (desired_grab, desired_visible) = if self.is_confined() {
            (self.grab_mode, false)
        } else {
            (egui::CursorGrab::None, true)
        };

        if self.last_applied_grab != Some(desired_grab) {
            ctx.send_viewport_cmd(egui::ViewportCommand::CursorGrab(desired_grab));
            self.last_applied_grab = Some(desired_grab);
        }
        if self.last_applied_visible != Some(desired_visible) {
            ctx.send_viewport_cmd(egui::ViewportCommand::CursorVisible(desired_visible));
            self.last_applied_visible = Some(desired_visible);
        }

        let events = ctx.input(|i| i.events.clone());

        // Extract raw hardware mouse motion deltas (WM_INPUT / DeviceEvent::MouseMotion).
        // On Windows with CursorGrab::Locked / Confined, ClipCursor traps pointer coordinates,
        // making pointer.delta() zero; raw motion is delivered via Event::MouseMoved.
        let mut raw_motion_delta = egui::Vec2::ZERO;
        for event in &events {
            if let Event::MouseMoved(vec) = event {
                raw_motion_delta += *vec;
            }
        }

        if self.is_confined() {
            // Detect if cursor reached viewport edge (when compositor ignores pointer lock in niri/Wayland)
            let mut edge_dx = 0i16;
            let mut edge_dy = 0i16;

            if let Some(pos) = ctx.input(|i| i.pointer.latest_pos()) {
                let margin = 6.0;
                let at_left = pos.x <= viewport_rect.left() + margin;
                let at_right = pos.x >= viewport_rect.right() - margin;
                let at_top = pos.y <= viewport_rect.top() + margin;
                let at_bottom = pos.y >= viewport_rect.bottom() - margin;

                self.is_at_edge = at_left || at_right || at_top || at_bottom;

                // If cursor is pushed against a window edge in niri/Wayland,
                // smoothly continue panning so the camera never freezes at borders!
                if at_left { edge_dx = -1; }
                else if at_right { edge_dx = 1; }
                if at_top { edge_dy = -1; }
                else if at_bottom { edge_dy = 1; }
            } else {
                self.is_at_edge = false;
            }

            // Prefer raw hardware mouse motion (from Event::MouseMoved) when available.
            // Fall back to pointer.delta() on platforms that only emit pointer events.
            let delta = if raw_motion_delta != egui::Vec2::ZERO {
                raw_motion_delta
            } else {
                ctx.input(|i| i.pointer.delta())
            };
            self.accum_x += delta.x * self.sensitivity;
            self.accum_y += delta.y * self.sensitivity;
            let eps_x = if self.accum_x >= 0.0 { 1e-4 } else { -1e-4 };
            let eps_y = if self.accum_y >= 0.0 { 1e-4 } else { -1e-4 };
            let mut send_dx = (self.accum_x + eps_x).trunc() as i16;
            let mut send_dy = (self.accum_y + eps_y).trunc() as i16;

            self.accum_x -= send_dx as f32;
            self.accum_y -= send_dy as f32;

            // Apply edge panning if cursor is stopped at window boundary
            if send_dx == 0 && edge_dx != 0 {
                send_dx = edge_dx;
            }
            if send_dy == 0 && edge_dy != 0 {
                send_dy = edge_dy;
            }

            if send_dx != 0 || send_dy != 0 {
                session.send_input(MsgInputEvent {
                    event_type: INPUT_TYPE_MOUSE_REL,
                    param1: 0,
                    param2: send_dx,
                    param3: send_dy,
                    key_down: 0,
                });
            }
        } else {
            ctx.send_viewport_cmd(egui::ViewportCommand::CursorGrab(egui::CursorGrab::None));
            ctx.send_viewport_cmd(egui::ViewportCommand::CursorVisible(true));

            // Sample latest cursor position every frame to keep guest cursor
            // exactly aligned with client mouse, even when window is resized.
            if let Some(pos) = ctx.input(|i| i.pointer.latest_pos()) {
                if viewport_rect.contains(pos) {
                    let rel_x = (pos.x - viewport_rect.left()) / viewport_rect.width();
                    let rel_y = (pos.y - viewport_rect.top()) / viewport_rect.height();
                    let abs_x = (rel_x.clamp(0.0, 1.0) * 65535.0).round() as u16;
                    let abs_y = (rel_y.clamp(0.0, 1.0) * 65535.0).round() as u16;

                    if self.last_cursor_pos != Some(pos) {
                        self.last_cursor_pos = Some(pos);
                        session.send_input(MsgInputEvent {
                            event_type: INPUT_TYPE_MOUSE_ABS,
                            param1: 0,
                            param2: abs_x as i16,
                            param3: abs_y as i16,
                            key_down: 0,
                        });
                    }
                }
            }
        }

        for event in events {
            match event {
                // Raw mouse motion processed above
                Event::MouseMoved(_) => {}
                // Key press / release
                Event::Key { key, pressed, repeat, modifiers: _, .. } => {
                    // F10 toggles in-game HUD overlay
                    if key == Key::F10 && pressed && !repeat {
                        self.show_hud = !self.show_hud;
                        continue;
                    }

                    // F12 toggles pointer confinement; Escape releases it
                    if key == Key::F12 && pressed && !repeat {
                        self.toggle_confinement();
                        continue;
                    }
                    if key == Key::Escape && pressed && self.is_confined() {
                        self.set_confined(false);
                        continue;
                    }

                    // If modifiers indicate Ctrl or specific key
                    if key == Key::F11 && pressed && !repeat {
                        ctx.send_viewport_cmd(egui::ViewportCommand::Fullscreen(
                            !ctx.input(|i| i.viewport().fullscreen.unwrap_or(false))
                        ));
                        continue;
                    }

                    if let Some(scancode) = key_to_ps2_scancode(key) {
                        session.send_input(MsgInputEvent {
                            event_type: INPUT_TYPE_KEY,
                            param1: scancode,
                            param2: 0,
                            param3: 0,
                            key_down: if pressed { 1 } else { 0 },
                        });
                    }
                }

                // Mouse motion (handled via latest_pos sampling above when unconfined)
                Event::PointerMoved(_) => {}
                // Mouse buttons
                Event::PointerButton { pos, button, pressed, .. } => {
                    if self.is_confined() || viewport_rect.contains(pos) {
                        if !self.is_confined() {
                            let rel_x = (pos.x - viewport_rect.left()) / viewport_rect.width();
                            let rel_y = (pos.y - viewport_rect.top()) / viewport_rect.height();
                            let abs_x = (rel_x.clamp(0.0, 1.0) * 65535.0).round() as u16;
                            let abs_y = (rel_y.clamp(0.0, 1.0) * 65535.0).round() as u16;
                            session.send_input(MsgInputEvent {
                                event_type: INPUT_TYPE_MOUSE_ABS,
                                param1: 0,
                                param2: abs_x as i16,
                                param3: abs_y as i16,
                                key_down: 0,
                            });
                        }

                        // Middle click toggles pointer confinement
                        if button == PointerButton::Middle && pressed {
                            self.toggle_confinement();
                            continue;
                        }

                        let btn_id: u16 = match button {
                            PointerButton::Primary => 1,   // Left
                            PointerButton::Secondary => 2, // Right
                            PointerButton::Middle => 3,    // Middle
                            _ => 0,
                        };

                        if btn_id != 0 {
                            session.send_input(MsgInputEvent {
                                event_type: INPUT_TYPE_MOUSE_BTN,
                                param1: btn_id,
                                param2: 0,
                                param3: 0,
                                key_down: if pressed { 1 } else { 0 },
                            });
                        }
                    }
                }

                // Mouse Wheel
                Event::MouseWheel { delta, .. } => {
                    let wheel_delta = (delta.y * 120.0) as i16;
                    if wheel_delta != 0 {
                        session.send_input(MsgInputEvent {
                            event_type: INPUT_TYPE_MOUSE_WHEEL,
                            param1: wheel_delta as u16,
                            param2: 0,
                            param3: 0,
                            key_down: 0,
                        });
                    }
                }

                _ => {}
            }
        }
    }

    pub fn send_ctrl_alt_del(session: &ClientSession) {
        // PS/2 Scancodes: Ctrl = 0x1D, Alt = 0x38, Del = 0x53 (extended)
        let sc_ctrl = 0x1D;
        let sc_alt = 0x38;
        let sc_del = 0x53;

        // Down
        session.send_input(MsgInputEvent { event_type: INPUT_TYPE_KEY, param1: sc_ctrl, param2: 0, param3: 0, key_down: 1 });
        session.send_input(MsgInputEvent { event_type: INPUT_TYPE_KEY, param1: sc_alt, param2: 0, param3: 0, key_down: 1 });
        session.send_input(MsgInputEvent { event_type: INPUT_TYPE_KEY, param1: sc_del, param2: 0, param3: 0, key_down: 1 });

        // Up
        session.send_input(MsgInputEvent { event_type: INPUT_TYPE_KEY, param1: sc_del, param2: 0, param3: 0, key_down: 0 });
        session.send_input(MsgInputEvent { event_type: INPUT_TYPE_KEY, param1: sc_alt, param2: 0, param3: 0, key_down: 0 });
        session.send_input(MsgInputEvent { event_type: INPUT_TYPE_KEY, param1: sc_ctrl, param2: 0, param3: 0, key_down: 0 });
    }
}

/// PS/2 Set 1 Scancodes for DirectInput / Windows XP games
pub fn key_to_ps2_scancode(key: Key) -> Option<u16> {
    match key {
        Key::W => Some(0x11),
        Key::A => Some(0x1E),
        Key::S => Some(0x1F),
        Key::D => Some(0x20),
        Key::Q => Some(0x10),
        Key::E => Some(0x12),
        Key::R => Some(0x13),
        Key::T => Some(0x14),
        Key::Y => Some(0x15),
        Key::U => Some(0x16),
        Key::I => Some(0x17),
        Key::O => Some(0x18),
        Key::P => Some(0x19),
        Key::F => Some(0x21),
        Key::G => Some(0x22),
        Key::H => Some(0x23),
        Key::J => Some(0x24),
        Key::K => Some(0x25),
        Key::L => Some(0x26),
        Key::Z => Some(0x2C),
        Key::X => Some(0x2D),
        Key::C => Some(0x2E),
        Key::V => Some(0x2F),
        Key::B => Some(0x30),
        Key::N => Some(0x31),
        Key::M => Some(0x32),

        Key::Space => Some(0x39),
        Key::Enter => Some(0x1C),
        Key::Escape => Some(0x01),
        Key::Tab => Some(0x0F),
        Key::Backspace => Some(0x0E),

        Key::Num0 => Some(0x0B),
        Key::Num1 => Some(0x02),
        Key::Num2 => Some(0x03),
        Key::Num3 => Some(0x04),
        Key::Num4 => Some(0x05),
        Key::Num5 => Some(0x06),
        Key::Num6 => Some(0x07),
        Key::Num7 => Some(0x08),
        Key::Num8 => Some(0x09),
        Key::Num9 => Some(0x0A),

        Key::ArrowUp => Some(0xE048),
        Key::ArrowDown => Some(0xE050),
        Key::ArrowLeft => Some(0xE04B),
        Key::ArrowRight => Some(0xE04D),

        Key::F1 => Some(0x3B),
        Key::F2 => Some(0x3C),
        Key::F3 => Some(0x3D),
        Key::F4 => Some(0x3E),
        Key::F5 => Some(0x3F),
        Key::F6 => Some(0x40),
        Key::F7 => Some(0x41),
        Key::F8 => Some(0x42),
        Key::F9 => Some(0x43),
        Key::F10 => Some(0x44),
        Key::F11 => Some(0x57),
        Key::F12 => Some(0x58),

        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_cursor_grab_command() {
        let _ = egui::ViewportCommand::CursorGrab(egui::CursorGrab::Locked);
        let _ = egui::ViewportCommand::CursorGrab(egui::CursorGrab::None);
        let _ = egui::ViewportCommand::CursorVisible(false);
        let _ = egui::ViewportCommand::CursorVisible(true);
        let ctx = egui::Context::default();
        let delta = ctx.input(|i| i.pointer.delta());
        assert_eq!(delta, egui::Vec2::ZERO);
    }

    #[test]
    fn test_ps2_scancode_mapping() {
        assert_eq!(key_to_ps2_scancode(Key::W), Some(0x11));
        assert_eq!(key_to_ps2_scancode(Key::A), Some(0x1E));
        assert_eq!(key_to_ps2_scancode(Key::S), Some(0x1F));
        assert_eq!(key_to_ps2_scancode(Key::D), Some(0x20));
        assert_eq!(key_to_ps2_scancode(Key::Space), Some(0x39));
        assert_eq!(key_to_ps2_scancode(Key::Enter), Some(0x1C));
        assert_eq!(key_to_ps2_scancode(Key::Escape), Some(0x01));
        assert_eq!(key_to_ps2_scancode(Key::Tab), Some(0x0F));
        assert_eq!(key_to_ps2_scancode(Key::ArrowUp), Some(0xE048));
        assert_eq!(key_to_ps2_scancode(Key::ArrowDown), Some(0xE050));
        assert_eq!(key_to_ps2_scancode(Key::F10), Some(0x44));
    }

    #[test]
    fn test_confinement_toggle() {
        let mut handler = InputHandler::new();
        assert!(!handler.is_confined());
        assert_eq!(handler.mode, ConfinementMode::Unconfined);

        handler.toggle_confinement();
        assert!(handler.is_confined());
        assert_eq!(handler.mode, ConfinementMode::Confined);

        handler.toggle_confinement();
        assert!(!handler.is_confined());

        handler.set_confined(true);
        assert!(handler.is_confined());
    }

    #[test]
    fn test_mouse_sensitivity_subpixel_accumulation() {
        let mut handler = InputHandler::new();
        assert_eq!(handler.sensitivity, 0.15);

        // delta of 2.0 points with 0.15 sens = 0.30 -> send_dx = 0, remainder 0.30
        handler.accum_x += 2.0 * handler.sensitivity;
        let eps = if handler.accum_x >= 0.0 { 1e-4 } else { -1e-4 };
        let send_dx = (handler.accum_x + eps).trunc() as i16;
        assert_eq!(send_dx, 0);
        assert!((handler.accum_x - 0.30).abs() < 1e-3);

        // After 5 more points (total 7 points * 0.15 = 1.05): send_dx = 1, remainder 0.05
        handler.accum_x += 5.0 * handler.sensitivity;
        let eps2 = if handler.accum_x >= 0.0 { 1e-4 } else { -1e-4 };
        let send_dx2 = (handler.accum_x + eps2).trunc() as i16;
        assert_eq!(send_dx2, 1);
        handler.accum_x -= send_dx2 as f32;
        assert!((handler.accum_x - 0.05).abs() < 1e-3);
    }

    #[test]
    fn test_event_mouse_moved_raw_delta() {
        let event = Event::MouseMoved(egui::Vec2::new(10.0, -5.0));
        if let Event::MouseMoved(vec) = event {
            assert_eq!(vec.x, 10.0);
            assert_eq!(vec.y, -5.0);
        } else {
            panic!("Expected Event::MouseMoved");
        }
    }
}
