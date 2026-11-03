// test_trigger_config.cpp — config-key effectiveness and
// per-source channel-filter configuration.
//
// Every accepted key must either take effect or be a parse error; the matrix
// below pins each rejected key by name, the flush default/override semantics,
// slice_mb boundaries, wait_idle preconditions, the auto_arm rename (old
// `enable` still works with a deprecation warning), channel-selector
// validation, and the --dump-config effective-value round trip.
#include "thyme/check.h"
#include "thyme/record/trigger_config.h"

#include <cstdio>
#include <string>

using namespace thyme;

namespace {

const char* kMinimal = R"(
# minimal deployable template (also used by --check-config in CI)
[trigger_center]
socket = /tmp/thyme_trigger.sock
max_concurrent_captures = 1
quota_state = /tmp/thyme_min.quota
default_source = manual
crash_priority = 100

[logging]
service_file = /tmp/thyme_service.log
service_max_bytes = 1048576
service_rotate_keep = 3

[recorder]
output_dir = /tmp/thyme_min
pending_bytes_quota = 67108864
flush_policy = idle
disk_quota_mb = 1000

[channel./h264_front]
kind = h264_stream
name = h264_front
bitrate_kbps = 4000

[channel./thyme_recr_100]
kind = mirror_ring

[source.manual]
channels = /h264_*, /thyme_recr_*
before_s = 2.5
after_s = 0.5
priority = 10
mute_s = 30
flush_policy = idle
quota_class = manual

[quota.manual]
max_triggers_per_hour = 30

[continuous]
auto_arm = true
channels = small_struct:*, pinned_slot:h264_front
slice_mb = 512
slice_s = 0
wait_idle = false
rate_check = warn
)";

bool Parse(const std::string& text, trigger_cfg::CenterConfig* cfg,
           std::string* err, std::string* warn = nullptr) {
  return trigger_cfg::ParseCenterConfig(text, cfg, err, warn);
}

// Each entry must fail and name its key in the error.
void TestRejectedKeys() {
  struct Case {
    const char* name;
    const char* text;
  };
  const Case cases[] = {
      {"use_cgroup",
       "[recorder]\nuse_cgroup = auto\n"},
      {"gop_s",
       "[channel./cam]\nkind = pinned_slot\ngop_s = 2\n"},
      {"max_concurrent_captures",
       "[trigger_center]\nmax_concurrent_captures = 2\n"},
      {"tick_ms", "[trigger_center]\ntick_ms = 0\n"},
      {"socket_poll_budget_us",
       "[trigger_center]\nsocket_poll_budget_us = 0\n"},
      {"max_trigger_rate_hz",
       "[trigger_center]\nmax_trigger_rate_hz = 0\n"},
      {"dedup_window_entries",
       "[trigger_center]\ndedup_window_entries = 0\n"},
      {"dedup_window_ms", "[trigger_center]\ndedup_window_ms = 0\n"},
      {"no_such_center_key",
       "[trigger_center]\nno_such_center_key = 1\n"},
      {"no_such_recorder_key", "[recorder]\nno_such_recorder_key = 1\n"},
      {"no_such_channel_key",
       "[channel./cam]\nno_such_channel_key = 1\n"},
      {"no_such_source_key",
       "[source.s]\nno_such_source_key = 1\n"},
      {"no_such_quota_key",
       "[quota.q]\nno_such_quota_key = 1\n"},
      {"no_such_continuous_key",
       "[continuous]\nno_such_continuous_key = 1\n"},
      {"no_such_chassis_key",
       "[chassis]\nno_such_chassis_key = 1\n"},
  };
  for (const Case& c : cases) {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(!Parse(c.text, &cfg, &err));
    CHECK(!err.empty());
    CHECK(err.find(c.name) != std::string::npos);
    std::fprintf(stderr, "  rejected %-24s -> %s\n", c.name, err.c_str());
  }

  // >1 concurrent captures is rejected instead of silently degraded.
  trigger_cfg::CenterConfig cfg;
  std::string err;
  CHECK(!Parse("[trigger_center]\nmax_concurrent_captures = 2\n", &cfg, &err));
  CHECK(err.find("only 1") != std::string::npos);

  CHECK(!Parse("[trigger_center]\npending_memory_mb = 64\n", &cfg, &err));
  CHECK(err.find("pending_memory_mb") != std::string::npos);
  CHECK(err.find("[recorder].pending_bytes_quota") != std::string::npos);

  CHECK(Parse("[trigger_center]\nallow_empty_sources = true\n"
              "[channel./video]\nkind = h264_stream\nname = video_front\n"
              "gop_s = 2\n",
              &cfg, &err));
  CHECK(cfg.recorder_sources.size() == 1);
  CHECK(cfg.recorder_sources[0].gop_ns == 2000000000ULL);
  CHECK(!Parse("[channel./video]\nkind = h264_stream\nname = video_front\n"
               "gop_s = 61\n",
               &cfg, &err));
  CHECK(err.find("gop_s") != std::string::npos);
}

