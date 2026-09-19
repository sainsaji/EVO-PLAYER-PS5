# Addon Architecture & Remote Streaming Research: Emby & Nuvio

Research and technical specification for extending **EVO Player** from a local USB media player into a connected home theater client via an extensible **Addon Subsystem**, with targeted support for **Emby / Jellyfin** media servers and **Nuvio / Stremio-compatible** streaming extensions.

---

## 1. Executive Summary & Strategic Goals

EVO Player currently functions as a high-performance, low-overhead native C media player on the PlayStation 5, reading directly from local block storage (`/mnt/usb0/`, `/mnt/usb1/`, `data/`).

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                             EVO PLAYER CORE                                 │
├───────────────────────────────┬─────────────────────────────────────────────┤
│         LOCAL STORAGE         │               ADDON SUBSYSTEM               │
│  /mnt/usb0/  /mnt/usb1/  data │   HTTP/HTTPS REST & JSON RPC (Async Queue)  │
└───────────────┬───────────────┴──────────────────────┬──────────────────────┘
                ▼                                      ▼
    ┌──────────────────────┐               ┌──────────────────────┐
    │  Direct File Access  │               │   Streaming Sources  │
    │  POSIX open/read     │               │  Emby, Jellyfin,     │
    │  Local MKV/MP4       │               │  Nuvio/Stremio, HLS  │
    └───────────┬──────────┘               └───────────┬──────────┘
                │                                      │
                └───────────────────┬──────────────────┘
                                    ▼
                     ┌─────────────────────────────┐
                     │     FFmpeg Libavformat      │
                     │  Demuxing & Software Decode │
                     └──────────────┬──────────────┘
                                    ▼
                     ┌─────────────────────────────┐
                     │   EVO UI & Audio/VideoOut   │
                     │   2D Framebuffer + 48kHz    │
                     └─────────────────────────────┘
```

### Core Goals
1. **Zero Degradation to Local Playback**: The local USB workflow must remain fast, deterministic, and free of mandatory network dependencies.
2. **Standardized Addon Interface (`evo_addon_t`)**: Create a C vtable abstraction separating UI and player core from specific remote APIs.
3. **Emby / Jellyfin Client**: Enable LAN/WAN connection to personal media servers with user authentication, library browsing, metadata/poster hydration, direct stream playback, and two-way progress reporting.
4. **Nuvio / Stremio Extension Support**: Implement a client engine for the Stremio Addon Protocol v3 (used by Nuvio and Stremio ecosystems), providing manifest discovery, catalog queries, metadata resolution, and direct HTTP/HTTPS / Debrid stream playback.
5. **Console-Safe Network Pipeline**: Implement non-blocking background network threads to ensure the 60fps UI framebuffer thread never stalls on DNS lookups or HTTP latency.

---

## 2. Remote Provider Specifications

### A. Emby / Jellyfin Ecosystem

Emby and Jellyfin expose a well-defined REST API over HTTP/HTTPS with JSON payloads.

```
EVO Player (PS5)                                Emby / Jellyfin Server
       │                                                   │
       │─── 1. POST /Users/AuthenticateByName ────────────>│
       │<── 2. 200 OK (AccessToken, UserId) ───────────────│
       │                                                   │
       │─── 3. GET /Users/{uid}/Views ────────────────────>│
       │<── 4. 200 OK (Root Libraries: Movies, Shows) ─────│
       │                                                   │
       │─── 5. GET /Users/{uid}/Items?ParentId={id} ──────>│
       │<── 6. 200 OK (Items List, MediaSources, Artwork) ─│
       │                                                   │
       │─── 7. GET /Videos/{id}/stream.mkv?Static=true ───>│ (Direct Play)
       │<── 8. HTTP 200 / 206 (Byte Stream to FFmpeg) ─────│
       │                                                   │
       │─── 9. POST /Sessions/Playing/Progress ───────────>│ (Heartbeat)
