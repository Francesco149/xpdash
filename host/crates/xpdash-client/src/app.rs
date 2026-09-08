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
    pub obs_source_mode: bool,
    last_applied_stream_w: u16,
    last_applied_stream_h: u16,
}

impl XpDashApp {
    pub fn new(_cc: &eframe::CreationContext<'_>, initial_target: Option<String>, obs_mode: bool) -> Self {
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
            obs_source_mode: obs_mode,
            last_applied_stream_w: 0,
            last_applied_stream_h: 0,
        }
    }
}

impl eframe::App for XpDashApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        // F9 hotkey toggles OBS 1x Source Mode
        if ctx.input(|i| i.key_pressed(egui::Key::F9)) {
            self.obs_source_mode = !self.obs_source_mode;
            log::info!("OBS 1x Source Mode toggled via F9: {}", self.obs_source_mode);
            if !self.obs_source_mode {
                ctx.send_viewport_cmd(egui::ViewportCommand::Title("xpdash — Windows XP Remote Console".to_string()));
            }
        }

        let window_size = ctx.input(|i| i.screen_rect().size());
        let cursor_pos = ctx.input(|i| i.pointer.hover_pos());

        if let Some(session) = &self.active_session {
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

                    // In OBS Source Mode: automatically lock window inner size to 1x native stream size
                    // and ensure window title is stable for OBS Window/Game Capture rules
                    if self.obs_source_mode {
                        let sw = metrics.screen_width;
                        let sh = metrics.screen_height;
                        if sw > 0 && sh > 0 && (sw != self.last_applied_stream_w || sh != self.last_applied_stream_h) {
                            self.last_applied_stream_w = sw;
                            self.last_applied_stream_h = sh;
                            log::info!("[OBS Mode] Auto-resizing window to exact 1x source dimensions: {}x{}", sw, sh);
                            ctx.send_viewport_cmd(egui::ViewportCommand::InnerSize(egui::vec2(sw as f32, sh as f32)));
                        }

                        let title = format!("xpdash — [{}] (OBS Source)", metrics.machine_name);
                        ctx.send_viewport_cmd(egui::ViewportCommand::Title(title));
                    }

                    // 1. Render Fullscreen Stream Viewport
                    let viewport_rect = CentralPanel::default()
                        .frame(egui::Frame::new().fill(Color32::BLACK))
                        .show(ctx, |ui| {
                            self.viewport.render(ui, session, &mut self.input, self.obs_source_mode)
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
                        &mut self.obs_source_mode,
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
