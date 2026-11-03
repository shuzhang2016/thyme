// test_record_manifest_subset.cpp — GEN-12/TST-11 runtime half of the record
// manifest contract.
//
// Generated `record_manifest.h` lists the channels the graph records. A
// deployment config may only describe those channels: `[channel.*]` entries
// that name anything else must fail startup instead of silently recording
// nothing. TRG-16 also checks source/continuous selectors so a legacy shm
// alias cannot select a channel outside that manifest.
#include "thyme/check.h"
#include "thyme/record/trigger_config.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace thyme;

namespace {

const char* kConfig = R"(
[trigger_center]
socket = /tmp/thyme_trigger.sock
allow_empty_sources = true

[recorder]
output_dir = /tmp/thyme_manifest_test
pending_bytes_quota = 67108864

[channel.foo]
kind = mirror_ring

[channel./bar]
kind = pinned_slot
name = bar
rate_hz = 10
avg_frame_bytes = 4096
)";

const char* kSupersetConfig = R"(
[trigger_center]
socket = /tmp/thyme_trigger.sock
allow_empty_sources = true

[recorder]
output_dir = /tmp/thyme_manifest_test
pending_bytes_quota = 67108864

[channel.foo]
kind = mirror_ring

[channel./camera_front]
kind = pinned_slot
name = camera_front
rate_hz = 10
avg_frame_bytes = 4096
)";

}  // namespace

