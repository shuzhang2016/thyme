// triggerctl.cpp — unified recorder/trigger CLI (ECO-02).
//
// Recorder operations (control socket, default /tmp/thyme_recorder.sock):
//   triggerctl arm|disarm [--socket PATH] [--timeout MS] [--json]
//   triggerctl status    [--socket PATH] [--timeout MS] [--json]
//   triggerctl verify <package-dir> [--key PATH] [--json]
//
// Trigger surface (trigger socket, default /tmp/thyme_trigger.sock):
//   triggerctl <source> [--socket PATH] [--priority N] [--force]
//              [--timestamp NS] [--timeout MS] [--json]
//
// `--socket` names the socket for the selected surface; `--control-socket` is
// the pre-ECO-02 alias for recorder commands (it wins when both are given).
// `--timeout` is milliseconds and doubles as the alias `--timeout-ms`.
//
// Exit codes are identical for every surface:
//   0  command accepted / status returned / package verified
//   1  rejected by the recorder, or the package failed verification
//   2  usage error (unknown flag/command, missing value)
//   5  transport failure (recorder/trigger center offline or timed out)
//
// The `status --json` schema and the verify document are documented in
// docs/guides/recorder-cli.md.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "thyme/record/package_integrity.h"
#include "thyme/record/record_control.h"
#include "thyme/record/trigger_client.h"
#include "thyme/record/triggered_recorder.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kExitTransport = 5;

constexpr const char *kDefaultTriggerSocket = "/tmp/thyme_trigger.sock";
constexpr const char *kDefaultControlSocket = "/tmp/thyme_recorder.sock";
constexpr uint32_t kDefaultControlTimeoutMs = 1000;
constexpr uint32_t kDefaultTriggerTimeoutMs = static_cast<uint32_t>(
    thyme::trigger_client::kDefaultTimeoutNs / 1000000ULL);

struct Options {
  std::string command;
  std::string positional;
  std::string socket;
  std::string control_socket;
  std::string key;
  int32_t priority = -1;
  uint64_t timestamp = 0;
  uint32_t timeout_ms = 0;  // 0 = surface default
  bool force = false;
  bool json = false;
  bool help = false;
};

void PrintUsage(FILE *out) {
  std::fprintf(
      out,
      "usage: triggerctl <command> [options]\n"
      "\n"
      "recorder commands (control socket, default %s):\n"
      "  arm                     arm continuous capture\n"
      "  disarm                  disarm continuous capture\n"
      "  status                  query recorder status\n"
      "  verify <package-dir>    verify seal + integrity.manifest\n"
      "\n"
      "trigger:\n"
      "  triggerctl <source> [--priority N] [--force] [--timestamp NS]\n"
      "\n"
      "options:\n"
      "  --socket PATH           socket for the selected surface\n"
      "  --control-socket PATH   alias of --socket for recorder commands\n"
      "  --timeout MS            reply deadline in ms (default: %u control, "
      "%u trigger)\n"
      "  --json                  machine-readable result on stdout\n"
      "  --key PATH              HMAC signing key (verify only)\n"
      "  -h, --help              this text\n"
      "\n"
      "exit codes: 0 ok, 1 rejected/verification failed, 2 usage, "
      "5 transport\n",
      kDefaultControlSocket, kDefaultControlTimeoutMs,
      kDefaultTriggerTimeoutMs);
}

bool IsRecorderCommand(const std::string &command) {
  return command == "arm" || command == "disarm" || command == "status" ||
         command == "verify";
}

std::string JsonEscape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
    }
  }
  return out;
}

const char *ManifestStateName(thyme::integrity::ManifestState state) {
  using thyme::integrity::ManifestState;
  switch (state) {
    case ManifestState::kOk:
      return "ok";
    case ManifestState::kAbsent:
      return "absent";
    case ManifestState::kInvalid:
      return "invalid";
  }
  return "unknown";
}

