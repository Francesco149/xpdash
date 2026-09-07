pub mod discovery;
pub mod session;
pub use discovery::{DiscoveredRig, DiscoveryService, LatencyBadge};
pub use session::{ClientSession, StreamState};
