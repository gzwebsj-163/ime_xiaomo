#!/usr/bin/env python3
"""
linux_embed_driver.py — 驱动 xiaomo 里嵌入的 TinyEMU Linux 内核核心

用法: python3 linux_embed_driver.py [--timeout N]
功能: 启动 temu → 等 busybox 提示符 → 发若干验证命令 → 收输出 → 退出
"""
import subprocess, sys, time, select, os, argparse

TEMU   = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "tinyemu-2019-12-21", "temu")
CFG    = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "diskimage-linux-riscv-2018-09-23", "root-riscv64.cfg")

CMDS = [
    "uname -a",            # 内核版本/架构
    "id",                  # 当前用户
    "cat /proc/version",   # 完整版本串
    "cat /proc/cpuinfo | head -12",
    "free",                # 内存
    "ls / | head -20",     # 根文件系统
    "echo LINUX-EMBED-OK", # 成功标记
]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=60)
    args = ap.parse_args()

    print(f"[driver] 启动 temu: {TEMU}")
    p = subprocess.Popen([TEMU, "-ctrlc", CFG],
                         stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT,
                         bufsize=0)

    out_buf = b""
    start = time.time()

    def pump(timeout_s):
        nonlocal out_buf
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            r, _, _ = select.select([p.stdout], [], [], 0.2)
            if r:
                chunk = os.read(p.stdout.fileno(), 4096)
                if not chunk:
                    return False
                out_buf += chunk
            else:
                return True  # 暂无更多输出，继续等
        return True

    # 阶段1: 等 boot 完成 (出现 busybox 提示符 ~ #)
    print("[driver] 等待 Linux 内核 boot 完成 ...")
    while time.time() - start < args.timeout:
        if not pump(0.5):
            break
        if b"~ #" in out_buf or b"~#" in out_buf:
            break
    if b"~ #" not in out_buf and b"~#" not in out_buf:
        print("[driver] 超时未等到 shell 提示符！")
        print(out_buf.decode(errors="replace"))
        p.kill(); sys.exit(1)

    print("[driver] ✅ shell 就绪，发送验证命令 ...\n" + "="*56)
    # 阶段2: 逐条发命令
    for cmd in CMDS:
        try:
            p.stdin.write(cmd.encode() + b"\n")
            p.stdin.flush()
        except BrokenPipeError:
            break
        pump(0.5)
        # 等命令执行完 (简单起见等一小段)
        time.sleep(0.6)
        pump(0.5)

    print(out_buf.decode(errors="replace"))
    print("="*56)

    # 阶段3: 退出 (Ctrl-A x)
    try:
        p.stdin.write(b"\x01x"); p.stdin.flush()
    except BrokenPipeError:
        pass
    time.sleep(0.5)
    if p.poll() is None:
        p.kill()
    p.wait()
    ok = b"LINUX-EMBED-OK" in out_buf
    print(f"[driver] 退出码={p.returncode}  成功标记={ok}")
    return 0 if ok else 2

if __name__ == "__main__":
    sys.exit(main())
