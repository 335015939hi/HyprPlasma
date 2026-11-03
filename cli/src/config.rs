use serde::Deserialize;

#[derive(Debug, Deserialize)]
pub struct Config {
    pub lock_command: String,
    pub logout_command: String,
    pub logout_request_command: String,
    pub shutdown_command: String,
    pub shutdown_request_command: String,
    pub reboot_command: String,
    pub reboot_request_command: String,
    pub switch_user_command: String,
    pub session_action_request_command: String,
    pub save_session_command: String,
    pub logout_and_shutdown_command: String,
    pub logout_and_reboot_command: String,
}

pub fn load_config() -> Result<Config, config::ConfigError> {
    let config_path = dirs::config_dir()
        .expect("Cannot determine config directory")
        .join("hypr")
        .join("hyprplasma.toml");
    println!("{}", config_path.display());
    let mut config = config::Config::builder();
    let default_commands = [
        "lock_command",
        "logout_command",
        "logout_request_command",
        "shutdown_command",
        "shutdown_request_command",
        "reboot_command",
        "reboot_request_command",
        "switch_user_command",
        "session_action_request_command",
        "save_session_command",
        "logout_and_shutdown_command",
        "logout_and_reboot_command",
    ];
    for key in default_commands {
        config = config.set_default(key, format!("notify-send -a hyprplasma '{key} is unset'"))?;
    }
    config
        .add_source(config::File::from(config_path).required(false))
        .build()?
        .try_deserialize()
}
