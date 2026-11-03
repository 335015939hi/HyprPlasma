use crate::CONFIG;
use anyhow::Result;
use std::process::Command;
use std::process::ExitStatus;
use zbus::connection;
use zbus::interface;

// org.kde.LogoutPrompt
// Object path: /LogoutPrompt

struct LogoutPrompt;

fn run_command(cmd: &String) -> Result<ExitStatus> {
    let status = Command::new("sh").arg("-c").arg(cmd).status()?;
    Ok(status)
}

#[interface(name = "org.kde.LogoutPrompt")]
impl LogoutPrompt {
    #[zbus(name = "promptLogout")]
    fn prompt_logout(&self) {
        println!("[LogoutPrompt] Logout requested");
        let cmd = &CONFIG.get().unwrap().logout_request_command;
        let status = run_command(cmd).unwrap();
        println!("{cmd} exited with {status}");
    }

    #[zbus(name = "promptShutDown")]
    fn prompt_shutdown(&self) {
        println!("[LogoutPrompt] Shutdown requested");
        let cmd = &CONFIG.get().unwrap().shutdown_request_command;
        let status = run_command(cmd).unwrap();
        println!("{cmd} exited with {status}");
    }

    #[zbus(name = "promptReboot")]
    fn prompt_reboot(&self) {
        println!("[LogoutPrompt] Reboot requested");
        let cmd = &CONFIG.get().unwrap().reboot_request_command;
        let status = run_command(cmd).unwrap();
        println!("{cmd} exited with {status}");
    }

    #[zbus(name = "promptAll")]
    fn prompt_all(&self) {
        println!("[LogoutPrompt] Show all session actions");
        let cmd = &CONFIG.get().unwrap().session_action_request_command;
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
        let cmd = &CONFIG.get().unwrap().logout_command;
        let status = run_command(cmd).unwrap();
        println!("{cmd} exited with {status}");
    }

    #[zbus(name = "logoutAndShutdown")]
    fn logout_and_shutdown(&self) {
        println!("[Shutdown] Logout and shutdown");
        let cmd = &CONFIG.get().unwrap().logout_and_shutdown_command;
        let status = run_command(cmd).unwrap();
        println!("{cmd} exited with {status}");
    }

    #[zbus(name = "logoutAndReboot")]
    fn logout_and_reboot(&self) {
        println!("[Shutdown] Logout and reboot");
        let cmd = &CONFIG.get().unwrap().logout_and_reboot_command;
        let status = run_command(cmd).unwrap();
        println!("{cmd} exited with {status}");
    }

    #[zbus(name = "saveSession")]
    fn save_session(&self) {
        println!("[Shutdown] Save session");
        let cmd = &CONFIG.get().unwrap().save_session_command;
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
        let cmd = &CONFIG.get().unwrap().lock_command;
        let status = run_command(cmd).unwrap();
        println!("{cmd} exited with {status}");
    }
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

    println!("HyprPlasma session services registered");

    tokio::signal::ctrl_c().await?;

    println!("Shutting down D-Bus services");

    Ok(())
}
