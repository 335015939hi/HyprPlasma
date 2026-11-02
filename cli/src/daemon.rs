use anyhow::Result;
use zbus::connection;
use zbus::interface;

// org.kde.LogoutPrompt
// Object path: /LogoutPrompt

struct LogoutPrompt;

#[interface(name = "org.kde.LogoutPrompt")]
impl LogoutPrompt {
    #[zbus(name = "promptLogout")]
    fn prompt_logout(&self) {
        println!("[LogoutPrompt] Logout requested");
    }

    #[zbus(name = "promptShutDown")]
    fn prompt_shutdown(&self) {
        println!("[LogoutPrompt] Shutdown requested");
    }

    #[zbus(name = "promptReboot")]
    fn prompt_reboot(&self) {
        println!("[LogoutPrompt] Reboot requested");
    }

    #[zbus(name = "promptAll")]
    fn prompt_all(&self) {
        println!("[LogoutPrompt] Show all session actions");
    }
}

// org.kde.Shutdown
// Object path: /Shutdown

struct Shutdown;

#[interface(name = "org.kde.Shutdown")]
impl Shutdown {
    fn logout(&self) {
        println!("[Shutdown] Logout");
    }

    #[zbus(name = "logoutAndShutdown")]
    fn logout_and_shutdown(&self) {
        println!("[Shutdown] Logout and shutdown");
    }

    #[zbus(name = "logoutAndReboot")]
    fn logout_and_reboot(&self) {
        println!("[Shutdown] Logout and reboot");
    }

    #[zbus(name = "saveSession")]
    fn save_session(&self) {
        println!("[Shutdown] Save session");
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
