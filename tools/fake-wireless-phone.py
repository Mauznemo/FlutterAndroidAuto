#!/usr/bin/env python3
"""Plays the phone's part of wireless Android Auto, as far as a script can.

Everything in the wireless path except a real phone can be checked on one machine.
This connects to the Unix socket the head unit opens when AA_WIRELESS_FAKE_PHONE is
set, speaks the Bluetooth side of the handshake, and then dials the address it was
given, which is the last thing a phone does before projection starts.

    AA_WIRELESS_FAKE_PHONE=/tmp/aaw.sock tools/run-example.sh --bundle
    tools/fake-wireless-phone.py /tmp/aaw.sock

It stops at the TCP connection on purpose. What happens after that is the ordinary
projection protocol, and only a phone has the certificate to get through the SSID
handshake, so this proves the plumbing rather than the projection.

No protobuf dependency: the four messages involved are small enough to encode and
decode by hand, and a test tool that needs pip on an offline machine is no test tool.
"""

import socket
import struct
import sys
import time

WIFI_START_REQUEST = 1
WIFI_INFO_REQUEST = 2
WIFI_INFO_RESPONSE = 3
WIFI_VERSION_REQUEST = 4
WIFI_VERSION_RESPONSE = 5
WIFI_CONNECTION_STATUS = 6
WIFI_START_RESPONSE = 7

NAMES = {
    WIFI_START_REQUEST: "WifiStartRequest",
    WIFI_INFO_REQUEST: "WifiInfoRequest",
    WIFI_INFO_RESPONSE: "WifiInfoResponse",
    WIFI_VERSION_REQUEST: "WifiVersionRequest",
    WIFI_VERSION_RESPONSE: "WifiVersionResponse",
    WIFI_CONNECTION_STATUS: "WifiConnectionStatus",
    WIFI_START_RESPONSE: "WifiStartResponse",
}


def read_varint(data, at):
    value = 0
    shift = 0
    while True:
        byte = data[at]
        at += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, at
        shift += 7


def decode(payload):
    """Every field of a flat protobuf message, keyed by field number."""
    fields = {}
    at = 0
    while at < len(payload):
        key, at = read_varint(payload, at)
        number, wire = key >> 3, key & 0x07
        if wire == 0:
            fields[number], at = read_varint(payload, at)
        elif wire == 2:
            length, at = read_varint(payload, at)
            fields[number] = payload[at:at + length].decode("utf-8", "replace")
            at += length
        else:
            raise ValueError(f"wire type {wire} is not one this script speaks")
    return fields


def write_varint(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def varint_field(number, value):
    return write_varint(number << 3) + write_varint(value)


def frame(message_id, payload=b""):
    return struct.pack(">HH", len(payload), message_id) + payload


def read_exactly(sock, count):
    data = b""
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise EOFError("the head unit closed the channel")
        data += chunk
    return data


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/aaw.sock"
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(path)
    print(f"[phone] pretending to be a phone on {path}")

    target = None
    network = None
    asked = False

    sock.settimeout(10)
    while True:
        try:
            header = read_exactly(sock, 4)
        except (EOFError, socket.timeout) as stop:
            print(f"[phone] {stop}")
            break
        length, message_id = struct.unpack(">HH", header)
        payload = read_exactly(sock, length) if length else b""
        fields = decode(payload)
        print(f"[phone] <- {NAMES.get(message_id, message_id)} {fields}")

        if message_id == WIFI_START_REQUEST:
            target = (fields.get(1, ""), fields.get(2, 0))
            if not asked:
                asked = True
                print("[phone] -> WifiInfoRequest")
                sock.sendall(frame(WIFI_INFO_REQUEST))

        elif message_id == WIFI_INFO_RESPONSE:
            network = fields
            print(
                f"[phone] told to join {fields.get(1)!r}, "
                f"passphrase {'set' if fields.get(2) else 'empty'}, "
                f"bssid {fields.get(3)!r}, security {fields.get(4)}, "
                f"access point type {fields.get(5)}"
            )
            print("[phone] -> WifiConnectionStatus STATUS_SUCCESS")
            sock.sendall(frame(WIFI_CONNECTION_STATUS, varint_field(1, 0)))
            break

    if network is None:
        print("[phone] FAIL: never got the network details")
        return 1
    if not target or not target[0]:
        print("[phone] FAIL: never got an address to dial")
        return 1

    host, port = target
    print(f"[phone] dialling {host}:{port}")
    # A real phone would join the network first. This one is already on the machine
    # it is dialling, which is exactly why this tool cannot test the Wi-Fi half.
    time.sleep(0.2)
    projection = socket.create_connection((host, port), timeout=5)
    print("[phone] connected, the head unit should now be waiting for a version "
          "response it will never get from this script")
    time.sleep(1.5)
    projection.close()
    sock.close()
    print("[phone] done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
