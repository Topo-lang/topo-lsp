"""
topo-lsp diagnostic and go-to-definition test for lsp_demo.
"""

import json, subprocess, sys, os
from typing import IO, Optional, Tuple


class LspTestSetupError(RuntimeError):
    """Raised when the LSP test harness cannot establish the subprocess
    pipes it needs to drive `topo-lsp`. Distinct from any LSP-level
    failure so test output reports the actual root cause instead of
    surfacing as an opaque `'NoneType' has no attribute 'write'`
    AttributeError from inside the framework."""

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

    msg_lines = ["ERROR: topo-lsp binary not found. Searched:"]
    if env:
        msg_lines.append("  TOPO_LSP_EXE={}  (not a file)".format(env))
    for c in candidates:
        msg_lines.append("  {}".format(c))
    msg_lines.append("Hint: build with  cmake --build build --target topo-lsp")
    msg_lines.append("      or set TOPO_LSP_EXE=/path/to/topo-lsp")
    print("\n".join(msg_lines), file=sys.stderr)
    sys.exit(1)


LSP_EXE = _find_lsp_exe()
WORKSPACE = os.path.join(PACKAGE_ROOT, "test", "e2e", "fixtures", "lsp_demo")
TOPO_DIR = os.path.join(WORKSPACE, "topo")
TOPO_FILES = ["types.topo", "engine.topo", "renderer.topo", "game.topo"]


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
        # See test_lsp.py for the rationale: Popen types stdio as
        # Optional[IO[bytes]]; we always pass PIPE so we screen here
        # rather than letting NoneType.write crash inside the framework.
        if self.proc.stdin is None or self.proc.stdout is None or self.proc.stderr is None:
            raise LspTestSetupError(
                f"Popen returned without stdio pipes for {exe!r}; "
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
                f"(method={method!r}): {e}"
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

        ``expected_id`` is Optional[int] to match send()'s return; None
        means the call was a notification, which has no response — we
        raise rather than block forever.
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


SEV = {1: "Error", 2: "Warning", 3: "Information", 4: "Hint"}


def main():
    client = LSPClient(LSP_EXE)

    print("=" * 60)
    print("  topo-lsp diagnostic test - lsp_demo workspace")
    print("=" * 60)

    print("\n--- Initialize ---")
    req_id = client.send("initialize", {
        "rootUri": path_to_uri(WORKSPACE),
        "capabilities": {},
    })
    resp, _ = client.read_response(req_id)
    result = resp.get("result", {})
    si = result.get("serverInfo", {})
    print("  Server: {} v{}".format(si.get("name", "?"), si.get("version", "?")))
    caps = result.get("capabilities", {})
    print("  Capabilities: hover={}, definition={}, completion={}".format(
        caps.get("hoverProvider", False),
        caps.get("definitionProvider", False),
        caps.get("completionProvider") is not None))

    client.send("initialized", {}, is_notification=True)

    total_diags = 0
    file_uris = {}

    for filename in TOPO_FILES:
        filepath = os.path.join(TOPO_DIR, filename)
        uri = path_to_uri(filepath)
        file_uris[filename] = uri
        with open(filepath, "r", encoding="utf-8") as f2:
            text = f2.read()

        print("\n--- didOpen: {} ---".format(filename))
        client.send("textDocument/didOpen", {
            "textDocument": {
                "uri": uri,
                "languageId": "topo",
                "version": 1,
                "text": text,
            }
        }, is_notification=True)

        notif = client.read_message()
        if notif.get("method") == "textDocument/publishDiagnostics":
            diags = notif.get("params", {}).get("diagnostics", [])
            if len(diags) == 0:
                print("  No diagnostics (file is clean)")
            else:
                total_diags += len(diags)
                for d in diags:
                    ln = d["range"]["start"]["line"] + 1
                    col = d["range"]["start"]["character"] + 1
                    sev = SEV.get(d.get("severity", 1), "?")
                    msg = d.get("message", "")
                    print("  [{}] {}:{}:{} -- {}".format(sev, filename, ln, col, msg))
        else:
            print("  WARNING: Expected publishDiagnostics, got: {}".format(notif.get("method")))

    print("\n--- Diagnostic Summary ---")
    print("  Total files opened: {}".format(len(TOPO_FILES)))
    print("  Total diagnostics:  {}".format(total_diags))

    # Go-to-definition: begin_frame in renderer.topo line 34 (0-idx: 33), col 7 (0-idx: 6)
    print("\n--- Go-to-Definition: begin_frame in renderer.topo (line 34, col 7) ---")
    renderer_uri = file_uris["renderer.topo"]
    req_id = client.send("textDocument/definition", {
        "textDocument": {"uri": renderer_uri},
        "position": {"line": 33, "character": 6},
    })
    resp, notifs = client.read_response(req_id)
    dr = resp.get("result")

    if dr is None:
        print("  Result: null (no definition found)")
    elif isinstance(dr, list):
        if len(dr) == 0:
            print("  Result: empty array (no definition found)")
        else:
            for loc in dr:
                tu = loc.get("uri", "")
                tl = loc["range"]["start"]["line"] + 1
                tc = loc["range"]["start"]["character"] + 1
                tf = tu.split("/")[-1]
                print("  Definition found: {}:{}:{}".format(tf, tl, tc))
                print("  URI: {}".format(tu))
    elif isinstance(dr, dict):
        tu = dr.get("uri", "")
        tl = dr["range"]["start"]["line"] + 1
        tc = dr["range"]["start"]["character"] + 1
        tf = tu.split("/")[-1]
        print("  Definition found: {}:{}:{}".format(tf, tl, tc))
        print("  URI: {}".format(tu))
    else:
        print("  Unexpected result: {}".format(dr))

    for n in notifs:
        if n.get("method") == "textDocument/publishDiagnostics":
            nd = n.get("params", {}).get("diagnostics", [])
            if nd:
                nf = n["params"].get("uri", "").split("/")[-1]
                print("  (also received {} diagnostic(s) for {})".format(len(nd), nf))

    print("\n--- Shutdown ---")
    client.shutdown()
    print("  Server exited cleanly.")

    stderr_out = client._stderr.read().decode(errors="replace")
    if stderr_out.strip():
        print("\n--- topo-lsp stderr ---")
        for line in stderr_out.strip().split("\n"):
            print("  " + line)

    print("\n" + "=" * 60)
    print("  Done.")
    print("=" * 60)
    return 0


if __name__ == "__main__":
    sys.exit(main())
