# topo-lsp

JSON-RPC LSP server for the [Topo](https://github.com/topo-lang/topo-core)
declaration language. Routes per-host completion / hover / definition queries
through per-language LSP bridges (clangd / rust-analyzer / jdtls / pyright /
typescript-language-server).

## Build

```sh
cmake -S . -B build -G Ninja \
    -DCMAKE_PREFIX_PATH=<prefix-with-topo-core+topo-lang+plugins> \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build
ctest --test-dir build --output-on-failure --timeout 120
cmake --install build --prefix /usr/local
```

## Upstream Topo packages

| Package | Required | Notes |
|---|---|---|
| `topo-core` | yes | LSPBridge interface, frontend, analysis |
| `topo-lang` | yes | LanguagePlugin registry framework |
| `topo-v8` | when TypeScript plugin is on | hosts `TopoV8TsServer` |
| `topo-lang-cpp` | optional | `TopoCppPlugin` (gated upstream on `TOPO_LANG_CPP_ENABLE_LLVM`) |
| `topo-lang-rust` | optional | `TopoRustPlugin` (gated upstream on `TOPO_LANG_RUST_ENABLE_LLVM`) |
| `topo-lang-java` | optional | `TopoJavaPlugin` |
| `topo-lang-python` | optional | `TopoPythonPlugin` |
| `topo-lang-typescript` | optional | `TopoTypeScriptPlugin` (needs `topo-v8`) |

Each plugin is detected at configure time. Missing or gated-out plugins are
omitted from the build with a `STATUS` message; the server still runs but
the corresponding host language has no LSP bridge.

## External LSP servers (runtime)

The per-host LSP bridges spawn external language servers as subprocesses.
Integration tests detect them at runtime and self-skip when absent:

| Host language | External server |
|---|---|
| C++ | `clangd` |
| Rust | `rust-analyzer` |
| Java | `jdtls` |
| Python | `pyright-langserver` |
| TypeScript | `typescript-language-server` |

## Consuming `topo-lsp`

```cmake
find_package(topo-lsp CONFIG REQUIRED)
target_link_libraries(my-host PRIVATE topo::lsp::TopoLSPLib)
```

Or spawn the installed `topo-lsp` executable as a JSON-RPC stdio subprocess.

## License

See [LICENSE](LICENSE).
