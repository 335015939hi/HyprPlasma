use anyhow::Result;
use std::collections::HashMap;
use std::process::Command;
use strum::Display;
use strum::EnumString;
use strum::VariantNames;

#[derive(strum::VariantNames, Debug, Eq, Hash, PartialEq, Display, EnumString)]
#[strum(serialize_all = "snake_case")]
pub enum ConfigKeys {
    LockCommand,
    LogoutCommand,
    ShutdownCommand,
    RebootCommand,
    SwitchUserCommand,
    SaveSessionCommand,
    LogoutAndShutdownCommand,
    LogoutAndRebootCommand,
    SessionActionsCommand,
}

#[derive(Debug)]
pub struct Config {
    map: HashMap<ConfigKeys, String>,
}

impl Config {
    pub fn get_raw(&self, key: ConfigKeys) -> &str {
        let val = self.map.get(&key);
        match val {
            Some(val) => val.as_str(),
            None => {
                let _ = Command::new("notify-send")
                    .arg("-a")
                    .arg("hyprplasma daemon")
                    .arg(format!("config option '{key}' not found"))
                    .status();
                return "";
            }
        }
    }
    pub fn get_as_str(&self, key: ConfigKeys) -> &str {
        let val = self.map.get(&key);
        match val {
            Some(val) => {
                if val.starts_with("str: ") {
                    let val = val.split_at(5);
                    return val.1;
                }
                return "";
            }
            None => {
                let _ = Command::new("notify-send")
                    .arg("-a")
                    .arg("hyprplasma daemon")
                    .arg(format!("config option '{key}' not found"))
                    .status();
                return "";
            }
        }
    }
}

pub fn load_config() -> Result<Config> {
    let mut config: HashMap<ConfigKeys, String> = HashMap::new();
    for key in ConfigKeys::VARIANTS {
        let hyprkey = format!("plugin.hyprplasma.{key}");
        println!("{key}");
        let output = Command::new("hyprctl")
            .arg("getoption")
            .arg(&hyprkey)
            .output()?;
        if !output.status.success() {
            let _ = Command::new("notify-send")
                .arg("-a")
                .arg("hyprplasma daemon")
                .arg(format!(
                    "hyprctl getoption {hyprkey} failed:{}",
                    output.status
                ))
                .spawn()?;
        }
        let mut value = String::new();
        let output_str = String::from_utf8_lossy(&output.stdout);
        let mut output: Vec<&str> = output_str.lines().collect();
        if output.len() < 2 {
            //hyprctl should print 'no such option' for non-existing options (one line). therefore
            //if there are less than 2 lines something went wrong.
            let _ = Command::new("notify-send")
                .arg("-a")
                .arg("hyprplasma daemon")
                .arg(format!("hyprctl getoption {hyprkey} failed:{}", output_str))
                .spawn()?;
        }
        output.pop();
        for line in output {
            if value.len() > 0 {
                value += "\n";
            }
            value += line;
        }
        config.insert(key.parse()?, value);
    }

    let config: Config = Config { map: config };
    Ok(config)
}
