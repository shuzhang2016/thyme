#include "catalog.h"

#include "util.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// Split a line on '|' into fields (empty fields preserved). Leading/trailing
// whitespace of each field is trimmed.
std::vector<std::string> SplitPipe(const std::string& line) {
  std::vector<std::string> out;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == '|') {
      out.push_back(gen_util::Trim(line.substr(start, i - start)));
      start = i + 1;
    }
  }
  return out;
}

struct LayoutScan {
  const std::string& text;
  std::size_t i = 0;

  void Skip() {
    for (;;) {
      while (i < text.size() &&
             std::isspace(static_cast<unsigned char>(text[i])) != 0) {
        ++i;
      }
      if (text.compare(i, 2, "//") == 0) {
        const std::size_t nl = text.find('\n', i);
        i = nl == std::string::npos ? text.size() : nl + 1;
        continue;
      }
      if (text.compare(i, 2, "/*") == 0) {
        const std::size_t end = text.find("*/", i + 2);
        if (end == std::string::npos) {
          i = text.size();
          return;
        }
        i = end + 2;
        continue;
      }
      return;
    }
  }

  bool Consume(char c) {
    Skip();
    if (i < text.size() && text[i] == c) {
      ++i;
      return true;
    }
    return false;
  }
};

bool ParseLayoutU32(LayoutScan* sc, std::uint32_t* out) {
  sc->Skip();
  if (sc->i >= sc->text.size() ||
      std::isdigit(static_cast<unsigned char>(sc->text[sc->i])) == 0) {
    return false;
  }
  std::uint64_t value = 0;
  while (sc->i < sc->text.size() &&
         std::isdigit(static_cast<unsigned char>(sc->text[sc->i])) != 0) {
    value = value * 10u + static_cast<unsigned>(sc->text[sc->i] - '0');
    if (value > 0xffffffffull) return false;
    ++sc->i;
  }
  if (sc->i < sc->text.size() &&
      (sc->text[sc->i] == 'u' || sc->text[sc->i] == 'U')) {
    ++sc->i;
  }
  *out = static_cast<std::uint32_t>(value);
  return true;
}

bool ParseLayoutString(LayoutScan* sc, std::string* out) {
  sc->Skip();
  if (sc->i >= sc->text.size() || sc->text[sc->i] != '"') return false;
  ++sc->i;
  out->clear();
  while (sc->i < sc->text.size() && sc->text[sc->i] != '"') {
    if (sc->text[sc->i] == '\\') return false;
    out->push_back(sc->text[sc->i]);
    ++sc->i;
  }
  if (sc->i >= sc->text.size()) return false;
  ++sc->i;
  return true;
}

bool ParseLayoutIdent(LayoutScan* sc, std::string* out) {
  sc->Skip();
  if (sc->i >= sc->text.size()) return false;
  const unsigned char c = static_cast<unsigned char>(sc->text[sc->i]);
  if (std::isalpha(c) == 0 && sc->text[sc->i] != '_') return false;
  const std::size_t begin = sc->i;
  ++sc->i;
  while (sc->i < sc->text.size()) {
    const unsigned char n = static_cast<unsigned char>(sc->text[sc->i]);
    if (std::isalnum(n) == 0 && sc->text[sc->i] != '_') break;
    ++sc->i;
  }
  *out = sc->text.substr(begin, sc->i - begin);
  return true;
}

