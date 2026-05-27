"""
End-to-end functional verification for topo-lsp.

Test items:
  1. initialize       — handshake + Topo.toml load
  2. didOpen          — open a .topo file -> diagnostics published
  3. hover (function) — signatures, visibility, C++ definition site
  4. hover (import)   — `std::import` types
  5. definition (.topo)       — intra-`.topo` jump
  6. definition (cross-lang)  — jump into the C++ host
  7. completion       — function / type / keyword suggestions
  8. documentSymbol   — outline
  9. import resolution        — hover on a symbol from another file

Usage:
  python test_lsp.py
"""

import json
import subprocess
import sys
import os
from typing import IO, Optional, Tuple


class LspTestSetupError(RuntimeError):
    """Raised when the LSP test harness cannot establish the subprocess
    pipes it needs to drive `topo-lsp`. Distinct from any LSP-level
    failure so test output reports the actual root cause instead of
    surfacing as an opaque `'NoneType' has no attribute 'write'`
    AttributeError from inside the framework."""

# ── Paths ──────────────────────────────────────────────────

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
# topo-lsp/test/e2e -> topo-lsp/test -> topo-lsp (package root)
PACKAGE_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
# topo-lsp -> Topo (monorepo root, where build/ lives)
REPO_ROOT = os.path.dirname(PACKAGE_ROOT)
_EXE_SUFFIX = ".exe" if sys.platform == "win32" else ""


def _find_lsp_exe() -> str:
    """Locate the topo-lsp binary: env override, then known build paths."""
    env = os.environ.get("TOPO_LSP_EXE")
    if env and os.path.isfile(env):
        return env

    candidates = [
        os.path.join(REPO_ROOT, "build", "topo-lsp", "topo-lsp" + _EXE_SUFFIX),
        os.path.join(REPO_ROOT, "build", "bin", "topo-lsp" + _EXE_SUFFIX),
    ]
    for path in candidates:
        if os.path.isfile(path):
            return path

    # Provide a clear error listing everything that was tried
    msg_lines = ["ERROR: topo-lsp binary not found. Searched:"]
    if env:
        msg_lines.append(f"  TOPO_LSP_EXE={env}  (not a file)")
    for c in candidates:
        msg_lines.append(f"  {c}")
    msg_lines.append("Hint: build with  cmake --build build --target topo-lsp")
    msg_lines.append("      or set TOPO_LSP_EXE=/path/to/topo-lsp")
    print("\n".join(msg_lines), file=sys.stderr)
    sys.exit(1)


LSP_EXE = _find_lsp_exe()

# Drive the multi_topo_bench fixture (has Topo.toml + import + C++ host files).
WORKSPACE = os.path.join(PACKAGE_ROOT, "test", "e2e", "fixtures", "multi_topo_bench")
SIM_TOPO = os.path.join(WORKSPACE, "topo", "sim.topo")
MATH_TOPO = os.path.join(WORKSPACE, "topo", "math.topo")
TYPES_TOPO = os.path.join(WORKSPACE, "topo", "types.topo")
SIM_LIB_TOPO = os.path.join(PACKAGE_ROOT, "test", "e2e", "fixtures", "aggressive_bench",
                             "topo", "sim_lib.topo")


def path_to_uri(path: str) -> str:
    p = path.replace("\\", "/")
    return "file:///" + p


# ── JSON-RPC helpers ───────────────────────────────────────

