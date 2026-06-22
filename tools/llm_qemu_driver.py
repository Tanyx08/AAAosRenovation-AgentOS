#!/usr/bin/env python3
"""
Run QEMU and bridge xv6 LLM requests to the host-side proxy automatically.

This script removes the manual copy/paste step:
  1. start QEMU
  2. wait for the xv6 shell prompt
  3. run `planner_agent llm-api`
  4. capture @@AGENTOS_LLM_REQ ... @@END from QEMU output
  5. call the demo or third-party API proxy
  6. send @@AGENTOS_LLM_RESP ... @@END back into xv6
"""

import argparse
import os
import pty
import select
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
sys.path.insert(0, str(SCRIPT_DIR))

import llm_proxy  # noqa: E402


def write_master(master_fd: int, text: str) -> None:
    os.write(master_fd, text.encode("utf-8"))


def make_response(args: argparse.Namespace, payload: str) -> str:
    request_id = llm_proxy.field(payload, "id", "0")
    role = llm_proxy.field(payload, "role", "planner")
    prompt = llm_proxy.field(payload, "prompt", "")

    if args.mode == "api":
        return llm_proxy.call_real_llm_api(
            request_id,
            role,
            prompt,
            args.api_key,
            args.model,
            args.api_url,
        )
    return llm_proxy.demo_model_response(request_id, role, prompt)


def terminate_qemu(proc: subprocess.Popen, master_fd: int) -> None:
    if proc.poll() is not None:
        return
    try:
        write_master(master_fd, "\x01x")
        proc.wait(timeout=5)
    except Exception:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()


def run_driver(args: argparse.Namespace) -> int:
    master_fd, slave_fd = pty.openpty()
    if args.disk_image:
        disk_image = Path(args.disk_image)
        shutil.copyfile(REPO_ROOT / "fs.img", disk_image)
        qemu_cmd = [
            "qemu-system-riscv64",
            "-machine",
            "virt",
            "-bios",
            "none",
            "-kernel",
            "kernel/kernel",
            "-m",
            "128M",
            "-smp",
            "1",
            "-nographic",
            "-global",
            "virtio-mmio.force-legacy=false",
            "-drive",
            f"file={disk_image},if=none,format=raw,id=x0",
            "-device",
            "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
        ]
    else:
        qemu_cmd = shlex.split(args.qemu_cmd)
    proc = subprocess.Popen(
        qemu_cmd,
        cwd=REPO_ROOT,
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=slave_fd,
        close_fds=True,
    )
    os.close(slave_fd)

    buffer = ""
    pending_response = ""
    sent_bridge_command = False
    sent_response = False
    saw_shell_after_response = False
    deadline = time.time() + args.timeout

    try:
        while time.time() < deadline:
            if proc.poll() is not None:
                return proc.returncode or 0
            ready, _, _ = select.select([master_fd], [], [], 0.2)
            if not ready:
                continue
            try:
                chunk = os.read(master_fd, 4096).decode("utf-8", errors="replace")
            except OSError:
                break
            if not chunk:
                continue
            sys.stdout.write(chunk)
            sys.stdout.flush()
            buffer = (buffer + chunk)[-8192:]

            if not sent_bridge_command and "$ " in buffer:
                write_master(master_fd, args.bridge_command + "\n")
                sent_bridge_command = True
                continue

            if sent_bridge_command and not sent_response:
                match = llm_proxy.REQ_RE.search(buffer)
                if match and not pending_response:
                    pending_response = make_response(args, match.group(1))
                if pending_response and "[LLM-Bridge] waiting for host proxy response line" in buffer:
                    print(f"\n[Host-Proxy] send response: {pending_response}", flush=True)
                    write_master(master_fd, pending_response + "\n")
                    sent_response = True
                    buffer = ""
                    continue

            if sent_response and "$ " in buffer:
                saw_shell_after_response = True
                terminate_qemu(proc, master_fd)
                break
    finally:
        terminate_qemu(proc, master_fd)
        os.close(master_fd)

    if not sent_bridge_command:
        print("[Host-Proxy] failed: xv6 shell prompt not found", file=sys.stderr)
        return 1
    if not sent_response:
        print("[Host-Proxy] failed: LLM request not found", file=sys.stderr)
        return 1
    if not saw_shell_after_response:
        print("[Host-Proxy] warning: response sent, shell prompt not observed", file=sys.stderr)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["demo", "api"], default="demo")
    parser.add_argument("--api-key", default=os.environ.get("AGENTOS_LLM_API_KEY", ""))
    parser.add_argument("--model", default=os.environ.get("AGENTOS_LLM_MODEL", ""))
    parser.add_argument("--api-url", default=os.environ.get("AGENTOS_LLM_API_URL", ""))
    parser.add_argument("--qemu-cmd", default="make qemu CPUS=1")
    parser.add_argument(
        "--disk-image",
        default="",
        help="copy fs.img to this path and boot QEMU from the copy",
    )
    parser.add_argument("--bridge-command", default="planner_agent llm-api")
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    return run_driver(args)


if __name__ == "__main__":
    raise SystemExit(main())