bool ParseFieldArray(const std::string& text, const std::string& ident,
                     std::vector<LayoutFieldCat>* fields, std::string* err) {
  const std::string needle = ident + "[]";
  const std::size_t at = text.find(needle);
  if (at == std::string::npos) {
    *err = "contract layout missing field array " + ident;
    return false;
  }
  LayoutScan sc{text, at + needle.size()};
  if (!sc.Consume('=') || !sc.Consume('{')) {
    *err = "contract layout field array " + ident + " has no initializer";
    return false;
  }
  for (;;) {
    sc.Skip();
    if (sc.Consume('}')) return true;
    if (!sc.Consume('{')) {
      *err = "contract layout field array " + ident + " is malformed";
      return false;
    }
    LayoutFieldCat field;
    std::uint32_t stride = 0;
    if (!ParseLayoutString(&sc, &field.name) || !sc.Consume(',') ||
        !ParseLayoutString(&sc, &field.type) || !sc.Consume(',') ||
        !ParseLayoutU32(&sc, &field.offset) || !sc.Consume(',') ||
        !ParseLayoutU32(&sc, &field.size) || !sc.Consume(',') ||
        !ParseLayoutU32(&sc, &field.count) || !sc.Consume(',') ||
        !ParseLayoutU32(&sc, &stride) || !sc.Consume('}')) {
      *err = "contract layout field array " + ident + " has a bad field";
      return false;
    }
    if (field.name.empty()) {
      *err = "contract layout field array " + ident + " has an empty name";
      return false;
    }
    fields->push_back(std::move(field));
    sc.Skip();
    if (sc.Consume(',')) continue;
    if (sc.Consume('}')) return true;
    *err = "contract layout field array " + ident + " is malformed";
    return false;
  }
}

bool ParseContractLayoutText(const std::string& text,
                             std::vector<ContractLayoutCat>* out,
                             std::string* err) {
  const std::string marker = "kContracts[]";
  const std::size_t at = text.find(marker);
  if (at == std::string::npos) return true;
  LayoutScan sc{text, at + marker.size()};
  if (!sc.Consume('=') || !sc.Consume('{')) {
    *err = "contract layout kContracts has no initializer";
    return false;
  }
  for (;;) {
    sc.Skip();
    if (sc.Consume('}')) return true;
    if (!sc.Consume('{')) {
      *err = "contract layout kContracts is malformed";
      return false;
    }
    ContractLayoutCat row;
    std::uint32_t field_count = 0;
    std::string fields_ident;
    if (!ParseLayoutString(&sc, &row.cpp_type) || !sc.Consume(',') ||
        !ParseLayoutU32(&sc, &row.payload_bytes) || !sc.Consume(',') ||
        !ParseLayoutU32(&sc, &field_count) || !sc.Consume(',') ||
        !ParseLayoutU32(&sc, &row.align) || !sc.Consume(',') ||
        !ParseLayoutIdent(&sc, &fields_ident) || !sc.Consume('}')) {
      *err = "contract layout kContracts has a bad row";
      return false;
    }
    if (row.cpp_type.empty()) {
      *err = "contract layout row has an empty cpp_type";
      return false;
    }
    for (const ContractLayoutCat& prev : *out) {
      if (prev.cpp_type == row.cpp_type) {
        *err = "contract layout duplicate cpp_type " + row.cpp_type;
        return false;
      }
    }
    if (!ParseFieldArray(text, fields_ident, &row.fields, err)) return false;
    if (row.fields.size() != field_count) {
      *err = "contract layout " + row.cpp_type + " field count does not match";
      return false;
    }
    out->push_back(std::move(row));
    sc.Skip();
    if (sc.Consume(',')) continue;
    if (sc.Consume('}')) return true;
    *err = "contract layout kContracts is malformed";
    return false;
  }
}

fs::path ResolveLayoutHeader(const fs::path& tpl, const std::string& include) {
  if (include.empty()) return {};
  const fs::path rel(include);
  const fs::path candidates[] = {
      tpl / rel,
      tpl / "include" / rel,
      tpl / "src" / "include" / rel,
  };
  for (const fs::path& candidate : candidates) {
    std::error_code ec;
    if (fs::is_regular_file(candidate, ec)) return candidate;
  }
  return {};
}

bool ParseBool(const std::string& s, bool* out) {
  if (s == "1" || s == "true") { *out = true; return true; }
  if (s == "0" || s == "false" || s.empty()) { *out = false; return true; }
  return false;
}

}  // namespace

