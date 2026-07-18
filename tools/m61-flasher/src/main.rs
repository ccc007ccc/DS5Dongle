#![cfg_attr(
    all(windows, not(debug_assertions), not(test)),
    windows_subsystem = "windows"
)]

use anyhow::{Context, Result, anyhow, bail};
use base64::Engine;
use reqwest::blocking::Client;
use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::env;
use std::fs::{self, File};
use std::io::{self, BufRead, BufReader, IsTerminal, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode, Stdio};
use std::sync::mpsc::{self, Receiver, Sender};
use std::thread;
use std::time::Duration;
use std::time::{SystemTime, UNIX_EPOCH};

const PRODUCT_NAME: &str = "M61 DualSense Adapter Flasher";
const FLASHER_VERSION: &str = env!("CARGO_PKG_VERSION");
const RELEASES_API: &str = "https://api.github.com/repos/ccc007ccc/DS5Dongle/releases?per_page=30";
const WCH_DRIVER_URL: &str = "https://www.wch-ic.com/download/file?id=65";
const WCH_SIGNER_FRAGMENT: &str = "Nanjing Qinheng Microelectronics Co., Ltd.";
const KNOWN_WCH_DRIVER_SHA256: &str =
    "458c37bdafbe4ce3cd0baf728c232b4b765b36d7463956e9b94cdf099212cad1";

const FLASH_CONFIG: &str = r#"[cfg]
# 0: no erase, 1: programmed section erase, 2: chip erase
erase = 1
skip_mode = 0x0, 0x0
boot2_isp_mode = 0

[boot2]
filedir = ./boot2_bl616_*.bin
address = 0x000000

[partition]
filedir = ./partition.bin
address = 0xE000

[FW]
filedir = ./m61_dualsense_hidp_probe_bl616.bin
address = @partition
"#;

const BLFLASH_BYTES: &[u8] = include_bytes!(env!("M61_BLFLASHCOMMAND_EMBED"));
const BLFLASH_SHA256: &str = "2c6f2578883166467395f58dee451e7e03c898c731de2f50bf21ea358f2235c3";

#[derive(Clone, Debug, Deserialize)]
struct GithubAsset {
    name: String,
    browser_download_url: String,
    size: u64,
    digest: Option<String>,
}

#[derive(Debug, Deserialize)]
struct GithubRelease {
    tag_name: String,
    name: Option<String>,
    html_url: String,
    published_at: Option<String>,
    draft: bool,
    prerelease: bool,
    assets: Vec<GithubAsset>,
}

#[derive(Clone, Debug)]
struct FlashRelease {
    tag: String,
    name: String,
    url: String,
    published_at: Option<String>,
    prerelease: bool,
    boot2: GithubAsset,
    partition: GithubAsset,
    firmware: GithubAsset,
}

#[derive(Debug, Default)]
struct Options {
    port: Option<String>,
    baud: u32,
    dry_run: bool,
    assume_yes: bool,
    list: bool,
    list_releases: bool,
    assets_info: bool,
    install_driver: bool,
    release: Option<String>,
    verify_release: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct Ch340Device {
    name: String,
    instance_id: String,
    error_code: u32,
    status: String,
    port: Option<String>,
}

struct RuntimeDirectory {
    path: PathBuf,
    preserve: bool,
}

impl Drop for RuntimeDirectory {
    fn drop(&mut self) {
        if !self.preserve {
            let _ = fs::remove_dir_all(&self.path);
        }
    }
}

fn main() -> ExitCode {
    if env::args_os().len() == 1 {
        return match run_gui() {
            Ok(()) => ExitCode::SUCCESS,
            Err(error) => {
                eprintln!("Failed to start M61 Flasher GUI: {error}");
                ExitCode::FAILURE
            }
        };
    }
    run_cli()
}

fn run_cli() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("\n错误 / Error: {error:#}");
            if io::stdin().is_terminal() {
                eprintln!("按 Enter 退出 / Press Enter to exit.");
                let _ = read_line();
            }
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<()> {
    let options = parse_options(env::args().skip(1))?;
    if !cfg!(windows) {
        bail!("M61 Flasher currently supports Windows 10/11 only");
    }

    print_banner();
    verify_embedded_tool()?;

    if options.assets_info {
        print_tool_info();
        return Ok(());
    }

    if options.list {
        print_devices(&probe_ch340_devices()?);
        return Ok(());
    }

    if options.install_driver {
        install_ch340_driver(options.assume_yes)?;
        return Ok(());
    }

    let client = github_client()?;
    let releases = fetch_flash_releases(&client)?;
    if options.list_releases {
        print_releases(&releases);
        return Ok(());
    }
    let release = choose_release(&releases, options.release.as_deref(), options.assume_yes)?;
    println!("已选择固件 / Firmware: {} — {}", release.tag, release.name);
    if options.verify_release {
        let _runtime = prepare_runtime(&client, &release)?;
        println!("\n{} 的完整刷写文件已下载并通过 SHA256 校验。", release.tag);
        println!("Verification completed; no device was opened and nothing was flashed.");
        return Ok(());
    }

    let mut devices = probe_ch340_devices()?;

    let port = if let Some(port) = options.port.as_deref() {
        normalize_port(port)?
    } else {
        loop {
            let usable: Vec<&Ch340Device> = devices
                .iter()
                .filter(|device| device.error_code == 0 && device.port.is_some())
                .collect();

            if !usable.is_empty() {
                break choose_port(&usable)?;
            }

            if devices.is_empty() {
                println!("未检测到 M61 的 CH340（USB VID:PID 1A86:7523）。");
                println!("请连接开发板的 CH340 USB 口，然后按 Enter 重试；输入 q 退出。");
                let answer = read_line()?;
                if answer.trim().eq_ignore_ascii_case("q") {
                    bail!("cancelled by user");
                }
            } else {
                println!("检测到了 CH340 硬件，但没有可用 COM 口，驱动可能未安装或异常。");
                print_devices(&devices);
                if prompt_yes_no(
                    "现在从 WCH 官方下载并安装 CH340 驱动？",
                    true,
                    options.assume_yes,
                )? {
                    install_ch340_driver(options.assume_yes)?;
                    println!("驱动安装器已结束。请重新插拔开发板，然后按 Enter 继续。");
                    let _ = read_line()?;
                } else {
                    bail!("CH340 driver is required before flashing");
                }
            }
            devices = probe_ch340_devices()?;
        }
    };

    println!("\n已选择 / Selected: {port}");
    println!("固件来源 / Source: {}", release.url);
    println!("\n请让开发板进入 UART ISP 下载模式：");
    println!("  1. 按住 BOOT");
    println!("  2. 点按并松开 RESET/RST");
    println!("  3. 松开 BOOT");
    println!("完成后按 Enter 开始刷写。刷写中不要拔线或按 Reset。");
    if !options.assume_yes {
        let answer = read_line()?;
        if answer.trim().eq_ignore_ascii_case("q") {
            bail!("cancelled by user");
        }
    }

    if options.dry_run {
        println!(
            "\n[dry-run] 将下载并刷写 {}，使用 {port} @ {} baud；未启动下载或刷写。",
            release.tag, options.baud
        );
        return Ok(());
    }

    let mut runtime = prepare_runtime(&client, &release)?;
    let first_status = run_flash(&runtime.path, &port, options.baud)?;
    if first_status.success() {
        println!("\n刷写成功。请松开 BOOT，并按一次 RESET/RST 正常启动开发板。");
        println!("Flashing completed successfully.");
        if !options.assume_yes {
            println!("按 Enter 退出 / Press Enter to exit.");
            let _ = read_line()?;
        }
        return Ok(());
    }

    runtime.preserve = true;
    eprintln!("\n第一次刷写失败，退出码：{:?}", first_status.code());
    eprintln!("日志保留在：{}", runtime.path.display());
    if options.baud != 115_200
        && prompt_yes_no(
            "确认仍处于 BOOT+RESET 的 ISP 模式后，是否用兼容档 115200 baud 重试？",
            true,
            options.assume_yes,
        )?
    {
        let retry_status = run_flash(&runtime.path, &port, 115_200)?;
        if retry_status.success() {
            runtime.preserve = false;
            println!("\n115200 baud 重试刷写成功。请按 RESET/RST 正常启动。");
            return Ok(());
        }
        bail!(
            "115200-baud retry also failed; logs: {}",
            runtime.path.display()
        );
    }

    bail!("flashing failed; logs: {}", runtime.path.display())
}

fn print_banner() {
    println!("============================================================");
    println!("{PRODUCT_NAME} {FLASHER_VERSION}");
    println!("Windows 一键刷写工具 / Windows one-file flasher");
    println!("============================================================\n");
}

fn print_help() {
    println!(
        "{PRODUCT_NAME} {FLASHER_VERSION}\n\n\
         Usage: m61-flasher.exe [options]\n\n\
         Options:\n  \
           --port COM5       Override automatic CH340 port selection\n  \
           --baud RATE       460800 (default) or 115200\n  \
           --list            List detected M61 CH340 devices\n  \
           --list-releases   List complete firmware Releases\n  \
           --release TAG     Select a Release without the menu\n  \
           --verify-release  Download and verify a Release without flashing\n  \
           --assets-info     Print the embedded flashing-tool hash\n  \
           --install-driver  Download, verify, and launch the official WCH driver\n  \
           --dry-run         Check everything without starting the flash process\n  \
           --yes             Accept prompts (intended for controlled automation)\n  \
           -h, --help        Show this help"
    );
}

fn parse_options(arguments: impl Iterator<Item = String>) -> Result<Options> {
    let mut options = Options {
        baud: 460_800,
        ..Options::default()
    };
    let mut arguments = arguments.peekable();
    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "-h" | "--help" => {
                print_help();
                std::process::exit(0);
            }
            "--port" => {
                options.port = Some(
                    arguments
                        .next()
                        .ok_or_else(|| anyhow!("--port requires COM name"))?,
                );
            }
            "--baud" => {
                let value = arguments
                    .next()
                    .ok_or_else(|| anyhow!("--baud requires a value"))?;
                options.baud = value.parse().context("invalid --baud value")?;
                if !matches!(options.baud, 460_800 | 115_200) {
                    bail!("--baud must be 460800 or 115200");
                }
            }
            "--dry-run" => options.dry_run = true,
            "--yes" => options.assume_yes = true,
            "--list" => options.list = true,
            "--list-releases" => options.list_releases = true,
            "--release" => {
                options.release = Some(
                    arguments
                        .next()
                        .ok_or_else(|| anyhow!("--release requires a tag"))?,
                );
            }
            "--assets-info" => options.assets_info = true,
            "--install-driver" => options.install_driver = true,
            "--verify-release" => options.verify_release = true,
            _ => bail!("unknown option: {argument} (use --help)"),
        }
    }
    Ok(options)
}

