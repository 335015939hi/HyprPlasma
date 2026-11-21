use anyhow::{Context, Result, bail};
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, Ordering},
};
use std::time::{Duration, Instant};
use tokio::process::Command;
use tokio::sync::Mutex as AsyncMutex;
use tokio::time::sleep;
use zbus::{Connection, Proxy, interface, object_server::SignalEmitter};

const LOGIND: &str = "org.freedesktop.login1";
const MANAGER_PATH: &str = "/org/freedesktop/login1";
const MANAGER_IFACE: &str = "org.freedesktop.login1.Manager";
const SESSION_IFACE: &str = "org.freedesktop.login1.Session";

#[derive(Clone)]
pub struct ScreenSaver {
    inner: Arc<ScreenSaverInner>,
}

struct ScreenSaverInner {
    lock_command: String,
    session_bus: Connection,
    active: AtomicBool,
    active_since: Mutex<Option<Instant>>,
    launch_guard: AsyncMutex<()>,
}

impl ScreenSaver {
    pub fn new(lock_command: String, session_bus: Connection) -> Self {
        Self {
            inner: Arc::new(ScreenSaverInner {
                lock_command,
                session_bus,
                active: AtomicBool::new(false),
                active_since: Mutex::new(None),
                launch_guard: AsyncMutex::new(()),
            }),
        }
    }

    pub fn is_active(&self) -> bool {
        self.inner.active.load(Ordering::Acquire)
    }

    pub fn active_time_seconds(&self) -> u32 {
        self.inner
            .active_since
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .map(|since| since.elapsed().as_secs().min(u32::MAX as u64) as u32)
            .unwrap_or(0)
    }

    /// Synchronize the exported ScreenSaver state with logind's LockedHint.
    /// This also emits ActiveChanged when the state actually changes.
    pub async fn sync_active_state(&self, active: bool) {
        self.set_active_state(active).await;
    }

    /// Start the configured locker if it is not already running.
    ///
    /// The child is reaped in the background, so neither the D-Bus Lock method
    /// nor logind's PrepareForSleep handler waits for the user to unlock.
    /// minimum_runtime is used by the sleep path to give Hyprlock time to draw
    /// its first frame before the logind delay inhibitor is released.
    pub async fn start(&self, minimum_runtime: Duration) -> Result<()> {
        let child_and_start = {
            let _guard = self.inner.launch_guard.lock().await;

            if self.is_active() {
                None
            } else {
                if self.inner.lock_command.trim().is_empty() {
                    bail!("lock_command is empty");
                }

                let child = Command::new("sh")
                    .arg("-c")
                    .arg(&self.inner.lock_command)
                    .spawn()
                    .with_context(|| {
                        format!("failed to start lock_command: {}", self.inner.lock_command)
                    })?;

                let spawned_at = Instant::now();
                self.set_active_state(true).await;
                Some((child, spawned_at))
            }
        };

        if let Some((mut child, spawned_at)) = child_and_start {
            let screen_saver = self.clone();
            tokio::spawn(async move {
                match child.wait().await {
                    Ok(status) if !status.success() => {
                        eprintln!("[screensaver] lock process exited unsuccessfully: {status}");
                    }
                    Ok(_) => {}
                    Err(error) => {
                        eprintln!("[screensaver] failed waiting for lock process: {error}");
                    }
                }

                screen_saver.set_active_state(false).await;
            });

            let remaining = minimum_runtime.saturating_sub(spawned_at.elapsed());
            if !remaining.is_zero() {
                sleep(remaining).await;
            }
        }

        Ok(())
    }

    async fn set_active_state(&self, active: bool) {
        let changed = {
            let mut since = self
                .inner
                .active_since
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            let was_active = self.inner.active.swap(active, Ordering::AcqRel);

            if active && !was_active {
                *since = Some(Instant::now());
            } else if !active && was_active {
                *since = None;
            }

            was_active != active
        };

        if !changed {
            return;
        }

        match SignalEmitter::new(&self.inner.session_bus, "/ScreenSaver") {
            Ok(emitter) => {
                if let Err(error) = emitter.active_changed(active).await {
                    eprintln!("[screensaver] failed to emit ActiveChanged: {error}");
                }
            }
            Err(error) => {
                eprintln!("[screensaver] failed to create signal emitter: {error}");
            }
        }

        if let Err(error) = set_logind_locked_hint(active).await {
            eprintln!("[screensaver] failed to update logind LockedHint: {error:#}");
        }
    }
}

async fn set_logind_locked_hint(locked: bool) -> Result<()> {
    let connection = Connection::system()
        .await
        .context("failed to connect to the system D-Bus")?;
    let manager = Proxy::new(&connection, LOGIND, MANAGER_PATH, MANAGER_IFACE)
        .await
        .context("failed to create logind Manager proxy")?;

    let session_path = match manager
        .call("GetSessionByPID", &(std::process::id(),))
        .await
    {
        Ok(path) => path,
        Err(pid_error) => {
            let session_id = std::env::var("XDG_SESSION_ID")
                .context("GetSessionByPID failed and XDG_SESSION_ID is not set")?;

            manager
                .call("GetSession", &(session_id.as_str(),))
                .await
                .with_context(|| {
                    format!(
                        "GetSessionByPID failed ({pid_error}) and GetSession({session_id}) failed"
                    )
                })?
        }
    };

    let session = Proxy::new_owned(
        connection,
        LOGIND.to_owned(),
        session_path,
        SESSION_IFACE.to_owned(),
    )
    .await
    .context("failed to create logind Session proxy")?;

    let _: () = session
        .call("SetLockedHint", &(locked,))
        .await
        .context("failed to update logind LockedHint")?;

    Ok(())
}

#[interface(name = "org.freedesktop.ScreenSaver")]
impl ScreenSaver {
    #[zbus(name = "Lock")]
    async fn lock(&self) -> zbus::fdo::Result<()> {
        self.start(Duration::ZERO)
            .await
            .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))
    }

    #[zbus(name = "GetActive")]
    fn get_active(&self) -> bool {
        self.is_active()
    }

    #[zbus(name = "GetActiveTime")]
    fn get_active_time(&self) -> u32 {
        self.active_time_seconds()
    }

    #[zbus(name = "SetActive")]
    async fn set_active_request(&self, active: bool) -> zbus::fdo::Result<bool> {
        // Unlocking is exclusively handled by the screen locker's
        // authentication flow. SetActive(false) must not bypass it.
        if active {
            self.start(Duration::ZERO)
                .await
                .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;
        }

        Ok(self.is_active())
    }

    #[zbus(signal)]
    async fn active_changed(
        signal_emitter: &SignalEmitter<'_>,
        active: bool,
    ) -> zbus::Result<()>;
}
