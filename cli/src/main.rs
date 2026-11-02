use anyhow::Result;
use clap::Parser;
use clap::Subcommand;

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
    match args.command {
        Commands::Daemon => {
            daemon::daemon()?;
        }
    }

    Ok(())
}
