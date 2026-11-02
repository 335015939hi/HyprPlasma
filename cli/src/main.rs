use anyhow::Result;
use clap::Parser;
use clap::Subcommand;

mod protocol;
pub mod wayland;

#[derive(Parser)]
#[command(version, about, long_about = None)]
struct Args {
    #[command(subcommand)]
    command: Subcommands,
}

#[derive(Subcommand)]
enum Subcommands {
    ToggleShowDesktop,
}

fn main() -> Result<()> {
    let args = Args::parse();
    match args.command {
        Subcommands::ToggleShowDesktop => {
            wayland::toggle_show_desktop(0)?;
        }
    }
    Ok(())
}
