// Native map geometry and packet selection, independent of rendering/network access.
// GPL-3.0-or-later.
#pragma once
#include "ui_model.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace droneid::ui {
constexpr double kPunjabLat = 31.1471, kPunjabLon = 75.3412;
constexpr double kMercatorLimit = 85.05112878;
inline bool valid_location(double lat, double lon) {
  return std::isfinite(lat) && std::isfinite(lon) && lat >= -90 && lat <= 90 &&
         lon >= -180 && lon <= 180 && !(lat == 0 && lon == 0);
}
inline bool packet_drone_location(const PacketRow& p) {
  return p.crc_ok && p.telemetry_parsed && p.drone_valid && valid_location(p.drone_lat,p.drone_lon);
}
struct MapPoint { double x, y; };
inline MapPoint project_location(double lat, double lon, int zoom) {
  const double scale = 256.0 * (1 << std::clamp(zoom,0,19));
  const double rad = std::clamp(lat,-kMercatorLimit,kMercatorLimit) * 3.14159265358979323846 / 180;
  return {(lon+180)/360*scale, (1-std::asinh(std::tan(rad))/3.14159265358979323846)*.5*scale};
}
inline MapPoint unproject_location(double x, double y, int zoom) {
  const double scale = 256.0 * (1 << std::clamp(zoom,0,19));
  x = std::fmod(std::fmod(x,scale)+scale,scale);
  y = std::clamp(y,0.0,scale);
  return {std::atan(std::sinh(3.14159265358979323846*(1-2*y/scale)))*180/3.14159265358979323846,
          x/scale*360-180}; // latitude, longitude
}
struct PacketSelection {
  std::optional<PacketRow> packet; // copy remains valid as new packets arrive / old rows expire
  double lat = kPunjabLat, lon = kPunjabLon;
  int zoom = 7;
  void reset() { packet.reset(); lat=kPunjabLat; lon=kPunjabLon; zoom=7; }
  void select(const PacketRow& p) {
    packet=p;
    if (packet_drone_location(p)) { lat=p.drone_lat; lon=p.drone_lon; zoom=14; }
    else { lat=kPunjabLat; lon=kPunjabLon; zoom=7; }
  }
};
}