void TestFlushDefault() {
  // [recorder].flush_policy is the default; an explicit idle survives it.
  {
    const std::string text =
        "[recorder]\nflush_policy = realtime\n"
        "[source.explicit_idle]\nflush_policy = idle\nquota_class = q\n"
        "[source.inherits]\nquota_class = q\n";
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(Parse(text, &cfg, &err));
    CHECK(cfg.recorder_flush_realtime);
    CHECK(cfg.sources[0].has_flush);
    CHECK(!cfg.sources[0].flush_realtime);  // explicit idle not overwritten
    CHECK(!cfg.sources[1].has_flush);
    CHECK(cfg.sources[1].flush_realtime);  // inherits realtime
  }
  // Explicit realtime with an idle recorder default works too.
  {
    const std::string text =
        "[recorder]\nflush_policy = idle\n"
        "[source.s]\nflush_policy = realtime\nquota_class = q\n";
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(Parse(text, &cfg, &err));
    CHECK(cfg.sources[0].has_flush);
    CHECK(cfg.sources[0].flush_realtime);
  }
}

void TestSliceBoundaries() {
  auto parses = [](const std::string& text) {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    return Parse(text, &cfg, &err);
  };
  // 0 is always an error.
  CHECK(!parses("[continuous]\nauto_arm = true\nslice_mb = 0\n"));
  // > disk_quota_mb is an error.
  CHECK(!parses("[recorder]\ndisk_quota_mb = 100\n"
      "[continuous]\nauto_arm = true\nslice_mb = 101\n"));
  // == disk_quota_mb is allowed.
  CHECK(parses("[recorder]\ndisk_quota_mb = 100\n"
      "[continuous]\nauto_arm = true\nslice_mb = 100\n"));
  // disk_quota_mb = 0 means unlimited.
  CHECK(parses("[recorder]\ndisk_quota_mb = 0\n"
      "[continuous]\nauto_arm = true\nslice_mb = 4096\n"));
  // The default slice (512 MiB) no longer fits a smaller quota and is
  // rejected when [continuous] is present rather than silently ignored.
  CHECK(!parses("[recorder]\ndisk_quota_mb = 100\n"
      "[continuous]\nauto_arm = true\n"));
}

void TestIncompleteRateMetadata() {
  trigger_cfg::CenterConfig cfg;
  std::string err;
  CHECK(Parse("[channel./c9_%pid_rec]\nkind = mirror_ring\n"
              "record_channels = alpha,bravo\n"
              "[continuous]\nauto_arm = true\n",
              &cfg, &err));
  trigger_cfg::ContinuousPlanReport report;
  std::string warn;
  CHECK(!trigger_cfg::ValidateContinuousPlan(cfg, &report, &warn, &err));
  CHECK(err.find("metadata is incomplete") != std::string::npos);
  const std::string dump = trigger_cfg::DumpConfig(cfg);
  CHECK(dump.find("rate_hz=0") == std::string::npos);
  CHECK(dump.find("avg_frame_bytes=0") == std::string::npos);
  CHECK(dump.find("record_channels=alpha,bravo") != std::string::npos);
  trigger_cfg::CenterConfig reparsed;
  CHECK(Parse(dump, &reparsed, &err));
  CHECK(trigger_cfg::DumpConfig(reparsed) == dump);
}

