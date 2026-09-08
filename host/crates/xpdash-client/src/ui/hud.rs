//! Slide-down in-game HUD overlay with live telemetry diagnostics and controls.

use egui::{Color32, CornerRadius, Frame, Margin, Pos2, Rect, Stroke, Vec2};

use crate::audio::AudioController;
use crate::network::ClientSession;
use crate::ui::input_handler::InputHandler;
use crate::ui::viewport::AspectRatioMode;

pub struct HudOverlay {
    pub is_pinned: bool,
}

impl HudOverlay {
    pub fn new() -> Self {
        Self { is_pinned: false }
    }

    pub fn render(
        &mut self,
        ctx: &egui::Context,
        session: &ClientSession,
        input: &mut InputHandler,
        audio: &AudioController,
        aspect_mode: &mut AspectRatioMode,
        cursor_pos: Option<Pos2>,
        window_size: Vec2,
    ) -> bool {
        // Trigger hover if cursor is in the top 16 pixels
        let is_hovering_top = cursor_pos.map(|p| p.y <= 16.0).unwrap_or(false);
        let visible = self.is_pinned || input.show_hud || is_hovering_top;

        if !visible {
            return false;
        }

        let metrics = session.metrics();
        let mut request_disconnect = false;

        let hud_width = 860.0_f32.min(window_size.x - 32.0);
        let hud_rect = Rect::from_min_size(
            Pos2::new((window_size.x - hud_width) / 2.0, 10.0),
            Vec2::new(hud_width, 100.0),
        );

        egui::Area::new(egui::Id::new("in_game_hud"))
            .fixed_pos(hud_rect.min)
            .order(egui::Order::Foreground)
            .show(ctx, |ui| {
                Frame::new()
                    .fill(Color32::from_rgba_premultiplied(18, 22, 28, 235))
                    .stroke(Stroke::new(1.5_f32, Color32::from_rgb(0, 180, 216)))
                    .corner_radius(CornerRadius::same(8))
                    .inner_margin(Margin::same(10))
                    .show(ui, |ui| {
                        ui.set_width(hud_width - 20.0);

                        // Row 1: Diagnostics Bar
                        ui.horizontal(|ui| {
                            ui.colored_label(Color32::from_rgb(0, 210, 255), egui::RichText::new("xpdash HUD").strong());
                            ui.separator();

                            // Latency Badge
                            let rtt = metrics.rtt_ms;
                            let (rtt_color, rtt_text) = if rtt < 5.0 {
                                (Color32::from_rgb(46, 204, 113), format!("{:.2} ms RTT", rtt))
                            } else if rtt <= 20.0 {
                                (Color32::from_rgb(241, 196, 15), format!("{:.2} ms RTT", rtt))
                            } else {
                                (Color32::from_rgb(231, 76, 60), format!("{:.2} ms RTT", rtt))
                            };
                            ui.colored_label(rtt_color, egui::RichText::new(format!("● {}", rtt_text)).strong());

                            // Glass-to-glass estimate: RTT/2 + 10ms buffer
                            let g2g = (rtt / 2.0) + 10.0;
                            ui.label(format!("G2G: ~{:.1} ms", g2g));

                            ui.separator();
                            // FPS & Bitrate
                            ui.label(format!("{:.1} FPS", metrics.fps));
                            ui.label(format!("{:.1} Mbps", metrics.kbps / 1000.0));

                            ui.separator();
                            // Resolution
                            ui.label(format!("{}x{}", metrics.screen_width, metrics.screen_height));

                            // Audio health
                            ui.separator();
                            ui.label(format!("Audio Jitter: ±{:.2}ms", metrics.audio_jitter_ms));

                            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                                if ui.small_button(if self.is_pinned { "📌 Unpin" } else { "📍 Pin" }).clicked() {
                                    self.is_pinned = !self.is_pinned;
                                }
                            });
                        });

                        ui.add_space(6.0);

                        // Row 2: Quick Action Bar
                        ui.horizontal(|ui| {
                            // Pointer Lock Toggle
                            let is_conf = input.is_confined();
                            let lock_btn_text = if is_conf {
                                egui::RichText::new("🔒 Confined [F12]").color(Color32::from_rgb(255, 170, 0))
                            } else {
                                egui::RichText::new("🔓 Unconfined [F12]").color(Color32::from_rgb(0, 220, 255))
                            };
                            if ui.button(lock_btn_text).clicked() {
                                input.toggle_confinement();
                            }

                            // Grab mode selector for Wayland / niri compatibility
                            let grab_label = match input.grab_mode {
                                egui::CursorGrab::Locked => "Grab: Locked",
                                egui::CursorGrab::Confined => "Grab: Confined",
                                egui::CursorGrab::None => "Grab: None",
                            };
                            if ui.small_button(grab_label).on_hover_text("Switch between Locked (pointer lock) and Confined (window bounds). Press F11 for Fullscreen on Wayland/niri.").clicked() {
                                input.grab_mode = match input.grab_mode {
                                    egui::CursorGrab::Locked => egui::CursorGrab::Confined,
                                    _ => egui::CursorGrab::Locked,
                                };
                            }

                            ui.separator();

                            // Volume Slider & Mute
                            let mut vol = audio.volume();
                            let is_muted = audio.is_muted();
                            let mute_btn = if is_muted { "🔇 Unmute" } else { "🔊 Mute" };
                            if ui.button(mute_btn).clicked() {
                                audio.set_muted(!is_muted);
                            }

                            ui.label("Vol:");
                            if ui.add(egui::Slider::new(&mut vol, 0.0..=1.5).text("").show_value(true)).changed() {
                                audio.set_volume(vol);
                            }

                            ui.separator();

                            // In-Game Mouse Sensitivity Slider
                            ui.label("Sens:");
                            let mut sens = input.sensitivity;
                            if ui.add(egui::Slider::new(&mut sens, 0.005..=0.10).text("").show_value(true)).changed() {
                                input.sensitivity = sens;
                            }

                            ui.separator();

                            // Aspect Ratio Mode
                            egui::ComboBox::from_label("")
                                .selected_text(aspect_mode.label())
                                .show_ui(ui, |ui| {
                                    ui.selectable_value(aspect_mode, AspectRatioMode::Fit4x3, "Fit 4:3 (Pillared)");
                                    ui.selectable_value(aspect_mode, AspectRatioMode::Integer1x, "Integer 1x");
                                    ui.selectable_value(aspect_mode, AspectRatioMode::Integer2x, "Integer 2x");
                                    ui.selectable_value(aspect_mode, AspectRatioMode::Stretch, "Bilinear Stretch");
                                });

                            ui.separator();

                            // Send Ctrl+Alt+Del
                            if ui.button("Send Ctrl+Alt+Del").clicked() {
                                InputHandler::send_ctrl_alt_del(session);
                            }

                            // Disconnect Button
                            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                                if ui.button(egui::RichText::new("⏹ Disconnect").color(Color32::from_rgb(235, 87, 87))).clicked() {
                                    request_disconnect = true;
                                }
                            });
                        });
                    });
            });

        request_disconnect
    }
}