```

#### Key Endpoints
1. **Authentication**:
   - `POST /Users/AuthenticateByName`
   - Headers: `X-Emby-Authorization: MediaBrowser Client="EVOPlayer", Device="PlayStation 5", DeviceId="{unique_guid}", Version="0.5.0"`
   - Body: `{"Username": "...", "Pw": "..."}`
   - Response: Returns `AccessToken` and `User.Id`.
2. **Library Exploration**:
   - `GET /Users/{UserId}/Views`: Returns root folders (e.g. Movies, TV Shows, Anime, Home Videos).
   - `GET /Users/{UserId}/Items?ParentId={ParentId}&Fields=Overview,MediaSources,UserData,ItemCounts`: Fetches folder contents and metadata.
3. **Stream Resolution (Direct Play)**:
   - `GET /Videos/{ItemId}/stream.{Container}?Static=true&MediaSourceId={SourceId}&api_key={Token}`
   - EVO Player natively handles containers (MKV, MP4, TS, AVI) and codecs (H.264, HEVC, VP9, AV1, DTS, TrueHD, AC3, EAC3), allowing **Direct Stream** without server transcoding in most cases.
4. **Playback Telemetry**:
   - `POST /Sessions/Playing`: Notifies server that playback started.
   - `POST /Sessions/Playing/Progress`: Periodic progress heartbeat (position in ticks: 1 tick = 100ns).
   - `POST /Sessions/Playing/Stopped`: Sends final position and marks items as watched when progress > 90%.

---

### B. Nuvio / Stremio Addon Protocol (v3)

Nuvio and Stremio decouple UI from content scrapers via stateless JSON endpoints over HTTP/HTTPS.

```
EVO Player (PS5)                                Remote Addon Server
       │                                                   │
       │─── 1. GET /manifest.json ────────────────────────>│
       │<── 2. 200 OK (Resources, Catalogs, ID Prefixes) ──│
       │                                                   │
       │─── 3. GET /catalog/{type}/{id}.json ─────────────>│
       │<── 4. 200 OK (Metas array: posters, titles) ──────│
       │                                                   │
       │─── 5. GET /stream/{type}/{id}.json ──────────────>│
       │<── 6. 200 OK (Streams: HTTP URLs, Debrid links) ──│
       │                                                   │
       │─── 7. GET /subtitles/{type}/{id}.json ───────────>│
       │<── 8. 200 OK (Subtitles array: URLs, languages) ──│
```

#### Protocol Resource Endpoints
1. **Manifest (`/manifest.json`)**:
   - Describes capabilities:
     ```json
     {
       "id": "org.nuvio.cinemeta",
       "version": "1.0.0",
       "name": "Cinemeta Catalog",
       "resources": ["catalog", "meta", "stream", "subtitles"],
       "types": ["movie", "series"],
       "catalogs": [
         { "type": "movie", "id": "top", "name": "Popular Movies" },
         { "type": "series", "id": "top", "name": "Popular Series" }
       ],
       "idPrefixes": ["tt"]
     }
     ```
2. **Catalog Browsing (`/catalog/{type}/{id}.json` or `/catalog/{type}/{id}/skip={offset}.json`)**:
   - Returns paginated list of items with IMDB IDs (`tt...`), titles, release years, and poster image URLs.
3. **Item Metadata (`/meta/{type}/{id}.json`)**:
   - Returns full synopsis, genres, backdrop image, runtime, and episode lists for series.
4. **Stream Resolution (`/stream/{type}/{id}.json`)**:
   - Returns available streams:
     ```json
     {
       "streams": [
         {
           "name": "Debrid 4K HDR",
           "title": "Movie.2024.2160p.HDR.HEVC.DTS-HD.mkv\n24.2 GB",
           "url": "https://debrid.provider.com/dl/token123/stream.mkv"
         },
         {
           "name": "Direct 1080p",
           "title": "Movie.2024.1080p.H264.AAC.mp4\n4.5 GB",
           "url": "https://direct.stream.host/video.mp4"
         }
       ]
     }
     ```

---

## 3. PS5 System Constraints & Network Stack Analysis

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           PS5 HOMEBREW ENVIRONMENT                         │
├─────────────────────────────────────────────────────────────────────────────┤
│  Kernel: FreeBSD 12.0 Derivative (Prospero OS)                              │
│  Userland: Standard BSD sockets, pthread, elfldr payload space              │
│  Memory: Up to ~3.5GB available in payload heap                             │
├───────────────────────────────┬─────────────────────────────────────────────┤
│          CAPABILITY           │             TECHNICAL STATUS                │
├───────────────────────────────┼─────────────────────────────────────────────┤
│ BSD TCP/UDP Sockets           │ NATIVE - socket(), connect(), select() OK   │
│ DNS Resolution                │ NATIVE - getaddrinfo() functional           │
│ Hardware TLS/SSL Engine       │ Proprietary SPRX (libSceSsl / libSceHttp)   │
│ Software TLS/SSL (OpenSSL)    │ COMPATIBLE - Static OpenSSL 3.x via SDK     │
│ FFmpeg Network I/O            │ CAPABLE - Needs --enable-network in build   │
│ JSON Parsing                  │ COMPATIBLE - cJSON / yyjson (Header + C)    │
└───────────────────────────────┴─────────────────────────────────────────────┘
```