fn sha256(bytes: &[u8]) -> String {
    hex::encode(Sha256::digest(bytes))
}

fn verify_embedded_tool() -> Result<()> {
    let actual = sha256(BLFLASH_BYTES);
    if actual != BLFLASH_SHA256 {
        bail!(
            "embedded BLFlashCommand.exe checksum mismatch: expected {}, got {}",
            BLFLASH_SHA256,
            actual
        );
    }
    Ok(())
}

fn print_tool_info() {
    println!("Embedded flashing tool:");
    println!(
        "{}  BLFlashCommand.exe  ({} bytes)",
        BLFLASH_SHA256,
        BLFLASH_BYTES.len()
    );
    println!("Firmware is selected and downloaded from GitHub Releases at runtime.");
}

fn github_client() -> Result<Client> {
    Client::builder()
        .user_agent(format!("M61-Flasher/{FLASHER_VERSION}"))
        .build()
        .context("failed to initialize HTTPS client")
}

fn sha256_digest(asset: &GithubAsset) -> Option<&str> {
    asset.digest.as_deref()?.strip_prefix("sha256:")
}

fn fetch_flash_releases(client: &Client) -> Result<Vec<FlashRelease>> {
    println!("正在读取 GitHub 固件列表 / Loading firmware Releases...");
    let github_releases: Vec<GithubRelease> = client
        .get(RELEASES_API)
        .header("Accept", "application/vnd.github+json")
        .send()
        .context("failed to query GitHub Releases")?
        .error_for_status()
        .context("GitHub Releases API returned an error")?
        .json()
        .context("failed to parse GitHub Releases response")?;

    let mut releases = Vec::new();
    for release in github_releases.into_iter().filter(|release| !release.draft) {
        let boot2: Vec<&GithubAsset> = release
            .assets
            .iter()
            .filter(|asset| {
                asset.name.starts_with("boot2_bl616_")
                    && asset.name.ends_with(".bin")
                    && sha256_digest(asset).is_some()
            })
            .collect();
        let partition = release
            .assets
            .iter()
            .find(|asset| asset.name == "partition.bin" && sha256_digest(asset).is_some());
        let firmware = release.assets.iter().find(|asset| {
            asset.name == "m61_dualsense_hidp_probe_bl616.bin" && sha256_digest(asset).is_some()
        });
        if boot2.len() != 1 || partition.is_none() || firmware.is_none() {
            continue;
        }

        releases.push(FlashRelease {
            tag: release.tag_name.clone(),
            name: release.name.unwrap_or_else(|| release.tag_name.clone()),
            url: release.html_url,
            published_at: release.published_at,
            prerelease: release.prerelease,
            boot2: boot2[0].clone(),
            partition: partition.expect("checked").clone(),
            firmware: firmware.expect("checked").clone(),
        });
    }

    if releases.is_empty() {
        bail!(
            "no complete Release contains boot2, partition.bin, application BIN, and GitHub SHA256 digests"
        );
    }
    Ok(releases)
}

fn print_releases(releases: &[FlashRelease]) {
    println!("\n可刷写固件 / Flashable firmware:");
    for (index, release) in releases.iter().enumerate() {
        let channel = if release.prerelease {
            "预发布 / prerelease"
        } else {
            "稳定 / stable"
        };
        let date = release
            .published_at
            .as_deref()
            .and_then(|value| value.get(..10))
            .unwrap_or("unknown date");
        let total = release.boot2.size + release.partition.size + release.firmware.size;
        println!(
            "  {}. {} | {} | {} | {:.1} KiB\n     {}",
            index + 1,
            release.tag,
            channel,
            date,
            total as f64 / 1024.0,
            release.name
        );
    }
}

