"""
The framing of Src_RL_Trading's protocol (src/net/protocol.cpp), so the C++ simulator can talk to the policy server:
every message is an 8-byte big-endian length followed by that many bytes of text, at most 1 MiB
"""
from __future__ import annotations

import socket

MAX_MESSAGE_BYTES = 1 << 20


def _recv_exact(sock: socket.socket, count: int) -> bytes | None:
    data = b""
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def recv_frame(sock: socket.socket) -> str | None:
    header = _recv_exact(sock, 8)
    if header is None:
        return None
    length = int.from_bytes(header, "big")
    if length > MAX_MESSAGE_BYTES:
        return None
    payload = _recv_exact(sock, length) if length else b""
    return None if payload is None else payload.decode()


def send_frame(sock: socket.socket, text: str) -> None:
    payload = text.encode()
    sock.sendall(len(payload).to_bytes(8, "big") + payload)
