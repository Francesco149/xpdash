//! Machine Discovery Dashboard: discovered machine roster grid, status cards, and manual connect bar.

use egui::{Color32, CornerRadius, Frame, Margin, RichText, Stroke};

use crate::network::{DiscoveredRig, DiscoveryService, LatencyBadge};

pub struct DashboardView {
    pub manual_ip: String,
}

impl DashboardView {
    pub fn new() -> Self {
        Self {
            manual_ip: "10.0.10.113".to_string(),
        }
    }

    pub fn render(
        &mut self,
        ui: &mut egui::Ui,
        discovery: &DiscoveryService,
    ) -> Option<String> {
        let mut connect_target = None;
        let roster = discovery.roster();
        let rigs: Vec<DiscoveredRig> = roster.read().clone();

        ui.add_space(12.0);

        // Header Banner
        Frame::new()
            .fill(Color32::from_rgb(18, 22, 28))
            .stroke(Stroke::new(1.0f32, Color32::from_rgb(34, 40, 49)))
            .corner_radius(CornerRadius::same(8))
            .inner_margin(Margin::same(16))
            .show(ui, |ui| {
                ui.horizontal(|ui| {
                    ui.vertical(|ui| {
                        ui.heading(
                            RichText::new("xpdash — Windows XP Remote Console")
                                .size(22.0)
                                .color(Color32::from_rgb(0, 210, 255))
                                .strong(),
                        );
                        ui.label(
                            RichText::new("Hardware EAX 3.0 Audio • 60 FPS Low-Latency Video • DirectInput Gaming")
                                .size(13.0)
                                .color(Color32::from_rgb(160, 175, 195)),
                        );
                    });

                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        ui.colored_label(
                            Color32::from_rgb(46, 204, 113),
                            RichText::new(format!("● LAN Discovery Active ({} Rigs)", rigs.len())).strong(),
                        );
                    });
                });
            });

        ui.add_space(16.0);

        // Manual Connect Bar
        Frame::new()
            .fill(Color32::from_rgb(22, 27, 34))
            .stroke(Stroke::new(1.0f32, Color32::from_rgb(40, 46, 56)))
            .corner_radius(CornerRadius::same(8))
            .inner_margin(Margin::same(12))
            .show(ui, |ui| {
                ui.horizontal(|ui| {
                    ui.label(RichText::new("Quick Connect:").strong().color(Color32::WHITE));
                    ui.add(
                        egui::TextEdit::singleline(&mut self.manual_ip)
                            .hint_text("Target IP (e.g. 10.0.10.113)")
                            .desired_width(180.0),
                    );

                    if ui.button(RichText::new("⚡ Connect").color(Color32::from_rgb(0, 210, 255)).strong()).clicked() {
                        if !self.manual_ip.trim().is_empty() {
                            connect_target = Some(self.manual_ip.trim().to_string());
                        }
                    }

                    if ui.button(RichText::new("🔍 Add to Probes").color(Color32::from_rgb(180, 200, 220))).clicked() {
                        if !self.manual_ip.trim().is_empty() {
                            discovery.add_target(self.manual_ip.trim().to_string());
                        }
                    }

                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        ui.label(
                            RichText::new("UDP 7022 Beacons • TCP 7020 Control • UDP 7021 Media")
                                .size(11.0)
                                .color(Color32::from_rgb(120, 135, 150)),
                        );
                    });
                });
            });

        ui.add_space(16.0);
        ui.heading(RichText::new("Discovered Rigs").size(16.0).color(Color32::from_rgb(220, 230, 242)));
        ui.add_space(8.0);

        // Machine Grid
        if rigs.is_empty() {
            Frame::new()
                .fill(Color32::from_rgb(18, 22, 28))
                .corner_radius(CornerRadius::same(8))
                .inner_margin(Margin::same(32))
                .show(ui, |ui| {
                    ui.vertical_centered(|ui| {
                        ui.label(
                            RichText::new("Scanning local network for Windows XP agents...")
                                .size(15.0)
                                .color(Color32::from_rgb(140, 160, 180)),
                        );
                        ui.add_space(8.0);
                        ui.label(
                            RichText::new("Make sure xpdash-agent.exe is running on the target machine.")
                                .size(12.0)
                                .color(Color32::from_rgb(100, 115, 130)),
                        );
                    });
                });
        } else {
            egui::ScrollArea::vertical().show(ui, |ui| {
                ui.horizontal_wrapped(|ui| {
                    for rig in &rigs {
                        if let Some(target) = render_rig_card(ui, rig) {
                            connect_target = Some(target);
                        }
                    }
                });
            });
        }

        connect_target
    }
}