fn choose_release(
    releases: &[FlashRelease],
    requested: Option<&str>,
    assume_yes: bool,
) -> Result<FlashRelease> {
    if let Some(tag) = requested {
        return releases
            .iter()
            .find(|release| release.tag.eq_ignore_ascii_case(tag))
            .cloned()
            .ok_or_else(|| {
                anyhow!("Release {tag} is unavailable or lacks a complete verified flash set")
            });
    }

    if assume_yes {
        return releases
            .iter()
            .find(|release| !release.prerelease)
            .or_else(|| releases.first())
            .cloned()
            .ok_or_else(|| anyhow!("no flashable Release"));
    }

    print_releases(releases);
    println!("请选择要下载并刷写的固件版本。通常选择列表中最新的稳定版。");
    loop {
        print!("输入序号 / Select firmware: ");
        io::stdout().flush()?;
        let answer = read_line()?;
        if answer.trim().eq_ignore_ascii_case("q") {
            bail!("cancelled by user");
        }
        if let Ok(index) = answer.trim().parse::<usize>()
            && (1..=releases.len()).contains(&index)
        {
            return Ok(releases[index - 1].clone());
        }
        println!("请输入 1 到 {}，或输入 q 退出。", releases.len());
    }
}

fn download_release_asset(client: &Client, asset: &GithubAsset, destination: &Path) -> Result<()> {
    println!(
        "下载 {} ({:.1} KiB)...",
        asset.name,
        asset.size as f64 / 1024.0
    );
    let mut response = client
        .get(&asset.browser_download_url)
        .send()
        .with_context(|| format!("failed to download {}", asset.name))?
        .error_for_status()
        .with_context(|| format!("download server rejected {}", asset.name))?;
    let mut bytes = Vec::with_capacity(asset.size.try_into().unwrap_or(0));
    response
        .copy_to(&mut bytes)
        .with_context(|| format!("failed while downloading {}", asset.name))?;
    if bytes.len() as u64 != asset.size {
        bail!(
            "download size mismatch for {}: expected {}, got {}",
            asset.name,
            asset.size,
            bytes.len()
        );
    }
    let expected = sha256_digest(asset)
        .ok_or_else(|| anyhow!("GitHub did not provide a SHA256 digest for {}", asset.name))?;
    let actual = sha256(&bytes);
    if actual != expected {
        bail!(
            "download checksum mismatch for {}: expected {}, got {}",
            asset.name,
            expected,
            actual
        );
    }
    fs::write(destination, bytes)
        .with_context(|| format!("failed to save {}", destination.display()))?;
    println!("校验通过 / Verified: {actual}");
    Ok(())
}

fn powershell_output(script: &str) -> Result<std::process::Output> {
    Command::new("powershell.exe")
        .args([
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            script,
        ])
        .output()
        .context("unable to start Windows PowerShell")
}

fn probe_ch340_devices() -> Result<Vec<Ch340Device>> {
    let script = r#"
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.Encoding]::UTF8
Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPDeviceID -like 'USB\VID_1A86&PID_7523*' } | ForEach-Object {
  $name=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes([string]$_.Name))
  $id=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes([string]$_.PNPDeviceID))
  "$name`t$id`t$([int]$_.ConfigManagerErrorCode)`t$([string]$_.Status)"
}
"#;
    let output = powershell_output(script)?;
    if !output.status.success() {
        bail!(
            "CH340 device query failed: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        );
    }
    parse_probe_output(&String::from_utf8_lossy(&output.stdout))
}

fn parse_probe_output(output: &str) -> Result<Vec<Ch340Device>> {
    let mut devices = Vec::new();
    for line in output.lines().filter(|line| !line.trim().is_empty()) {
        let fields: Vec<&str> = line.split('\t').collect();
        if fields.len() != 4 {
            bail!("unexpected CH340 probe output: {line}");
        }
        let decode = |value: &str| -> Result<String> {
            let bytes = base64::engine::general_purpose::STANDARD
                .decode(value)
                .context("invalid base64 from CH340 probe")?;
            String::from_utf8(bytes).context("invalid UTF-8 from CH340 probe")
        };
        let name = decode(fields[0])?;
        let instance_id = decode(fields[1])?;
        devices.push(Ch340Device {
            port: find_com_port(&name),
            name,
            instance_id,
            error_code: fields[2].parse().context("invalid PnP error code")?,
            status: fields[3].to_owned(),
        });
    }
    Ok(devices)
}

fn find_com_port(name: &str) -> Option<String> {
    let upper = name.to_ascii_uppercase();
    let bytes = upper.as_bytes();
    let mut index = 0;
    while index + 3 <= bytes.len() {
        if &bytes[index..index + 3] == b"COM" {
            let start = index + 3;
            let mut end = start;
            while end < bytes.len() && bytes[end].is_ascii_digit() {
                end += 1;
            }
            if end > start {
                return Some(upper[index..end].to_owned());
            }
        }
        index += 1;
    }
    None
}

fn normalize_port(port: &str) -> Result<String> {
    let upper = port.trim().to_ascii_uppercase();
    if upper.len() <= 3
        || !upper.starts_with("COM")
        || !upper[3..].bytes().all(|byte| byte.is_ascii_digit())
    {
        bail!("invalid COM port: {port}");
    }
    Ok(upper)
}

fn print_devices(devices: &[Ch340Device]) {
    if devices.is_empty() {
        println!("No connected CH340 1A86:7523 device was detected.");
        return;
    }
    for (index, device) in devices.iter().enumerate() {
        println!(
            "{}. {} | port={} | PnP={} ({}) | {}",
            index + 1,
            device.name,
            device.port.as_deref().unwrap_or("unavailable"),
            device.status,
            device.error_code,
            device.instance_id
        );
    }
}

fn choose_port(devices: &[&Ch340Device]) -> Result<String> {
    if devices.len() == 1 {
        let device = devices[0];
        println!("自动检测到：{}", device.name);
        return Ok(device.port.clone().expect("filtered port"));
    }

    println!("检测到多个 CH340，请选择 M61 对应的串口：");
    for (index, device) in devices.iter().enumerate() {
        println!("  {}. {}", index + 1, device.name);
    }
    loop {
        print!("输入序号 / Select: ");
        io::stdout().flush()?;
        let answer = read_line()?;
        if let Ok(index) = answer.trim().parse::<usize>()
            && (1..=devices.len()).contains(&index)
        {
            return Ok(devices[index - 1].port.clone().expect("filtered port"));
        }
        println!("请输入 1 到 {}。", devices.len());
    }
}

fn prompt_yes_no(question: &str, default_yes: bool, assume_yes: bool) -> Result<bool> {
    if assume_yes {
        println!("{question} [auto: yes]");
        return Ok(true);
    }
    print!(
        "{question} {} ",
        if default_yes { "[Y/n]" } else { "[y/N]" }
    );
    io::stdout().flush()?;
    let answer = read_line()?;
    let answer = answer.trim().to_ascii_lowercase();
    if answer.is_empty() {
        return Ok(default_yes);
    }
    Ok(matches!(answer.as_str(), "y" | "yes" | "是"))
}

fn read_line() -> Result<String> {
    let mut line = String::new();
    io::stdin().read_line(&mut line)?;
    Ok(line)
}

fn quote_powershell_literal(path: &Path) -> String {
    format!("'{}'", path.to_string_lossy().replace('\'', "''"))
}