### Technical Considerations for PS5:

1. **TLS / HTTPS Encryption**:
   - Most modern Emby instances and Nuvio/Stremio endpoints use HTTPS (TLS 1.2/1.3).
   - In PS5 homebrew payloads, linking `libSceSsl` requires runtime symbol resolution or proprietary stubs.
   - **Recommended Solution**: Statically link `libssl.a` and `libcrypto.a` from the PS5 Payload SDK (pacbrew sysroot includes pre-built OpenSSL for Prospero). This avoids all firmware-dependent SPRX calls.

2. **FFmpeg Build Configuration**:
   - `scripts/build-ffmpeg.sh` minimal profile currently disables networking with `--disable-network`.
   - **Required Build Adjustment**:
     ```bash
     --enable-network
     --enable-protocol=http
     --enable-protocol=https
     --enable-protocol=tcp
     --enable-protocol=tls
     --enable-openssl
     --enable-demuxer=hls
     --enable-demuxer=dash
     ```

3. **Non-Blocking Architecture**:
   - The PS5 framebuffer renderer runs on the main thread at 60Hz. Any synchronous HTTP call on this thread causes visible UI stutter or frame drops.
   - All network requests (REST API queries, poster image downloads) must operate through an asynchronous task queue (`evo_net_queue`) handled by background worker threads.

---

## 4. Proposed Addon Architecture for EVO Player

To maintain clean separation between the UI, playback engine, and remote providers, all addon functionality resides in a dedicated `projects/evoplayer/addons/` subsystem.

```
projects/evoplayer/
├── addons/
│   ├── include/
│   │   ├── evo_addon.h          # Universal Addon VTable and Data Types
│   │   ├── evo_net.h            # Async HTTP/HTTPS client & connection pool
│   │   └── evo_json.h           # cJSON wrapper & serialization helpers
│   └── src/
│       ├── evo_net.c            # Worker thread pool, libcurl/socket HTTP
│       ├── evo_addon_mgr.c      # Registry, storage persistence, dispatch
│       ├── addon_emby.c         # Emby & Jellyfin implementation
│       └── addon_nuvio.c        # Nuvio & Stremio Protocol engine
```

### A. Universal Addon Interface (`evo_addon.h`)

```c
#ifndef EVO_ADDON_H
#define EVO_ADDON_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    EVO_ADDON_EMBY = 0,
    EVO_ADDON_JELLYFIN,
    EVO_ADDON_NUVIO_STREMIO,
    EVO_ADDON_WEBDAV,
    EVO_ADDON_CUSTOM
} evo_addon_type_t;

typedef enum {
    EVO_ITEM_FOLDER = 0,
    EVO_ITEM_MOVIE,
    EVO_ITEM_SERIES,
    EVO_ITEM_EPISODE,
    EVO_ITEM_AUDIO,
    EVO_ITEM_STREAM_LINK
} evo_item_type_t;

typedef struct {
    char id[128];
    char title[128];
    char subtitle[128];
    char poster_url[256];
    char backdrop_url[256];
    char stream_url[512];
    int64_t duration_ms;
    int64_t resume_pos_ms;
    evo_item_type_t type;
    bool has_children;
} evo_media_item_t;

typedef struct {
    char name[64];
    char title[128];
    char url[512];
    char quality[32];
    char codec[32];
    int64_t size_bytes;
} evo_stream_choice_t;

typedef struct evo_addon {
    const char *name;
    const char *version;
    evo_addon_type_t type;
    void *user_data;

    /* Lifecycle */
    int  (*init)(struct evo_addon *self);
    void (*shutdown)(struct evo_addon *self);

    /* Navigation & Discovery */
    int  (*get_root_items)(struct evo_addon *self, evo_media_item_t **items, int *count);
    int  (*get_folder_items)(struct evo_addon *self, const char *parent_id, int offset, int limit, evo_media_item_t **items, int *count);
    int  (*search)(struct evo_addon *self, const char *query, evo_media_item_t **items, int *count);

    /* Stream & Media Resolution */
    int  (*resolve_streams)(struct evo_addon *self, const char *item_id, evo_stream_choice_t **streams, int *count);

    /* Playback Telemetry / Status */
    void (*on_playback_start)(struct evo_addon *self, const char *item_id);
    void (*on_playback_progress)(struct evo_addon *self, const char *item_id, int64_t pos_ms, int64_t dur_ms);
    void (*on_playback_stop)(struct evo_addon *self, const char *item_id, int64_t pos_ms, bool completed);
} evo_addon_t;

#endif /* EVO_ADDON_H */
```

