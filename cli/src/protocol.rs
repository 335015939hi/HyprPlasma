//! Rust client bindings generated from the root-level protocol XML.
//!
//! The XML is the single source of truth. wayland-scanner generates the Rust
//! proxy types during Cargo compilation.

pub mod hyprplasma_control {
    use wayland_client;

    pub mod __interfaces {
        use wayland_client::protocol::__interfaces::*;

        wayland_scanner::generate_interfaces!("../hyprplasma-control-v1.xml");
    }

    use self::__interfaces::*;

    wayland_scanner::generate_client_code!("../hyprplasma-control-v1.xml");
}

// Make Cargo track the XML as an explicit compilation dependency.
// The proc-macro paths above are relative to cli/Cargo.toml.
const _PROTOCOL_XML: &str = include_str!("../../hyprplasma-control-v1.xml");
