#!/usr/bin/env python3
"""
mock_xtream_server.py - Standalone Xtream Codes test server for EVO Player.

Provides a fully compliant, self-contained player_api.php server running on
your local network to test EVO Player's Xtream Codes IPTV client.
Serves legal, public 24/7 Live HLS streams, VOD movies, and series.

Usage:
    python tools/mock_xtream_server.py
    python tools/mock_xtream_server.py --port 8080
"""

import http.server
import json
import socket
import socketserver
import sys
import urllib.parse

PORT = 8080

# Pre-verified legal public live and VOD streams
STREAMS = {
    # Live streams
    "live_101": "https://dwamdstream102.akamaized.net/hls/live/2015525/dwstream102/index.m3u8",
    "live_102": "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8",
    "live_103": "https://demo.unified-streaming.com/k8s/features/stable/video/tears-of-steel/tears-of-steel.ism/.m3u8",
    "live_104": "https://cph-p2p-msl.akamaized.net/hls/live/2000341/test/master.m3u8",
    # VOD Movies
    "movie_201": "https://raw.githubusercontent.com/mediaelement/mediaelement-files/master/big_buck_bunny.mp4",
    "movie_202": "https://filesamples.com/samples/video/mp4/sample_1280x720.mp4",
    # Series Episodes
    "series_401": "https://raw.githubusercontent.com/mediaelement/mediaelement-files/master/big_buck_bunny.mp4",
    "series_402": "https://filesamples.com/samples/video/mp4/sample_960x540.mp4",
}

def get_local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('8.8.8.8', 80))
        ip = s.getsockname()[0]
    except Exception:
        ip = '127.0.0.1'
    finally:
        s.close()
    return ip

class XtreamHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        sys.stdout.write("[%s] %s\n" % (self.log_date_time_string(), format % args))
        sys.stdout.flush()

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        qs = urllib.parse.parse_qs(parsed.query)

        # 1. Stream playback endpoints (/live/..., /movie/..., /series/...)
        parts = [p for p in path.split('/') if p]
        if len(parts) >= 4 and parts[0] in ('live', 'movie', 'series'):
            stype = parts[0]
            # parts: [type, user, pass, filename]
            fname = parts[3]
            stream_id = fname.split('.')[0]
            key = f"{stype}_{stream_id}"
            target_url = STREAMS.get(key)
            if target_url:
                self.log_message("Redirecting %s (%s) -> %s", self.path, key, target_url)
                self.send_response(302)
                self.send_header('Location', target_url)
                self.send_header('Access-Control-Allow-Origin', '*')
                self.end_headers()
                return
            else:
                self.send_error(404, f"Stream {key} not found")
                return

        # 2. Xtream Codes API endpoint
        if path == '/player_api.php':
            action = qs.get('action', [None])[0]

            self.send_response(200)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()

            local_ip = get_local_ip()

            # No action -> Authentication check
            if not action:
                data = {
                    "user_info": {
                        "username": qs.get('username', ['demo'])[0],
                        "password": qs.get('password', ['demo'])[0],
                        "message": "Connected to EVO Mock Xtream Codes Server",
                        "auth": 1,
                        "status": "Active",
                        "exp_date": "1893456000",
                        "is_trial": "0",
                        "active_cons": "0",
                        "created_at": "1609459200",
                        "max_connections": "5",
                        "allowed_output_formats": ["m3u8", "ts"]
                    },
                    "server_info": {
                        "url": local_ip,
                        "port": str(PORT),
                        "https_port": "8443",
                        "server_protocol": "http",
                        "rtmp_port": "8880",
                        "timezone": "UTC",
                        "timestamp_now": 1727700000,
                        "time_now": "2026-09-30 13:30:00"
                    }
                }
                self.wfile.write(json.dumps(data).encode('utf-8'))
                return

            # Live Categories
            if action == 'get_live_categories':
                data = [
                    {"category_id": "1", "category_name": "News & International", "parent_id": 0},
                    {"category_id": "2", "category_name": "Animation & Entertainment", "parent_id": 0},
                    {"category_id": "3", "category_name": "Test & Showcase Streams", "parent_id": 0},
                ]
                self.wfile.write(json.dumps(data).encode('utf-8'))
                return

            # Live Streams
            if action == 'get_live_streams':
                cat_id = qs.get('category_id', [None])[0]
                streams = [
                    {
                        "num": 1,
                        "name": "Deutsche Welle (DW English)",
                        "stream_type": "live",
                        "stream_id": 101,
                        "stream_icon": "https://upload.wikimedia.org/wikipedia/commons/thumb/7/75/Deutsche_Welle_symbol_2012.svg/320px-Deutsche_Welle_symbol_2012.svg.png",
                        "epg_channel_id": "dw.de",
                        "added": "1600000000",
                        "category_id": "1",
                        "custom_sid": "",
                        "tv_archive": 0,
                        "direct_source": ""
                    },
                    {
                        "num": 2,
                        "name": "Big Buck Bunny 1080p (Mux HLS)",
                        "stream_type": "live",
                        "stream_id": 102,
                        "stream_icon": "https://upload.wikimedia.org/wikipedia/commons/thumb/c/c5/Big_buck_bunny_poster_big.jpg/320px-Big_buck_bunny_poster_big.jpg",
                        "epg_channel_id": "",
                        "added": "1600000000",
                        "category_id": "2",
                        "custom_sid": "",
                        "tv_archive": 0,
                        "direct_source": ""
                    },
                    {
                        "num": 3,
                        "name": "Tears of Steel 1080p (Unified HLS)",
                        "stream_type": "live",
                        "stream_id": 103,
                        "stream_icon": "https://upload.wikimedia.org/wikipedia/commons/thumb/e/e0/Tears_of_Steel_poster.jpg/320px-Tears_of_Steel_poster.jpg",
                        "epg_channel_id": "",
                        "added": "1600000000",
                        "category_id": "2",
                        "custom_sid": "",
                        "tv_archive": 0,
                        "direct_source": ""
                    },
                    {
                        "num": 4,
                        "name": "Akamai HLS Test Stream",
                        "stream_type": "live",
                        "stream_id": 104,
                        "stream_icon": "https://upload.wikimedia.org/wikipedia/commons/thumb/8/8b/Akamai_logo.svg/320px-Akamai_logo.svg.png",
                        "epg_channel_id": "",
                        "added": "1600000000",
                        "category_id": "3",
                        "custom_sid": "",
                        "tv_archive": 0,
                        "direct_source": ""
                    }
                ]
                if cat_id:
                    streams = [s for s in streams if s["category_id"] == cat_id]
                self.wfile.write(json.dumps(streams).encode('utf-8'))
                return

            # VOD Categories
            if action == 'get_vod_categories':
                data = [
                    {"category_id": "10", "category_name": "Open Source Cinema", "parent_id": 0}
                ]
                self.wfile.write(json.dumps(data).encode('utf-8'))
                return

            # VOD Streams
            if action == 'get_vod_streams':
                streams = [
                    {
                        "num": 1,
                        "name": "Big Buck Bunny (2008)",
                        "stream_type": "movie",
                        "stream_id": 201,
                        "stream_icon": "https://upload.wikimedia.org/wikipedia/commons/thumb/c/c5/Big_buck_bunny_poster_big.jpg/320px-Big_buck_bunny_poster_big.jpg",
                        "rating": "8.5",
                        "rating_5based": 4.2,
                        "added": "1600000000",
                        "category_id": "10",
                        "container_extension": "mp4"
                    },
                    {
                        "num": 2,
                        "name": "HD 720p Showcase Video",
                        "stream_type": "movie",
                        "stream_id": 202,
                        "stream_icon": "https://upload.wikimedia.org/wikipedia/commons/thumb/a/a7/Camcord.svg/320px-Camcord.svg.png",
                        "rating": "7.9",
                        "rating_5based": 3.9,
                        "added": "1600000000",
                        "category_id": "10",
                        "container_extension": "mp4"
                    }
                ]
                cat_id = qs.get('category_id', [None])[0]
                if cat_id:
                    streams = [s for s in streams if s["category_id"] == cat_id]
                self.wfile.write(json.dumps(streams).encode('utf-8'))
                return

            # Series Categories
            if action == 'get_series_categories':
                data = [
                    {"category_id": "20", "category_name": "Animated Shorts & Series", "parent_id": 0}
                ]
                self.wfile.write(json.dumps(data).encode('utf-8'))
                return

            # Series List
            if action == 'get_series':
                data = [
                    {
                        "num": 1,
                        "name": "Blender Open Projects",
                        "series_id": 301,
                        "cover": "https://upload.wikimedia.org/wikipedia/commons/thumb/0/0c/Blender_logo_no_text.svg/320px-Blender_logo_no_text.svg.png",
                        "plot": "Open-source animated films and short series produced by Blender Foundation.",
                        "cast": "Open Source Community",
                        "director": "Ton Roosendaal",
                        "genre": "Animation",
                        "releaseDate": "2024",
                        "rating": "9.2",
                        "rating_5based": 4.6,
                        "category_id": "20"
                    }
                ]
                self.wfile.write(json.dumps(data).encode('utf-8'))
                return

            # Series Info
            if action == 'get_series_info':
                data = {
                    "seasons": [
                        {
                            "air_date": "2024",
                            "episode_count": 2,
                            "id": 1,
                            "name": "Season 1",
                            "overview": "First season of open projects.",
                            "season_number": 1,
                            "cover": "https://upload.wikimedia.org/wikipedia/commons/thumb/0/0c/Blender_logo_no_text.svg/320px-Blender_logo_no_text.svg.png"
                        }
                    ],
                    "info": {
                        "name": "Blender Open Projects",
                        "cover": "https://upload.wikimedia.org/wikipedia/commons/thumb/0/0c/Blender_logo_no_text.svg/320px-Blender_logo_no_text.svg.png",
                        "plot": "Open-source animated series",
                        "genre": "Animation",
                        "rating": "9.2"
                    },
                    "episodes": {
                        "1": [
                            {
                                "id": "401",
                                "episode_num": 1,
                                "title": "Big Buck Bunny (Short Film)",
                                "container_extension": "mp4",
                                "info": {"duration_secs": 596, "duration": "00:09:56"}
                            },
                            {
                                "id": "402",
                                "episode_num": 2,
                                "title": "HD Nature Showcase",
                                "container_extension": "mp4",
                                "info": {"duration_secs": 120, "duration": "00:02:00"}
                            }
                        ]
                    }
                }
                self.wfile.write(json.dumps(data).encode('utf-8'))
                return

            # Unknown action fallback
            self.wfile.write(b"[]")
            return

        # Default fallback
        self.send_error(404, "Not Found")