void TestBindShmPid() {
  trigger_cfg::CenterConfig cfg;
  std::string err;
  CHECK(Parse(
      "[channel./c9_%pid_rec]\nkind = mirror_ring\n"
      "record_channels = alpha\n"
      "[chassis]\npage_shm = /c9_%pid_chassis\n"
      "[source.manual]\nchannels = /c9_%pid_*\nquota_class = manual\n"
      "[continuous]\nchannels = /c9_%pid_rec\n"
      "[recorder]\nsocket = /tmp/%pid.sock\n",
      &cfg, &err));
  CHECK(!trigger_cfg::BindShmPid(&cfg, 0, &err));
  CHECK(err.find("%pid") != std::string::npos);
  CHECK(cfg.recorder_sources[0].shm_name == "/c9_%pid_rec");
  CHECK(trigger_cfg::BindShmPid(&cfg, 4242, &err));
  CHECK(cfg.recorder_sources[0].shm_name == "/c9_4242_rec");
  CHECK(cfg.chassis.page_shm == "/c9_4242_chassis");
  CHECK(cfg.sources[0].channels[0] == "/c9_4242_*");
  CHECK(cfg.cont_channels[0] == "/c9_4242_rec");
  CHECK(cfg.recorder_socket == "/tmp/%pid.sock");
  trigger_cfg::CenterConfig plain;
  CHECK(Parse("[channel./demo]\nkind = mirror_ring\n"
              "[trigger_center]\nallow_empty_sources = true\n",
              &plain, &err));
  CHECK(trigger_cfg::BindShmPid(&plain, 0, &err));
  CHECK(plain.recorder_sources[0].shm_name == "/demo");
}

void TestWaitIdle() {
  trigger_cfg::CenterConfig cfg;
  std::string err;
  CHECK(
      !Parse("[continuous]\nauto_arm = true\nwait_idle = true\n", &cfg, &err));
  CHECK(err.find("demo_idle") != std::string::npos);
  CHECK(Parse(
      "[continuous]\nauto_arm = true\nwait_idle = true\ndemo_idle = true\n",
      &cfg, &err));
  CHECK(Parse("[chassis]\npage_shm = /can_chassis\n"
      "[continuous]\nauto_arm = true\nwait_idle = true\n",
      &cfg, &err));
  CHECK(cfg.chassis_present);
}

void TestAutoArmRename() {
  // Deprecated spelling still works and warns.
  {
    trigger_cfg::CenterConfig cfg;
    std::string err, warn;
    CHECK(Parse("[continuous]\nenable = true\n", &cfg, &err, &warn));
    CHECK(cfg.cont_auto_arm);
    CHECK(warn.find("deprecated") != std::string::npos);
    CHECK(warn.find("auto_arm") != std::string::npos);
    CHECK(warn.find("line 2") != std::string::npos);
  }
  // New name is warning-free.
  {
    trigger_cfg::CenterConfig cfg;
    std::string err, warn;
    CHECK(Parse("[continuous]\nauto_arm = true\n", &cfg, &err, &warn));
    CHECK(cfg.cont_auto_arm);
    CHECK(warn.empty());
  }
}

