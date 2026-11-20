#!/usr/bin/env python3
"""Servidor SNTP mínimo (RFC 4330) para o PC dar a hora ao FOSTON pela rede USB.

O BigFoston OS tenta 192.168.7.1 primeiro na atualização automática
(Ajustes > Data). Sem um servidor NTP no PC, rode este:

    sudo python3 tools/bigfoston/sntp_server.py            # porta 123
    python3 tools/bigfoston/sntp_server.py --port 12300    # sem root; no aparelho
                                                            # use 192.168.7.1:12300

Responde com a hora deste PC (stratum 2, quem garante a precisão é o
relógio do PC). Só escuta no endereço da rede USB por padrão.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import socket
import struct
import time

NTP_DELTA = 2208988800          # 1900-01-01 -> 1970-01-01


def ntp_ts(t):
    sec = int(t)
    return struct.pack("!II", sec + NTP_DELTA, int((t - sec) * (1 << 32)) & 0xffffffff)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bind", default="192.168.7.1", help="endereço local (padrão: a rede USB)")
    ap.add_argument("--port", type=int, default=123)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    print(f"SNTP em {args.bind}:{args.port}")
    while True:
        data, peer = sock.recvfrom(512)
        recv = time.time()
        if len(data) < 48:
            continue
        mode = data[0] & 7
        if mode != 3:                       # só pedidos de cliente
            continue
        vn = (data[0] >> 3) & 7
        reply = bytes([(0 << 6) | (vn << 3) | 4, 2, data[2], 0xEC])   # LI 0, servidor, stratum 2
        reply += struct.pack("!II", 0, 0x0100)                        # root delay 0, dispersion ~4 ms
        reply += b"LOCL"                                              # reference id
        reply += ntp_ts(recv)                                         # reference
        reply += data[40:48]                                          # origin = transmit do cliente
        reply += ntp_ts(recv)                                         # receive
        reply += ntp_ts(time.time())                                  # transmit
        sock.sendto(reply, peer)
        if args.verbose:
            print(f"{time.strftime('%H:%M:%S')} {peer[0]}:{peer[1]}")


if __name__ == "__main__":
    main()