def main():
    global PORT
    if len(sys.argv) > 1 and sys.argv[1].isdigit():
        PORT = int(sys.argv[1])

    local_ip = get_local_ip()
    server_address = ('0.0.0.0', PORT)
    httpd = socketserver.TCPServer(server_address, XtreamHandler)

    print("=" * 65)
    print("   EVO Player — Mock Xtream Codes IPTV Test Server")
    print("=" * 65)
    print(f" Server listening on:  http://{local_ip}:{PORT}")
    print(f" Xtream API endpoint:  http://{local_ip}:{PORT}/player_api.php")
    print("-" * 65)
    print(" Enter these credentials in EVO Player on your PS5:")
    print(f"   Server / Host:      {local_ip}")
    print(f"   Port:               {PORT}")
    print(f"   Username:           demo")
    print(f"   Password:           demo")
    print(f"   Use HTTPS:          No (0)")
    print("-" * 65)
    print(" Or copy this into /mnt/usb0/.evo_xtream.conf on your USB stick:")
    print(f"   host={local_ip}")
    print(f"   port={PORT}")
    print(f"   username=demo")
    print(f"   password=demo")
    print(f"   use_https=0")
    print(f"   stream_format=m3u8")
    print("=" * 65)
    print(" Press Ctrl+C to stop the server.")
    print("")

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping Xtream test server.")
        httpd.server_close()

if __name__ == '__main__':
    main()
