use std::sync::Mutex;

use anyhow::Result;
use clap::Parser;
use clap::Subcommand;

mod config;
mod daemon;

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
