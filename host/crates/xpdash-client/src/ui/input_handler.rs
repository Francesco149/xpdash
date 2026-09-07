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
}

impl InputHandler {
    pub fn new() -> Self {
        Self {
            mode: ConfinementMode::Unconfined,
            last_cursor_pos: None,
            show_hud: false,
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
    }

    pub fn set_confined(&mut self, confined: bool) {
        self.mode = if confined {
            ConfinementMode::Confined
        } else {
            ConfinementMode::Unconfined
        };
        self.last_cursor_pos = None;
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
        // Enforce OS cursor lock mode
        if self.is_confined() {
            ctx.send_viewport_cmd(egui::ViewportCommand::CursorGrab(egui::CursorGrab::Locked));
            ctx.send_viewport_cmd(egui::ViewportCommand::CursorVisible(false));

            // Continuous relative hardware delta tracking (never clamped by window borders)
            let delta = ctx.input(|i| i.pointer.delta());
            if delta.x != 0.0 || delta.y != 0.0 {
                session.send_input(MsgInputEvent {
                    event_type: INPUT_TYPE_MOUSE_REL,
                    param1: 0,
                    param2: delta.x as i16,
                    param3: delta.y as i16,
                    key_down: 0,
                });
            }
        } else {
            ctx.send_viewport_cmd(egui::ViewportCommand::CursorGrab(egui::CursorGrab::None));
            ctx.send_viewport_cmd(egui::ViewportCommand::CursorVisible(true));
        }

        let events = ctx.input(|i| i.events.clone());
        for event in events {
            match event {
                // Key press / release
                Event::Key { key, pressed, repeat, modifiers: _, .. } => {
                    // Right Control or F12 toggles confinement
                    if key == Key::F10 && pressed && !repeat {
                        self.show_hud = !self.show_hud;
                        continue;
                    }

                    // Check for Right-Ctrl or escape release
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

                // Mouse motion
                Event::PointerMoved(pos) => {
                    if !self.is_confined() && viewport_rect.contains(pos) {
                        // Map absolute coordinate to guest space (0..65535)
                        let rel_x = (pos.x - viewport_rect.left()) / viewport_rect.width();
                        let rel_y = (pos.y - viewport_rect.top()) / viewport_rect.height();
                        let abs_x = (rel_x.clamp(0.0, 1.0) * 65535.0) as u16;
                        let abs_y = (rel_y.clamp(0.0, 1.0) * 65535.0) as u16;

                        session.send_input(MsgInputEvent {
                            event_type: INPUT_TYPE_MOUSE_ABS,
                            param1: 0,
                            param2: abs_x as i16,
                            param3: abs_y as i16,
                            key_down: 0,
                        });
                    }
                }

                // Mouse buttons
                Event::PointerButton { pos, button, pressed, .. } => {
                    if viewport_rect.contains(pos) {
                        if !self.is_confined() && pressed {
                            // Clicking into viewport captures cursor
                            self.set_confined(true);
                            self.last_cursor_pos = Some(pos);
                        }

                        let btn_flag: u16 = match button {
                            PointerButton::Primary => 0x0001,   // Left
                            PointerButton::Secondary => 0x0002, // Right
                            PointerButton::Middle => 0x0004,    // Middle
                            _ => 0,
                        };

                        if btn_flag != 0 {
                            session.send_input(MsgInputEvent {
                                event_type: INPUT_TYPE_MOUSE_BTN,
                                param1: btn_flag,
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
}
