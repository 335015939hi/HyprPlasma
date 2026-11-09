use anyhow::Result;
use clap::Parser;
use clap::Subcommand;
use std::process::Command;

mod config;
mod daemon;
mod logind;

#[derive(Parser)]
struct Args {
    #[command(subcommand)]
    /// command
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// start hyprplasma daemon
    Daemon,
}

pub fn notify_send(msg: &str) {
    let _ = Command::new("notify-send")
        .arg("-a")
        .arg("hyprplasma daemon")
        .arg(msg)
        .spawn();
}

fn main() -> Result<()> {
    let args = Args::parse();
    config::init()?;
    match args.command {
        Commands::Daemon => {
            daemon::daemon()?;
        }
    }

    Ok(())
}