fn install_ch340_driver(assume_yes: bool) -> Result<()> {
    if !prompt_yes_no(
        "将从 wch-ic.com 下载官方 CH341SER 驱动并弹出 Windows UAC，继续？",
        true,
        assume_yes,
    )? {
        bail!("driver installation cancelled");
    }

    let temp_path = env::temp_dir().join(format!("CH341SER-M61-{}.EXE", std::process::id()));
    println!("正在下载官方驱动 / Downloading official WCH driver...");
    let client = Client::builder()
        .user_agent("M61-Flasher/0.8.1")
        .build()
        .context("failed to initialize HTTPS client")?;
    let mut response = client
        .get(WCH_DRIVER_URL)
        .send()
        .context("failed to download the WCH driver")?
        .error_for_status()
        .context("WCH driver server returned an error")?;
    let mut file =
        File::create(&temp_path).context("failed to create temporary driver installer")?;
    io::copy(&mut response, &mut file).context("failed to save WCH driver installer")?;
    file.flush()?;

    let mut downloaded = Vec::new();
    File::open(&temp_path)?.read_to_end(&mut downloaded)?;
    let downloaded_hash = sha256(&downloaded);
    println!("驱动 SHA256: {downloaded_hash}");
    if downloaded_hash != KNOWN_WCH_DRIVER_SHA256 {
        println!("提示：WCH 官方安装包已更新，将以有效数字签名作为信任依据。");
    }

    let quoted = quote_powershell_literal(&temp_path);
    let signature_script = format!(
        "$s=Get-AuthenticodeSignature -LiteralPath {quoted}; [Console]::OutputEncoding=[Text.Encoding]::UTF8; Write-Output ($s.Status.ToString() + \"`t\" + $s.SignerCertificate.Subject)"
    );
    let signature = powershell_output(&signature_script)?;
    if !signature.status.success() {
        let _ = fs::remove_file(&temp_path);
        bail!("unable to verify WCH driver Authenticode signature");
    }
    let signature_text = String::from_utf8_lossy(&signature.stdout);
    let (status, signer) = signature_text
        .trim()
        .split_once('\t')
        .ok_or_else(|| anyhow!("unexpected Authenticode result"))?;
    if status != "Valid" || !signer.contains(WCH_SIGNER_FRAGMENT) {
        let _ = fs::remove_file(&temp_path);
        bail!("driver signature rejected: status={status}, signer={signer}");
    }
    println!("数字签名有效：{signer}");
    println!("即将请求管理员权限，请在官方 WCH 安装窗口中完成安装。");

    let install_script = format!(
        "$p=Start-Process -FilePath {quoted} -Verb RunAs -Wait -PassThru; exit $p.ExitCode"
    );
    let install_status = Command::new("powershell.exe")
        .args([
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            &install_script,
        ])
        .status()
        .context("unable to launch the WCH driver installer")?;
    let _ = fs::remove_file(&temp_path);
    if !install_status.success() {
        bail!("WCH driver installer failed or UAC was cancelled");
    }
    Ok(())
}

fn unique_runtime_path() -> Result<PathBuf> {
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .context("system clock is before UNIX epoch")?
        .as_nanos();
    Ok(env::temp_dir().join(format!(
        "M61Flasher-{}-{}-{nonce}",
        env!("CARGO_PKG_VERSION"),
        std::process::id()
    )))
}

fn prepare_runtime(client: &Client, release: &FlashRelease) -> Result<RuntimeDirectory> {
    let path = unique_runtime_path()?;
    fs::create_dir(&path).with_context(|| format!("failed to create {}", path.display()))?;
    let tool_path = path.join("BLFlashCommand.exe");
    fs::write(&tool_path, BLFLASH_BYTES).context("failed to extract BLFlashCommand.exe")?;
    if sha256(&fs::read(&tool_path)?) != BLFLASH_SHA256 {
        bail!("temporary BLFlashCommand.exe checksum mismatch");
    }
    fs::write(path.join("flash_prog_cfg.ini"), FLASH_CONFIG)?;

    for asset in [&release.boot2, &release.partition, &release.firmware] {
        download_release_asset(client, asset, &path.join(&asset.name))?;
    }
    Ok(RuntimeDirectory {
        path,
        preserve: false,
    })
}

fn run_flash(runtime: &Path, port: &str, baud: u32) -> Result<std::process::ExitStatus> {
    println!("\n开始刷写 / Flashing {port} @ {baud} baud...");
    Command::new(runtime.join("BLFlashCommand.exe"))
        .args([
            "--interface=uart",
            &format!("--baudrate={baud}"),
            &format!("--port={port}"),
            "--chipname=bl616",
            "--config=flash_prog_cfg.ini",
            "--reset",
        ])
        .current_dir(runtime)
        .stdin(Stdio::inherit())
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit())
        .status()
        .context("failed to start embedded Bouffalo flashing tool")
}

enum GuiEvent {
    Releases(std::result::Result<Vec<FlashRelease>, String>),
    Devices(std::result::Result<Vec<Ch340Device>, String>),
    Log(String),
    DriverDone(std::result::Result<(), String>),
    FlashDone {
        success: bool,
        message: String,
        runtime: Option<PathBuf>,
        port: String,
        baud: u32,
    },
}

struct RetryState {
    runtime: PathBuf,
    port: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Language {
    ZhCn,
    En,
}

impl Language {
    fn tr<'a>(self, zh_cn: &'a str, en: &'a str) -> &'a str {
        match self {
            Self::ZhCn => zh_cn,
            Self::En => en,
        }
    }

    fn display_name(self) -> &'static str {
        match self {
            Self::ZhCn => "简体中文",
            Self::En => "English",
        }
    }
}

fn language_override() -> Option<Language> {
    match env::var("M61_FLASHER_LANG")
        .unwrap_or_default()
        .trim()
        .to_ascii_lowercase()
        .as_str()
    {
        "zh" | "zh-cn" | "zh_cn" => Some(Language::ZhCn),
        "en" | "en-us" | "en_us" => Some(Language::En),
        _ => None,
    }
}

#[cfg(windows)]
fn system_language() -> Language {
    if let Some(language) = language_override() {
        return language;
    }
    let mut locale = [0_u16; 85];
    let length = unsafe {
        windows_sys::Win32::Globalization::GetUserDefaultLocaleName(
            locale.as_mut_ptr(),
            locale.len() as i32,
        )
    };
    if length > 1 {
        let value = String::from_utf16_lossy(&locale[..length as usize - 1]);
        if value.to_ascii_lowercase().starts_with("zh") {
            return Language::ZhCn;
        }
    }
    Language::En
}

#[cfg(not(windows))]
fn system_language() -> Language {
    if let Some(language) = language_override() {
        return language;
    }
    if env::var("LANG")
        .unwrap_or_default()
        .to_ascii_lowercase()
        .starts_with("zh")
    {
        Language::ZhCn
    } else {
        Language::En
    }
}

struct FlasherApp {
    tx: Sender<GuiEvent>,
    rx: Receiver<GuiEvent>,
    releases: Vec<FlashRelease>,
    devices: Vec<Ch340Device>,
    selected_release: usize,
    selected_port: Option<String>,
    baud: u32,
    loading_releases: bool,
    loading_devices: bool,
    busy: Option<String>,
    status: String,
    log: String,
    show_isp_dialog: bool,
    show_driver_dialog: bool,
    retry: Option<RetryState>,
    language: Language,
}

impl FlasherApp {
    fn new(cc: &eframe::CreationContext<'_>) -> Self {
        install_cjk_font(&cc.egui_ctx);
        cc.egui_ctx.set_visuals(eframe::egui::Visuals::dark());
        let (tx, rx) = mpsc::channel();
        let language = system_language();
        let mut app = Self {
            tx,
            rx,
            releases: Vec::new(),
            devices: Vec::new(),
            selected_release: 0,
            selected_port: None,
            baud: 460_800,
            loading_releases: false,
            loading_devices: false,
            busy: None,
            status: language.tr("正在初始化...", "Initializing...").to_owned(),
            log: String::new(),
            show_isp_dialog: false,
            show_driver_dialog: false,
            retry: None,
            language,
        };
        app.refresh_releases();
        app.refresh_devices();
        app
    }

