fn main() {
    let args: Vec<String> = std::env::args().collect();
    let out = treeminer_orchestrator::run(args);
    eprint!("{}", out.stderr);
    print!("{}", out.stdout);
    std::process::exit(out.code);
}
