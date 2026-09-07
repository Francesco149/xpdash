use std::net::SocketAddr;
use tokio::net::UdpSocket;
use xpdash_core::{UDP_BEACON_PORT, TCP_CONTROL_PORT, UDP_MEDIA_PORT};

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    env_logger::init_from_env(env_logger::Env::default().default_filter_or("info"));
    log::info!("xpdash-server initializing...");

    // Bind discovery broadcaster
    let socket = UdpSocket::bind("0.0.0.0:0").await?;
    socket.set_broadcast(true)?;

    let target: SocketAddr = format!("255.255.255.255:{}", UDP_BEACON_PORT).parse()?;

    log::info!(
        "Broadcasting LAN discovery beacons on UDP {} -> Control: {}, Media: {}",
        UDP_BEACON_PORT, TCP_CONTROL_PORT, UDP_MEDIA_PORT
    );

    let mut beacon = Vec::with_capacity(64);
    beacon.extend_from_slice(b"XPD\x01");
    beacon.extend_from_slice(&TCP_CONTROL_PORT.to_le_bytes());
    beacon.extend_from_slice(&UDP_MEDIA_PORT.to_le_bytes());
    beacon.push(0); // Name length
    beacon.push(6);
    beacon.extend_from_slice(b"XPDash");

    let mut interval = tokio::time::interval(std::time::Duration::from_secs(2));
    loop {
        interval.tick().await;
        let _ = socket.send_to(&beacon, target).await;
        log::debug!("Sent discovery beacon");
    }
}