bool LoadCatalog(const std::string& tpl_dir, TemplateCatalog* out) {
  if (out == nullptr) return false;
  *out = TemplateCatalog{};
  const fs::path base(tpl_dir);

  // banner.txt: missing = default. Present but unreadable = error.
  {
    const fs::path p = base / "banner.txt";
    std::error_code ec;
    if (fs::exists(p, ec)) {
      std::string body;
      if (!gen_util::ReadFile(p, &body)) {
        std::fprintf(stderr, "LoadCatalog: cannot read %s\n", p.c_str());
        return false;
      }
      out->banner = gen_util::Trim(body);
    }
  }

  // contracts_include.txt: header path for record_registry POD types. Missing =
  // default "contracts/types.h".
  {
    const fs::path p = base / "contracts_include.txt";
    std::error_code ec;
    if (fs::exists(p, ec)) {
      std::string body;
      if (!gen_util::ReadFile(p, &body)) {
        std::fprintf(stderr, "LoadCatalog: cannot read %s\n", p.c_str());
        return false;
      }
      out->contracts_include = gen_util::Trim(body);
    }
  }

  // contracts_json.txt (GEN-04): explicit declaration that the project ships a
  // gen_contracts JSON header, replacing the old emit_viz filesystem probe of
  // <root>/include. Content is the include path (e.g. "t9/contracts/
  // contracts_json.h"); empty content derives the default: contracts_json.h
  // next to contracts_include. Missing file = no contracts JSON support.
  {
    const fs::path p = base / "contracts_json.txt";
    std::error_code ec;
    if (fs::exists(p, ec)) {
      std::string body;
      if (!gen_util::ReadFile(p, &body)) {
        std::fprintf(stderr, "LoadCatalog: cannot read %s\n", p.c_str());
        return false;
      }
      out->contracts_json_include = gen_util::Trim(body);
      if (out->contracts_json_include.empty()) {
        out->contracts_json_include =
            (fs::path(out->contracts_include).parent_path() /
             "contracts_json.h").generic_string();
      }
    }
  }

  // contracts_layout.txt (ABI-09): include path of the gen_contracts runtime
  // field-layout table. Missing = no field-level schema (the recorder keeps
  // the manifest identity but reports the layout as unavailable).
  {
    const fs::path p = base / "contracts_layout.txt";
    std::error_code ec;
    if (fs::exists(p, ec)) {
      std::string body;
      if (!gen_util::ReadFile(p, &body)) {
        std::fprintf(stderr, "LoadCatalog: cannot read %s\n", p.c_str());
        return false;
      }
      out->contracts_layout_include = gen_util::Trim(body);
      const fs::path header =
          ResolveLayoutHeader(base, out->contracts_layout_include);
      if (!header.empty()) {
        std::string layout_text;
        if (!gen_util::ReadFile(header, &layout_text)) {
          std::fprintf(stderr, "LoadCatalog: cannot read %s\n", header.c_str());
          return false;
        }
        std::string layout_err;
        if (!ParseContractLayoutText(layout_text, &out->contract_layouts,
                                     &layout_err)) {
          std::fprintf(stderr, "LoadCatalog: %s (%s)\n", layout_err.c_str(),
                       header.c_str());
          return false;
        }
      }
    }
  }

  // mp_includes.inc: raw text (may be empty). Missing = empty.
  {
    const fs::path p = base / "mp_includes.inc";
    std::error_code ec;
    if (fs::exists(p, ec)) {
      std::string body;
      if (!gen_util::ReadFile(p, &body)) {
        std::fprintf(stderr, "LoadCatalog: cannot read %s\n", p.c_str());
        return false;
      }
      out->mp_includes = body;
    }
  }

  // hz_comment.txt: raw text for the kWorkerHz comment in wiring.cpp. The
  // placeholder {n} is substituted with the worker-thread count at emit time.
  // Missing = empty (EmitWiringCpp falls back to its generic two-liner).
  {
    const fs::path p = base / "hz_comment.txt";
    std::error_code ec;
    if (fs::exists(p, ec)) {
      std::string body;
      if (!gen_util::ReadFile(p, &body)) {
        std::fprintf(stderr, "LoadCatalog: cannot read %s\n", p.c_str());
        return false;
      }
      out->hz_comment = body;
    }
  }

  // extras.txt: one shared buffer per line, pipe-separated:
  //   name | cpp_type | read_types(csv) | write_types(csv)
  // Legacy bare tokens still work: "chassis" = CanInChassisBuffer read by
  // fusion/decision/planning/arcomm; "arctrl" = ArControlBuffer written by
  // arcomm.
  {
    const fs::path p = base / "extras.txt";
    std::error_code ec;
    if (fs::exists(p, ec)) {
      std::string body;
      if (!gen_util::ReadFile(p, &body)) {
        std::fprintf(stderr, "LoadCatalog: cannot read %s\n", p.c_str());
        return false;
      }
      std::istringstream ls(body);
      std::string raw;
      while (std::getline(ls, raw)) {
        const std::string t = gen_util::Trim(raw);
        if (t.empty() || t[0] == '#') continue;
        if (t == "chassis") {
          out->extras.push_back(ExtraBufCat{"chassis", "CanInChassisBuffer",
                                            "fusion,decision,planning,arcomm",
                                            ""});
          continue;
        }
        if (t == "arctrl") {
          out->extras.push_back(
              ExtraBufCat{"arctrl", "ArControlBuffer", "", "arcomm"});
          continue;
        }
        const auto f = SplitPipe(t);
        if (f.size() < 2 || !gen_util::IsIdent(f[0])) {
          std::fprintf(stderr, "LoadCatalog: bad extras.txt line: %s\n",
                       raw.c_str());
          return false;
        }
        ExtraBufCat x;
        x.name = f[0];
        x.cpp_type = f[1];
        if (f.size() >= 3) x.read_types = f[2];
        if (f.size() >= 4) x.write_types = f[3];
        out->extras.push_back(std::move(x));
      }
    }
  }

  // pools.txt: type|prefix|index_pipe(0/1)
  {
    const fs::path p = base / "pools.txt";
    std::error_code ec;
    if (fs::exists(p, ec)) {
      std::string body;
      if (!gen_util::ReadFile(p, &body)) {
        std::fprintf(stderr, "LoadCatalog: cannot read %s\n", p.c_str());
        return false;
      }
      std::istringstream ls(body);
      std::string raw;
      while (std::getline(ls, raw)) {
        const std::string t = gen_util::Trim(raw);
        if (t.empty() || t[0] == '#') continue;
        const auto f = SplitPipe(t);
        if (f.size() < 2) {
          std::fprintf(stderr, "LoadCatalog: bad pools.txt line: %s\n", raw.c_str());
          return false;
        }
        PoolCat c;
        c.type = f[0];
        c.prefix = f[1];
        if (!gen_util::IsIdent(c.type) || !gen_util::IsIdent(c.prefix)) {
          std::fprintf(stderr, "LoadCatalog: invalid identifier in pools.txt: %s\n",
                       raw.c_str());
          return false;
        }
        bool ip = false;
        if (f.size() >= 3 && !ParseBool(f[2], &ip)) {
          std::fprintf(stderr, "LoadCatalog: bad index_pipe in pools.txt: %s\n", raw.c_str());
          return false;
        }
        c.index_pipe = ip;
        out->pools.push_back(std::move(c));
      }
    }
  }

  // known_types.txt: type|cls|member|ctor|flags
  {
    const fs::path p = base / "known_types.txt";
    std::error_code ec;
    if (fs::exists(p, ec)) {
      std::string body;
      if (!gen_util::ReadFile(p, &body)) {
        std::fprintf(stderr, "LoadCatalog: cannot read %s\n", p.c_str());
        return false;
      }
      std::istringstream ls(body);
      std::string raw;
      while (std::getline(ls, raw)) {
        const std::string t = gen_util::Trim(raw);
        if (t.empty() || t[0] == '#') continue;
        const auto f = SplitPipe(t);
        if (f.size() < 3) {
          std::fprintf(stderr, "LoadCatalog: bad known_types.txt line: %s\n", raw.c_str());
          return false;
        }
        KnownCat c;
        c.type = f[0];
        c.cls = f[1];
        c.member = f[2];
        if (f.size() >= 4) c.ctor = f[3];
        if (f.size() >= 5) c.flags = f[4];
        out->known.push_back(std::move(c));
      }
    }
  }

  // Built-in default aggregator: the health_reporter "noworker" type is
  // always known, independent of whether --derive-template wrote a
  // known_types.txt row for it. Injection in main.cpp appends the node by
  // default; this row lets every emit stage resolve the type even when the
  // project's known_types.txt predates the default-injection feature.
  {
    bool have = false;
    for (const auto& k : out->known) {
      if (k.type == "health_reporter") { have = true; break; }
    }
    if (!have) {
      KnownCat c;
      c.type = "health_reporter";
      c.cls = "HealthReporterThread";
      c.member = "reporter";
      c.ctor = "HEALTH";
      c.flags = "noworker";
      out->known.push_back(std::move(c));
    }
  }

  // contracts.txt: type|channel|is_out(0/1)
  {
    const fs::path p = base / "contracts.txt";
    std::error_code ec;
    if (fs::exists(p, ec)) {
      std::string body;
      if (!gen_util::ReadFile(p, &body)) {
        std::fprintf(stderr, "LoadCatalog: cannot read %s\n", p.c_str());
        return false;
      }
      std::istringstream ls(body);
      std::string raw;
      while (std::getline(ls, raw)) {
        const std::string t = gen_util::Trim(raw);
        if (t.empty() || t[0] == '#') continue;
        const auto f = SplitPipe(t);
        if (f.size() < 3) {
          std::fprintf(stderr, "LoadCatalog: bad contracts.txt line: %s\n", raw.c_str());
          return false;
        }
        ContractCat c;
        c.type = f[0];
        c.channel = f[1];
        bool is_out = false;
        if (!ParseBool(f[2], &is_out)) {
          std::fprintf(stderr, "LoadCatalog: bad is_out in contracts.txt: %s\n", raw.c_str());
          return false;
        }
        c.is_out = is_out;
        out->contracts.push_back(std::move(c));
      }
    }
  }

  // integration_hints.txt: type|sdk_label|step1;step2;...  (project-specific SDK notes)
  {
    const fs::path p = base / "integration_hints.txt";
    std::error_code ec;
    if (fs::exists(p, ec)) {
      std::string body;
      if (!gen_util::ReadFile(p, &body)) {
        std::fprintf(stderr, "LoadCatalog: cannot read %s\n", p.c_str());
        return false;
      }
      std::istringstream ls(body);
      std::string raw;
      while (std::getline(ls, raw)) {
        const std::string t = gen_util::Trim(raw);
        if (t.empty() || t[0] == '#') continue;
        const auto f = SplitPipe(t);
        if (f.size() < 2) {
          std::fprintf(stderr, "LoadCatalog: bad integration_hints.txt line: %s\n",
                       raw.c_str());
          return false;
        }
        IntegrationHintCat h;
        h.type = f[0];
        h.sdk_label = f[1];
        if (f.size() >= 3) h.checklist = f[2];
        out->integration_hints.push_back(std::move(h));
      }
    }
  }

  // shm_kinds.txt: cpp_type|kind
  {
    const fs::path p = base / "shm_kinds.txt";
    std::error_code ec;
    if (fs::exists(p, ec)) {
      std::string body;
      if (!gen_util::ReadFile(p, &body)) {
        std::fprintf(stderr, "LoadCatalog: cannot read %s\n", p.c_str());
        return false;
      }
      std::istringstream ls(body);
      std::string raw;
      while (std::getline(ls, raw)) {
        const std::string t = gen_util::Trim(raw);
        if (t.empty() || t[0] == '#') continue;
        const auto f = SplitPipe(t);
        if (f.size() < 2) {
          std::fprintf(stderr, "LoadCatalog: bad shm_kinds.txt line: %s\n", raw.c_str());
          return false;
        }
        ShmKindCat c;
        c.cpp_type = f[0];
        c.kind = f[1];
        if (!gen_util::IsIdent(c.cpp_type) || !gen_util::IsIdent(c.kind)) {
          std::fprintf(stderr, "LoadCatalog: invalid identifier in shm_kinds.txt: %s\n",
                       raw.c_str());
          return false;
        }
        out->shm_kinds.push_back(std::move(c));
      }
    }
  }

  // contract_sizes.txt: cpp_type|bytes — GEN-08 schema bridge written by
  // gen_contracts. `bytes` is the bare payload struct size (FrameHeader
  // excluded) in the natural layout. Missing file = no schema knowledge.
  {
    const fs::path p = base / "contract_sizes.txt";
    std::error_code ec;
    if (fs::exists(p, ec)) {
      std::string body;
      if (!gen_util::ReadFile(p, &body)) {
        std::fprintf(stderr, "LoadCatalog: cannot read %s\n", p.c_str());
        return false;
      }
      std::istringstream ls(body);
      std::string raw;
      while (std::getline(ls, raw)) {
        const std::string t = gen_util::Trim(raw);
        if (t.empty() || t[0] == '#') continue;
        const auto f = SplitPipe(t);
        if (f.size() != 2 || !gen_util::IsIdent(f[0])) {
          std::fprintf(stderr, "LoadCatalog: bad contract_sizes.txt line: %s\n",
                       raw.c_str());
          return false;
        }
        std::uint64_t v = 0;
        bool numeric = !f[1].empty();
        for (const char ch : f[1]) {
          if (ch < '0' || ch > '9') { numeric = false; break; }
          v = v * 10u + static_cast<std::uint64_t>(ch - '0');
          if (v > 0xFFFFFFFFu) { numeric = false; break; }
        }
        if (!numeric || v == 0) {
          std::fprintf(stderr,
                       "LoadCatalog: bad contract_sizes.txt bytes for %s: %s\n",
                       f[0].c_str(), f[1].c_str());
          return false;
        }
        if (FindContractSize(*out, f[0]) != nullptr) {
          std::fprintf(stderr,
                       "LoadCatalog: duplicate contract_sizes.txt row for %s\n",
                       f[0].c_str());
          return false;
        }
        out->contract_sizes.push_back(
            ContractSizeCat{f[0], static_cast<std::uint32_t>(v)});
      }
    }
  }

  return true;
}

const PoolCat* FindPool(const TemplateCatalog& c, const std::string& pool) {
  for (const auto& p : c.pools) if (p.type == pool) return &p;
  return nullptr;
}

const KnownCat* FindKnown(const TemplateCatalog& c, const std::string& type) {
  for (const auto& k : c.known) if (k.type == type) return &k;
  return nullptr;
}

const IntegrationHintCat* FindIntegrationHint(const TemplateCatalog& c,
                                              const std::string& type) {
  for (const auto& h : c.integration_hints)
    if (h.type == type) return &h;
  return nullptr;
}

bool HasInc(const std::string& tpl_dir, const std::string& type) {
  const fs::path p = fs::path(tpl_dir) / (type + "_generated.h.inc");
  std::error_code ec;
  return fs::exists(p, ec);
}

bool ExtraBufListed(const std::string& csv, const std::string& type) {
  std::size_t start = 0;
  for (std::size_t i = 0; i <= csv.size(); ++i) {
    if (i == csv.size() || csv[i] == ',') {
      if (csv.substr(start, i - start) == type) return true;
      start = i + 1;
    }
  }
  return false;
}
