use xpdash_core::audio;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    env_logger::init_from_env(env_logger::Env::default().default_filter_or("info"));
    log::info!("xpdash-client initializing...");
    log::info!(
        "Audio engine configured for {} Hz {} ch {} bit ({} ms slices)",
        audio::SAMPLE_RATE,
        audio::CHANNELS,
        audio::BITS_PER_SAMPLE,
        audio::FRAME_MS
    );
    Ok(())
}