    fn append_log(&mut self, message: impl AsRef<str>) {
        if !self.log.is_empty() {
            self.log.push('\n');
        }
        self.log.push_str(message.as_ref().trim_end());
        if self.log.len() > 200_000 {
            let boundary = self.log.len() - 150_000;
            self.log.drain(..boundary);
        }
    }

    fn refresh_releases(&mut self) {
        if self.loading_releases {
            return;
        }
        self.loading_releases = true;
        self.status = self
            .language
            .tr(
                "正在读取 GitHub 固件列表...",
                "Loading firmware list from GitHub...",
            )
            .to_owned();
        let tx = self.tx.clone();
        thread::spawn(move || {
            let result = github_client()
                .and_then(|client| fetch_flash_releases(&client))
                .map_err(|error| format!("{error:#}"));
            let _ = tx.send(GuiEvent::Releases(result));
        });
    }

    fn refresh_devices(&mut self) {
        if self.loading_devices {
            return;
        }
        self.loading_devices = true;
        let tx = self.tx.clone();
        thread::spawn(move || {
            let result = probe_ch340_devices().map_err(|error| format!("{error:#}"));
            let _ = tx.send(GuiEvent::Devices(result));
        });
    }

    fn usable_ports(&self) -> Vec<String> {
        self.devices
            .iter()
            .filter(|device| device.error_code == 0)
            .filter_map(|device| device.port.clone())
            .collect()
    }

    fn selected_release(&self) -> Option<FlashRelease> {
        self.releases.get(self.selected_release).cloned()
    }

    fn relocalize_status(&mut self) {
        if self.busy.is_some() {
            self.busy = Some(
                self.language
                    .tr("操作进行中...", "Operation in progress...")
                    .to_owned(),
            );
            self.status = self
                .language
                .tr(
                    "请等待当前操作完成。",
                    "Wait for the current operation to finish.",
                )
                .to_owned();
        } else if self.loading_releases {
            self.status = self
                .language
                .tr(
                    "正在读取 GitHub 固件列表...",
                    "Loading firmware list from GitHub...",
                )
                .to_owned();
        } else if let Some(port) = &self.selected_port {
            self.status = match self.language {
                Language::ZhCn => format!("CH340 已就绪：{port}"),
                Language::En => format!("CH340 is ready: {port}"),
            };
        } else if self.devices.is_empty() {
            self.status = self
                .language
                .tr(
                    "未检测到 M61 CH340，请连接开发板串口 USB。",
                    "M61 CH340 was not detected. Connect the board's serial USB port.",
                )
                .to_owned();
        } else {
            self.status = self
                .language
                .tr(
                    "检测到 CH340，但驱动/COM 口不可用。",
                    "CH340 was detected, but its driver/COM port is unavailable.",
                )
                .to_owned();
        }
    }

    fn start_driver_install(&mut self) {
        if self.busy.is_some() {
            return;
        }
        let language = self.language;
        self.busy = Some(
            language
                .tr(
                    "正在下载并启动 WCH 官方驱动安装器...",
                    "Downloading and starting the official WCH driver installer...",
                )
                .to_owned(),
        );
        self.status = language
            .tr(
                "请留意 Windows UAC 和 WCH 安装窗口。",
                "Watch for the Windows UAC and WCH installer windows.",
            )
            .to_owned();
        self.append_log(language.tr(
            "开始下载 WCH 官方 CH341SER 驱动，并验证 Authenticode 签名。",
            "Downloading the official WCH CH341SER driver and verifying its Authenticode signature.",
        ));
        let tx = self.tx.clone();
        thread::spawn(move || {
            let result = install_ch340_driver(true).map_err(|error| format!("{error:#}"));
            let _ = tx.send(GuiEvent::DriverDone(result));
        });
    }

    fn start_flash(&mut self) {
        let Some(release) = self.selected_release() else {
            self.status = self
                .language
                .tr("请先选择固件版本。", "Select a firmware version first.")
                .to_owned();
            return;
        };
        let Some(port) = self.selected_port.clone() else {
            self.status = self
                .language
                .tr(
                    "没有可用的 CH340 COM 口。",
                    "No usable CH340 COM port is available.",
                )
                .to_owned();
            return;
        };
        let language = self.language;
        let baud = self.baud;
        self.busy = Some(match language {
            Language::ZhCn => format!("正在准备并刷写 {}...", release.tag),
            Language::En => format!("Preparing and flashing {}...", release.tag),
        });
        self.status = language
            .tr(
                "正在下载并校验所选 Release，请勿断开开发板。",
                "Downloading and verifying the selected Release. Do not disconnect the board.",
            )
            .to_owned();
        self.append_log(match language {
            Language::ZhCn => format!("准备刷写 {} 到 {} @ {} baud", release.tag, port, baud),
            Language::En => format!("Preparing {} for {} @ {} baud", release.tag, port, baud),
        });
        let tx = self.tx.clone();
        thread::spawn(move || {
            let outcome = (|| -> Result<(std::process::ExitStatus, RuntimeDirectory)> {
                let client = github_client()?;
                let _ = tx.send(GuiEvent::Log(match language {
                    Language::ZhCn => {
                        format!("从 GitHub Release 下载并校验 {} 三件套...", release.tag)
                    }
                    Language::En => format!(
                        "Downloading and verifying the complete {} flash set...",
                        release.tag
                    ),
                }));
                let runtime = prepare_runtime(&client, &release)?;
                let _ = tx.send(GuiEvent::Log(
                    language
                        .tr(
                            "下载校验完成，启动 Bouffalo 刷写核心。",
                            "Download verification passed. Starting the Bouffalo flashing core.",
                        )
                        .to_owned(),
                ));
                let status = run_flash_streaming(&runtime.path, &port, baud, &tx)?;
                Ok((status, runtime))
            })();

            match outcome {
                Ok((status, _runtime)) if status.success() => {
                    let _ = tx.send(GuiEvent::FlashDone {
                        success: true,
                        message: language
                            .tr(
                                "刷写成功。请松开 BOOT，并按一次 RESET/RST 正常启动。",
                                "Flashing succeeded. Release BOOT and press RESET/RST once to start normally.",
                            )
                            .to_owned(),
                        runtime: None,
                        port,
                        baud,
                    });
                }
                Ok((status, mut runtime)) => {
                    runtime.preserve = true;
                    let runtime_path = runtime.path.clone();
                    let _ = tx.send(GuiEvent::FlashDone {
                        success: false,
                        message: match language {
                            Language::ZhCn => format!(
                                "刷写失败（退出码 {:?}）。可确认 ISP 后用 115200 baud 重试。日志目录：{}",
                                status.code(),
                                runtime_path.display()
                            ),
                            Language::En => format!(
                                "Flashing failed (exit code {:?}). Confirm ISP mode, then retry at 115200 baud. Logs: {}",
                                status.code(),
                                runtime_path.display()
                            ),
                        },
                        runtime: Some(runtime_path),
                        port,
                        baud,
                    });
                }
                Err(error) => {
                    let _ = tx.send(GuiEvent::FlashDone {
                        success: false,
                        message: match language {
                            Language::ZhCn => format!("准备或刷写失败：{error:#}"),
                            Language::En => format!("Preparation or flashing failed: {error:#}"),
                        },
                        runtime: None,
                        port,
                        baud,
                    });
                }
            }
        });
    }

