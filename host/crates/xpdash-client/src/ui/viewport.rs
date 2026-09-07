//! Hardware-accelerated texture streaming surface with aspect-ratio preserving scaling.

use egui::{Color32, CornerRadius, Pos2, Rect, Stroke, StrokeKind, TextureHandle, TextureOptions, Vec2};

use crate::network::ClientSession;
use crate::ui::input_handler::{ConfinementMode, InputHandler};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AspectRatioMode {
    Fit4x3,
    Integer1x,
    Integer2x,
    Stretch,
}

impl AspectRatioMode {
    pub fn label(&self) -> &'static str {
        match self {
            Self::Fit4x3 => "Fit 4:3 (Pillared)",
            Self::Integer1x => "Integer 1x",
            Self::Integer2x => "Integer 2x",
            Self::Stretch => "Bilinear Stretch",
        }
    }
}

pub struct StreamViewport {
    texture: Option<TextureHandle>,
    last_frame_index: u32,
    pub aspect_mode: AspectRatioMode,
}

impl StreamViewport {
    pub fn new() -> Self {
        Self {
            texture: None,
            last_frame_index: 0,
            aspect_mode: AspectRatioMode::Fit4x3,
        }
    }

    pub fn render(
        &mut self,
        ui: &mut egui::Ui,
        session: &ClientSession,
        input: &mut InputHandler,
    ) -> Rect {
        let ctx = ui.ctx();
        let latest = session.latest_frame();

        // 1. Update texture if new frame arrived
        if let Some(ref frame) = latest {
            if frame.frame_index != self.last_frame_index || self.texture.is_none() {
                self.last_frame_index = frame.frame_index;
                let pixels: &[egui::Color32] = bytemuck::cast_slice(&frame.rgba_pixels);
                let color_image = egui::ColorImage {
                    size: [frame.width as usize, frame.height as usize],
                    pixels: pixels.to_vec(),
                };

                let tex_options = match self.aspect_mode {
                    AspectRatioMode::Integer1x | AspectRatioMode::Integer2x => TextureOptions::NEAREST,
                    _ => TextureOptions::LINEAR,
                };

                match &mut self.texture {
                    Some(tex) => tex.set(color_image, tex_options),
                    None => {
                        self.texture = Some(ctx.load_texture("stream_viewport", color_image, tex_options));
                    }
                }
            }
        }

        // 2. Cursor icon: Crosshair when confined, Default when unconfined
        if input.is_confined() {
            ctx.set_cursor_icon(egui::CursorIcon::Crosshair);
        } else {
            ctx.set_cursor_icon(egui::CursorIcon::Default);
        }

        // 3. Compute Viewport Rectangle based on Aspect Ratio mode
        let avail_rect = ui.available_rect_before_wrap();
        let avail_size = avail_rect.size();

        let (guest_w, guest_h) = latest.as_ref().map(|f| (f.width as f32, f.height as f32)).unwrap_or((800.0, 600.0));

        let target_size = calculate_target_size(self.aspect_mode, avail_size, guest_w, guest_h);

        // Center viewport inside available space
        let offset = (avail_size - target_size) * 0.5;
        let viewport_rect = Rect::from_min_size(avail_rect.min + offset, target_size);

        // Fill background with deep black pillars
        ui.painter().rect_filled(avail_rect, CornerRadius::ZERO, Color32::BLACK);

        // 4. Paint Texture
        if let Some(ref tex) = self.texture {
            let uv = Rect::from_min_max(Pos2::new(0.0, 0.0), Pos2::new(1.0, 1.0));
            ui.painter().image(tex.id(), viewport_rect, uv, Color32::WHITE);
        } else {
            // Waiting for video stream banner
            ui.painter().rect_filled(viewport_rect, CornerRadius::same(4), Color32::from_rgb(16, 20, 26));
            ui.painter().text(
                viewport_rect.center(),
                egui::Align2::CENTER_CENTER,
                "Connecting to Windows XP Stream...",
                egui::FontId::proportional(18.0),
                Color32::from_rgb(0, 200, 240),
            );
        }

        // 5. Ambient Confinement Border
        let (border_color, stroke_width) = match input.mode {
            ConfinementMode::Confined => (Color32::from_rgb(255, 170, 0), 2.5f32), // Amber glow
            ConfinementMode::Unconfined => (Color32::from_rgb(0, 180, 216), 1.5f32), // Cyan subtle border
        };
        ui.painter().rect_stroke(viewport_rect, CornerRadius::ZERO, Stroke::new(stroke_width, border_color), StrokeKind::Inside);

        // Request continuous repaint while streaming
        ctx.request_repaint();

        viewport_rect
    }
}

/// Pure function calculating target viewport dimensions
pub fn calculate_target_size(
    mode: AspectRatioMode,
    avail_size: Vec2,
    guest_w: f32,
    guest_h: f32,
) -> Vec2 {
    let guest_aspect = guest_w / guest_h;
    match mode {
        AspectRatioMode::Fit4x3 => {
            let mut w = avail_size.x;
            let mut h = w / guest_aspect;
            if h > avail_size.y {
                h = avail_size.y;
                w = h * guest_aspect;
            }
            Vec2::new(w, h)
        }
        AspectRatioMode::Integer1x => {
            Vec2::new(guest_w.min(avail_size.x), guest_h.min(avail_size.y))
        }
        AspectRatioMode::Integer2x => {
            Vec2::new((guest_w * 2.0).min(avail_size.x), (guest_h * 2.0).min(avail_size.y))
        }
        AspectRatioMode::Stretch => avail_size,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_aspect_ratio_fit_4x3_pillared() {
        // 16:9 monitor (1920x1080) with 800x600 (4:3) stream
        let avail = Vec2::new(1920.0, 1080.0);
        let size = calculate_target_size(AspectRatioMode::Fit4x3, avail, 800.0, 600.0);
        // Height should be 1080, width should be 1080 * (4/3) = 1440
        assert_eq!(size.y, 1080.0);
        assert_eq!(size.x, 1440.0);
    }

    #[test]
    fn test_aspect_ratio_integer_scaling() {
        let avail = Vec2::new(1920.0, 1080.0);
        let size_1x = calculate_target_size(AspectRatioMode::Integer1x, avail, 800.0, 600.0);
        assert_eq!(size_1x, Vec2::new(800.0, 600.0));

        let size_2x = calculate_target_size(AspectRatioMode::Integer2x, avail, 800.0, 600.0);
        assert_eq!(size_2x.x, 1600.0);
        assert_eq!(size_2x.y, 1080.0); // clamped by window height 1080
    }

    #[test]
    fn test_aspect_ratio_stretch() {
        let avail = Vec2::new(1280.0, 720.0);
        let size = calculate_target_size(AspectRatioMode::Stretch, avail, 800.0, 600.0);
        assert_eq!(size, avail);
    }
}
