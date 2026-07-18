use std::env;
use std::fs;
use std::path::{Path, PathBuf};

const SUPPORT_ASSETS: &[(&str, &str, &str)] = &[
    (
        "chips/bl616/eflash_loader/eflash_loader_cfg.ini",
        "M61_EFLASH_LOADER_INI_EMBED",
        "eflash_loader_cfg.ini",
    ),
    (
        "chips/bl616/eflash_loader/eflash_loader_cfg.conf",
        "M61_EFLASH_LOADER_CONF_EMBED",
        "eflash_loader_cfg.conf",
    ),
    (
        "chips/bl616/efuse_bootheader/flash_para.bin",
        "M61_FLASH_PARA_EMBED",
        "flash_para.bin",
    ),
];

fn copy_asset(source: &Path, destination: &Path) {
    if !source.is_file() {
        panic!(
            "missing flasher build asset: {}\nSet M61_BLFLASHCOMMAND to the locked SDK BLFlashCommand.exe.",
            source.display()
        );
    }
    fs::copy(source, destination).unwrap_or_else(|error| {
        panic!(
            "failed to copy {} to {}: {error}",
            source.display(),
            destination.display()
        )
    });
    println!("cargo:rerun-if-changed={}", source.display());
}

fn main() {
    println!("cargo:rerun-if-env-changed=M61_BLFLASHCOMMAND");

    let manifest_dir = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").unwrap());

    let flash_source = env::var_os("M61_BLFLASHCOMMAND")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            manifest_dir.join("../../artifacts/m61-flasher-v0.8.1/BLFlashCommand.exe")
        });
    let flash_destination = out_dir.join("BLFlashCommand.exe");
    copy_asset(&flash_source, &flash_destination);
    println!(
        "cargo:rustc-env=M61_BLFLASHCOMMAND_EMBED={}",
        flash_destination.display()
    );

    let flash_root = flash_source.parent().unwrap_or_else(|| {
        panic!("M61_BLFLASHCOMMAND must point to BLFlashCommand.exe inside bouffalo_flash_cube")
    });
    for (relative, variable, output_name) in SUPPORT_ASSETS {
        let source = flash_root.join(relative);
        let destination = out_dir.join(output_name);
        copy_asset(&source, &destination);
        println!("cargo:rustc-env={variable}={}", destination.display());
    }
}
