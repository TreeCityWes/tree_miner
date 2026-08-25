fn main() {
    println!("cargo:rerun-if-changed=native/stub.c");
    println!("cargo:rerun-if-changed=../../../src/hashapi/treeminer_hash.h");
    cc::Build::new()
        .file("native/stub.c")
        .include("../../../src/hashapi")
        .warnings(true)
        .compile("treeminer_hash_stub");
}
