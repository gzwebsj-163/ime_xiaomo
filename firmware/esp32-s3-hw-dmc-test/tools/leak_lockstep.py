#!/usr/bin/env python3
"""检验 泄漏帧 与 「退避后重连」行 是否 1:1 锁定。
若严格相等 => 从机 push 与主机重试在时间上完全绑定,
   这与「从机 rx=0 什么都没收到」形成矛盾, 值得深挖。
"""
import re, sys

ANSI = re.compile(rb'\x1b\[[0-9;]*[A-Za-z]')
raw = open(sys.argv[1], 'rb').read()
clean = ANSI.sub(b'', raw).decode('utf-8', 'replace')

frames = len(re.findall(r'\xaa\x55.{0,3}\x03.{0,3}DMCP', clean, re.S))
backoff = clean.count('退避') - clean.count('需要从设备') * 0
handshake = clean.count('握手失败')
stat = clean.count('[stat] phase=')
slavestat = clean.count('[slave-stat]')

print(f"泄漏帧(AA55+DMCP)     = {frames}")
print(f"'退避 Nms 后重连' 行  = {backoff}")
print(f"'握手失败' 行         = {handshake}")
print(f"'[stat] phase=' 行    = {stat}")
print(f"'[slave-stat]' 行     = {slavestat}")
print()
print(f"帧 / 退避 = {frames/backoff:.3f}" if backoff else "无退避行")
print(f"退避 / 握手失败 = {backoff/handshake:.3f}" if handshake else "")

# 从机自述的 push 计数
m = re.findall(r'\[slave-stat\] rx=(\d+) tx=(\d+) crc_err=(\d+) push=(\d+)', clean)
print()
print("从机自报 [slave-stat] 末值 =", m[-1] if m else "无")
m2 = re.findall(r'\[stat\] phase=\S+ ready=\d+ state=\S+ ok=(\d+) fail=(\d+) reconnect=(\d+) push=(\d+) tx=(\d+) rx=(\d+) crc_err=(\d+) to_err=(\d+)', clean)
print("主机末条 [stat] =", m2[-1] if m2 else "无")
