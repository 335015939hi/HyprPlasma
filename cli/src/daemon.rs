use crate::config;
use crate::config::ConfigKeys;
use anyhow::Result;
use std::env;
use std::io;
use std::io::BufRead;
use std::io::BufReader;
use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::process::Command;
use std::process::ExitStatus;
use zbus::connection;
use zbus::interface;

// org.kde.LogoutPrompt
// Object path: /LogoutPrompt

struct LogoutPrompt;

fn run_command(cmd: &str) -> Result<ExitStatus> {
    let status = Command::new("sh").arg("-c").arg(cmd).status()?;
    Ok(status)
}

#[interface(name = "org.kde.LogoutPrompt")]
impl LogoutPrompt {
    #[zbus(name = "promptLogout")]
    fn prompt_logout(&self) {
        println!("[LogoutPrompt] Logout requested");
        let cmd = &config::get_as_str(ConfigKeys::LogoutCommand);
        let status = run_command(cmd).unwrap();
        println!("{cmd} exited with {status}");
    }

    #[zbus(name = "promptShutDown")]
    fn prompt_shutdown(&self) {
        println!("[LogoutPrompt] Shutdown requested");
        let cmd = &config::get_as_str(ConfigKeys::ShutdownCommand);
        let status = run_command(cmd).unwrap();
        println!("{cmd} exited with {status}");
    }

    #[zbus(name = "promptReboot")]
    fn prompt_reboot(&self) {
        println!("[LogoutPrompt] Reboot requested");
        let cmd = &config::get_as_str(ConfigKeys::RebootCommand);
        let status = run_command(cmd).unwrap();
        println!("{cmd} exited with {status}");
    }

    #[zbus(name = "promptAll")]
    fn prompt_all(&self) {
        println!("[LogoutPrompt] Show all session actions");
        let cmd = &config::get_as_str(ConfigKeys::SessionActionsCommand);
        let status = run_command(cmd).unwrap();
        println!("{cmd} exited with {status}");
    }
}

// org.kde.Shutdown
// Object path: /Shutdown

struct Shutdown;

#[interface(name = "org.kde.Shutdown")]
impl Shutdown {
    fn logout(&self) {
        println!("[Shutdown] Logout");
        let cmd = &config::get_as_str(ConfigKeys::LogoutCommand);
        let status = run_command(cmd).unwrap();
        println!("{cmd} exited with {status}");
    }

    #[zbus(name = "logoutAndShutdown")]
    fn logout_and_shutdown(&self) {
        println!("[Shutdown] Logout and shutdown");
        let cmd = &config::get_as_str(ConfigKeys::LogoutAndShutdownCommand);
        let status = run_command(cmd).unwrap();
        println!("{cmd} exited with {status}");
    }

    #[zbus(name = "logoutAndReboot")]
    fn logout_and_reboot(&self) {
        println!("[Shutdown] Logout and reboot");
        let cmd = &config::get_as_str(ConfigKeys::LogoutAndRebootCommand);
        let status = run_command(cmd).unwrap();
        println!("{cmd} exited with {status}");
    }

    #[zbus(name = "saveSession")]
    fn save_session(&self) {
        println!("[Shutdown] Save session");
        let cmd = &config::get_as_str(ConfigKeys::SaveSessionCommand);
        let status = run_command(cmd).unwrap();
        println!("{cmd} exited with {status}");
    }
}

// org.freedesktop.ScreenSaver
// Object path: /ScreenSaver

struct ScreenSaver;

#[interface(name = "org.freedesktop.ScreenSaver")]
impl ScreenSaver {
    #[zbus(name = "Lock")]
    fn lock(&self) {
        println!("[ScreenSaver] Lock requested");
        let cmd = &config::get_as_str(ConfigKeys::LockCommand);
        let status = run_command(cmd).unwrap();
        println!("{cmd} exited with {status}");
    }
}

fn socket_path() -> io::Result<PathBuf> {
    let runtime =
        env::var("XDG_RUNTIME_DIR").map_err(|e| io::Error::new(io::ErrorKind::NotFound, e))?;

    let instance = env::var("HYPRLAND_INSTANCE_SIGNATURE")
        .map_err(|e| io::Error::new(io::ErrorKind::NotFound, e))?;

    Ok(PathBuf::from(runtime)
        .join("hypr")
        .join(instance)
        .join(".socket2.sock"))
}

async fn config_watcher() -> Result<()> {
    let stream = UnixStream::connect(socket_path()?)?;
    let mut reader = BufReader::new(stream);
    let mut line = String::new();

    loop {
        line.clear();

        if reader.read_line(&mut line)? == 0 {
            break;
        }

        if let Some((event, _data)) = line.trim_end().split_once(">>") {
            match event {
                "configreloaded" => {
                    println!("Hyprland configuration reloaded");
                    let _ = config::reload();
                }
                _ => {}
            }
        }
    }
    Ok(())
}

#[tokio::main]
pub async fn daemon() -> Result<()> {
    // Register LogoutPrompt
    let _logout_prompt = connection::Builder::session()
        .name("org.kde.LogoutPrompt")
        .serve_at("/LogoutPrompt", LogoutPrompt)
        .build()
        .await?;

    // Register Shutdown
    let _shutdown = connection::Builder::session()
        .name("org.kde.Shutdown")
        .serve_at("/Shutdown", Shutdown)
        .build()
        .await?;

    // Register ScreenSaver
    let _screen_saver = connection::Builder::session()
        .name("org.freedesktop.ScreenSaver")
        .serve_at("/ScreenSaver", ScreenSaver)
        .build()
        .await?;

    let _config_watch = config_watcher().await?;

    println!("HyprPlasma session services registered");

    tokio::signal::ctrl_c().await?;

    println!("Shutting down D-Bus services");

    Ok(())
}