---

### B. UI Navigation & Rail Integration

To surface addons naturally alongside local storage, the main Navigation Rail (`evo_rail`) is extended with a new **STREAMING / NETWORK** section:

```
┌──────┐
│ (▶)  │  NOW PLAYING
│      │
│ (⌂)  │  HOME / RECENT
│      │
│ (▤)  │  USB DRIVES (/mnt/usb0, usb1)
│      │
│ (☁)  │  STREAMING & ADDONS (Emby, Nuvio, Stremio)  <-- NEW RAIL SECTION
│      │
│ (★)  │  FAVORITES
│      │
│ (⚙)  │  SETTINGS
│      │
│ (⚒)  │  TOOLS
└──────┘
```

#### New UI Screens:
1. **`SCREEN_ADDONS_DASHBOARD`**:
   - Lists configured providers (e.g. `Home Emby Server`, `Cinemeta`, `Torrentio / Real-Debrid`).
   - Action buttons: "Add Server", "Add Nuvio Addon URL", "Sync Libraries".
2. **`SCREEN_ADDONS_BROWSE`**:
   - Media explorer with poster thumbnails, synopsis sidebar, and pagination for streaming catalogs.
3. **`SCREEN_STREAM_PICKER`**:
   - Modal dialog displaying resolved streams with quality badges (e.g. `[4K HDR HEVC - 22.4 GB]`, `[1080p SDR AAC - 4.1 GB]`).

---

## 5. Implementation Roadmap & Phased Execution

```
PHASE 1: Foundation (Network & Protocols)
  ├── 1.1 Enable FFmpeg HTTP/HTTPS protocols with static OpenSSL
  ├── 1.2 Implement async HTTP request queue (evo_net) on BSD sockets
  └── 1.3 Add cJSON parser module to project

PHASE 2: Addon Subsystem & Emby Client
  ├── 2.1 Implement evo_addon_mgr and configuration serialization (settings file)
  ├── 2.2 Build addon_emby with User Authentication & Direct Stream resolution
  ├── 2.3 Wire Playback telemetry (Start, Heartbeat, Stop) into main player loop
  └── 2.4 Verify Direct Play of 1080p/4K MKV/MP4 streams from Emby server

PHASE 3: Nuvio / Stremio Engine
  ├── 3.1 Implement Stremio Protocol v3 manifest & catalog parser
  ├── 3.2 Add stream resolver supporting direct HTTP(S) and Debrid endpoints
  └── 3.3 Add subtitle discovery and external WebVTT/SRT injection

PHASE 4: UI Experience & Media Art Caching
  ├── 4.1 Add STREAMING rail item & Addon Dashboard screen
  ├── 4.2 Extend thumbnail worker (media/prospero_thumbnail.c) for HTTP poster caching
  └── 4.3 Add Stream Selector modal (SCREEN_STREAM_PICKER)
```

---

## 6. Risk Assessment & Mitigations

| Risk | Impact | Likelihood | Mitigation Strategy |
|---|---|---|---|
| **Network thread blocking UI** | High (UI freeze) | Medium | Strictly isolate all HTTP calls into `evo_net` background worker queue with `pthread` mutex/condvars. |
| **SSL/TLS handshake failure on PS5** | High (Cannot connect) | Low | Use static OpenSSL from Payload SDK sysroot rather than relying on firmware SPRX exports. |
| **In-Memory Poster Cache Overflow** | Medium (Out of memory) | Medium | Cap network poster cache (e.g. 32MB max, LRU eviction) and stream directly to local disk cache (`/data/evoplayer/cache/`). |
| **Bitrate spikes exceeding buffer** | High (Video stutter) | High | Increase FFmpeg `probesize` and `analyzeduration`, and configure `AVIOContext` ring-buffer sizing for streaming URLs. |
| **Server Transcoding Trigger** | Medium (Higher latency) | Low | Send complete `DirectPlayProfile` headers detailing PS5's extensive native codec capabilities (H.264, HEVC, VP9, AV1, DTS, TrueHD, AC3, EAC3, FLAC, AAC). |

---

## 7. Conclusion

Adding Emby and Nuvio/Stremio support to EVO Player is fully feasible within the existing architectural constraints of the PlayStation 5 payload environment. 

Because EVO Player already includes high-performance software decoding for nearly all contemporary audio/video formats, direct streaming without server transcoding is achievable. Implementing a clean `evo_addon_t` abstraction ensures that EVO Player expands its capabilities into network streaming while maintaining its lightweight, standalone USB player speed.