// Returns false (and fills *error) for a usage error.
bool ParseArgs(int argc, char **argv, Options *opt, std::string *error) {
  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    const auto needs_value = [&](const char *flag) {
      if (i + 1 >= argc) {
        *error = std::string(flag) + " requires a value";
        return false;
      }
      return true;
    };
    if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
      opt->help = true;
    } else if (std::strcmp(arg, "--json") == 0) {
      opt->json = true;
    } else if (std::strcmp(arg, "--force") == 0) {
      opt->force = true;
    } else if (std::strcmp(arg, "--socket") == 0) {
      if (!needs_value(arg))
        return false;
      opt->socket = argv[++i];
    } else if (std::strcmp(arg, "--control-socket") == 0) {
      if (!needs_value(arg))
        return false;
      opt->control_socket = argv[++i];
    } else if (std::strcmp(arg, "--key") == 0) {
      if (!needs_value(arg))
        return false;
      opt->key = argv[++i];
    } else if (std::strcmp(arg, "--priority") == 0) {
      if (!needs_value(arg))
        return false;
      opt->priority = static_cast<int32_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(arg, "--timestamp") == 0) {
      if (!needs_value(arg))
        return false;
      opt->timestamp = std::strtoull(argv[++i], nullptr, 10);
    } else if (std::strcmp(arg, "--timeout") == 0 ||
               std::strcmp(arg, "--timeout-ms") == 0) {
      if (!needs_value(arg))
        return false;
      const long ms = std::atol(argv[++i]);
      if (ms <= 0 || ms > 60000) {
        *error = std::string(arg) + " must be 1..60000";
        return false;
      }
      opt->timeout_ms = static_cast<uint32_t>(ms);
    } else if (arg[0] == '-') {
      *error = std::string("unknown option ") + arg;
      return false;
    } else if (opt->command.empty()) {
      opt->command = arg;
    } else if (opt->positional.empty()) {
      opt->positional = arg;
    } else {
      *error = std::string("unexpected argument ") + arg;
      return false;
    }
  }
  return true;
}

int PrintControlAck(const Options &opt, const std::string &command,
                    const std::string &socket) {
  if (opt.json) {
    std::printf(
        "{\"schema\":1,\"kind\":\"thyme-recorder-control\",\"ok\":true,"
        "\"command\":\"%s\",\"socket\":\"%s\"}\n",
        command.c_str(), JsonEscape(socket).c_str());
  } else {
    std::printf("%s sent\n", command.c_str());
  }
  return kExitOk;
}

int RunControl(const Options &opt) {
  const std::string socket =
      !opt.control_socket.empty()
          ? opt.control_socket
          : (!opt.socket.empty() ? opt.socket : kDefaultControlSocket);
  const uint32_t timeout_ms =
      opt.timeout_ms != 0 ? opt.timeout_ms : kDefaultControlTimeoutMs;

  const char *command =
      opt.command == "status" && opt.json ? "status-json" : opt.command.c_str();
  const thyme::record_control::Result result =
      thyme::record_control::ExecuteCommand(socket.c_str(), command,
                                            timeout_ms);
  const bool status = opt.command == "status";
  if (!result.transport_ok ||
      result.code != thyme::record_control::ControlCode::kAccepted) {
    if (opt.json) {
      std::printf(
          "{\"schema\":1,\"kind\":\"%s\",\"ok\":false,"
          "\"command\":\"%s\",\"socket\":\"%s\",\"error\":\"%s\","
          "\"code\":\"%s\"}\n",
          status ? "thyme-recorder-status" : "thyme-recorder-control",
          opt.command.c_str(), JsonEscape(socket).c_str(),
          JsonEscape(result.error).c_str(),
          thyme::record_control::ControlCodeName(result.code));
    }
    std::fprintf(stderr, "triggerctl: %s\n", result.error.c_str());
    return result.transport_ok ? kExitFailed : kExitTransport;
  }
  if (status) {
    std::printf("%s\n", result.payload.c_str());
    return kExitOk;
  }
  return PrintControlAck(opt, opt.command, socket);
}