    fn start_retry(&mut self, retry: RetryState) {
        let language = self.language;
        self.busy = Some(
            language
                .tr("正在以 115200 baud 重试...", "Retrying at 115200 baud...")
                .to_owned(),
        );
        self.status = language
            .tr(
                "兼容速度重试中，请勿断开开发板。",
                "Compatibility-speed retry in progress. Do not disconnect the board.",
            )
            .to_owned();
        self.append_log(match language {
            Language::ZhCn => format!("使用 {} @ 115200 baud 重试", retry.port),
            Language::En => format!("Retrying {} @ 115200 baud", retry.port),
        });
        let tx = self.tx.clone();
        thread::spawn(move || {
            let result = run_flash_streaming(&retry.runtime, &retry.port, 115_200, &tx);
            match result {
                Ok(status) if status.success() => {
                    let _ = fs::remove_dir_all(&retry.runtime);
                    let _ = tx.send(GuiEvent::FlashDone {
                        success: true,
                        message: language
                            .tr(
                                "115200 baud 重试成功。请按 RESET/RST 正常启动。",
                                "The 115200-baud retry succeeded. Press RESET/RST to start normally.",
                            )
                            .to_owned(),
                        runtime: None,
                        port: retry.port,
                        baud: 115_200,
                    });
                }
                Ok(status) => {
                    let path = retry.runtime;
                    let _ = tx.send(GuiEvent::FlashDone {
                        success: false,
                        message: match language {
                            Language::ZhCn => format!(
                                "115200 baud 重试仍失败（退出码 {:?}）。日志目录：{}",
                                status.code(),
                                path.display()
                            ),
                            Language::En => format!(
                                "The 115200-baud retry failed (exit code {:?}). Logs: {}",
                                status.code(),
                                path.display()
                            ),
                        },
                        runtime: Some(path),
                        port: retry.port,
                        baud: 115_200,
                    });
                }
                Err(error) => {
                    let path = retry.runtime;
                    let _ = tx.send(GuiEvent::FlashDone {
                        success: false,
                        message: match language {
                            Language::ZhCn => {
                                format!("重试失败：{error:#}；日志目录：{}", path.display())
                            }
                            Language::En => {
                                format!("Retry failed: {error:#}; logs: {}", path.display())
                            }
                        },
                        runtime: Some(path),
                        port: retry.port,
                        baud: 115_200,
                    });
                }
            }
        });
    }

    fn process_events(&mut self) {
        while let Ok(event) = self.rx.try_recv() {
            match event {
                GuiEvent::Releases(Ok(releases)) => {
                    self.loading_releases = false;
                    self.releases = releases;
                    self.selected_release = 0;
                    self.status = match self.language {
                        Language::ZhCn => {
                            format!("已加载 {} 个完整固件 Release。", self.releases.len())
                        }
                        Language::En => {
                            format!("Loaded {} complete firmware Releases.", self.releases.len())
                        }
                    };
                    self.append_log(self.language.tr(
                        "GitHub 固件列表已更新。仅显示三件套齐全且带 SHA256 的版本。",
                        "The GitHub firmware list was updated. Only complete three-file sets with SHA256 are shown.",
                    ));
                }
                GuiEvent::Releases(Err(error)) => {
                    self.loading_releases = false;
                    self.status = self
                        .language
                        .tr("读取固件列表失败。", "Failed to load the firmware list.")
                        .to_owned();
                    self.append_log(match self.language {
                        Language::ZhCn => format!("固件列表错误：{error}"),
                        Language::En => format!("Firmware list error: {error}"),
                    });
                }
                GuiEvent::Devices(Ok(devices)) => {
                    self.loading_devices = false;
                    self.devices = devices;
                    let ports = self.usable_ports();
                    if self
                        .selected_port
                        .as_ref()
                        .is_none_or(|selected| !ports.contains(selected))
                    {
                        self.selected_port = ports.first().cloned();
                    }
                    if let Some(port) = &self.selected_port {
                        self.status = match self.language {
                            Language::ZhCn => format!("CH340 已就绪：{port}"),
                            Language::En => format!("CH340 is ready: {port}"),
                        };
                    } else if self.devices.is_empty() {
                        self.status = self
                            .language
                            .tr(
                                "未检测到 M61 CH340，请连接开发板串口 USB。",
                                "M61 CH340 was not detected. Connect the board's serial USB port.",
                            )
                            .to_owned();
                    } else {
                        self.status = self
                            .language
                            .tr(
                                "检测到 CH340，但驱动/COM 口不可用。",
                                "CH340 was detected, but its driver/COM port is unavailable.",
                            )
                            .to_owned();
                    }
                }
                GuiEvent::Devices(Err(error)) => {
                    self.loading_devices = false;
                    self.status = self
                        .language
                        .tr("CH340 检测失败。", "CH340 detection failed.")
                        .to_owned();
                    self.append_log(match self.language {
                        Language::ZhCn => format!("设备检测错误：{error}"),
                        Language::En => format!("Device detection error: {error}"),
                    });
                }
                GuiEvent::Log(line) => self.append_log(line),
                GuiEvent::DriverDone(Ok(())) => {
                    self.busy = None;
                    self.status = self
                        .language
                        .tr(
                            "驱动安装器已结束，请重新插拔开发板。",
                            "The driver installer finished. Reconnect the board.",
                        )
                        .to_owned();
                    self.append_log(self.language.tr(
                        "WCH 驱动安装器已结束；正在重新检测 CH340。",
                        "The WCH driver installer finished; detecting CH340 again.",
                    ));
                    self.refresh_devices();
                }
                GuiEvent::DriverDone(Err(error)) => {
                    self.busy = None;
                    self.status = self
                        .language
                        .tr(
                            "驱动安装失败或 UAC 被取消。",
                            "Driver installation failed or UAC was cancelled.",
                        )
                        .to_owned();
                    self.append_log(match self.language {
                        Language::ZhCn => format!("驱动安装错误：{error}"),
                        Language::En => format!("Driver installation error: {error}"),
                    });
                }
                GuiEvent::FlashDone {
                    success,
                    message,
                    runtime,
                    port,
                    baud,
                } => {
                    self.busy = None;
                    self.status = message.clone();
                    self.append_log(&message);
                    if success {
                        self.retry = None;
                    } else if baud != 115_200 {
                        self.retry = runtime.map(|runtime| RetryState { runtime, port });
                    } else {
                        self.retry = None;
                    }
                }
            }
        }
    }

    fn device_status_text(&self) -> (&'static str, eframe::egui::Color32) {
        if self.loading_devices {
            (
                self.language.tr("正在检测...", "Detecting..."),
                eframe::egui::Color32::YELLOW,
            )
        } else if self.selected_port.is_some() {
            (
                self.language.tr("驱动正常", "Ready"),
                eframe::egui::Color32::LIGHT_GREEN,
            )
        } else if self.devices.is_empty() {
            (
                self.language.tr("未连接", "Not detected"),
                eframe::egui::Color32::GRAY,
            )
        } else {
            (
                self.language.tr("驱动异常", "Driver required"),
                eframe::egui::Color32::LIGHT_RED,
            )
        }
    }
}