void TestChannelSelectors() {
  const std::string manifest =
      "[channel./cam_a]\nkind = pinned_slot\nname = cam_a\n"
      "rate_hz = 10\navg_frame_bytes = 128\n"
      "[channel./cam_b]\nkind = pinned_slot\nname = cam_b\n"
      "rate_hz = 10\navg_frame_bytes = 128\n"
      "[channel./mirror_1]\nkind = mirror_ring\n";
  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(Parse(manifest + "[source.s]\nchannels = cam_a, /mirror_*\n"
                    "quota_class = q\n",
                &cfg, &err));
    CHECK(cfg.sources[0].channels.size() == 2);
    CHECK(cfg.sources[0].channels[0] == "cam_a");
    CHECK(cfg.sources[0].channels[1] == "/mirror_*");
  }
  // '*' means "all configured channels".
  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(Parse(manifest + "[source.s]\nchannels = *\nquota_class = q\n", &cfg,
                &err));
  }
  // Unknown name is a config error (no silent record-everything).
  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(!Parse(manifest + "[source.s]\nchannels = cam_a, typo_channel\n"
                     "quota_class = q\n",
                 &cfg, &err));
    CHECK(err.find("typo_channel") != std::string::npos);
    CHECK(err.find("matches no [channel.*]") != std::string::npos);
  }
  // Selectors require a manifest.
  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(
        !Parse("[source.s]\nchannels = cam_a\nquota_class = q\n", &cfg, &err));
    CHECK(err.find("[channel.*]") != std::string::npos);
  }
  // Omitted key = all channels.
  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(Parse(manifest + "[source.s]\nquota_class = q\n", &cfg, &err));
    CHECK(cfg.sources[0].channels.empty());
  }
  // Both contexts accept typed globs, exclusions and decimation.
  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(Parse(manifest +
                    "[source.s]\nchannels = pinned_slot:cam_*, !pinned_slot:cam_b, "
                    "small_struct:/mirror_*@dec=2\nquota_class = q\n"
                    "[continuous]\nchannels = pinned_slot:cam_*@dec=2, "
                    "!pinned_slot:cam_b\n",
                &cfg, &err));
    trigger_cfg::ContinuousPlanReport report;
    std::string warn;
    CHECK(trigger_cfg::ValidateContinuousPlan(cfg, &report, &warn, &err));
    CHECK(report.sustained_bps == 640);
  }
  // Exclusion priority applies in source too; an empty result is rejected.
  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(!Parse(manifest +
                     "[source.s]\nchannels = pinned_slot:cam_a, !pinned_slot:cam_*\n"
                     "quota_class = q\n",
                 &cfg, &err));
    CHECK(err.find("select no") != std::string::npos);
  }
  // A typed glob that decimates h264 is rejected, not just an exact name.
  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(!Parse("[channel./video]\nkind = h264_stream\nname = video_front\n"
                 "[continuous]\nchannels = pinned_slot:video_*@dec=2\n",
                 &cfg, &err));
    CHECK(err.find("GOP") != std::string::npos);
  }
  // Invalid suffixes are errors in source as well as continuous.
  for (const char *suffix : {"@dec=0", "@dec=2x", "@rate=2"}) {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(!Parse(manifest + "[source.s]\nchannels = cam_a" + suffix +
                     "\nquota_class = q\n",
                 &cfg, &err));
    CHECK(err.find("dec") != std::string::npos);
  }
}

