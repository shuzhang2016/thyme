#include "thyme/replay/replay_playlist.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include "thyme/record/triggered_recorder.h"

namespace thyme {
namespace replay {
namespace {

bool IsDirectory(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool IsPackage(const std::string& path) {
  struct stat st {};
  return stat((path + "/meta.tbin").c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

uint64_t ReadSliceSeq(const std::string& dir, uint64_t fallback) {
  std::unique_ptr<std::FILE, decltype(&std::fclose)> file(
      std::fopen((dir + "/wal.log").c_str(), "rb"), &std::fclose);
  if (file == nullptr) {
    return fallback;
  }
  triggered::WalHeader header{};
  const std::size_t read = std::fread(&header, sizeof(header), 1, file.get());
  if (read != 1 ||
      std::memcmp(header.magic, triggered::kWalMagic,
                  sizeof(triggered::kWalMagic)) != 0 ||
      (header.trigger_id & triggered::kContinuousWalMark) !=
          triggered::kContinuousWalMark) {
    return fallback;
  }
  return header.trigger_id & ~triggered::kContinuousWalMark;
}

}  // namespace

ReplayPlaylist::ReplayPlaylist() = default;

ReplayPlaylist::~ReplayPlaylist() { Close(); }

ReplayStatus ReplayPlaylist::Discover(const std::string& root,
                                      std::vector<std::string>* dirs) {
  if (!IsDirectory(root)) {
    error_ = "playlist root is not a directory: " + root;
    return ReplayStatus::kNotADirectory;
  }
  if (IsPackage(root)) {
    dirs->push_back(root);
    return ReplayStatus::kOk;
  }
  DIR* directory = opendir(root.c_str());
  if (directory == nullptr) {
    error_ = "cannot scan playlist root: " + root;
    return ReplayStatus::kNotADirectory;
  }
  while (dirent* entry = readdir(directory)) {
    if (std::strncmp(entry->d_name, "continuous_", 11) != 0) {
      continue;
    }
    const std::string dir = root + "/" + entry->d_name;
    if (IsDirectory(dir) && IsPackage(dir)) {
      dirs->push_back(dir);
    }
  }
  closedir(directory);
  if (dirs->empty()) {
    error_ = "no continuous_* packages under " + root;
    return ReplayStatus::kMissingMeta;
  }
  return ReplayStatus::kOk;
}

ReplayStatus ReplayPlaylist::Open(const std::string& root,
                                  const ReplayPlayerConfig& cfg) {
  Close();
  std::vector<std::string> dirs;
  const ReplayStatus st = Discover(root, &dirs);
  if (st != ReplayStatus::kOk) {
    return st;
  }
  return Open(dirs, cfg);
}

ReplayStatus ReplayPlaylist::Open(const std::vector<std::string>& dirs,
                                  const ReplayPlayerConfig& cfg) {
  Close();
  if (dirs.empty()) {
    error_ = "playlist is empty";
    return ReplayStatus::kInvalidArgument;
  }
  std::vector<ReplayPlaylistSlice> ordered;
  ordered.reserve(dirs.size());
  for (const std::string& dir : dirs) {
    ReplayPackage pkg;
    const ReplayStatus st = pkg.Open(dir);
    if (st != ReplayStatus::kOk) {
      error_ = "playlist preflight failed for " + dir + ": " + pkg.error();
      return st;
    }
    ReplayPlaylistSlice slice;
    slice.dir = dir;
    slice.first_mono_ns = pkg.first_mono_ns();
    slice.last_mono_ns = pkg.last_mono_ns();
    slice.slice_seq = ReadSliceSeq(dir, slice.first_mono_ns);
    ordered.push_back(std::move(slice));
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const ReplayPlaylistSlice& a, const ReplayPlaylistSlice& b) {
              if (a.slice_seq != b.slice_seq) {
                return a.slice_seq < b.slice_seq;
              }
              if (a.first_mono_ns != b.first_mono_ns) {
                return a.first_mono_ns < b.first_mono_ns;
              }
              return a.dir < b.dir;
            });

  ReplayPlayerConfig play_cfg = cfg;
  play_cfg.skip_overlaps = !cfg.stitch_timeline;
  if (cfg.stitch_timeline) {
    ReplayStatus plan_status = BuildReplayStitchPlan(&ordered, &error_);
    if (plan_status != ReplayStatus::kOk) {
      return plan_status;
    }
  } else {
    for (ReplayPlaylistSlice&slice : ordered) {
      slice.timeline_first_ns = slice.first_mono_ns;
      slice.timeline_last_ns = slice.last_mono_ns;
      slice.has_events = true;
    }
  }
  ReplayPlayer validator;
  ReplayPlayerConfig validate_cfg;
  validate_cfg.skip_overlaps = play_cfg.skip_overlaps;
  validate_cfg.pinned_slots.clear();
  validate_cfg.small_structs.clear();
  ReplayStatus st = validator.Open(ordered.front().dir, validate_cfg);
  for (std::size_t i = 1; st == ReplayStatus::kOk && i < ordered.size(); ++i) {
    st = validator.OpenNextPackage(ordered[i].dir, false);
  }
  const std::string validate_error = validator.error();
  validator.Close();
  if (st != ReplayStatus::kOk) {
    error_ = "playlist contract preflight failed: " + validate_error;
    return st;
  }
  st = player_.Open(ordered.front().dir, play_cfg);
  if (st != ReplayStatus::kOk) {
    error_ = "playlist player open failed: " + player_.error();
    return st;
  }
  slices_ = std::move(ordered);
  current_ = 0;
  open_ = true;
  error_.clear();
  return ReplayStatus::kOk;
}

void ReplayPlaylist::Close() {
  player_.Close();
  slices_.clear();
  current_ = 0;
  open_ = false;
  error_.clear();
}

ReplayStatus ReplayPlaylist::Pump() {
  if (!open_) {
    return ReplayStatus::kInvalidArgument;
  }
  for (;;) {
    const ReplayStatus st = player_.Pump();
    if (st != ReplayStatus::kEnd) {
      if (st != ReplayStatus::kOk && st != ReplayStatus::kStopped) {
        error_ = player_.error();
      }
      return st;
    }
    if (current_ + 1 >= slices_.size()) {
      return ReplayStatus::kEnd;
    }
    const ReplayPlaylistSlice&next_slice = slices_[current_ + 1];
    const ReplayStatus next =
        player_.OpenNextPackage(next_slice.dir, true, next_slice.offset_ns);
    if (next != ReplayStatus::kOk) {
      error_ = player_.error();
      return next;
    }
    current_ += 1;
  }
}

ReplayStatus ReplayPlaylist::Run() {
  for (;;) {
    const ReplayStatus st = Pump();
    if (st == ReplayStatus::kOk) {
      continue;
    }
    if (st == ReplayStatus::kEnd) {
      return ReplayStatus::kOk;
    }
    return st;
  }
}

ReplayStatus ReplayPlaylist::Seek(uint64_t mono_ns) {
  if (!open_) {
    return ReplayStatus::kInvalidArgument;
  }
  std::size_t target = slices_.size() - 1;
  for (std::size_t i = 0; i < slices_.size(); ++i) {
    if (mono_ns <= slices_[i].timeline_last_ns) {
      target = i;
      break;
    }
  }
  if (target != current_) {
    const ReplayStatus st = player_.OpenNextPackage(slices_[target].dir, false,
                                                    slices_[target].offset_ns);
    if (st != ReplayStatus::kOk) {
      error_ = player_.error();
      return st;
    }
    current_ = target;
  }
  const ReplayStatus st = player_.Seek(mono_ns);
  if (st != ReplayStatus::kOk) {
    error_ = player_.error();
  }
  return st;
}

void ReplayPlaylist::Stop() { player_.Stop(); }

}  // namespace replay
}  // namespace thyme
