fn main() {
    // Rerun only when this script changes. A build script that names nothing
    // is rerun whenever any file in the package changes, and the C shims,
    // CMakeLists.txt and INTERFACES.md live here too. The firmware build runs
    // cargo every time, so without this, editing any of them would recompile
    // the whole kernel, 17 s on the device target.
    println!("cargo:rerun-if-changed=build.rs");

    // Only run ESP-IDF build integration when targeting ESP
    #[cfg(target_os = "espidf")]
    embuild::espidf::sysenv::output();
}
