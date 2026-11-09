//! Integrates the HyprPlasma session daemon with systemd-logind.
//!
//! The daemon listens on the system bus for session Lock/Unlock signals and
//! PrepareForSleep. A sleep delay inhibitor is held while the session is
//! running, released only after the configured lock command has been started,
//! and reacquired after resume.

use anyhow::{Context, Result, anyhow, bail};
use futures_util::StreamExt;
use std::time::Duration;
use tokio::process::Command;
use tokio::time::timeout;
use zbus::Connection;
use zbus::OwnedObjectPath;
use zbus::Proxy;

const LOGIND: &str = "org.freedesktop.login1";
const MANAGER_PATH: &str = "/org/freedesktop/login1";
const MANAGER_IFACE: &str = "org.freedesktop.login1.Manager";
const SESSION_IFACE: &str = "org.freedesktop.login1.Session";

// Logind's default InhibitDelayMaxSec is five seconds. Waiting less than that
// gives the locker time to create its lock surface without holding up sleep
// for the whole delay window.
const LOCK_STARTUP_GRACE: Duration = Duration::from_millis(750);

async fn get_session_path(manager: &Proxy<'_>) -> Result<OwnedObjectPath> {
    match manager
        .call("GetSessionByPID", &(std::process::id(),))
        .await
    {
        Ok(path) => Ok(path),
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
                })
        }
    }
}

async fn acquire_sleep_inhibitor(manager: &Proxy<'_>) -> Result<zbus::OwnedFd> {
    manager
        .call(
            "Inhibit",
            &(
                "sleep",
                "HyprPlasma",
                "Start the session lock screen before sleeping",
                "delay",
            ),
        )
        .await
        .context("failed to acquire logind sleep delay inhibitor")
}

async fn set_locked_hint(session: &Proxy<'static>, locked: bool) -> Result<()> {
    let _: () = session
        .call("SetLockedHint", &(locked,))
        .await
        .context("failed to update logind LockedHint")?;
    Ok(())
}

/// Launch the configured locker without blocking the daemon until the user
/// unlocks. Short-lived commands are treated as lock requests (for example,
/// a command that asks another process to display the lock screen). A
/// long-running locker process is reaped in the background and clears
/// LockedHint when it exits.
async fn start_lock_command(session: &Proxy<'static>, command: &str) -> Result<()> {
    if command.trim().is_empty() {
        bail!("lock_command is empty");
    }

    let mut child = Command::new("sh")
        .arg("-c")
        .arg(command)
        .spawn()
        .with_context(|| format!("failed to start lock_command: {command}"))?;

    match timeout(LOCK_STARTUP_GRACE, child.wait()).await {
        Ok(status) => {
            let status = status.context("failed waiting for lock_command")?;
            if !status.success() {
                bail!("lock_command exited unsuccessfully: {status}");
            }

            // A successful one-shot command is assumed to have handed the
            // request to the screen locker. The locker should clear this hint
            // when authentication unlocks the session.
            set_locked_hint(session, true).await?;
        }
        Err(_) => {
            // A command that remains running is likely the locker itself.
            // Mark the session locked after the startup grace period, then
            // clear the hint once that process exits.
            set_locked_hint(session, true).await?;

            let session = session.clone();
            tokio::spawn(async move {
                match child.wait().await {
                    Ok(status) if !status.success() => {
                        eprintln!("[logind] lock process exited unsuccessfully: {status}");
                    }
                    Err(error) => {
                        eprintln!("[logind] failed waiting for lock process: {error}");
                    }
                    _ => {}
                }

                if let Err(error) = set_locked_hint(&session, false).await {
                    eprintln!("[logind] failed to clear LockedHint after unlock: {error:#}");
                }
            });
        }
    }

    Ok(())
}