fn render_rig_card(ui: &mut egui::Ui, rig: &DiscoveredRig) -> Option<String> {
    let mut connect_target = None;
    let card_width = 380.0;

    Frame::new()
        .fill(Color32::from_rgb(20, 25, 32))
        .stroke(Stroke::new(1.2f32, Color32::from_rgb(45, 55, 68)))
        .corner_radius(CornerRadius::same(8))
        .inner_margin(Margin::same(16))
        .show(ui, |ui| {
            ui.set_width(card_width);

            // Card Header: Rig Name & Status
            ui.horizontal(|ui| {
                ui.label(RichText::new("🖥").size(22.0));
                ui.vertical(|ui| {
                    ui.heading(RichText::new(&rig.name).size(17.0).color(Color32::WHITE).strong());
                    ui.label(
                        RichText::new(format!("{}:{}", rig.ip, rig.control_port))
                            .size(12.0)
                            .color(Color32::from_rgb(130, 145, 165)),
                    );
                });

                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    match rig.latency_category() {
                        LatencyBadge::Excellent(ms) => {
                            ui.colored_label(Color32::from_rgb(46, 204, 113), RichText::new(format!("● {:.2} ms", ms)).strong());
                        }
                        LatencyBadge::Good(ms) => {
                            ui.colored_label(Color32::from_rgb(241, 196, 15), RichText::new(format!("● {:.2} ms", ms)).strong());
                        }
                        LatencyBadge::Warning(ms) => {
                            ui.colored_label(Color32::from_rgb(231, 76, 60), RichText::new(format!("● {:.2} ms", ms)).strong());
                        }
                        LatencyBadge::Unknown => {
                            ui.colored_label(Color32::GRAY, "● Offline");
                        }
                    }
                });
            });

            ui.add_space(10.0);
            ui.separator();
            ui.add_space(8.0);

            // Specs Badges
            ui.horizontal(|ui| {
                // OS Badge
                render_badge(ui, &rig.os_badge, Color32::from_rgb(30, 80, 160), Color32::WHITE);
                // Screen Resolution
                render_badge(
                    ui,
                    &format!("{}x{}@{}bpp", rig.screen_width, rig.screen_height, rig.bpp),
                    Color32::from_rgb(40, 50, 65),
                    Color32::from_rgb(200, 215, 230),
                );
            });

            ui.add_space(6.0);

            // Audio Hardware Badge
            ui.horizontal(|ui| {
                ui.label(RichText::new("Audio:").size(11.0).color(Color32::from_rgb(130, 145, 160)));
                render_badge(
                    ui,
                    &rig.audio_badge,
                    Color32::from_rgb(80, 40, 110),
                    Color32::from_rgb(230, 190, 255),
                );
            });

            ui.add_space(6.0);

            // Security Status
            ui.horizontal(|ui| {
                ui.label(RichText::new("Security:").size(11.0).color(Color32::from_rgb(130, 145, 160)));
                ui.colored_label(
                    Color32::from_rgb(46, 204, 113),
                    RichText::new("🛡 Verified Session").size(11.0).strong(),
                );
            });

            ui.add_space(12.0);

            // Connect Button
            let btn = egui::Button::new(
                RichText::new("▶ Connect to Console")
                    .size(14.0)
                    .color(Color32::BLACK)
                    .strong(),
            )
            .fill(Color32::from_rgb(0, 210, 255))
            .corner_radius(CornerRadius::same(5));

            if ui.add_sized([card_width - 8.0, 32.0], btn).clicked() {
                connect_target = Some(rig.ip.clone());
            }
        });

    ui.add_space(12.0);
    connect_target
}

fn render_badge(ui: &mut egui::Ui, text: &str, bg: Color32, fg: Color32) {
    Frame::new()
        .fill(bg)
        .corner_radius(CornerRadius::same(4))
        .inner_margin(Margin::symmetric(6, 2))
        .show(ui, |ui| {
            ui.label(RichText::new(text).size(11.0).color(fg).strong());
        });
}