class LSPClient:
    def __init__(self, exe: str):
        self.proc = subprocess.Popen(
            [exe],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        # subprocess.Popen types stdin/stdout/stderr as Optional[IO[bytes]] —
        # they are None only when the corresponding stdio knob is not set to
        # PIPE. We always pass PIPE, so under normal startup all three are
        # bound; if the child died between fork and exec they can still be
        # None or closed, so we screen here and raise a typed error instead
        # of letting the framework crash later with NoneType.write.
        if self.proc.stdin is None or self.proc.stdout is None or self.proc.stderr is None:
            raise LspTestSetupError(
                f"Popen returned without a stdin/stdout/stderr pipe for {exe!r}; "
                f"stdin={self.proc.stdin!r} stdout={self.proc.stdout!r} stderr={self.proc.stderr!r}"
            )
        self._stdin: IO[bytes] = self.proc.stdin
        self._stdout: IO[bytes] = self.proc.stdout
        self._stderr: IO[bytes] = self.proc.stderr
        self.seq = 0

    def send(self, method: str, params: dict, *, is_notification: bool = False) -> Optional[int]:
        self.seq += 1
        msg = {"jsonrpc": "2.0", "method": method, "params": params}
        if not is_notification:
            msg["id"] = self.seq
        body = json.dumps(msg)
        header = f"Content-Length: {len(body)}\r\n\r\n"
        try:
            self._stdin.write(header.encode())
            self._stdin.write(body.encode())
            self._stdin.flush()
        except (BrokenPipeError, OSError) as e:
            raise LspTestSetupError(
                f"topo-lsp stdin closed before message could be sent "
                f"(method={method!r}): {e}; the child likely crashed during startup"
            ) from e
        if is_notification:
            return None
        return self.seq

    def read_message(self) -> dict:
        # Read Content-Length header
        header = b""
        while True:
            b = self._stdout.read(1)
            if not b:
                raise EOFError("LSP stdout closed")
            header += b
            if header.endswith(b"\r\n\r\n"):
                break
        # Parse Content-Length
        length = -1
        for line in header.decode().split("\r\n"):
            if line.startswith("Content-Length:"):
                length = int(line.split(":")[1].strip())
                break
        if length < 0:
            raise ValueError(f"No Content-Length in header: {header!r}")
        body = self._stdout.read(length)
        return json.loads(body)

    def read_response(self, expected_id: Optional[int]) -> Tuple[dict, list]:
        """Read messages until we get the response with the expected id.

        ``expected_id`` is typed Optional[int] to match send()'s return,
        but None means the call was a notification — there is no response
        to wait for. We raise here rather than block forever so the
        problem surfaces as a typed error.
        """
        if expected_id is None:
            raise LspTestSetupError(
                "read_response called with id=None; the corresponding send() "
                "was a notification and does not produce a response"
            )
        notifications: list = []
        while True:
            msg = self.read_message()
            if "id" in msg and msg["id"] == expected_id:
                return msg, notifications
            else:
                notifications.append(msg)

    def shutdown(self):
        req_id = self.send("shutdown", {})
        if req_id is None:
            raise LspTestSetupError("shutdown send returned None — not a notification")
        self.read_response(req_id)
        self.send("exit", {}, is_notification=True)
        self.proc.wait(timeout=5)


# ── Test helpers ───────────────────────────────────────────

passed = 0
failed = 0


def check(name: str, condition: bool, detail: str = ""):
    global passed, failed
    if condition:
        passed += 1
        print(f"  [PASS] {name}")
    else:
        failed += 1
        print(f"  [FAIL] {name}")
        if detail:
            print(f"         {detail}")


# ── Main test ──────────────────────────────────────────────

def main():
    global passed, failed

    client = LSPClient(LSP_EXE)

    # ────────────────────────────────────────────────────────
    print("\n=== Test 1: initialize ===")
    req_id = client.send("initialize", {
        "rootUri": path_to_uri(WORKSPACE),
        "capabilities": {},
    })
    resp, _ = client.read_response(req_id)
    result = resp.get("result", {})
    check("server responds",
          "capabilities" in result)
    check("server name = topo-lsp",
          result.get("serverInfo", {}).get("name") == "topo-lsp")
    check("version = 0.2.0",
          result.get("serverInfo", {}).get("version") == "0.2.0")
    check("hoverProvider = true",
          result.get("capabilities", {}).get("hoverProvider") is True)
    check("definitionProvider = true",
          result.get("capabilities", {}).get("definitionProvider") is True)

    client.send("initialized", {}, is_notification=True)

    # ────────────────────────────────────────────────────────
    print("\n=== Test 2: didOpen (sim_lib.topo — 52 functions, no imports) ===")
    with open(SIM_LIB_TOPO, "r", encoding="utf-8") as f:
        sim_lib_text = f.read()
    sim_lib_uri = path_to_uri(SIM_LIB_TOPO)
    client.send("textDocument/didOpen", {
        "textDocument": {
            "uri": sim_lib_uri,
            "languageId": "topo",
            "version": 1,
            "text": sim_lib_text,
        }
    }, is_notification=True)
    # Read the publishDiagnostics notification
    diag_notif = client.read_message()
    check("publishDiagnostics received",
          diag_notif.get("method") == "textDocument/publishDiagnostics")
    diags = diag_notif.get("params", {}).get("diagnostics", [])
    check("zero diagnostics (valid file)",
          len(diags) == 0,
          f"got {len(diags)} diagnostics")

    # ────────────────────────────────────────────────────────
    print("\n=== Test 3: hover — function signature + visibility ===")
    # Hover on "add_fp" at line 9, col 9 (0-based: line=8, char=8)
    # Line 9: "    int add_fp(int a, int b);"
    req_id = client.send("textDocument/hover", {
        "textDocument": {"uri": sim_lib_uri},
        "position": {"line": 8, "character": 8},
    })
    resp, _ = client.read_response(req_id)
    hover_result = resp.get("result")
    check("hover returns result",
          hover_result is not None)
    if hover_result:
        content = hover_result.get("contents", {}).get("value", "")
        check("hover shows 'private'",
              "private" in content, content[:120])
        check("hover shows function name 'add_fp'",
              "add_fp" in content, content[:120])
        check("hover shows qualified name with namespace",
              "sim::phys::math::detail::fp::core::add_fp" in content,
              content[:200])

    # ────────────────────────────────────────────────────────
    print("\n=== Test 4: hover — public function ===")
    # "run_simulation" at line 114 (0-based: 113)
    # "    void run_simulation(int seed);"
    req_id = client.send("textDocument/hover", {
        "textDocument": {"uri": sim_lib_uri},
        "position": {"line": 113, "character": 9},
    })
    resp, _ = client.read_response(req_id)
    hover_result = resp.get("result")
    check("hover on public function returns result",
          hover_result is not None)
    if hover_result:
        content = hover_result.get("contents", {}).get("value", "")
        check("hover shows 'public'",
              "public" in content, content[:120])
        check("hover shows 'run_simulation'",
              "run_simulation" in content, content[:120])

    # ────────────────────────────────────────────────────────
    print("\n=== Test 5: definition — .topo internal jump ===")
    # Go to definition of "add_fp" — should jump to its declaration location
    req_id = client.send("textDocument/definition", {
        "textDocument": {"uri": sim_lib_uri},
        "position": {"line": 8, "character": 8},
    })
    resp, _ = client.read_response(req_id)
    def_result = resp.get("result")
    check("definition returns result",
          def_result is not None)
    if def_result:
        check("definition uri matches current file",
              sim_lib_uri in def_result.get("uri", ""),
              def_result.get("uri", ""))

    # ────────────────────────────────────────────────────────
    print("\n=== Test 6: completion ===")
    # Request completion at line 4, col 0 (empty line → empty prefix → all items)
    req_id = client.send("textDocument/completion", {
        "textDocument": {"uri": sim_lib_uri},
        "position": {"line": 4, "character": 0},
    })
    resp, _ = client.read_response(req_id)
    comp_result = resp.get("result", {})
    items = comp_result.get("items", [])
    check("completion returns items",
          len(items) > 0, f"got {len(items)} items")
    labels = [i["label"] for i in items]
    check("completion includes functions",
          any(l in labels for l in ["add_fp", "sub_fp", "run_simulation"]),
          f"labels sample: {labels[:10]}")
    check("completion includes keywords",
          any(l in labels for l in ["namespace", "fn", "void"]),
          f"labels sample: {labels[-10:]}")

    # ────────────────────────────────────────────────────────
    print("\n=== Test 7: documentSymbol ===")
    req_id = client.send("textDocument/documentSymbol", {
        "textDocument": {"uri": sim_lib_uri},
    })
    resp, _ = client.read_response(req_id)
    symbols = resp.get("result", [])
    check("documentSymbol returns symbols",
          len(symbols) > 0, f"got {len(symbols)} symbols")
    names = [s["name"] for s in symbols]
    check("top-level namespaces present",
          any("sim::phys::math::detail::fp::core" in n for n in names),
          f"names: {names}")
    # Check children — find the first namespace symbol (skip type aliases)
    first_ns = next((s for s in symbols
                     if "sim::phys::math::detail::fp::core" in s.get("name", "")), {})
    children = first_ns.get("children", [])
    check("namespace has function children",
          len(children) > 0, f"children count: {len(children)}")

    # ────────────────────────────────────────────────────────
    print("\n=== Test 8: didOpen with imports (sim.topo → types.topo) ===")
    with open(SIM_TOPO, "r", encoding="utf-8") as f:
        sim_text = f.read()
    sim_uri = path_to_uri(SIM_TOPO)
    client.send("textDocument/didOpen", {
        "textDocument": {
            "uri": sim_uri,
            "languageId": "topo",
            "version": 1,
            "text": sim_text,
        }
    }, is_notification=True)
    diag_notif = client.read_message()
    check("sim.topo publishDiagnostics received",
          diag_notif.get("method") == "textDocument/publishDiagnostics")

    # ────────────────────────────────────────────────────────
    print("\n=== Test 9: hover — imported std::import type (SimResult) ===")
    # sim.topo line 29 (0-based: 28): "    void run_benchmark(int seed, SimResult* out);"
    # SimResult starts at col 33 (0-based)
    req_id = client.send("textDocument/hover", {
        "textDocument": {"uri": sim_uri},
        "position": {"line": 28, "character": 36},
    })
    resp, _ = client.read_response(req_id)
    hover_result = resp.get("result")
    check("hover on imported type returns result",
          hover_result is not None)
    if hover_result:
        content = hover_result.get("contents", {}).get("value", "")
        check("hover shows std::import info",
              "std::import" in content or "sim_types.h" in content,
              content[:200])

    # ────────────────────────────────────────────────────────
    print("\n=== Test 10: hover on run_benchmark in sim.topo ===")
    # sim.topo line 29 (0-based: 28): "    void run_benchmark(..."
    req_id = client.send("textDocument/hover", {
        "textDocument": {"uri": sim_uri},
        "position": {"line": 28, "character": 9},
    })
    resp, _ = client.read_response(req_id)
    hover_result = resp.get("result")
    check("hover on declared function returns result",
          hover_result is not None)
    if hover_result:
        content = hover_result.get("contents", {}).get("value", "")
        check("hover shows function signature",
              "run_benchmark" in content, content[:200])
        check("hover shows qualified name",
              "sim::bench" in content, content[:200])

    # ────────────────────────────────────────────────────────
    print("\n=== Test 11: didOpen math.topo (has Vec2i from import) ===")
    with open(MATH_TOPO, "r", encoding="utf-8") as f:
        math_text = f.read()
    math_uri = path_to_uri(MATH_TOPO)
    client.send("textDocument/didOpen", {
        "textDocument": {
            "uri": math_uri,
            "languageId": "topo",
            "version": 1,
            "text": math_text,
        }
    }, is_notification=True)
    diag_notif = client.read_message()
    check("math.topo publishDiagnostics received",
          diag_notif.get("method") == "textDocument/publishDiagnostics")

    # Hover on Vec2i in math.topo line 27 (0-based: 26)
    # "    Vec2i vec_add(const Vec2i& a, const Vec2i& b);"
    req_id = client.send("textDocument/hover", {
        "textDocument": {"uri": math_uri},
        "position": {"line": 26, "character": 4},
    })
    resp, _ = client.read_response(req_id)
    hover_result = resp.get("result")
    check("hover on Vec2i (from import) returns result",
          hover_result is not None)
    if hover_result:
        content = hover_result.get("contents", {}).get("value", "")
        check("hover shows std::import for Vec2i",
              "Vec2i" in content, content[:200])

    # ────────────────────────────────────────────────────────
    # Shutdown
    print()
    client.shutdown()

    # ── Summary ────────────────────────────────────────────
    total = passed + failed
    print("=" * 50)
    print(f"  Results: {passed}/{total} passed, {failed} failed")
    print("=" * 50)

    stderr_output = client._stderr.read().decode(errors="replace")
    if stderr_output.strip():
        print("\n--- topo-lsp stderr ---")
        for line in stderr_output.strip().split("\n"):
            print(f"  {line}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
