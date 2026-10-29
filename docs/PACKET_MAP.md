# Packet details and OpenStreetMap

The right panel now contains selected-packet information and an interactive
OpenStreetMap map. The polar direction plot and its calibration buttons have
been removed from this panel; existing DF command-line tools remain available.
A map marker is a position reported in a decoded packet, not a direction-finding
measurement or an independently verified physical position.

## Operation

1. Connect to a live source or replay a recording.
2. Click any row in **Decoded packets**, including an unsupported-payload row.
3. The details panel shows its source, serial/type when parsed, sequence, receipt
   timestamp, checksum status, coordinates, altitude/height and raw payload hex.
   Scroll inside the details panel for the full payload and **Clear selection**.
   **Copy payload hex** copies the received payload to the clipboard.
4. A valid drone coordinate centers the map on the drone. Drone markers are red,
   pilot/operator markers blue, and home markers green, with distinct labels.
   **Center drone**, **Center pilot/operator** and **Center home** appear only for
   available coordinates. Click a different packet to inspect that packet's data.
5. Without a selection, or when a selected packet has no valid drone coordinate,
   the default is Punjab, India: **31.1471 N, 75.3412 E**, zoom 7. This is a view
   center, not an inferred drone location. Invalid, nonfinite, out-of-range,
   unparsed and zero/zero sentinel coordinates never create a drone marker.
6. Drag the map to pan; use the mouse wheel or **+ / -** to zoom. **Punjab** resets
   the view. **Clear selection** clears the selected packet and returns to Punjab.

Selection retains a copy of the exact clicked packet. Incoming packets do not
move it to a different row or silently replace its coordinates. If that packet
leaves the bounded live log, the panel labels it as a retained selection. Packet
numbers are unique within the application session, even if sequence numbers and
receipt timestamps repeat.

## Network and cache

This is a native C++/OpenGL slippy map, with GPL source in `src/ui/map_view.cpp`
and public OpenStreetMap raster tiles. No browser engine or API key is needed.
Only tiles visible in the current viewport are requested, through one background
worker. There is no region-download, prefetch or tile redistribution feature.
The tile loader has bounded response sizes, image dimensions and GPU memory;
HTTP failures do not block packet decoding. Missing tiles show a placeholder.

**Online tiles** permits HTTPS requests to `tile.openstreetmap.org`. Requests
identify PunjDrone and reveal the viewed tile area to that service; packet
payloads, identities and telemetry records are not uploaded. Turn it off to use
only previously cached tiles. `DRONEID_MAP_OFFLINE=1` starts in that mode, useful
for isolated environments and automated tests. An uncached area needs internet
access; no bundled world or Punjab tile archive is claimed.

Cache locations:

- Windows: `%LOCALAPPDATA%\punjdrone\osm-tiles`.
- Linux: `$XDG_CACHE_HOME/punjdrone/osm-tiles`, or `~/.cache/punjdrone/osm-tiles`.

Successful tiles are cached for at least seven days. Expired tiles use a
conditional request; failed requests retain available cached tiles. Failed tiles
have a retry delay. The map remains usable while receiving, and renderer cleanup
releases GPU textures before the OpenGL context is destroyed.

Visible attribution links to [OpenStreetMap copyright](https://www.openstreetmap.org/copyright).
The implementation follows the [OSMF raster tile usage policy](https://operations.osmfoundation.org/policies/tiles/)
consulted on 29 October 2026. The public service has no availability guarantee.
For larger deployments or a requirement for complete offline maps, use an
appropriate tile service/self-hosting implementation; this release uses OSM's
public endpoint.

## Building and verification

Linux GUI builds additionally need libcurl development headers/library (for
example the distribution's `libcurl4-openssl-dev` package). Windows uses the
system WinHTTP DLL, with no extra downloader runtime. The PNG decoder is vendored
stb_image under its MIT option; its license accompanies both distributions.

Headless tests cover packet identity/selection across log eviction, invalid and
pilot-only positions, projection round trips, polar clamping and world wrapping.
The release evidence in `data/analysis/final_20261029/` includes visible native
and Windows/Wine mouse-click checks with real saved IQ, screenshots, a real X310
sweep, radio load tests, packet corpus parity and O4 regression results. See that
folder's report for passed checks and remaining acceptance gates.
