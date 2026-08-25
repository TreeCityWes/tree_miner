fn main() {
    println!("cargo:rerun-if-changed=../../../src/hashapi/treeminer_hash.h");
    if std::env::var("CARGO_FEATURE_CUDA").is_ok() {
        link_cmake_lib();
        return;
    }
    println!("cargo:rerun-if-changed=native/stub.c");
    cc::Build::new()
        .file("native/stub.c")
        .include("../../../src/hashapi")
        .warnings(true)
        .compile("treeminer_hash");
}

fn link_cmake_lib() {
    println!("cargo:rerun-if-env-changed=TREEMINER_HASH_LIB_DIR");
    println!("cargo:rerun-if-env-changed=TREEMINER_HASH_LIB");
    if let Ok(lib) = std::env::var("TREEMINER_HASH_LIB") {
        if !lib.is_empty() {
            let path = std::path::PathBuf::from(&lib);
            let dir = path.parent().unwrap_or_else(|| std::path::Path::new("."));
            emit_link(dir.to_string_lossy().as_ref());
            return;
        }
    }
    match std::env::var("TREEMINER_HASH_LIB_DIR") {
        Ok(dir) if !dir.is_empty() => emit_link(&dir),
        _ => {
            panic!(
                "treeminer-hash `cuda` feature requires libtreeminer_hash.\n\
                 Build it (does not change xenblocksMiner):\n\
                   cmake -S treeminer -B build -DTREEMINER_BUILD_HASH_FFI=ON\n\
                   cmake --build build --target treeminer_hash\n\
                 then:\n\
                   export TREEMINER_HASH_LIB_DIR=$(pwd)/build/lib\n\
                   cargo build -p treeminer-hash --features cuda\n\
                 Source build/treeminer-hash-cuda.env if CMake already generated it."
            );
        }
    }
}

fn emit_link(dir: &str) {
    println!("cargo:rustc-link-search=native={dir}");
    println!("cargo:rustc-link-lib=dylib=treeminer_hash");
    let os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if os == "linux" || os == "macos" {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
    }
}