void TestDumpConfig() {
  trigger_cfg::CenterConfig cfg;
  std::string err;
  CHECK(Parse(kMinimal, &cfg, &err));

  const std::string dump = trigger_cfg::DumpConfig(cfg);
  // Effective values, including defaults, are materialised.
  CHECK(dump.find("[trigger_center]") != std::string::npos);
  CHECK(dump.find("allow_empty_sources=false") != std::string::npos);
  CHECK(dump.find("crash_priority=100") != std::string::npos);
  CHECK(dump.find("pending_memory_mb") == std::string::npos);
  CHECK(dump.find("pending_bytes_quota=67108864") != std::string::npos);
  CHECK(dump.find("idle_hold_s=30") != std::string::npos);
  CHECK(dump.find("idle_fallback=always_busy") != std::string::npos);
  CHECK(dump.find("io_rate_limit_mbps=20") != std::string::npos);
  CHECK(dump.find("disk_quota_mb=1000") != std::string::npos);
  CHECK(dump.find("incomplete_policy=keep") != std::string::npos);
  CHECK(dump.find("[channel./h264_front]") != std::string::npos);
  CHECK(dump.find("kind=h264_stream") != std::string::npos);
  CHECK(dump.find("[channel./thyme_recr_100]") != std::string::npos);
  CHECK(dump.find("kind=mirror_ring") != std::string::npos);
  CHECK(dump.find("channels=/h264_*,/thyme_recr_*") != std::string::npos);
  CHECK(dump.find("auto_arm=true") != std::string::npos);
  CHECK(dump.find("rate_check=warn") != std::string::npos);
  CHECK(dump.find("slice_mb=512") != std::string::npos);
  CHECK(dump.find("use_cgroup") == std::string::npos);
  CHECK(dump.find("gop_s=0") != std::string::npos);

  // Round trip: the dump parses back to the same effective values, and a
  // second dump is byte-identical.
  trigger_cfg::CenterConfig again;
  std::string err2;
  CHECK(Parse(dump, &again, &err2));
  CHECK(again.socket_path == cfg.socket_path);
  CHECK(again.default_source == cfg.default_source);
  CHECK(again.recorder_pending_quota == cfg.recorder_pending_quota);
  CHECK(again.recorder_disk_quota_bytes == cfg.recorder_disk_quota_bytes);
  CHECK(again.recorder_sources.size() == cfg.recorder_sources.size());
  CHECK(again.sources.size() == cfg.sources.size());
  CHECK(again.sources[0].channels == cfg.sources[0].channels);
  CHECK(again.sources[0].flush_realtime == cfg.sources[0].flush_realtime);
  CHECK(again.cont_auto_arm == cfg.cont_auto_arm);
  CHECK(again.cont_section_present == cfg.cont_section_present);
  CHECK(again.cont_channels == cfg.cont_channels);
  CHECK(again.cont_slice_bytes == cfg.cont_slice_bytes);
  CHECK(again.cont_rate_check == cfg.cont_rate_check);
  CHECK(again.quota_state_path == cfg.quota_state_path);
  CHECK(again.log_service_file == "/tmp/thyme_service.log");
  CHECK(again.log_service_max_bytes == 1048576);
  CHECK(again.log_service_rotate_keep == 3);
  CHECK(trigger_cfg::DumpConfig(again) == dump);
  trigger_cfg::CenterConfig without_quota_state = cfg;
  without_quota_state.quota_state_path.clear();
  CHECK(trigger_cfg::ReloadRequiresRestart(cfg, without_quota_state));
  trigger_cfg::CenterConfig changed_service_log = cfg;
  changed_service_log.log_service_file.clear();
  CHECK(trigger_cfg::ReloadRequiresRestart(cfg, changed_service_log));

  // A trigger-only config (no [continuous]) round-trips without the section
  // being invented by the dump.
  {
    trigger_cfg::CenterConfig plain;
    std::string perr;
    CHECK(Parse("[trigger_center]\ndefault_source = s\n"
        "[recorder]\noutput_dir = /tmp/x\n"
        "[channel./cam]\nkind = pinned_slot\nname = cam\n"
        "[source.s]\nchannels = cam\nquota_class = q\n",
        &plain, &perr));
    CHECK(!plain.cont_section_present);
    const std::string d = trigger_cfg::DumpConfig(plain);
    CHECK(d.find("# channels=*") == std::string::npos);  // source has cam
    CHECK(d.find("channels=cam") != std::string::npos);
    CHECK(d.find("\n[continuous]") == std::string::npos);
    trigger_cfg::CenterConfig back;
    CHECK(Parse(d, &back, &perr));
    CHECK(!back.cont_section_present);
    CHECK(back.sources[0].channels == plain.sources[0].channels);
  }

  std::fprintf(stderr, "---- effective config dump ----\n%s", dump.c_str());
}

