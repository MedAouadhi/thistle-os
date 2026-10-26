use std::path::Path;
use std::process::Command;

fn main() {
    // Rerun only when this script changes. A build script that names nothing
    // is rerun whenever any file in the package changes, and the C shims,
    // CMakeLists.txt and INTERFACES.md live here too. The firmware build runs
    // cargo every time, so without this, editing any of them would recompile
    // the whole kernel, 17 s on the device target.
    println!("cargo:rerun-if-changed=build.rs");

    firmware_version();

    // Only run ESP-IDF build integration when targeting ESP
    #[cfg(target_os = "espidf")]
    embuild::espidf::sysenv::output();
}

/// The firmware's version is its tag (version.rs). The firmware build passes
/// ESP-IDF's PROJECT_VER in as THISTLE_FW_VERSION, so the kernel reports what
/// the image's description holds; the simulator and the host tests have it
/// from tools/firmware_version.sh here, the script PROJECT_VER comes from.
fn firmware_version() {
    println!("cargo:rerun-if-env-changed=THISTLE_FW_VERSION");
    let version = match std::env::var("THISTLE_FW_VERSION") {
        Ok(v) if !v.trim().is_empty() => v.trim().to_string(),
        _ => {
            println!("cargo:rerun-if-changed=../../tools/firmware_version.sh");
            watch_git_head();
            run("sh", &["../../tools/firmware_version.sh", "../.."])
                .unwrap_or_else(|| "0.0.0-unknown".to_string())
        }
    };
    let version = version.strip_prefix('v').unwrap_or(&version).to_string();
    println!("cargo:rustc-env=THISTLE_FW_VERSION={version}");
}

fn run(program: &str, args: &[&str]) -> Option<String> {
    let out = Command::new(program).args(args).output().ok()?;
    if !out.status.success() {
        return None;
    }
    let text = String::from_utf8(out.stdout).ok()?.trim().to_string();
    (!text.is_empty()).then_some(text)
}

fn git(args: &[&str]) -> Option<String> {
    run("git", args)
}

/// A commit or a checkout moves HEAD or the branch it names; describe again
/// then. (Uncommitted changes appearing, `-dirty`, are noticed at the next
/// commit, as ESP-IDF's own PROJECT_VER notices them.)
fn watch_git_head() {
    let mut watch = vec!["HEAD".to_string()];
    if let Some(branch) = git(&["symbolic-ref", "-q", "HEAD"]) {
        watch.push(branch);
    }
    watch.push("packed-refs".to_string());
    for name in watch {
        if let Some(path) = git(&["rev-parse", "--path-format=absolute", "--git-path", &name]) {
            if Path::new(&path).exists() {
                println!("cargo:rerun-if-changed={path}");
            }
        }
    }
}
