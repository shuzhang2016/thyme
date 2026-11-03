#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "thyme/replay/replay_player.h"
#include "thyme/replay/replay_timeline.h"

namespace thyme {
namespace replay {

using ReplayPlaylistSlice = ReplayTimelineSlice;

// Library-level absolute or stitched replay over an ordered set of packages.
// There is one ReplayPlayer and therefore one set of injectors/producer gates
// for the whole playlist; package transitions replace only its ReplaySession.
class ReplayPlaylist {
 public:
  ReplayPlaylist();
  ~ReplayPlaylist();
  ReplayPlaylist(const ReplayPlaylist&) = delete;
  ReplayPlaylist& operator=(const ReplayPlaylist&) = delete;

  // With no meta.tbin in root, discovers its immediate continuous_* children.
  ReplayStatus Open(const std::string& root, const ReplayPlayerConfig& cfg);
  // Explicit packages are validated and sorted by the same slice ordering rule.
  ReplayStatus Open(const std::vector<std::string>& dirs,
                    const ReplayPlayerConfig& cfg);
  void Close();
  bool is_open() const { return open_; }

  ReplayStatus Pump();
  ReplayStatus Run();
  ReplayStatus Seek(uint64_t mono_ns);
  void Stop();

  const std::vector<ReplayPlaylistSlice>& slices() const { return slices_; }
  std::size_t current_slice() const { return current_; }
  const PlayerStats& stats() const { return player_.stats(); }
  const std::string& error() const { return error_; }
  ReplayPlayer& player() { return player_; }
  const ReplayPlayer& player() const { return player_; }

 private:
  ReplayStatus Discover(const std::string& root,
                        std::vector<std::string>* dirs);

  ReplayPlayer player_;
  std::vector<ReplayPlaylistSlice> slices_;
  std::size_t current_ = 0;
  bool open_ = false;
  std::string error_;
};

}  // namespace replay
}  // namespace thyme