void TestManifestModes() {
  const std::string topology = "[trigger_center]\nallow_empty_sources = true\n"
      "[channel./cam]\nkind = pinned_slot\nname = cam\n"
      "[channel./mirror]\nkind = mirror_ring\n";
  const std::string bulk_only = "[trigger_center]\nallow_empty_sources = true\n"
      "[channel./cam]\nkind = pinned_slot\nname = cam\n";
  const std::string mirror_only =
      "[trigger_center]\nallow_empty_sources = true\n"
      "[channel./mirror]\nkind = mirror_ring\n";
  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    std::string warn;
    CHECK(Parse(topology, &cfg, &err, &warn));
    CHECK(cfg.manifest_mode == integrity::ManifestMode::kFull);
    CHECK(warn.empty());
    CHECK(trigger_cfg::DumpConfig(cfg).find("manifest=full") !=
          std::string::npos);
  }
  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    std::string warn;
    CHECK(Parse("[trigger_center]\nallow_empty_sources = true\n"
              "[channel./cam]\nkind = pinned_slot\nname = cam\nmanifest = off\n"
              "[security]\nmanifest = metadata\n",
              &cfg, &err, &warn));
    CHECK(cfg.manifest_mode == integrity::ManifestMode::kMetadata);
    CHECK(cfg.recorder_sources[0].manifest ==
          triggered::SourceConfig::ManifestPolicy::kOff);
    CHECK(warn.find("explicit integrity downgrade") != std::string::npos);
    CHECK(warn.find("explicit bulk integrity downgrade") != std::string::npos);
    const std::string dump = trigger_cfg::DumpConfig(cfg);
    trigger_cfg::CenterConfig reparsed;
    CHECK(Parse(dump, &reparsed, &err));
    CHECK(trigger_cfg::DumpConfig(reparsed) == dump);
    trigger_cfg::CenterConfig changed = cfg;
    changed.manifest_mode = integrity::ManifestMode::kFull;
    CHECK(trigger_cfg::ReloadRequiresRestart(cfg, changed));
  }
  for (const char* value : {"mixed", "none", "FULL"}) {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(!Parse(topology + "[security]\nmanifest = " + value + "\n", &cfg, &err));
    CHECK(err.find("manifest") != std::string::npos);
  }
  for (const char* value : {"metadata", "off"}) {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(!Parse(mirror_only + "manifest = " + value + "\n", &cfg, &err));
    CHECK(err.find("small structs") != std::string::npos);
  }
  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(!Parse(bulk_only + "manifest = sampled\n", &cfg, &err));
    CHECK(err.find("full|metadata|off") != std::string::npos);
  }
}

}  // namespace

// [recorder].incomplete_policy / incomplete_max_age_s.
void TestIncompletePolicy() {
  const char* kEmpty = "[trigger_center]\nallow_empty_sources = true\n";
  // Default is keep (safe default).
  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(Parse(std::string(kEmpty) + "[recorder]\noutput_dir = /tmp/x\n", &cfg,
                &err));
    CHECK(cfg.recorder_incomplete_policy == "keep");
    CHECK(cfg.recorder_incomplete_max_age_ns == 0);
  }
  // All three values parse; delete_after_age takes an age knob.
  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(Parse(std::string(kEmpty) +
                    "[recorder]\nincomplete_policy = delete_after_age\n"
                    "incomplete_max_age_s = 3600\n",
                &cfg, &err));
    CHECK(cfg.recorder_incomplete_policy == "delete_after_age");
    CHECK(cfg.recorder_incomplete_max_age_ns == 3600000000000ull);
  }
  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(Parse(std::string(kEmpty) +
                    "[recorder]\nincomplete_policy = delete_immediately\n",
                &cfg, &err));
    CHECK(cfg.recorder_incomplete_policy == "delete_immediately");
  }
  // Unknown policy values are parse errors (typo-safe).
  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(
        !Parse(std::string(kEmpty) + "[recorder]\nincomplete_policy = purge\n",
                 &cfg, &err));
    CHECK(err.find("incomplete_policy") != std::string::npos);
  }
}

int main() {
  TestRejectedKeys();
  TestFlushDefault();
  TestSliceBoundaries();
  TestIncompleteRateMetadata();
  TestBindShmPid();
  TestWaitIdle();
  TestAutoArmRename();
  TestChannelSelectors();
  TestDumpConfig();
  TestManifestModes();
  TestIncompletePolicy();
  std::printf("test_trigger_config: all checks passed\n");
  return 0;
}
