#!/usr/bin/env python3
"""Provision a per-run key, then send encrypted keyboard/mouse input.

  input-console.py provision
  input-console.py key PRINTSCREEN
  input-console.py text                 # text read from stdin, never an argument
  input-console.py click X Y            # absolute display coordinates

Launch with WOWPS5_INPUT_TLS=1 after provision. Requires Python 3.13+.
The key file is private and contains no account credentials.
"""
import argparse
import io
import os
import socket
import ssl
import struct
import sys
import time
from ftplib import FTP
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'vendor/PS5_Vulkan/tools'))
import ps5_console

KEY = ROOT / 'work/console-input.key'
KEYS = {'TAB': 9, 'ENTER': 13, 'ESC': 27, 'BACKSPACE': 8, 'PRINTSCREEN': 44,
        'HOME': 36, 'END': 35, 'LEFT': 37, 'UP': 38, 'RIGHT': 39, 'DOWN': 40,
        'CTRL': 17, 'SHIFT': 16, 'A': 65}


def connect(settings, key=None):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE  # Authentication uses the PSK, not X.509.
    context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_3
    secret = key if key is not None else KEY.read_bytes()
    context.set_psk_client_callback(lambda hint: ('wowps5-input', secret))
    raw = socket.create_connection((settings['host'], 37081), timeout=10)
    return context.wrap_socket(raw, server_hostname=None)


def event(channel, kind, code=0, flags=0, x=0, y=0):
    channel.sendall(struct.pack('!IIIIii', 0x57503549, kind, code, flags, x, y))
    if channel.recv(1) != b'\x01':
        raise RuntimeError('input event was rejected')
    time.sleep(0.03)


def keypress(channel, code):
    event(channel, 1, code)
    event(channel, 1, code, 2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['provision', 'key', 'text', 'click', 'select-all', 'check-auth'])
    parser.add_argument('values', nargs='*')
    args = parser.parse_args()
    settings = ps5_console.load_settings()
    if args.action == 'provision':
        secret = os.urandom(32)
        fd = os.open(KEY, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, 'wb') as file:
            file.write(secret)
        with FTP() as ftp:
            ftp.connect(settings['host'], settings['ftp_port'], timeout=15)
            ftp.login(settings['ftp_user'], settings['ftp_password'])
            ftp.storbinary('STOR /data/homebrew/PPSA99220/wine-input.key', io.BytesIO(secret))
        print('Provisioned a fresh input key for the next launch.')
        return
    if args.action == 'check-auth':
        with connect(settings):
            pass  # Establish availability with the correct key first.
        try:
            with connect(settings, os.urandom(32)) as channel:
                event(channel, 1, 0)  # no meaningful key; must never be accepted
        except (ssl.SSLError, ConnectionResetError):
            print('PASS: incorrect input key rejected')
        else:
            raise RuntimeError('unauthenticated input was accepted')
    with connect(settings) as channel:
        if args.action == 'key':
            for name in args.values:
                keypress(channel, KEYS[name.upper()])
        elif args.action == 'text':
            # UTF-16 units go through Wine's Unicode keyboard input path.
            text = sys.stdin.read().rstrip('\n')
            for (code,) in struct.iter_unpack('<H', text.encode('utf-16-le')):
                event(channel, 1, code, 4)
                event(channel, 1, code, 6)
        elif args.action == 'click':
            x, y = map(int, args.values)
            event(channel, 2, flags=0x8001, x=x, y=y)
            event(channel, 2, flags=2)
            event(channel, 2, flags=4)
        elif args.action == 'select-all':
            event(channel, 1, KEYS['CTRL'])
            keypress(channel, KEYS['A'])
            event(channel, 1, KEYS['CTRL'], 2)
        print('Authenticated input accepted.')


if __name__ == '__main__':
    main()
