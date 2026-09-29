#!/usr/bin/env python3
"""
Simple high-performance FTP server for sharing E:\ on the local network.
Allows anonymous access with read-only permissions (ideal for media streaming to EVO Player).
"""
import os
import sys
import socket
from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import FTPHandler
from pyftpdlib.servers import FTPServer

def get_lan_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(('8.8.8.8', 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return '127.0.0.1'

def main():
    share_dir = r"E:\\"
    if not os.path.exists(share_dir):
        print(f"Error: {share_dir} does not exist.")
        sys.exit(1)

    port = 21
    lan_ip = get_lan_ip()

    authorizer = DummyAuthorizer()
    # 'elradfmwMT' = all permissions; 'elr' = read-only (list, cwd, retrieve)
    authorizer.add_anonymous(share_dir, perm='elr')

    handler = FTPHandler
    handler.authorizer = authorizer
    handler.masquerade_address = lan_ip
    handler.passive_ports = range(60000, 60050)
    handler.banner = "EVO Player Laptop FTP Server Ready."

    address = ('0.0.0.0', port)
    try:
        server = FTPServer(address, handler)
    except PermissionError:
        port = 2121
        print(f"Port 21 restricted, falling back to port {port}...")
        address = ('0.0.0.0', port)
        server = FTPServer(address, handler)

    print("=" * 60)
    print("  EVO Player — Local Laptop FTP Server")
    print("=" * 60)
    print(f"  Serving Directory : {share_dir}")
    print(f"  Laptop IP Address : {lan_ip}")
    print(f"  FTP Port          : {port}")
    print(f"  Anonymous Access  : Enabled (read-only)")
    print("-" * 60)
    if port == 21:
        print(f"  To connect in EVO: Type '{lan_ip}' (or '{lan_ip}:21')")
    else:
        print(f"  To connect in EVO: Type '{lan_ip}:{port}'")
    print("=" * 60)
    print("Server running. Press Ctrl+C to stop.")

    server.max_cons = 64
    server.max_cons_per_ip = 16
    server.serve_forever()

if __name__ == '__main__':
    main()