impl eframe::App for FlasherApp {
    fn update(&mut self, ctx: &eframe::egui::Context, _frame: &mut eframe::Frame) {
        self.process_events();
        let busy = self.busy.is_some();
        let language = self.language;
        ctx.send_viewport_cmd(eframe::egui::ViewportCommand::Title(format!(
            "{} {FLASHER_VERSION}",
            language.tr(
                "M61 DualSense 适配器刷写器",
                "M61 DualSense Adapter Flasher"
            )
        )));
        if busy && ctx.input(|input| input.viewport().close_requested()) {
            ctx.send_viewport_cmd(eframe::egui::ViewportCommand::CancelClose);
            self.status = language
                .tr(
                    "当前操作尚未结束，暂时不能关闭窗口。",
                    "The current operation is still running; the window cannot close yet.",
                )
                .to_owned();
        }

        eframe::egui::TopBottomPanel::top("header").show(ctx, |ui| {
            ui.add_space(8.0);
            ui.horizontal(|ui| {
                ui.heading(language.tr(
                    "M61 DualSense 适配器刷写器",
                    "M61 DualSense Adapter Flasher",
                ));
                ui.separator();
                ui.label(language.tr("语言", "Language"));
                eframe::egui::ComboBox::from_id_salt("language_combo")
                    .selected_text(self.language.display_name())
                    .show_ui(ui, |ui| {
                        ui.selectable_value(
                            &mut self.language,
                            Language::ZhCn,
                            Language::ZhCn.display_name(),
                        );
                        ui.selectable_value(
                            &mut self.language,
                            Language::En,
                            Language::En.display_name(),
                        );
                    });
            });
            ui.label(language.tr(
                "选择 GitHub Release 固件，自动检测 CH340，并安全刷写 BL616/BL618",
                "Select a GitHub Release, detect CH340 automatically, and safely flash BL616/BL618",
            ));
            ui.add_space(8.0);
        });

        eframe::egui::TopBottomPanel::bottom("footer").show(ctx, |ui| {
            ui.add_space(4.0);
            ui.small(language.tr(
                "固件不内置于 EXE；只从 ccc007ccc/DS5Dongle Releases 下载完整且带 SHA256 的版本。下载/刷写时请保持联网。WebUI：https://ds5.766677.xyz/",
                "Firmware is not embedded. Only complete Releases with SHA256 are downloaded from ccc007ccc/DS5Dongle. Keep the PC online while downloading/flashing. WebUI: https://ds5.766677.xyz/",
            ));
            ui.add_space(4.0);
        });

        if self.language != language {
            self.relocalize_status();
            ctx.request_repaint();
        }

        eframe::egui::CentralPanel::default().show(ctx, |ui| {
            eframe::egui::Grid::new("settings_grid")
                .num_columns(2)
                .spacing([18.0, 12.0])
                .show(ui, |ui| {
                    ui.label(language.tr("固件版本", "Firmware"));
                    ui.horizontal(|ui| {
                        if self.loading_releases {
                            ui.spinner();
                        }
                        let selected_text = self
                            .selected_release()
                            .map(|release| format!("{} — {}", release.tag, release.name))
                            .unwrap_or_else(|| {
                                language.tr("暂无可用固件", "Unavailable").to_owned()
                            });
                        ui.add_enabled_ui(!busy && !self.releases.is_empty(), |ui| {
                            eframe::egui::ComboBox::from_id_salt("release_combo")
                                .selected_text(selected_text)
                                .width(430.0)
                                .show_ui(ui, |ui| {
                                    for (index, release) in self.releases.iter().enumerate() {
                                        let channel = if release.prerelease {
                                            language.tr(" [预发布]", " [prerelease]")
                                        } else {
                                            ""
                                        };
                                        ui.selectable_value(
                                            &mut self.selected_release,
                                            index,
                                            format!(
                                                "{}{} — {}",
                                                release.tag, channel, release.name
                                            ),
                                        );
                                    }
                                });
                        });
                        if ui
                            .add_enabled(
                                !busy,
                                eframe::egui::Button::new(language.tr("刷新", "Refresh")),
                            )
                            .clicked()
                        {
                            self.refresh_releases();
                        }
                    });
                    ui.end_row();

                    ui.label(language.tr("CH340 状态", "CH340 status"));
                    ui.horizontal(|ui| {
                        let (text, color) = self.device_status_text();
                        ui.colored_label(color, text);
                        if ui
                            .add_enabled(
                                !busy,
                                eframe::egui::Button::new(language.tr("重新检测", "Detect again")),
                            )
                            .clicked()
                        {
                            self.refresh_devices();
                        }
                        if ui
                            .add_enabled(
                                !busy,
                                eframe::egui::Button::new(
                                    language.tr("安装/修复驱动", "Install/repair driver"),
                                ),
                            )
                            .clicked()
                        {
                            self.show_driver_dialog = true;
                        }
                    });
                    ui.end_row();

                    ui.label(language.tr("串口", "COM port"));
                    let ports = self.usable_ports();
                    ui.add_enabled_ui(!busy && !ports.is_empty(), |ui| {
                        eframe::egui::ComboBox::from_id_salt("port_combo")
                            .selected_text(
                                self.selected_port
                                    .as_deref()
                                    .unwrap_or(language.tr("没有可用串口", "No usable port")),
                            )
                            .show_ui(ui, |ui| {
                                for port in ports {
                                    ui.selectable_value(
                                        &mut self.selected_port,
                                        Some(port.clone()),
                                        port,
                                    );
                                }
                            });
                    });
                    ui.end_row();

                    ui.label(language.tr("刷写速度", "Baud rate"));
                    ui.horizontal(|ui| {
                        ui.add_enabled_ui(!busy, |ui| {
                            ui.radio_value(
                                &mut self.baud,
                                460_800,
                                language.tr("460800（推荐/快速）", "460800 (recommended/fast)"),
                            );
                            ui.radio_value(
                                &mut self.baud,
                                115_200,
                                language.tr("115200（兼容）", "115200 (compatibility)"),
                            );
                        });
                    });
                    ui.end_row();
                });

            ui.add_space(12.0);
            ui.horizontal(|ui| {
                let can_flash =
                    !busy && self.selected_release().is_some() && self.selected_port.is_some();
                if ui
                    .add_enabled(
                        can_flash,
                        eframe::egui::Button::new(
                            eframe::egui::RichText::new(
                                language.tr("下载并刷写", "Download & Flash"),
                            )
                            .strong()
                            .color(eframe::egui::Color32::WHITE),
                        )
                        .fill(eframe::egui::Color32::from_rgb(36, 112, 178))
                        .min_size([250.0, 38.0].into()),
                    )
                    .clicked()
                {
                    self.show_isp_dialog = true;
                }
                if let Some(operation) = &self.busy {
                    ui.spinner();
                    ui.label(operation);
                }
            });

            ui.add_space(8.0);
            ui.label(&self.status);
            ui.separator();
            ui.label(language.tr("操作日志", "Log"));
            eframe::egui::ScrollArea::vertical()
                .stick_to_bottom(true)
                .max_height(170.0)
                .show(ui, |ui| {
                    ui.add(
                        eframe::egui::TextEdit::multiline(&mut self.log)
                            .font(eframe::egui::TextStyle::Monospace)
                            .desired_width(f32::INFINITY)
                            .desired_rows(7)
                            .interactive(false),
                    );
                });
        });

        if self.show_isp_dialog {
            eframe::egui::Window::new(language.tr("进入 UART ISP", "Enter download mode"))
                .collapsible(false)
                .resizable(false)
                .anchor(eframe::egui::Align2::CENTER_CENTER, [0.0, 0.0])
                .show(ctx, |ui| {
                    ui.label(language.tr(
                        "请按以下顺序操作开发板：",
                        "Use this sequence on the board:",
                    ));
                    ui.label(language.tr("1. 按住 BOOT", "1. Hold BOOT"));
                    ui.label(
                        language.tr("2. 点按并松开 RESET/RST", "2. Press and release RESET/RST"),
                    );
                    ui.label(language.tr("3. 松开 BOOT", "3. Release BOOT"));
                    ui.add_space(8.0);
                    ui.colored_label(
                        eframe::egui::Color32::YELLOW,
                        language.tr(
                            "刷写开始后不要拔线、不要按 Reset。",
                            "Do not disconnect the cable or press Reset after flashing starts.",
                        ),
                    );
                    ui.horizontal(|ui| {
                        if ui.button(language.tr("取消", "Cancel")).clicked() {
                            self.show_isp_dialog = false;
                        }
                        if ui
                            .button(
                                language
                                    .tr("我已进入 ISP，开始刷写", "ISP is ready — start flashing"),
                            )
                            .clicked()
                        {
                            self.show_isp_dialog = false;
                            self.start_flash();
                        }
                    });
                });
        }

        if self.show_driver_dialog {
            eframe::egui::Window::new(language.tr("安装 CH340 驱动", "Install CH340 driver"))
                .collapsible(false)
                .resizable(false)
                .anchor(eframe::egui::Align2::CENTER_CENTER, [0.0, 0.0])
                .show(ctx, |ui| {
                    ui.label(language.tr(
                        "将从 wch-ic.com 通过 HTTPS 下载官方 CH341SER 安装器。",
                        "The official CH341SER installer will be downloaded over HTTPS from wch-ic.com.",
                    ));
                    ui.label(language.tr(
                        "工具会先验证 Authenticode 签名者，再弹出 Windows UAC。",
                        "The Authenticode signer is verified before Windows UAC is requested.",
                    ));
                    ui.horizontal(|ui| {
                        if ui.button(language.tr("取消", "Cancel")).clicked() {
                            self.show_driver_dialog = false;
                        }
                        if ui
                            .button(language.tr(
                                "下载并安装官方驱动",
                                "Download and install official driver",
                            ))
                            .clicked()
                        {
                            self.show_driver_dialog = false;
                            self.start_driver_install();
                        }
                    });
                });
        }

        if self.retry.is_some() {
            eframe::egui::Window::new(language.tr("刷写失败", "Flashing failed"))
                .collapsible(false)
                .resizable(false)
                .anchor(eframe::egui::Align2::CENTER_CENTER, [0.0, 0.0])
                .show(ctx, |ui| {
                    ui.label(language.tr(
                        "请再次确认开发板处于 BOOT+RESET 的 UART ISP 模式。",
                        "Confirm that the board is still in BOOT+RESET UART ISP mode.",
                    ));
                    ui.label(language.tr(
                        "可以用更慢但更兼容的 115200 baud 重试。失败日志会保留在临时目录。",
                        "You can retry at the slower, more compatible 115200 baud. Failure logs remain in the temporary directory.",
                    ));
                    ui.horizontal(|ui| {
                        if ui
                            .button(language.tr("关闭，不重试", "Close without retry"))
                            .clicked()
                        {
                            self.retry = None;
                        }
                        if ui
                            .button(language.tr(
                                "115200 baud 重试",
                                "Retry at 115200 baud",
                            ))
                            .clicked()
                            && let Some(retry) = self.retry.take()
                        {
                            self.start_retry(retry);
                        }
                    });
                });
        }

        if busy || self.loading_devices || self.loading_releases {
            ctx.request_repaint_after(Duration::from_millis(100));
        }
    }
}

