#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ssh_helper.py — 上传文件到宿主机 + 在 mocode 容器内执行命令或脚本。
用法:
  python3 ssh_helper.py put <local> <remote_host_path>
  python3 ssh_helper.py run <remote_command>
"""
import sys, paramiko

HOST = "8.163.46.174"; USER = "root"; PASS = "34023260aa.."; PORT = 22

def get_client():
    import time
    last = None
    for _ in range(6):
        try:
            c = paramiko.SSHClient()
            c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
            c.connect(HOST, port=PORT, username=USER, password=PASS, timeout=15,
                      allow_agent=False, look_for_keys=False, banner_timeout=12, auth_timeout=12)
            return c
        except Exception as e:
            last = e
            time.sleep(3)
            continue
    raise last


def run(cmd):
    c = get_client()
    try:
        stdin, stdout, stderr = c.exec_command(cmd, timeout=300)
        out = stdout.read().decode('utf-8', 'replace')
        err = stderr.read().decode('utf-8', 'replace')
        sys.stdout.write(out)
        if err:
            sys.stderr.write("STDERR:\n" + err)
    finally:
        c.close()
    return 0

def put(local, remote):
    c = get_client()
    try:
        sftp = c.open_sftp()
        sftp.put(local, remote)
        sftp.close()
        print(f"put {local} -> {remote}")
    finally:
        c.close()

if __name__ == "__main__":
    a = sys.argv
    if a[1] == "run":
        sys.exit(run(a[2]))
    elif a[1] == "put":
        put(a[2], a[3])
    else:
        print("unknown cmd")
