use anyhow::Result;
use clap::Parser;
use clap::Subcommand;
use std::sync::OnceLock;

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

pub static CONFIG: OnceLock<config::Config> = OnceLock::new();

fn main() -> Result<()> {
    let args = Args::parse();
    CONFIG.set(config::load_config()?).unwrap();
    match args.command {
        Commands::Daemon => {
            daemon::daemon()?;
        }
    }

    Ok(())
}
