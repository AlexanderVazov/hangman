#!/usr/bin/env python3
"""
PUBLIC tests for the `hangman-server` / `hangman-client` exercise.
Run from the project root: python3 test_hangman_public.py

Strategy: we launch two hangman-client processes (each in its own thread),
feed them guesses via stdin, and observe stdout. Since the protocol is not
specified we parse output lines looking for the patterns described in the spec:
  "Word: <masked>"
  "Incorrect guesses: ..."
  "YOU WIN! :)" / "You Lose! :(" / "Tie :/"
"""

import sys, os, time, socket, subprocess, threading, queue
sys.path.insert(0, os.path.dirname(__file__))
from test_harness import build, expect, print_summary

SERVER = "./hangman-server"
CLIENT = "./hangman-client"

def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]

# ── Driven client ──────────────────────────────────────────────────────────────

class DrivenClient:
    """
    Wraps a hangman-client subprocess and lets a test script feed it
    one guess at a time, reading back all output lines until the next prompt.
    """
    PROMPT_MARKERS = ("Word:", "YOU WIN", "You Lose", "Tie :/")

    def __init__(self, host: str, port: int, word: str):
        self.proc = subprocess.Popen(
            [CLIENT, host, str(port), word],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        self._out_q: queue.Queue[str] = queue.Queue()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        self.all_output: list[str] = []

    def _read_loop(self):
        buf = b""
        while True:
            chunk = self.proc.stdout.read(1)
            if not chunk:
                if buf:
                    self._out_q.put(buf.decode(errors="replace"))
                break
            buf += chunk
            if b"\n" in buf:
                lines = buf.split(b"\n")
                for l in lines[:-1]:
                    self._out_q.put(l.decode(errors="replace"))
                buf = lines[-1]

    def read_until_prompt(self, timeout: float = 5.0) -> list[str]:
        """Collect lines until we see a Word: prompt or an end-of-game marker."""
        lines = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self._out_q.get(timeout=0.1)
                lines.append(line)
                self.all_output.append(line)
                if any(m in line for m in self.PROMPT_MARKERS):
                    # Drain a tiny bit more for the same "frame"
                    time.sleep(0.05)
                    while True:
                        try:
                            extra = self._out_q.get_nowait()
                            lines.append(extra)
                            self.all_output.append(extra)
                        except queue.Empty:
                            break
                    return lines
            except queue.Empty:
                continue
        return lines  # timeout — return what we have

    def guess(self, letter: str):
        """Send a single-letter guess."""
        self.proc.stdin.write((letter + "\n").encode())
        self.proc.stdin.flush()

    def close(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()

    @property
    def stderr(self) -> str:
        err = b""
        try:
            err = self.proc.stderr.read()
        except Exception:
            pass
        return err.decode(errors="replace")

# ── Server helper ──────────────────────────────────────────────────────────────

def start_server(port: int) -> subprocess.Popen:
    proc = subprocess.Popen(
        [SERVER, str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        line = proc.stdout.readline().decode(errors="replace")
        if f"Listening on {port}" in line:
            return proc
        if proc.poll() is not None:
            break
        time.sleep(0.05)
    return proc  # best-effort

def stop_server(proc: subprocess.Popen):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

# ── Tests ──────────────────────────────────────────────────────────────────────

def test_server_ready():
    port = free_port()
    proc = subprocess.Popen(
        [SERVER, str(port)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    line = proc.stdout.readline().decode(errors="replace")
    stop_server(proc)
    expect(f"Listening on {port}" in line,
           "server-ready: prints 'Listening on <port>...'",
           f"got: {line!r}")

def test_basic_game():
    """
    Player A picks 'cat', Player B picks 'dog'.
    A guesses c,a,t (all correct, 0 wrong).
    B guesses d,o,g (all correct, 0 wrong).
    Result: Tie.
    """
    port = free_port()
    srv  = start_server(port)
    ca   = None
    cb   = None
    try:
        ca = DrivenClient("127.0.0.1", port, "cat")
        cb = DrivenClient("127.0.0.1", port, "dog")

        # ── Player A guesses 'dog' correctly ──────────────────────────────────
        lines_a = ca.read_until_prompt()
        word_lines_a = [l for l in lines_a if l.strip().startswith("Word:")]
        expect(len(word_lines_a) >= 1,
               "basic: client A receives 'Word:' prompt",
               f"lines: {lines_a}")

        # ── Player B guesses 'cat' correctly ─────────────────────────────────
        lines_b = cb.read_until_prompt()
        word_lines_b = [l for l in lines_b if l.strip().startswith("Word:")]
        expect(len(word_lines_b) >= 1,
               "basic: client B receives 'Word:' prompt",
               f"lines: {lines_b}")

        # Interleave guesses: A guesses d, B guesses c
        ca.guess("d")
        cb.guess("c")
        lines_a = ca.read_until_prompt()
        lines_b = cb.read_until_prompt()

        ca.guess("o")
        cb.guess("a")
        lines_a = ca.read_until_prompt()
        lines_b = cb.read_until_prompt()

        ca.guess("g")
        cb.guess("t")
        # Both should finish; collect final output
        final_a = ca.read_until_prompt(timeout=6)
        final_b = cb.read_until_prompt(timeout=6)

        all_a = "\n".join(ca.all_output)
        all_b = "\n".join(cb.all_output)

        # Both clients should reach a result message
        result_keywords = ("YOU WIN", "You Lose", "Tie")
        expect(any(k in all_a for k in result_keywords),
               "basic: client A receives a result message",
               f"output:\n{all_a}")
        expect(any(k in all_b for k in result_keywords),
               "basic: client B receives a result message",
               f"output:\n{all_b}")

        # Both guessed perfectly → Tie
        expect("Tie" in all_a or "Tie" in all_b,
               "basic: result is Tie when both guess perfectly",
               f"A output:\n{all_a}\nB output:\n{all_b}")

    finally:
        if ca: ca.close()
        if cb: cb.close()
        stop_server(srv)

def main():
    print("Building hangman-server and hangman-client...")
    if not build("hangman-server") or not build("hangman-client"):
        sys.exit(1)
    print()
    print("Running public tests for hangman-server / hangman-client")
    print("─" * 50)

    test_server_ready()
    test_basic_game()

    passed, total = print_summary()
    sys.exit(0 if passed == total else 1)

if __name__ == "__main__":
    main()
