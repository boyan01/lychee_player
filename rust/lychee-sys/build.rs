// Links liblychee as a shared library: the one in LYCHEE_LIB_DIR if set
// (e.g. taken from the Android AAR or the Apple xcframework), else built
// from this repository's core with CMake. The CMake build takes FFmpeg
// from LYCHEE_FFMPEG_ROOT (tool/build_ffmpeg.sh output, LGPL) if set, else
// from pkg-config, which on a development Mac is Homebrew's GPL build.
//
// Dependents read the directory as DEP_LYCHEE_LIB_DIR, to ship the
// library next to their binary. `cargo run` and `cargo test` find it
// there by themselves.

use std::env;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=LYCHEE_LIB_DIR");
    println!("cargo:rerun-if-env-changed=LYCHEE_FFMPEG_ROOT");
    let lib_dir = match env::var_os("LYCHEE_LIB_DIR") {
        Some(dir) => PathBuf::from(dir),
        None => build_core(),
    };
    println!("cargo:rustc-link-search=native={}", lib_dir.display());
    println!("cargo:rustc-link-lib=dylib=lychee");
    println!("cargo:lib_dir={}", lib_dir.display());
}

fn build_core() -> PathBuf {
    let root = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap()).join("../..");
    println!(
        "cargo:rerun-if-changed={}",
        root.join("CMakeLists.txt").display()
    );
    println!("cargo:rerun-if-changed={}", root.join("core").display());

    let mut config = cmake::Config::new(&root);
    config
        .define("BUILD_SHARED_LIBS", "ON")
        .define("LYCHEE_BUILD_EXAMPLES", "OFF")
        .define("LYCHEE_BUILD_TESTS", "OFF")
        // Always optimized: at -O0 the per-pixel frame conversion alone
        // cannot keep up with 1080p.
        .profile("Release")
        .build_target("lychee");
    if let Some(ffmpeg) = env::var_os("LYCHEE_FFMPEG_ROOT") {
        config.define("LYCHEE_FFMPEG_ROOT", ffmpeg);
    }
    let core = config.build().join("build").join("core");
    // Multi-config generators (Visual Studio) add a directory per profile.
    let multi_config = core.join(config.get_profile());
    if multi_config.is_dir() {
        multi_config
    } else {
        core
    }
}
