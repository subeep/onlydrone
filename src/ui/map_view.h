// GPL-3.0-or-later.
#pragma once
#include "map_state.h"
#include <memory>
namespace droneid::ui {
class MapView {
 public:
  MapView();
  ~MapView();
  MapView(const MapView&) = delete;
  MapView& operator=(const MapView&) = delete;
  void draw(PacketSelection& selection);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
