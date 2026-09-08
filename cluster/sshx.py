#!/usr/bin/env python3
"""sshx.py — 通用远端执行/传输 (kickpi / 服务器 / 手机备用)。
用法:
  python3 sshx.py <host> run 'command'
  python3 sshx.py <host> put <local> <remote>
  python3 sshx.py <host> get <remote> <local>
host: kickpi | server
"""
import sys, os, paramiko

HOSTS = {
    "kickpi": dict(host=os.environ.get("XIAOMO_KICKPI_HOST", "192.168.0.148"),
                   user=os.environ.get("XIAOMO_KICKPI_USER", "kickpi"),
                   passwd=os.environ.get("XIAOMO_KICKPI_PW", "kickpi"), port=22),
    "server": dict(host=os.environ.get("XIAOMO_SERVER_HOST", ""),
                   user=os.environ.get("XIAOMO_SERVER_USER", "root"),
                   passwd=os.environ.get("XIAOMO_SERVER_PW", ""), port=22),
}

# 本地真实凭据兜底: cluster/sshx_secrets.py（.gitignore 排除，不入库；
# 也可用环境变量 XIAOMO_SERVER_HOST/USER/PW 覆盖）
try:
    from sshx_secrets import HOSTS as _LOCAL_HOSTS
    for _k, _v in _LOCAL_HOSTS.items():
        HOSTS.setdefault(_k, {}).update(_v)
except ImportError:
    pass

def get_client(name):
    cfg = HOSTS[name]
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(cfg["host"], port=cfg["port"], username=cfg["user"],
              password=cfg["passwd"], timeout=30,
              allow_agent=False, look_for_keys=False)
    return c

def run(name, cmd, timeout=600):
    c = get_client(name)
    try:
        stdin, stdout, stderr = c.exec_command(cmd, timeout=timeout)
        out = stdout.read().decode('utf-8', 'replace')
        err = stderr.read().decode('utf-8', 'replace')
        rc = stdout.channel.recv_exit_status()
        sys.stdout.write(out)
        if err:
            sys.stderr.write("STDERR:\n" + err)
        return rc
    finally:
        c.close()

def put(name, local, remote):
    c = get_client(name)
    try:
        sftp = c.open_sftp()
        # 自动建目录
        parts = remote.rsplit("/", 1)
        if len(parts) == 2 and parts[0]:
            try: sftp.mkdir(parts[0])
            except Exception: pass
        sftp.put(local, remote)
        sftp.close()
        print(f"put {local} -> {remote}")
    finally:
        c.close()

def get(name, remote, local):
    c = get_client(name)
    try:
        sftp = c.open_sftp()
        sftp.get(remote, local)
        sftp.close()
        print(f"get {remote} -> {local}")
    finally:
        c.close()

if __name__ == "__main__":
    a = sys.argv
    if len(a) < 4:
        print(__doc__); sys.exit(1)
    name = a[1]
    if a[2] == "run":
        sys.exit(run(name, a[3]))
    elif a[2] == "put":
        put(name, a[3], a[4])
    elif a[2] == "get":
        get(name, a[3], a[4])
    else:
        print("unknown cmd"); sys.exit(2)
