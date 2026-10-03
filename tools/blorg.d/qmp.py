#!/usr/bin/env python3
"""Tiny QMP / QEMU guest agent client for blorg guest. Standard library only.

    qmp.py qmp SOCK <command> [json-args]      one QMP command, prints the return
    qmp.py qmp SOCK hmp "<monitor command>"    a human-monitor command (savevm, ...)
    qmp.py qga SOCK <command> [json-args]      one guest-agent command
    qmp.py qga SOCK exec <timeout-s> <powershell...>
                                                run PowerShell in the guest via the
                                                agent; prints stdout/stderr, exits
                                                with the guest process's exit code

QMP is the hypervisor's control channel (snapshots, screenshots, power).
The guest agent is the fallback way into Windows when SSH is not answering,
e.g. after a driver bug has taken the network stack down with it.
"""

import base64
import json
import socket
import sys
import time


class Channel:
    def __init__(self, path, timeout):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(path)
        self.buf = b""

    def send(self, obj):
        self.sock.sendall(json.dumps(obj).encode() + b"\n")

    def recv(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("channel closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def reply(self):
        # QMP interleaves asynchronous events with replies; skip them.
        while True:
            msg = self.recv()
            if "event" in msg:
                continue
            if "error" in msg:
                raise RuntimeError(msg["error"].get("desc", str(msg["error"])))
            return msg.get("return")

    def call(self, command, args=None):
        req = {"execute": command}
        if args:
            req["arguments"] = args
        self.send(req)
        return self.reply()


def open_qmp(path):
    ch = Channel(path, timeout=600)  # savevm of a busy guest can take a while
    ch.recv()  # greeting
    ch.call("qmp_capabilities")
    return ch


def open_qga(path):
    ch = Channel(path, timeout=30)
    # The agent channel is a byte stream with no framing guarantee across
    # reconnects; guest-sync with a random id discards stale output.
    token = int(time.time() * 1000) % 2**31
    ch.send({"execute": "guest-sync", "arguments": {"id": token}})
    while ch.reply() != token:
        pass
    return ch


def qga_exec(ch, timeout, script):
    pid = ch.call("guest-exec", {
        "path": "powershell.exe",
        "arg": ["-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
        "capture-output": True,
    })["pid"]
    deadline = time.time() + timeout
    while True:
        st = ch.call("guest-exec-status", {"pid": pid})
        if st.get("exited"):
            break
        if time.time() > deadline:
            print(f"qga exec: timed out after {timeout}s (pid {pid})", file=sys.stderr)
            return 124
        time.sleep(1)
    for key, stream in (("out-data", sys.stdout), ("err-data", sys.stderr)):
        if key in st:
            stream.write(base64.b64decode(st[key]).decode("utf-8", "replace"))
    return st.get("exitcode", 1)


def main(argv):
    if len(argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2
    kind, path, command = argv[1], argv[2], argv[3]
    rest = argv[4:]
    try:
        if kind == "qmp":
            ch = open_qmp(path)
            if command == "hmp":
                out = ch.call("human-monitor-command", {"command-line": " ".join(rest)})
                sys.stdout.write(out or "")
                # HMP reports failures as text, not as a QMP error.
                if out and out.lower().startswith(("error", "could not")):
                    return 1
                return 0
            args = json.loads(rest[0]) if rest else None
        elif kind == "qga":
            ch = open_qga(path)
            if command == "exec":
                return qga_exec(ch, int(rest[0]), " ".join(rest[1:]))
            args = json.loads(rest[0]) if rest else None
        else:
            print(__doc__, file=sys.stderr)
            return 2
        result = ch.call(command, args)
        print(json.dumps(result, indent=2))
        return 0
    except (OSError, ConnectionError, RuntimeError, ValueError) as e:
        print(f"{kind}: {command}: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