pub async fn run(lock_command: String) -> Result<()> {
    let connection = Connection::system()
        .await
        .context("failed to connect to the system D-Bus")?;

    let manager = Proxy::new(&connection, LOGIND, MANAGER_PATH, MANAGER_IFACE)
        .await
        .context("failed to create logind Manager proxy")?;

    // Prefer PID lookup so the daemon works even if XDG_SESSION_ID was not
    // propagated to its environment. Fall back to the standard session ID for
    // user-manager-launched instances.
    let session_path = get_session_path(&manager).await?;
    let session = Proxy::new_owned(
        connection.clone(),
        LOGIND.to_owned(),
        session_path,
        SESSION_IFACE.to_owned(),
    )
    .await
    .context("failed to create logind Session proxy")?;

    // Install all signal matches before acquiring the delay inhibitor. This
    // avoids missing PrepareForSleep(true) between setup and inhibitor setup.
    let mut lock_stream = session
        .receive_signal("Lock")
        .await
        .context("failed to subscribe to logind Session.Lock")?;
    let mut unlock_stream = session
        .receive_signal("Unlock")
        .await
        .context("failed to subscribe to logind Session.Unlock")?;
    let mut sleep_stream = manager
        .receive_signal("PrepareForSleep")
        .await
        .context("failed to subscribe to logind PrepareForSleep")?;
    let mut locked_hint_stream = session.receive_property_changed::<bool>("LockedHint").await;

    let mut locked = session
        .get_property::<bool>("LockedHint")
        .await
        .unwrap_or(false);

    let mut sleep_inhibitor = Some(acquire_sleep_inhibitor(&manager).await?);

    println!(
        "[logind] monitoring session {} (LockedHint={locked})",
        session.path()
    );

    loop {
        tokio::select! {
            message = lock_stream.next() => {
                let _message = message
                    .context("logind Session.Lock stream closed")?;

                println!("[logind] session lock requested");
                locked = session
                    .get_property::<bool>("LockedHint")
                    .await
                    .unwrap_or(locked);

                if !locked {
                    match start_lock_command(&session, &lock_command).await {
                        Ok(()) => locked = true,
                        Err(error) => eprintln!("[logind] failed to start lock screen: {error:#}"),
                    }
                }
            }

            message = unlock_stream.next() => {
                let _message = message
                    .context("logind Session.Unlock stream closed")?;

                // Unlock is a request to the session's screen-lock client.
                // Do not kill a locker or clear LockedHint here: the locker
                // must finish its own unlock flow and report the state change.
                println!("[logind] session unlock requested");
            }

            message = sleep_stream.next() => {
                let message = message
                    .context("logind PrepareForSleep stream closed")?;
                let preparing: bool = message
                    .body()
                    .deserialize()
                    .context("invalid PrepareForSleep signal payload")?;

                if preparing {
                    println!("[logind] preparing for sleep");
                    locked = session
                        .get_property::<bool>("LockedHint")
                        .await
                        .unwrap_or(locked);

                    if !locked {
                        match start_lock_command(&session, &lock_command).await {
                            Ok(()) => locked = true,
                            Err(error) => {
                                // Do not keep the machine awake indefinitely if
                                // the configured locker cannot be started.
                                eprintln!("[logind] failed to lock before sleep: {error:#}");
                            }
                        }
                    }

                    // Closing the FD releases the delay inhibitor, allowing
                    // logind to proceed with suspend/hibernate.
                    drop(sleep_inhibitor.take());
                } else {
                    println!("[logind] resumed");
                    locked = session
                        .get_property::<bool>("LockedHint")
                        .await
                        .unwrap_or(locked);

                    if sleep_inhibitor.is_none() {
                        sleep_inhibitor = Some(acquire_sleep_inhibitor(&manager).await?);
                    }
                }
            }

            change = locked_hint_stream.next() => {
                let change = change
                    .ok_or_else(|| anyhow!("logind LockedHint property stream closed"))?;
                match change.get().await {
                    Ok(value) => {
                        locked = value;
                        println!("[logind] LockedHint={locked}");
                    }
                    Err(error) => {
                        eprintln!("[logind] failed to read LockedHint update: {error}");
                    }
                }
            }
        }
    }
}
