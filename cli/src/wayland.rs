//! Small client for the HyprPlasma Wayland control protocol.
//!
//! This file deliberately contains no daemon loop or session functionality.
//! It only discovers the plugin's global and sends its one request.

use std::error::Error;
use std::io;

use wayland_client::protocol::wl_registry;
use wayland_client::{Connection, Dispatch, Proxy, QueueHandle};

use crate::protocol::hyprplasma_control::hyprplasma_control_v1::HyprplasmaControlV1;

#[derive(Default)]
struct RegistryState {
    control: Option<HyprplasmaControlV1>,
}

impl Dispatch<wl_registry::WlRegistry, ()> for RegistryState {
    fn event(
        state: &mut Self,
        registry: &wl_registry::WlRegistry,
        event: wl_registry::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wl_registry::Event::Global {
            name,
            interface,
            version,
        } = event
        {
            if interface != HyprplasmaControlV1::interface().name
                || state.control.is_some()
            {
                return;
            }

            state.control = Some(
                registry.bind::<HyprplasmaControlV1, _, _>(
                    name,
                    version.min(1),
                    qh,
                    (),
                ),
            );
        }
    }
}

// This protocol interface has no server-to-client events.
wayland_client::delegate_noop!(
    RegistryState: ignore HyprplasmaControlV1
);

/// Connect to the current Wayland compositor and request a show-desktop toggle.
///
/// The final roundtrip waits until the compositor has processed the request,
/// so this one-shot helper can safely return before its connection is dropped.
pub fn toggle_show_desktop() -> Result<(), Box<dyn Error>> {
    let connection = Connection::connect_to_env()?;

    let mut event_queue = connection.new_event_queue();
    let qh = event_queue.handle();

    let display = connection.display();
    let _registry = display.get_registry(&qh, ());

    let mut state = RegistryState::default();

    // Receive the initial registry globals and bind the HyprPlasma interface.
    event_queue.roundtrip(&mut state)?;

    let control = state.control.as_ref().ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::NotFound,
            "HyprPlasma control protocol is not advertised by this compositor",
        )
    })?;

    control.toggle_show_desktop();

    // Ensure the request is sent and processed before this function returns.
    connection.flush()?;
    event_queue.roundtrip(&mut state)?;

    Ok(())
}