int main() {
  const std::vector<std::string> manifest = {"bar", "foo", "zulu"};

  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    if (!trigger_cfg::ParseCenterConfig(kConfig, &cfg, &err)) {
      std::fprintf(stderr, "parse kConfig: %s\n", err.c_str());
      CHECK(false);
    }
    CHECK(trigger_cfg::ValidateManifestSubset(cfg, manifest, &err));
  }

  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    if (!trigger_cfg::ParseCenterConfig(kSupersetConfig, &cfg, &err)) {
      std::fprintf(stderr, "parse kSupersetConfig: %s\n", err.c_str());
      CHECK(false);
    }
    CHECK(!trigger_cfg::ValidateManifestSubset(cfg, manifest, &err));
    CHECK(err.find("camera_front") != std::string::npos);
    CHECK(err.find("not recorded") != std::string::npos);
    CHECK(err.find("[channel./camera_front]") != std::string::npos);
  }

  {
    // Empty manifest = codegen disabled (standalone trigger_center); no check.
    trigger_cfg::CenterConfig cfg;
    std::string err;
    if (!trigger_cfg::ParseCenterConfig(kSupersetConfig, &cfg, &err)) {
      std::fprintf(stderr, "parse kSupersetConfig: %s\n", err.c_str());
      CHECK(false);
    }
    CHECK(trigger_cfg::ValidateManifestSubset(cfg, {}, &err));
  }

  {
    // Generated-table form: kRecordManifestChannels / kRecordManifestCount are
    // exactly a `const char* const[]` + count, so the host passes them as-is.
    const char* const table[] = {"bar", "foo", "zulu"};
    const char* const empty_table[] = {nullptr};
    const char* const null_entry[] = {nullptr};
    trigger_cfg::CenterConfig cfg;
    std::string err;
    if (!trigger_cfg::ParseCenterConfig(kSupersetConfig, &cfg, &err)) {
      std::fprintf(stderr, "parse kSupersetConfig: %s\n", err.c_str());
      CHECK(false);
    }
    CHECK(!trigger_cfg::ValidateManifestSubset(cfg, table, 3, &err));
    CHECK(err.find("camera_front") != std::string::npos);
    CHECK(!trigger_cfg::ValidateManifestSubset(cfg, empty_table, 0, &err));
    CHECK(err.find("not recorded") != std::string::npos);
    CHECK(!trigger_cfg::ValidateManifestSubset(cfg, nullptr, 1, &err));
    CHECK(err.find("table is null") != std::string::npos);
    CHECK(!trigger_cfg::ValidateManifestSubset(cfg, null_entry, 1, &err));
    CHECK(err.find("null channel") != std::string::npos);
  }

  {
    // A generated empty topology is valid only without recording sources or
    // selectors. An empty source selector means "all" and must also fail.
    const char* const empty_table[] = {nullptr};
    const std::string empty_config =
        "[trigger_center]\nallow_empty_sources = true\n";
    for (const char* suffix : {
             "",
             "[source.all]\nquota_class = q\n",
             "[source.all]\nchannels = small_struct:*\nquota_class = q\n",
             "[continuous]\n",
             "[continuous]\nchannels = small_struct:*\n",
         }) {
      trigger_cfg::CenterConfig cfg;
      std::string err;
      const bool selectors_only = suffix[0] != '\0';
      const std::string text = selectors_only ? std::string(kConfig) + suffix
                                              : empty_config + suffix;
      if (!trigger_cfg::ParseCenterConfig(text, &cfg, &err)) {
        std::fprintf(stderr, "parse empty topology suffix '%s': %s\n", suffix,
                     err.c_str());
        CHECK(false);
      }
      if (selectors_only) cfg.recorder_sources.clear();
      const bool valid =
          trigger_cfg::ValidateManifestSubset(cfg, empty_table, 0, &err);
      CHECK(valid == (suffix[0] == '\0'));
      if (!valid) CHECK(err.find("empty graph record manifest") !=
                            std::string::npos ||
                        err.find("outside the graph record manifest") !=
                            std::string::npos);
    }
  }

  const std::string aliased =
      "[trigger_center]\nallow_empty_sources = true\n"
      "[channel.legacy]\nkind = mirror_ring\nname = bar\n";
  {
    // Source and continuous globs resolve against manifest names as well as
    // configured channels. The exclusion cannot broaden the selected set.
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(trigger_cfg::ParseCenterConfig(
        aliased + "[source.s]\nchannels = small_struct:b*\nquota_class = q\n"
        "[continuous]\nchannels = small_struct:*, !small_struct:zulu\n",
        &cfg, &err));
    CHECK(trigger_cfg::ValidateManifestSubset(cfg, manifest, &err));
  }
  for (const char *section : {
           "[source.s]\nchannels = small_struct:legacy\nquota_class = q\n",
           "[continuous]\nchannels = small_struct:legacy\n"}) {
    // `legacy` is a shm alias accepted by the recorder, but only `bar` is
    // recorded by the graph. Selector validation must not bypass the manifest
    // through that alias.
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(trigger_cfg::ParseCenterConfig(aliased + section, &cfg, &err));
    CHECK(!trigger_cfg::ValidateManifestSubset(cfg, manifest, &err));
    CHECK(err.find("outside the graph record manifest") != std::string::npos);
  }

  {
    // OPS-13: legacy parsing is unchanged, while a layered base is restricted
    // to generated channel sections and supplies the strict manifest.
    trigger_cfg::CenterConfig legacy;
    std::string err;
    CHECK(trigger_cfg::ParseCenterConfig(kConfig, &legacy, &err));
    CHECK(legacy.recorder_sources.size() == 2);

    trigger_cfg::CenterConfig cfg;
    CHECK(!trigger_cfg::ParseLayeredCenterConfig(
        "\n[recorder]\noutput_dir = /tmp/thyme_manifest_test\n",
        "[trigger_center]\nallow_empty_sources = true\n", &cfg, &err));
    CHECK(err.find("base line 2") != std::string::npos);
    CHECK(!trigger_cfg::ParseLayeredCenterConfig(
        "[channel.foo]\nkind = mirror_ring\n",
        "\n[channel.bar]\nkind = pinned_slot\n", &cfg, &err));
    CHECK(err.find("policy line 2") != std::string::npos);
    CHECK(!trigger_cfg::ParseLayeredCenterConfig(
        "[channel.foo]\nkind = mirror_ring\n", "[chassis]\npage_shm = /c\n",
        &cfg, &err));
    CHECK(err.find("policy line 1") != std::string::npos);
    CHECK(trigger_cfg::ParseLayeredCenterConfig(
        "[channel.foo]\nkind = mirror_ring\n[chassis]\npage_shm = /c\n",
        "[trigger_center]\nallow_empty_sources = true\n", &cfg, &err));
    CHECK(cfg.chassis.page_shm == "/c");
  }

  {
    const std::string empty_policy =
        "[trigger_center]\nallow_empty_sources = true\n"
        "[recorder]\noutput_dir = /tmp/thyme_manifest_test\n";
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(trigger_cfg::ParseLayeredCenterConfig("", empty_policy, &cfg, &err));
    CHECK(cfg.recorder_sources.empty());
    CHECK(!trigger_cfg::ParseLayeredCenterConfig(
        "", empty_policy + "[source.all]\nquota_class = q\n", &cfg, &err));
    CHECK(err.find("empty graph record manifest") != std::string::npos);
  }

  {
    const std::string base =
        "[channel.legacy]\nkind = mirror_ring\nname = bar\n";
    const std::string policy =
        "[trigger_center]\nallow_empty_sources = true\n"
        "[recorder]\noutput_dir = /tmp/thyme_manifest_test\n";
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(trigger_cfg::ParseLayeredCenterConfig(
        base, policy +
                  "[source.s]\nchannels = small_struct:b*\nquota_class = q\n"
                  "[continuous]\nchannels = small_struct:*\n",
        &cfg, &err));
    CHECK(!trigger_cfg::ParseLayeredCenterConfig(
        base, policy +
                  "[source.s]\nchannels = small_struct:legacy\nquota_class = q\n",
        &cfg, &err));
    CHECK(err.find("outside the graph record manifest") != std::string::npos);
  }

  {
    const std::string aggregate =
        "[channel./c9_%pid_rec]\nkind = mirror_ring\n"
        "record_channels = alpha,bravo\nrate_hz = 20\navg_frame_bytes = 40\n";
    const char* const table[] = {"alpha", "bravo"};
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(trigger_cfg::ParseLayeredCenterConfig(
        aggregate, "[trigger_center]\nallow_empty_sources = true\n", &cfg, &err));
    CHECK(trigger_cfg::ValidateManifestSubset(cfg, table, 2, &err));
    CHECK(!trigger_cfg::ParseLayeredCenterConfig(
        aggregate + "[channel./c9_%pid_rec]\nkind = mirror_ring\n",
        "[trigger_center]\nallow_empty_sources = true\n", &cfg, &err));
    CHECK(err.find("duplicate channel") != std::string::npos);
    CHECK(!trigger_cfg::ParseLayeredCenterConfig(
        aggregate, "[trigger_center]\nallow_empty_sources = true\n"
        "[channel.other]\nkind = mirror_ring\n", &cfg, &err));
    const char* const wrong[] = {"alpha", "gamma"};
    CHECK(trigger_cfg::ParseLayeredCenterConfig(
        aggregate, "[trigger_center]\nallow_empty_sources = true\n", &cfg, &err));
    CHECK(!trigger_cfg::ValidateManifestSubset(cfg, wrong, 2, &err));
    CHECK(err.find("bravo") != std::string::npos);
  }

  return 0;
}
