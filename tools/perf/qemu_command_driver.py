#!/usr/bin/env python3
"""Boot xv6, run one shell command, and stop QEMU after it completes."""

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


ROOT_DIR = Path(__file__).resolve().parents[2]


def write_master(fd: int, value: str) -> None:
    os.write(fd, value.encode("utf-8"))


def stop_qemu(proc: subprocess.Popen, master_fd: int) -> None:
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--command", required=True)
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--qemu-cmd", default="make qemu CPUS=1")
    parser.add_argument("--disk-image", default="")
    args = parser.parse_args()

    if args.disk_image:
        disk_image = Path(args.disk_image)
        shutil.copyfile(ROOT_DIR / "fs.img", disk_image)
        qemu_cmd = [
            "qemu-system-riscv64",
            "-machine", "virt",
            "-bios", "none",
            "-kernel", "kernel/kernel",
            "-m", "128M",
            "-smp", "1",
            "-nographic",
            "-global", "virtio-mmio.force-legacy=false",
            "-drive", f"file={disk_image},if=none,format=raw,id=x0",
            "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
        ]
    else:
        qemu_cmd = shlex.split(args.qemu_cmd)

    master_fd, slave_fd = pty.openpty()
    proc = subprocess.Popen(
        qemu_cmd,
        cwd=ROOT_DIR,
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=slave_fd,
        close_fds=True,
    )
    os.close(slave_fd)

    sent_command = False
    completed = False
    buffer = ""
    deadline = time.time() + args.timeout

    try:
        while time.time() < deadline:
            if proc.poll() is not None:
                break
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
            buffer = (buffer + chunk)[-16384:]

            if not sent_command and "$ " in buffer:
                write_master(master_fd, args.command + "\n")
                sent_command = True
                buffer = ""
                continue

            if sent_command and "$ " in buffer:
                completed = True
                break
    finally:
        stop_qemu(proc, master_fd)
        os.close(master_fd)

    if not sent_command:
        print("[HOST] error=xv6_shell_prompt_not_found", file=sys.stderr)
        return 1
    if not completed:
        print("[HOST] error=benchmark_did_not_return_to_shell", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