int RunVerify(const Options &opt) {
  const std::string &package = opt.positional;
  if (package.empty()) {
    std::fprintf(stderr, "triggerctl: verify needs a package directory\n");
    return kExitUsage;
  }

  const thyme::integrity::VerifyResult result =
      thyme::integrity::VerifyPackageManifest(package, opt.key);
  const bool sealed =
      thyme::triggered::TriggeredRecorder::PackageComplete(package);
  const bool ok = sealed && result.ok();
  const std::string state = ManifestStateName(result.state);
  std::string outcome = ok ? "OK" : "FAILED";
  if (ok &&
      result.info.manifest_mode != thyme::integrity::ManifestMode::kFull) {
    outcome += "(";
    outcome += thyme::integrity::ManifestModeName(result.info.manifest_mode);
    outcome += ")";
  }

  if (opt.json) {
    std::printf(
        "{\"schema\":1,\"kind\":\"thyme-recorder-verify\",\"ok\":%s,"
        "\"package\":\"%s\",\"sealed\":%s,\"manifest\":\"%s\","
        "\"intact\":%s,\"partial\":%s,\"signed\":%s,"
        "\"integrity_mode\":\"%s\",\"signature_checked\":%s,"
        "\"signature_ok\":%s,\"chain_ok\":%s,"
        "\"files_ok\":%llu,\"files_missing\":%llu,\"files_mismatch\":%llu,"
        "\"files_unlisted\":%llu,\"files_unverified\":%llu,\"error\":\"%s\"}\n",
        ok ? "true" : "false", JsonEscape(package).c_str(),
        sealed ? "true" : "false", state.c_str(),
        result.intact() ? "true" : "false", result.partial ? "true" : "false",
        result.signed_manifest ? "true" : "false",
        thyme::integrity::ManifestModeName(result.info.manifest_mode),
        result.signature_checked ? "true" : "false",
        result.signature_ok ? "true" : "false",
        result.chain_ok ? "true" : "false",
        static_cast<unsigned long long>(result.files_ok),
        static_cast<unsigned long long>(result.files_missing),
        static_cast<unsigned long long>(result.files_mismatch),
        static_cast<unsigned long long>(result.files_unlisted),
        static_cast<unsigned long long>(result.files_unverified),
        JsonEscape(result.error).c_str());
  } else {
    std::printf(
        "verify: %s package=%s sealed=%s manifest=%s intact=%s files_ok=%llu "
        "missing=%llu mismatch=%llu unlisted=%llu unverified_payload=%llu\n",
        outcome.c_str(), package.c_str(), sealed ? "true" : "false",
        state.c_str(), result.intact() ? "true" : "false",
        static_cast<unsigned long long>(result.files_ok),
        static_cast<unsigned long long>(result.files_missing),
        static_cast<unsigned long long>(result.files_mismatch),
        static_cast<unsigned long long>(result.files_unlisted),
        static_cast<unsigned long long>(result.files_unverified));
    if (!result.error.empty()) {
      std::fprintf(stderr, "triggerctl: verify: %s\n", result.error.c_str());
    }
  }
  return ok ? kExitOk : kExitFailed;
}

