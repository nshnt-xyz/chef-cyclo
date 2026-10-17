# Offline maps and elevation

[Research index](README.md) · [UI and ride application plan](../next-steps/ui-and-ride-app.md)

Research of 2026-10-17 on loading map tiles onto the phone, keeping them offline under `/data/v1/maps`, and refreshing them when a network is available. Sizes below are real `pmtiles extract --dry-run` results against the live builds of that day, not estimates, unless marked. The sample regions (Bengaluru, Karnataka) only stand in for "a city" and "a state"; the real region is still to be chosen.

**Decided 2026-10-17: OpenStreetMap is the map data.** Indian and commercial sources were considered and rejected (below). Theming is not a factor in the format choice.

## Where the data comes from

All options use OpenStreetMap data. What is not allowed is fetching map images from the OSM Foundation's own tile servers (`tile.openstreetmap.org`) for offline use: the [tile usage policy](https://operations.osmfoundation.org/policies/tiles/) names "download for offline use" and "save area for later" as prohibited bulk downloading and blocks such applications without notice. The policy itself points at self-hosted tiles or a provider that allows offline use, and suggests vector tiles.

| Source | What it is | Fit |
|---|---|---|
| [Protomaps basemap](https://docs.protomaps.com/basemaps/downloads) | Daily planet build of OSM as vector tiles (MVT) in one PMTiles file, z0 to z15, about 139 GB. Any region can be cut out with HTTP range requests. ODbL, OSM attribution. | **Recommended.** The phone can refresh its own region directly. |
| Geofabrik extracts + Planetiler (or tilemaker) on the host | Raw OSM `.osm.pbf` per region (India southern zone 518 MB, western 191 MB), turned into our own vector schema. | Only if road `surface` (which Protomaps leaves out) proves worth a host build job. Cycle route relations do not matter: India has essentially none (user, 2026-10-18). |
| [OpenFreeMap](https://openfreemap.org) | Weekly full-planet MBTiles (OpenMapTiles schema). | No regional cut; a planet download to extract from. No advantage over Protomaps here. |
| tile.openstreetmap.org, CyclOSM, commercial raster APIs | Pre-drawn raster images. | Offline bulk fetch is prohibited or licensed. Not usable. |
| [Mapterhorn](https://mapterhorn.com) | Planet terrain (Copernicus GLO-30 base, 30 m) as Terrarium-encoded lossless WebP (VP8L), 512 px tiles, z0 to z12, in one PMTiles file. | **Recommended for elevation** (the Climb page's DEM). |

Protomaps' roads layer has `kind` (highway, major_road, minor_road, path, ...) and `kind_detail` including `cycleway`, `path`, `track`, `bridleway` and `steps`, plus oneway, bridge and tunnel. POIs include `drinking_water`, `bicycle_parking` and `bicycle_repair_station`. There is **no surface and no cycle route network** ([layers](https://docs.protomaps.com/basemaps/layers)). India has essentially no cycle routes, so only surface could justify our own tiles, and its tagging coverage in India is unchecked. **Protomaps as-is** (2026-10-18).

### What bike computers use

Garmin, Wahoo and Hammerhead all ship OpenStreetMap-derived vector maps rendered on the device, downloaded per region over Wi-Fi. Garmin uses its own `.img` format, Wahoo its own, and Karoo (Android) reportedly Mapsforge with a checksum manifest that flags side-loaded files. COROS uses Mapbox data. Karoo regions for large countries are listed at 5 to over 9 GB. The most common user complaint is region downloads failing on hotel and public Wi-Fi, so our updater must retry cleanly. The open Mapsforge format, with monthly OpenAndroMaps regional files for cycling and hiking (India included), is an alternative packaging to PMTiles. Its readers are Java, so it would need its own C reader; with no cycle routes in India it offers no data advantage here.

### Indian sources (rejected for map data)

- **MapmyIndia (Mappls)**: commercial, license keys with daily hit caps and mandatory branding; nothing found that permits offline storage on our own device.
- **Ola Maps**: an online API; no offline terms found.
- **Survey of India (Nakshe)**: free vector data only at 1:1M; the 1:50K topo sheets are per-sheet PDFs behind Aadhaar login with an Aadhaar barcode watermark.

Indian elevation sources remain useful as a DEM upgrade: the Survey of India **10 m** terrain model from 1:50K contours (same portal, per sheet, Aadhaar login) and ISRO Bhuvan's 30 m CartoDEM v3 (registration; at least as accurate as SRTM and TanDEM-X on Indian plains in published comparisons, none found against Copernicus). Terrain does not change, so a one-time manual download converted on the host into the same PMTiles layout is acceptable if Mapterhorn's climb totals prove noisy.

Maps published in India must show the Survey of India international boundary; global OSM does not. This does not affect a personal device at riding zooms. If a zoomed-out India view is ever shown, swap in the official boundary from the Indian OSM community ([openstreetmap.in](https://openstreetmap.in)).

## Measured sizes

Protomaps build `20261017` (OSM replication time 2026-10-17 04:00 UTC):

| Region | Max zoom | Archive | Transfer | HTTP requests |
|---|---|---|---|---|
| Bengaluru, about 65×60 km | 15 | 42 MB | 44 MB | 56 |
| Bengaluru | 14 | 18 MB | 19 MB | 50 |
| Karnataka (bbox) | 15 | 393 MB | 413 MB | 133 |
| Karnataka (bbox) | 14 | 196 MB | 206 MB | 96 |

Mapterhorn `planet.pmtiles` (DEM):

| Region | Max zoom | Archive |
|---|---|---|
| Bengaluru | 12 | 9.6 MB |
| Karnataka (bbox) | 12 | 418 MB |
| Karnataka (bbox) | 11 | 112 MB |

z11 is about 38 m per pixel at 512 px, close to GLO-30's native 30 m; z12 (19 m) only oversamples it, so z11 is enough for climb totals and gradient. A whole state with z15 vector tiles and a z11 DEM is about 500 MB, under 1 % of the 55 GB userdata. A `--region` GeoJSON polygon instead of a bbox will cut these further for irregular areas.

## Raster or vector on the phone

Theming is not a factor (decided 2026-10-17), so the choice rests on these:

- **Updating without a PC.** No free source allows bulk raster download, so raster tiles would have to be rendered by our own host job (from the same vector data) and published somewhere the phone can fetch. Vector tiles can be cut straight from the Protomaps build by the phone itself whenever Wi-Fi is up.
- **Pixel density.** The panel is about 400 ppi. Standard 256 px raster tiles are unreadably small at that density, so raster would need 512 px or larger renders and still blurs between zoom levels. Vector geometry is drawn at the exact scale; z15 data overzooms cleanly to street level.
- **Track-up rotation.** Rotating a raster rotates its labels (upside down when heading south) and needs a full-screen image transform per heading change. Vector geometry rotates for free and labels are drawn upright.
- **Storage.** Raster at 512 px would be several GB for a state (estimate: 332,658 tiles at z0 to z15 for Karnataka, at an assumed 10 to 30 KB each, is 3 to 10 GB) against 393 MB of vector tiles.
- **Device work.** Raster is the simpler client (decode an image and blit it). Vector needs a renderer: that is the cost of the recommendation.

**Recommendation: vector tiles, rendered on the phone.**

## On-device renderer

Nothing off the shelf fits: MapLibre Native, Tangram and Organic Maps need OpenGL; libosmscout (LGPL, C++, Cairo or AGG backends) has its own database format built on a host and would bring a large C++ dependency. The pieces of a small C renderer are each simple:

- **PMTiles reader**: 127-byte header, gzip-compressed varint directories, one `pread` per tile. The [spec](https://github.com/protomaps/PMTiles/blob/main/spec/v3/spec.md) is short; a C reader is a few hundred lines. Needs zlib.
- **MVT decoder**: a protobuf with three message types, with geometry as MoveTo/LineTo/ClosePath commands on a 4096 extent. Hand-written in C; no protobuf library needed (vtzero is the C++ reference).
- **Drawing**: roads and paths as stroked polylines, water, parks and land use as filled polygons with holes, the ridden track and later the route on top. LVGL 9.6 has ThorVG-backed vector drawing (`LV_USE_VECTOR_GRAPHIC`, `LV_USE_THORVG_INTERNAL`), which does stroked and filled paths with even-odd holes on the CPU. Neither is enabled in `tools/chefui/lv_conf.h` today. The alternative is LVGL's line and triangle drawing with our own polygon triangulation.
- **Labels**: place names upright at points first. Road names along lines are a later step.
- **Elevation**: decode the z11 Terrarium WebP tiles (`(R*256 + G + B/256) - 32768` metres) with libwebp's lossless decoder, sample along the track, smooth, and sum climb. Alternatively convert the DEM to a raw int16 grid on the host so the phone needs no WebP decoder.

Render only when the view changes: the position moves several metres, the heading changes a few degrees, or the zoom changes. Cache decoded tiles in RAM. The first prototype should measure frame time for the densest Bengaluru z15 view on the phone before committing to ThorVG or our own rasteriser.

## Refresh when the network is available

A `chef-maps` job, started by the system UI or a timer, when all of these hold:

1. NetworkManager reports Wi-Fi connected (not a phone hotspot, if we can tell).
2. The clock is valid (chrony synced), since TLS needs it.
3. No ride is being recorded and the map is not open (no `ride` or `map` lease).
4. The battery is above a threshold or charging, and free space is at least twice the expected archive size.

Then:

1. Find the newest build from `https://build-metadata.protomaps.dev/builds.json` (builds older than a week are deleted, so never hard-code a date). Read its header and skip the update if the local file already has that build's `planetiler:osm:osmosisreplicationtime`.
2. `pmtiles extract <build> /data/v1/maps/.staging/basemap.pmtiles --region=<region.geojson> --maxzoom=15`. It fetches only the needed byte ranges: 133 requests, about 413 MB, for Karnataka.
3. `pmtiles verify`, `fsync`, then `rename` over `/data/v1/maps/basemap.pmtiles`, keeping the previous file as `basemap.pmtiles.1` until the new one has been opened once. A reader that already has the old file open keeps its inode, so a swap never breaks an open map.
4. The DEM follows the same steps from Mapterhorn but rarely changes: refresh it only when the region changes or once a year.

An interrupted extract is discarded and retried next time; `pmtiles extract` does not resume. The official arm64 `pmtiles` binary is a 54 MB static Go executable (v1.31.2). It works as-is from `/data` for a first version. A small C extractor sharing the renderer's PMTiles reader would replace it later.

Protomaps discourages hotlinking its builds for serving live maps, but documents `pmtiles extract` from them as the way to get a region. One device cutting a region once a month is light use. If that changes, a monthly host or Cloudflare R2 job can mirror the region and the phone fetches it whole from there.

Attribution "© OpenStreetMap" and "© Mapterhorn" (with the Copernicus credit from its attribution list) must be shown, for example on the About page and in a corner of the Map page.

## Downloadable regions

Decided 2026-10-17: the rider downloads **regions** (like Garmin's and Wahoo's map managers), always at **full street detail (z15)**, starting with **Goa**. The prototype is deferred; this section records the design research.

### Measured region sizes

Same builds as above, cut with `--region` polygons instead of bboxes:

| Region | Map z15 | Transfer, requests | DEM z11 |
|---|---|---|---|
| **Goa** (OSM relation 11251493, simplified) | **13 MB** | 14 MB, 57 | **4.3 MB** |
| India north-eastern zone (Geofabrik) | 219 MB | 230 MB, 449 | |
| India eastern zone | 452 MB | 474 MB, 493 | |
| India western zone (includes Goa) | 457 MB | 479 MB, 222 | |
| India northern zone | 537 MB | 564 MB, 377 | |
| India central zone | 716 MB | 752 MB, 519 | |
| India southern zone | 882 MB | 926 MB, 109 | |

All of India at z15 is about 3.3 GB. The phone measured 60 to 70 Mbit/s HTTPS download on 5 GHz Wi-Fi ([speed test](../../logs/wifi-cloudflare-speedtest-2026-10-02.txt)), so Goa takes a few seconds and the largest zone about two minutes plus request latency.

### Region catalog

The phone needs a list of named regions with polygons and expected sizes.

- **Decided 2026-10-18: Indian states only** (states and union territories). Geofabrik's world index ([`index-v1.json`](https://download.geofabrik.de/index-v1.json), 554 regions with polygons) is not used.
- **India**: Geofabrik stops at six multi-state zones. States and union territories come from OSM `boundary=administrative` + `admin_level=4` relations (Goa is relation 11251493), simplified to about 0.002° (Goa's polygon is a few hundred points).
- Build the catalog on the host (a script over Overpass or a Geofabrik India extract with osmium), store it as one small GeoJSON with id, name, parent, polygon and measured size, and ship it in the image or fetch it with the maps. Regenerate it rarely; boundaries hardly change.
- Grow each state polygon by a margin so a ride that crosses a state line does not run off the map at the border: **10 km by default** (adjustable). Border tiles are then in both neighbouring regions, which is harmless.
- Adjacent regions overlap at their edges. One file per region keeps download, update and delete independent.

### Getting a region onto the phone

Two ways, both producing `/data/v1/maps/<region>.pmtiles` (+ `<region>.dem.pmtiles`):

| | A. Phone cuts the region itself | B. Our own mirror |
|---|---|---|
| How | Phone runs the extract against the newest Protomaps and Mapterhorn builds. | A monthly job (GitHub Actions or the PC) cuts every catalog region and uploads the files to Cloudflare R2 with a manifest of sizes and SHA-256 sums; the phone downloads whole files. |
| Infrastructure | None. | A scheduled job plus an R2 bucket. India's 3.3 GB of maps plus DEMs fits R2's 10 GB free storage, and R2 has no egress fees. |
| Interrupted download | `pmtiles extract` cannot resume. Our own C extractor could keep a progress journal and refetch only the missing ranges. | `curl -C -` resumes a plain file. Hotel and public Wi-Fi failures are the main complaint about commercial units, so this matters. |
| Phone code | The PMTiles reader plus an extractor: Hilbert tile IDs, tile cover of the polygon, range coalescing, and writing new directories with the root kept under 16 KiB. Or the 54 MB Go binary for a start. | curl and a SHA-256 check. |
| Freshness | Any day's build, per region. | As fresh as the job's schedule. |

**Decided 2026-10-18: A, the phone cuts its own region**, long term. Start with the Go binary from `/data` for Goa (14 MB, 57 requests: a failed download simply restarts). Before large regions or travel downloads matter, replace it with our own resumable C extractor that shares the renderer's PMTiles reader. B stays a fallback if Protomaps access ever becomes a problem; it only changes the downloader, not the file layout or renderer.

### Updating and deleting

- **Decided 2026-10-18: refresh every time Wi-Fi connects.** On each NetworkManager Wi-Fi connect, the phone checks the newest Protomaps build (one small `builds.json` request plus a header read) and refreshes every installed region whose file is from an older build. Builds are daily, so a region downloads at most once a day; reconnecting on the same day costs only the check. The other conditions still apply (valid clock, no ride, no open map, battery or charger, free space for a second copy), and the old file is kept until the new one is verified and swapped in. If a condition fails, the check runs again at the next connect or when the condition clears (for example the ride ends). PMTiles has no deltas, so each refresh downloads the whole region: Goa 14 MB a day is nothing; a large state (400 to 900 MB) daily is heavy but was accepted. The DEM is not refreshed this way (terrain does not change); it is fetched once per region.
- Neighbouring regions from different build dates may differ slightly at their shared border tiles. The renderer prefers the newest file that has a tile.
- Delete removes both files. The Storage page (P05) already counts `/data/v1/maps`.

### Reading several regions

The renderer opens every installed region archive, keeps each root directory (at most 16 KiB with the header) and recently used leaf directories in RAM, and looks a tile up in each archive, newest build first, until one has it. Format notes for the C reader, from the [PMTiles v3 spec](https://github.com/protomaps/PMTiles/blob/main/spec/v3/spec.md):

- 127-byte little-endian header: magic `PMTiles`, version 3, offsets and lengths of the root directory, metadata, leaf directories and tile data, tile counts, the clustered flag, compression and tile type enums, zoom range, bounds and center.
- Directories are varint columns (entry count, delta tile IDs, run lengths, lengths, offsets stored +1 with 0 meaning "contiguous with the previous entry"), compressed as a whole. Run length 0 marks a leaf directory pointer; use one level of leaves at most.
- Tile IDs are cumulative Hilbert curve positions from z0. Test vectors are in the spec (z12 (3423, 1763) → 19078479).
- Protomaps uses gzip for directories and tiles (zlib needed); Mapterhorn tiles are uncompressed WebP.

### Maps page

A Settings page to browse the catalog (India states first), show each region's size, age and status, download over Wi-Fi with progress, update and delete. It needs a design round in the Claude Design project, like the other system pages.

## Road surface coverage

Checked 2026-10-18 whether OSM's `surface` tag is common enough in India to justify our own tile build (Protomaps drops it). Length-weighted counts with pyosmium over the openstreetmap.fr Goa extract (7.5 MB, 2026-10-17) and Geofabrik's India western zone (221 MB, 2026-10-16), plus a live Overpass count for Goa:

| Road class | Goa km | Goa tagged | Western zone km | Western zone tagged |
|---|---|---|---|---|
| Main (motorway to tertiary) | 2,891 | 25.1 % | 243,310 | 8.6 % |
| Unclassified | 1,438 | 19.0 % | 128,111 | 4.8 % |
| Residential | 3,772 | 5.3 % | 167,024 | 3.1 % |
| Service | 723 | 9.3 % | 31,975 | 5.3 % |
| Track | 1,831 | 7.4 % | 53,754 | 69.5 % |
| Path, footway, cycleway | 579 | 13.4 % | 11,354 | 6.0 % |
| All | 11,234 | 13.2 % | 635,529 | 11.3 % |

Overpass (live, 2026-10-17 18:48 UTC) agrees for Goa: 3,029 of 36,998 highway ways (8.2 %) have a surface tag. The western zone's high track figure is almost entirely `surface=unpaved` on `highway=track` (99.8 % of tagged track length is unpaved), which the road class already says; without tracks, coverage is about 6 %. Where main roads are tagged they are 97 % paved.

**Conclusion: not worth our own tile build.** Surface is missing on roughly 9 of 10 kilometres. The road class Protomaps keeps (`track`, `path` versus the road classes) already separates most unpaved from paved riding. **Protomaps as-is is final.** Revisit only if Indian tagging improves a lot.

## Open decisions

None for the data and download design. The prototype (on-phone render time for Goa) waits for the go-ahead.
