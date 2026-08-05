use std::env;
use std::path::PathBuf;

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());

    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    let target_arch = env::var("CARGO_CFG_TARGET_ARCH").unwrap();

    let (os_dir, arch_dir) = match (target_os.as_str(), target_arch.as_str()) {
        ("linux", "x86_64") => ("linux", "x86_64"),
        (os, arch) => panic!(
            "unsupported Rust package target {os}-{arch}; the current approved crate payload is linux-x86_64"
        ),
    };

    println!("cargo:rerun-if-env-changed=ZLINK_RUST_NATIVE_DIR");
    let native_dir = match env::var_os("ZLINK_RUST_NATIVE_DIR") {
        Some(path) => PathBuf::from(path),
        None => manifest_dir
            .join("native")
            .join(format!("{os_dir}-{arch_dir}")),
    };
    if !native_dir.is_dir() {
        panic!(
            "Rust native runtime directory does not exist: {}",
            native_dir.display()
        );
    }
    // The crate payload is the only implicit runtime source. In particular,
    // do not discover or prefer a repository `core/build` directory here:
    // that would let a clean consumer silently execute a different Core
    // candidate than the one packaged with this crate.
    let package_version = env::var("CARGO_PKG_VERSION").expect("Cargo package version is required");
    let versioned_runtime = native_dir.join(format!("libzlink.so.{package_version}"));
    if !native_dir.join("libzlink.so").is_file() || !versioned_runtime.is_file() {
        panic!(
            "Rust native payload must contain libzlink.so and libzlink.so.{package_version}: {}",
            native_dir.display()
        );
    }
    println!(
        "cargo:rerun-if-changed={}",
        native_dir.join("libzlink.so").display()
    );
    println!("cargo:rerun-if-changed={}", versioned_runtime.display());
    println!("cargo:rustc-link-search=native={}", native_dir.display());
    println!("cargo:rustc-link-lib=dylib=zlink");

    if target_os != "windows" {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", native_dir.display());
    }
}
