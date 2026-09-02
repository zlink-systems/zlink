use std::env;
use std::path::PathBuf;

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());

    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    let target_arch = env::var("CARGO_CFG_TARGET_ARCH").unwrap();

    let (os_dir, arch_dir) = match (target_os.as_str(), target_arch.as_str()) {
        ("linux", "x86_64") => ("linux", "x86_64"),
        ("windows", "x86_64") => ("windows", "x86_64"),
        (os, arch) => panic!(
            "unsupported Rust package target {os}-{arch}; the approved crate payloads are linux-x86_64 and windows-x86_64"
        ),
    };

    println!("cargo:rerun-if-env-changed=ZLINK_CORE_SOURCE");
    println!("cargo:rerun-if-env-changed=ZLINK_CORE_INCLUDE_DIR");
    println!("cargo:rerun-if-env-changed=ZLINK_CORE_LIB_DIR");
    println!("cargo:rerun-if-env-changed=ZLINK_RUST_NATIVE_DIR");
    let local_core = env::var("ZLINK_CORE_SOURCE").ok().as_deref() == Some("local");
    let core_include_dir = if local_core {
        let include_dir = env::var_os("ZLINK_CORE_INCLUDE_DIR").map(PathBuf::from)
            .unwrap_or_else(|| panic!(
                "ZLINK_CORE_SOURCE=local requires ZLINK_CORE_INCLUDE_DIR; source bindings/tools/local_core_runtime.sh first"
            ));
        if !include_dir.join("zlink.h").is_file() {
            panic!("Core headers are missing from {}", include_dir.display());
        }
        include_dir
    } else {
        manifest_dir.join("include")
    };
    let native_dir = if local_core {
        PathBuf::from(env::var_os("ZLINK_CORE_LIB_DIR").unwrap_or_else(|| panic!(
            "ZLINK_CORE_SOURCE=local requires ZLINK_CORE_LIB_DIR; source bindings/tools/local_core_runtime.sh first"
        )))
    } else {
        match env::var_os("ZLINK_RUST_NATIVE_DIR") {
            Some(path) => PathBuf::from(path),
            None => manifest_dir
                .join("native")
                .join(format!("{os_dir}-{arch_dir}")),
        }
    };
    if !native_dir.is_dir() {
        panic!(
            "Rust native runtime directory does not exist: {}",
            native_dir.display()
        );
    }
    // Outside an explicit local selection, the crate payload is the only
    // implicit runtime source. In particular,
    // do not discover or prefer a repository `core/build` directory here:
    // that would let a clean consumer silently execute a different Core
    // candidate than the one packaged with this crate.
    let core_header = std::fs::read_to_string(core_include_dir.join("zlink.h"))
        .expect("approved Core zlink.h must be readable");
    let core_version = ["MAJOR", "MINOR", "PATCH"]
        .iter()
        .map(|name| {
            core_header
                .lines()
                .find_map(|line| line.strip_prefix(&format!("#define ZLINK_VERSION_{name} ")))
                .expect("approved Core zlink.h must define its version")
        })
        .collect::<Vec<_>>()
        .join(".");
    if target_os == "windows" {
        let runtime = native_dir.join("zlink.dll");
        let import_library = native_dir.join("zlink.lib");
        if !runtime.is_file() || !import_library.is_file() {
            panic!(
                "Rust Windows native payload must contain zlink.dll and zlink.lib: {}",
                native_dir.display()
            );
        }
        println!("cargo:rerun-if-changed={}", runtime.display());
        println!("cargo:rerun-if-changed={}", import_library.display());
    } else {
        let versioned_runtime = native_dir.join(format!("libzlink.so.{core_version}"));
        let runtime = native_dir.join("libzlink.so");
        if !runtime.is_file() || (!local_core && !versioned_runtime.is_file()) {
            panic!(
                "Rust native runtime must contain libzlink.so{}: {}",
                if local_core {
                    ""
                } else {
                    " and its exact versioned payload"
                },
                native_dir.display()
            );
        }
        println!("cargo:rerun-if-changed={}", runtime.display());
        if versioned_runtime.is_file() {
            println!("cargo:rerun-if-changed={}", versioned_runtime.display());
        }
    }
    println!("cargo:rustc-link-search=native={}", native_dir.display());
    println!("cargo:rustc-link-lib=dylib=zlink");

    if target_os != "windows" {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", native_dir.display());
    }
}
