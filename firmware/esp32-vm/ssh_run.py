#!/usr/bin/env python3
"""SSH helper: run remote command or scp file to/from server.
Usage:
  python3 ssh_run.py run 'command'
  python3 ssh_run.py put <local> <remote>
  python3 ssh_run.py get <remote> <local>
"""
import sys, paramiko, io

HOST = "8.163.46.174"
USER = "root"
PASS = "34023260aa.."
PORT = 22

def get_client():
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, port=PORT, username=USER, password=PASS, timeout=30, allow_agent=False, look_for_keys=False)
    return c

def run(cmd):
    c = get_client()
    stdin, stdout, stderr = c.exec_command(cmd, timeout=600)
    out = stdout.read().decode('utf-8', 'replace')
    err = stderr.read().decode('utf-8', 'replace')
    rc = stdout.channel.recv_exit_status()
    c.close()
    sys.stdout.write(out)
    if err:
        sys.stderr.write("STDERR:\n" + err)
    return rc

def put(local, remote):
    c = get_client()
    sftp = c.open_sftp()
    sftp.put(local, remote)
    sftp.close()
    c.close()
    print(f"put {local} -> {remote}")

def get(remote, local):
    c = get_client()
    sftp = c.open_sftp()
    sftp.get(remote, local)
    sftp.close()
    c.close()
    print(f"get {remote} -> {local}")

if __name__ == "__main__":
    a = sys.argv
    if a[1] == "run":
        sys.exit(run(a[2]))
    elif a[1] == "put":
        put(a[2], a[3])
    elif a[1] == "get":
        get(a[2], a[3])
    else:
        print("unknown cmd")
