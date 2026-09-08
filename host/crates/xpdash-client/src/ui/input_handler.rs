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
    pub last_modifiers: egui::Modifiers,
    pub last_super_down: bool,
}

impl InputHandler {
    pub fn new() -> Self {
        Self {
            mode: ConfinementMode::Unconfined,
            last_cursor_pos: None,
            show_hud: false,
            sensitivity: 1.0,
            accum_x: 0.0,
            accum_y: 0.0,
            is_at_edge: false,
            grab_mode: egui::CursorGrab::Locked,
            last_applied_grab: None,
            last_applied_visible: None,
            last_modifiers: egui::Modifiers::default(),
            last_super_down: false,
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
        if self.mode == ConfinementMode::Unconfined {
            self.last_super_down = false;
            self.last_modifiers = egui::Modifiers::default();
        }
    }

    pub fn toggle_confinement_with_release(&mut self, session: &ClientSession) {
        let was_confined = self.is_confined();
        self.toggle_confinement();
        if was_confined {
            self.release_all_modifiers(session);
        }
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
        if !confined {
            self.last_super_down = false;
            self.last_modifiers = egui::Modifiers::default();
        }
    }

    pub fn set_confined_with_release(&mut self, confined: bool, session: &ClientSession) {
        let was_confined = self.is_confined();
        self.set_confined(confined);
        if was_confined && !confined {
            self.release_all_modifiers(session);
        }
    }

    /// Release all active modifier keys (Super, Shift, Ctrl, Alt) on the guest
    pub fn release_all_modifiers(&mut self, session: &ClientSession) {
        if self.last_super_down {
            self.last_super_down = false;
            session.send_input(MsgInputEvent {
                event_type: INPUT_TYPE_KEY,
                param1: 0xE05B, // Left Windows / Super Key
                param2: 0,
                param3: 0,
                key_down: 0,
            });
        }
        if self.last_modifiers.shift {
            session.send_input(MsgInputEvent {
                event_type: INPUT_TYPE_KEY,
                param1: 0x2A, // Left Shift
                param2: 0,
                param3: 0,
                key_down: 0,
            });
        }
        if self.last_modifiers.ctrl {
            session.send_input(MsgInputEvent {
                event_type: INPUT_TYPE_KEY,
                param1: 0x1D, // Left Ctrl
                param2: 0,
                param3: 0,
                key_down: 0,
            });
        }
        if self.last_modifiers.alt {
            session.send_input(MsgInputEvent {
                event_type: INPUT_TYPE_KEY,
                param1: 0x38, // Left Alt
                param2: 0,
                param3: 0,
                key_down: 0,
            });
        }
        self.last_modifiers = egui::Modifiers::default();
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
        // Special Windows & modifier keys handling during confinement
        if self.is_confined() {
            // 1. Windows / Super Key capture
            let super_down = is_super_key_down(ctx);
            if super_down != self.last_super_down {
                self.last_super_down = super_down;
                session.send_input(MsgInputEvent {
                    event_type: INPUT_TYPE_KEY,
                    param1: 0xE05B, // Left Windows Key (PS/2 Set 1)
                    param2: 0,
                    param3: 0,
                    key_down: if super_down { 1 } else { 0 },
                });
            }

            // 2. Modifier keys: Shift, Ctrl, Alt
            let current_modifiers = ctx.input(|i| i.modifiers);
            if current_modifiers.shift != self.last_modifiers.shift {
                session.send_input(MsgInputEvent {
                    event_type: INPUT_TYPE_KEY,
                    param1: 0x2A, // Left Shift
                    param2: 0,
                    param3: 0,
                    key_down: if current_modifiers.shift { 1 } else { 0 },
                });
            }
            if current_modifiers.ctrl != self.last_modifiers.ctrl {
                session.send_input(MsgInputEvent {
                    event_type: INPUT_TYPE_KEY,
                    param1: 0x1D, // Left Ctrl
                    param2: 0,
                    param3: 0,
                    key_down: if current_modifiers.ctrl { 1 } else { 0 },
                });
            }
            if current_modifiers.alt != self.last_modifiers.alt {
                session.send_input(MsgInputEvent {
                    event_type: INPUT_TYPE_KEY,
                    param1: 0x38, // Left Alt
                    param2: 0,
                    param3: 0,
                    key_down: if current_modifiers.alt { 1 } else { 0 },
                });
            }
            self.last_modifiers = current_modifiers;
        } else if self.last_super_down
            || self.last_modifiers.shift
            || self.last_modifiers.ctrl
            || self.last_modifiers.alt
        {
            self.release_all_modifiers(session);
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

                    // F12 toggles pointer confinement; Escape releases it — LOCKOUT PROTECTION:
                    // F12 is the dedicated undo key and is NEVER captured or forwarded to guest!
                    if key == Key::F12 {
                        if pressed && !repeat {
                            self.toggle_confinement_with_release(session);
                        }
                        continue;
                    }
                    if key == Key::Escape && pressed && self.is_confined() {
                        self.set_confined_with_release(false, session);
                        continue;
                    }

                    // F11 toggles Fullscreen
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
                            self.toggle_confinement_with_release(session);
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
        Key::F12 => None, // Reserved as dedicated confinement toggle/undo key (lockout protection)

        // Navigation & editing keys
        Key::Insert => Some(0xE052),
        Key::Delete => Some(0xE053),
        Key::Home => Some(0xE047),
        Key::End => Some(0xE04F),
        Key::PageUp => Some(0xE049),
        Key::PageDown => Some(0xE051),

        // Punctuation & symbols
        Key::Colon | Key::Semicolon => Some(0x27),
        Key::Comma => Some(0x33),
        Key::Period => Some(0x34),
        Key::Slash => Some(0x35),
        Key::Backslash | Key::Pipe => Some(0x2B),
        Key::OpenBracket | Key::OpenCurlyBracket => Some(0x1A),
        Key::CloseBracket | Key::CloseCurlyBracket => Some(0x1B),
        Key::Backtick => Some(0x29),
        Key::Minus => Some(0x0C),
        Key::Equals | Key::Plus => Some(0x0D),
        Key::Quote => Some(0x28),
        _ => None,
    }
}

#[cfg(target_os = "linux")]
fn detect_super_key_linux() -> bool {
    use std::fs::File;
    use std::os::unix::io::AsRawFd;
    use std::path::PathBuf;
    use std::sync::LazyLock;

    const KEY_LEFTMETA: usize = 125;
    const KEY_RIGHTMETA: usize = 126;
    const EVIOCGKEY_64: libc::c_ulong = 0x80404518;
    const EVIOCGBIT_KEY_64: libc::c_ulong = 0x80404521;

    static KEYBOARD_DEVS: LazyLock<Vec<PathBuf>> = LazyLock::new(|| {
        let mut list = Vec::new();
        if let Ok(entries) = std::fs::read_dir("/dev/input") {
            for entry in entries.flatten() {
                let path = entry.path();
                if let Some(name) = path.file_name().and_then(|n| n.to_str()) {
                    if name.starts_with("event") {
                        if let Ok(file) = File::open(&path) {
                            let fd = file.as_raw_fd();
                            let mut cap_mask = [0u8; 64];
                            let ret = unsafe { libc::ioctl(fd, EVIOCGBIT_KEY_64, cap_mask.as_mut_ptr()) };
                            if ret >= 0 {
                                let byte_idx = KEY_LEFTMETA / 8;
                                let bit_idx = KEY_LEFTMETA % 8;
                                if (cap_mask[byte_idx] & (1 << bit_idx)) != 0 {
                                    list.push(path);
                                }
                            }
                        }
                    }
                }
            }
        }
        list
    });
    let devs = &*KEYBOARD_DEVS;

    for path in devs {
        if let Ok(file) = File::open(path) {
            let fd = file.as_raw_fd();
            let mut key_mask = [0u8; 64];
            let ret = unsafe { libc::ioctl(fd, EVIOCGKEY_64, key_mask.as_mut_ptr()) };
            if ret >= 0 {
                let left_byte = KEY_LEFTMETA / 8;
                let left_bit = KEY_LEFTMETA % 8;
                let right_byte = KEY_RIGHTMETA / 8;
                let right_bit = KEY_RIGHTMETA % 8;
                if (key_mask[left_byte] & (1 << left_bit)) != 0
                    || (key_mask[right_byte] & (1 << right_bit)) != 0
                {
                    return true;
                }
            }
        }
    }
    false
}

#[cfg(target_os = "windows")]
fn detect_super_key_windows() -> bool {
    extern "system" {
        fn GetAsyncKeyState(vKey: i32) -> i16;
    }
    const VK_LWIN: i32 = 0x5B;
    const VK_RWIN: i32 = 0x5C;
    unsafe {
        (GetAsyncKeyState(VK_LWIN) as u16 & 0x8000 != 0)
            || (GetAsyncKeyState(VK_RWIN) as u16 & 0x8000 != 0)
    }
}

/// Detect whether the Special Windows / Super key is currently pressed on the host
pub fn is_super_key_down(ctx: &egui::Context) -> bool {
    // 1. egui modifier state check (e.g. macOS Cmd or platforms exposing command)
    if ctx.input(|i| i.modifiers.mac_cmd) {
        return true;
    }

    // 2. Linux evdev hardware key state
    #[cfg(target_os = "linux")]
    {
        if detect_super_key_linux() {
            return true;
        }
    }

    // 3. Windows GetAsyncKeyState hardware key state
    #[cfg(target_os = "windows")]
    {
        if detect_super_key_windows() {
            return true;
        }
    }

    false
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

        // F12 must NEVER be mapped to a scancode (lockout protection)
        assert_eq!(key_to_ps2_scancode(Key::F12), None);

        // Navigation keys
        assert_eq!(key_to_ps2_scancode(Key::Insert), Some(0xE052));
        assert_eq!(key_to_ps2_scancode(Key::Delete), Some(0xE053));
        assert_eq!(key_to_ps2_scancode(Key::Home), Some(0xE047));
        assert_eq!(key_to_ps2_scancode(Key::End), Some(0xE04F));

        // Punctuation
        assert_eq!(key_to_ps2_scancode(Key::Semicolon), Some(0x27));
        assert_eq!(key_to_ps2_scancode(Key::Comma), Some(0x33));
        assert_eq!(key_to_ps2_scancode(Key::Period), Some(0x34));
        assert_eq!(key_to_ps2_scancode(Key::Slash), Some(0x35));
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
        assert_eq!(handler.sensitivity, 1.0);

        // delta of 0.4 points with 1.0 sens = 0.4 -> send_dx = 0, remainder 0.4
        handler.accum_x += 0.4 * handler.sensitivity;
        let eps = if handler.accum_x >= 0.0 { 1e-4 } else { -1e-4 };
        let send_dx = (handler.accum_x + eps).trunc() as i16;
        assert_eq!(send_dx, 0);
        assert!((handler.accum_x - 0.40).abs() < 1e-3);

        // After 0.8 more points (total 1.2 * 1.0 = 1.2): send_dx = 1, remainder 0.20
        handler.accum_x += 0.8 * handler.sensitivity;
        let eps2 = if handler.accum_x >= 0.0 { 1e-4 } else { -1e-4 };
        let send_dx2 = (handler.accum_x + eps2).trunc() as i16;
        assert_eq!(send_dx2, 1);
        handler.accum_x -= send_dx2 as f32;
        assert!((handler.accum_x - 0.20).abs() < 1e-3);
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

    #[test]
    fn test_lockout_protection_f12_not_mapped() {
        // F12 must NEVER map to any guest scancode (lockout protection guarantee)
        assert_eq!(key_to_ps2_scancode(Key::F12), None);
    }

    #[test]
    fn test_is_super_key_down_safety() {
        let ctx = egui::Context::default();
        // Must execute safely on current platform without panicking
        let _ = is_super_key_down(&ctx);
    }

    #[test]
    fn test_modifier_tracking_state() {
        let mut handler = InputHandler::new();
        assert_eq!(handler.sensitivity, 1.0);
        assert_eq!(handler.last_modifiers, egui::Modifiers::default());
        assert!(!handler.last_super_down);

        // Simulate modifiers changing
        handler.last_modifiers.shift = true;
        handler.last_modifiers.ctrl = true;
        handler.last_super_down = true;

        // set_confined(false) resets modifier tracking
        handler.set_confined(false);
        assert_eq!(handler.last_modifiers, egui::Modifiers::default());
        assert!(!handler.last_super_down);
    }
}
