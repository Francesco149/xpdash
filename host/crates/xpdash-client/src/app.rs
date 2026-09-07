//! Main xpdash-client GUI application implementing eframe::App.

use egui::{CentralPanel, Color32};

use crate::audio::AudioController;
use crate::network::{ClientSession, DiscoveryService, StreamState};
use crate::ui::dashboard::DashboardView;
use crate::ui::hud::HudOverlay;
use crate::ui::input_handler::InputHandler;
use crate::ui::viewport::StreamViewport;

pub struct XpDashApp {
    discovery: DiscoveryService,
    dashboard: DashboardView,
    viewport: StreamViewport,
    hud: HudOverlay,
    input: InputHandler,
    audio: AudioController,
    active_session: Option<ClientSession>,
}

impl XpDashApp {
    pub fn new(_cc: &eframe::CreationContext<'_>, initial_target: Option<String>) -> Self {
        let start_paused = initial_target.is_some();
        let discovery = DiscoveryService::start(start_paused);
        let dashboard = DashboardView::new();
        let viewport = StreamViewport::new();
        let hud = HudOverlay::new();
        let input = InputHandler::new();
        let audio = AudioController::new();

        let active_session = initial_target.map(|t| ClientSession::connect(t, Some(audio.producer())));

        Self {
            discovery,
            dashboard,
            viewport,
            hud,
            input,
            audio,
            active_session,
        }
    }
}

impl eframe::App for XpDashApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        let window_size = ctx.input(|i| i.screen_rect().size());
        let cursor_pos = ctx.input(|i| i.pointer.hover_pos());

        if let Some(ref session) = self.active_session {
            // Active Streaming Mode
            match session.state() {
                StreamState::Disconnected(reason) => {
                    log::info!("Stream session disconnected: {}", reason);
                    self.active_session = None;
                    self.input.set_confined(false);
                    self.discovery.resume();
                }
                _ => {
                    let metrics = session.metrics();

                    // 1. Render Fullscreen Stream Viewport
                    let viewport_rect = CentralPanel::default()
                        .frame(egui::Frame::new().fill(Color32::BLACK))
                        .show(ctx, |ui| {
                            self.viewport.render(ui, session, &mut self.input)
                        })
                        .inner;

                    // 2. Process Input Capture & Scancode Translation
                    self.input.handle_events(
                        ctx,
                        viewport_rect,
                        metrics.screen_width,
                        metrics.screen_height,
                        session,
                    );

                    // 3. Render Slide-Down In-Game HUD (Hover top or F10)
                    let request_disconnect = self.hud.render(
                        ctx,
                        session,
                        &mut self.input,
                        &self.audio,
                        &mut self.viewport.aspect_mode,
                        cursor_pos,
                        window_size,
                    );

                    if request_disconnect {
                        session.disconnect();
                        self.active_session = None;
                        self.input.set_confined(false);
                        self.discovery.resume();
                    }
                }
            }
        } else {
            // Machine Discovery Dashboard Mode
            CentralPanel::default()
                .frame(egui::Frame::new().fill(Color32::from_rgb(13, 17, 23)))
                .show(ctx, |ui| {
                    if let Some(target_ip) = self.dashboard.render(ui, &self.discovery) {
                        log::info!("Initiating stream connection to target: {}", target_ip);
                        self.discovery.pause();
                        self.active_session = Some(ClientSession::connect(target_ip, Some(self.audio.producer())));
                    }
                });
        }
    }
}