int RunTrigger(const Options &opt) {
  const std::string socket =
      opt.socket.empty() ? kDefaultTriggerSocket : opt.socket;
  const uint32_t timeout_ms =
      opt.timeout_ms != 0 ? opt.timeout_ms : kDefaultTriggerTimeoutMs;
  const uint64_t timeout_ns = static_cast<uint64_t>(timeout_ms) * 1000000ULL;

  const thyme::trigger_client::Result r = thyme::trigger_client::Trigger(
      socket.c_str(), opt.command.c_str(), opt.timestamp, opt.priority,
      opt.force, timeout_ns);
  const char *code = "unknown";
  switch (r.code) {
    case thyme::trigger_proto::TriggerCode::kAccepted:
      code = "accepted";
      break;
    case thyme::trigger_proto::TriggerCode::kMuted:
      code = "muted";
      break;
    case thyme::trigger_proto::TriggerCode::kQuotaExceeded:
      code = "quota_exceeded";
      break;
    case thyme::trigger_proto::TriggerCode::kUnknownSource:
      code = "unknown_source";
      break;
    case thyme::trigger_proto::TriggerCode::kPreemptedByHigher:
      code = "preempted_by_higher";
      break;
    case thyme::trigger_proto::TriggerCode::kRecorderOffline:
      code = "recorder_offline";
      break;
    case thyme::trigger_proto::TriggerCode::kBusy:
      code = "busy";
      break;
    case thyme::trigger_proto::TriggerCode::kInvalidTimestamp:
      code = "invalid_timestamp";
      break;
    case thyme::trigger_proto::TriggerCode::kUnsupportedVersion:
      code = "unsupported_version";
      break;
    case thyme::trigger_proto::TriggerCode::kReplayRejected:
      code = "replay_rejected";
      break;
    case thyme::trigger_proto::TriggerCode::kAuthRejected:
      code = "auth_rejected";
      break;
    case thyme::trigger_proto::TriggerCode::kMalformedRequest:
      code = "malformed_request";
      break;
    case thyme::trigger_proto::TriggerCode::kTransportTimeout:
      code = "transport_timeout";
      break;
    case thyme::trigger_proto::TriggerCode::kCancelled:
      code = "cancelled";
      break;
    case thyme::trigger_proto::TriggerCode::kUnknownTrigger:
      code = "unknown_trigger";
      break;
    case thyme::trigger_proto::TriggerCode::kCaptureSealed:
      code = "capture_sealed";
      break;
    case thyme::trigger_proto::TriggerCode::kStartFailed:
      code = "start_failed";
      break;
    case thyme::trigger_proto::TriggerCode::kPreemptFailed:
      code = "preempt_failed";
      break;
  }

  if (!r.transport_ok) {
    if (opt.json) {
      std::printf(
          "{\"schema\":1,\"kind\":\"thyme-trigger-result\",\"ok\":false,"
          "\"source\":\"%s\",\"socket\":\"%s\",\"code\":\"%s\","
          "\"transport_ok\":false,\"result_unknown\":%s,"
          "\"request_id\":%llu,\"attempts\":%u,\"retries\":%u}\n",
          JsonEscape(opt.command).c_str(), JsonEscape(socket).c_str(), code,
          r.code == thyme::trigger_proto::TriggerCode::kTransportTimeout
              ? "true"
              : "false",
          static_cast<unsigned long long>(r.request_id), r.attempts, r.retries);
    }
    if (r.code == thyme::trigger_proto::TriggerCode::kTransportTimeout) {
      std::fprintf(stderr,
                   "triggerctl: result unknown after bounded retries; the "
                   "request may have executed (request_id=%llu)\n",
                   static_cast<unsigned long long>(r.request_id));
    } else {
      std::fprintf(stderr, "triggerctl: no reply (recorder/center offline)\n");
    }
    return kExitTransport;
  }

  if (r.code == thyme::trigger_proto::TriggerCode::kReplayRejected) {
    std::fprintf(
        stderr,
        "triggerctl: replay_rejected: duplicate or old trigger sequence; "
        "retry with a new sequence\n");
  }
  const bool accepted = r.code == thyme::trigger_proto::TriggerCode::kAccepted;
  if (opt.json) {
    std::printf("{\"schema\":1,\"kind\":\"thyme-trigger-result\",\"ok\":%s,"
        "\"source\":\"%s\",\"socket\":\"%s\",\"code\":\"%s\","
                "\"transport_ok\":true,\"result_unknown\":false,"
                "\"trigger_id\":%llu,\"priority\":%d,\"priority_clamped\":%s,"
                "\"request_id\":%llu,\"attempts\":%u,\"retries\":%u,"
                "\"deduplicated\":%s}\n",
        accepted ? "true" : "false", JsonEscape(opt.command).c_str(),
        JsonEscape(socket).c_str(), code,
        static_cast<unsigned long long>(r.trigger_id),
        static_cast<int>(r.effective_priority),
                r.priority_clamped ? "true" : "false",
                static_cast<unsigned long long>(r.request_id), r.attempts,
                r.retries, r.deduplicated ? "true" : "false");
  } else {
    std::printf("code=%s trigger_id=%llu priority=%d%s\n", code,
                static_cast<unsigned long long>(r.trigger_id),
                static_cast<int>(r.effective_priority),
                r.priority_clamped ? " (hint clamped)" : "");
  }
  return accepted ? kExitOk : kExitFailed;
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  std::string error;
  if (!ParseArgs(argc, argv, &opt, &error)) {
    std::fprintf(stderr, "triggerctl: %s\n", error.c_str());
    PrintUsage(stderr);
    return kExitUsage;
  }
  if (opt.help) {
    PrintUsage(stdout);
    return kExitOk;
  }
  if (opt.command.empty()) {
    std::fprintf(stderr, "triggerctl: missing command\n");
    PrintUsage(stderr);
    return kExitUsage;
  }

  const bool recorder = IsRecorderCommand(opt.command);
  if (opt.command != "verify" && !opt.key.empty()) {
    std::fprintf(stderr, "triggerctl: --key only applies to verify\n");
    return kExitUsage;
  }
  if (recorder && opt.command != "verify" && !opt.positional.empty()) {
    std::fprintf(stderr, "triggerctl: %s takes no positional argument\n",
                 opt.command.c_str());
    return kExitUsage;
  }
  if (recorder && (opt.force || opt.priority != -1 || opt.timestamp != 0)) {
    std::fprintf(stderr,
                 "triggerctl: --force/--priority/--timestamp only apply to "
                 "trigger sources\n");
    return kExitUsage;
  }
  if (!recorder && !opt.positional.empty()) {
    std::fprintf(stderr, "triggerctl: unexpected argument %s\n",
                 opt.positional.c_str());
    return kExitUsage;
  }

  if (opt.command == "verify")
    return RunVerify(opt);
  if (recorder)
    return RunControl(opt);
  return RunTrigger(opt);
}