fn install_cjk_font(context: &eframe::egui::Context) {
    let Some(windows_directory) = env::var_os("WINDIR").map(PathBuf::from) else {
        return;
    };
    let font_directory = windows_directory.join("Fonts");
    let candidates = [
        font_directory.join("msyh.ttc"),
        font_directory.join("msyh.ttf"),
        font_directory.join("simhei.ttf"),
    ];
    let Some(bytes) = candidates.iter().find_map(|path| fs::read(path).ok()) else {
        return;
    };
    let mut fonts = eframe::egui::FontDefinitions::default();
    fonts.font_data.insert(
        "windows-cjk".to_owned(),
        eframe::egui::FontData::from_owned(bytes).into(),
    );
    for family in [
        eframe::egui::FontFamily::Proportional,
        eframe::egui::FontFamily::Monospace,
    ] {
        fonts
            .families
            .entry(family)
            .or_default()
            .insert(0, "windows-cjk".to_owned());
    }
    context.set_fonts(fonts);
}

fn run_flash_streaming(
    runtime: &Path,
    port: &str,
    baud: u32,
    tx: &Sender<GuiEvent>,
) -> Result<std::process::ExitStatus> {
    let mut child = Command::new(runtime.join("BLFlashCommand.exe"))
        .args([
            "--interface=uart",
            &format!("--baudrate={baud}"),
            &format!("--port={port}"),
            "--chipname=bl616",
            "--config=flash_prog_cfg.ini",
            "--reset",
        ])
        .current_dir(runtime)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .context("failed to start embedded Bouffalo flashing tool")?;

    let stdout = child.stdout.take().context("missing flasher stdout")?;
    let stderr = child.stderr.take().context("missing flasher stderr")?;
    let stdout_tx = tx.clone();
    let stderr_tx = tx.clone();
    let stdout_thread = thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(|line| line.ok()) {
            let _ = stdout_tx.send(GuiEvent::Log(line));
        }
    });
    let stderr_thread = thread::spawn(move || {
        for line in BufReader::new(stderr).lines().map_while(|line| line.ok()) {
            let _ = stderr_tx.send(GuiEvent::Log(line));
        }
    });
    let status = child.wait()?;
    let _ = stdout_thread.join();
    let _ = stderr_thread.join();
    Ok(status)
}

fn run_gui() -> Result<()> {
    verify_embedded_tool()?;
    let options = eframe::NativeOptions {
        viewport: eframe::egui::ViewportBuilder::default()
            .with_inner_size([820.0, 680.0])
            .with_min_inner_size([720.0, 580.0]),
        ..Default::default()
    };
    eframe::run_native(
        &format!("M61 DualSense Flasher {FLASHER_VERSION}"),
        options,
        Box::new(|cc| Ok(Box::new(FlasherApp::new(cc)))),
    )
    .map_err(|error| anyhow!(error.to_string()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn extracts_com_port_from_friendly_name() {
        assert_eq!(
            find_com_port("USB-SERIAL CH340 (COM5)"),
            Some("COM5".to_owned())
        );
        assert_eq!(find_com_port("CH340 driver missing"), None);
    }

    #[test]
    fn parses_pnp_probe_record() {
        let output = "VVNCLVNFUklBTCBDSDM0MCAoQ09NNSk=\tVVNCXFZJRF8xQTg2JlBJRF83NTIzXDE=\t0\tOK\n";
        let parsed = parse_probe_output(output).unwrap();
        assert_eq!(parsed.len(), 1);
        assert_eq!(parsed[0].name, "USB-SERIAL CH340 (COM5)");
        assert_eq!(parsed[0].port.as_deref(), Some("COM5"));
        assert_eq!(parsed[0].error_code, 0);
    }

    #[test]
    fn validates_port_names() {
        assert_eq!(normalize_port("com12").unwrap(), "COM12");
        assert!(normalize_port("COM").is_err());
        assert!(normalize_port("ttyUSB0").is_err());
    }

    #[test]
    fn embedded_flashing_tool_matches_pinned_hash() {
        verify_embedded_tool().unwrap();
    }

    #[test]
    fn gui_language_catalog_switches_both_languages() {
        assert_eq!(Language::ZhCn.tr("中文", "English"), "中文");
        assert_eq!(Language::En.tr("中文", "English"), "English");
        assert_eq!(Language::ZhCn.display_name(), "简体中文");
        assert_eq!(Language::En.display_name(), "English");
    }
}
