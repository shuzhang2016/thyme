#include "scaffold.h"

#include "catalog.h"
#include "emit_internal.h"
#include "util.h"

#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Generated base a node's handwritten thread derives from: every
// <type>_generated.h.inc (known) and every unknown-type projection header
// defines <Cls>Generated, so cls + "Generated" holds for both paths.
std::string ThreadBase(const std::string& cls) { return cls + "Generated"; }

// Write src/nodes/<id>.h and .cpp for one node: an empty OnTick/OnProduce or
// OnLoop stub for cyclic nodes, or empty On<Channel> handler stubs (one per
// in-edge) for exec=event nodes. The same generic shape for known and unknown types alike
// (node templates under tpl_dir/nodes are no longer consulted). `force`
// overwrites existing files (reset to the empty stub); otherwise only missing
// files are written. Returns false on IO error.
bool ScaffoldNode(const fs::path& root, const Graph& g, const Node& n,
                  bool force, const TemplateCatalog& cat,
                  std::vector<std::string>* written) {
  const fs::path nodes_dir = root / "src" / "nodes";
  const fs::path h_path = nodes_dir / (n.id + ".h");
  const fs::path cpp_path = nodes_dir / (n.id + ".cpp");
  const KnownCat* known = FindKnown(cat, n.type);
  const std::string cls = n.shard_count > 1
                              ? gen_emit::HandThreadClassName(cat, n)
                              : known != nullptr
                                    ? known->cls
                                    : gen_emit::PascalCase(n.type) + "Thread";
  const bool event = (n.exec == "event");
  const std::string base = n.shard_count > 1
                               ? gen_emit::PascalCase(n.id) +
                                     (event && FindKnown(cat, n.type) != nullptr
                                          ? "EventGenerated"
                                          : "ThreadGenerated")
                               : ThreadBase(cls);

  std::ostringstream h;
  h << "#pragma once\n";
  h << "#include \"generated/" << n.id << "_generated.h\"\n";
  h << "class " << cls << " final : public " << base << " {\n";
  h << " public:\n  using " << base << "::" << base << ";\n";
  h << " protected:\n";
  std::ostringstream cpp;
  cpp << "#include \"nodes/" << n.id << ".h\"\n";
  if (event) {
    bool any = false;
    for (const auto& e : g.edges) {
      if (e.to != n.id) continue;
      any = true;
      const std::string sample_type =
          gen_emit::IsPinnedSlotDelivery(g, cat, e)
              ? gen_emit::PinnedSlotRefType(g, cat, e) + "::Sample"
              : gen_emit::FrameRefType(g, cat, e) + "::Sample";
      h << "  void On" << gen_util::PascalCase(e.channel) << "(const " << sample_type
        << "& v) override;\n";
      cpp << "void " << cls << "::On" << gen_util::PascalCase(e.channel) << "(const "
          << sample_type << "&) {}\n";
    }
    if (!any) h << "  // no in-edges: no handlers to override\n";
  } else if (n.produce == "many" &&
             gen_emit::HasTickEdge(g, cat, gen_emit::EdgesOf(g, n))) {
    h << "  bool OnProduce(const TickIn& in, TickOut* out) override;\n";
    cpp << "bool " << cls
        << "::OnProduce(const TickIn&, TickOut*) { return false; }\n";
  } else if (n.produce != "many" &&
             gen_emit::HasTickEdge(g, cat, gen_emit::EdgesOf(g, n))) {
    h << "  bool OnTick(const TickIn& in, TickOut* out) override;\n";
    cpp << "bool " << cls
        << "::OnTick(const TickIn&, TickOut*) { return false; }\n";
  } else {
    h << "  void OnLoop() override;\n";
    cpp << "void " << cls << "::OnLoop() {}\n";
  }
  h << "};\n";

  const bool h_exists = fs::exists(h_path);
  const bool cpp_exists = fs::exists(cpp_path);
  if (force) {
    if (h_exists) written->push_back(h_path.string());
    if (cpp_exists) written->push_back(cpp_path.string());
    if (!gen_util::WriteFile(h_path, h.str())) return false;
    if (!gen_util::WriteFile(cpp_path, cpp.str())) return false;
  } else {
    if (!h_exists) { if (!gen_util::WriteFile(h_path, h.str())) return false; }
    if (!cpp_exists) { if (!gen_util::WriteFile(cpp_path, cpp.str())) return false; }
  }
  return true;
}

// Copy a single template file to a destination, optionally substituting {{ID}}.
// force overwrites; otherwise only writes if the destination is missing.
// A missing template file is silently skipped (the generic template set
// does not ship a stub-types header).
bool CopyTpl(const fs::path& tpl, const fs::path& dst, bool force,
              bool subst_id, const std::string& id,
              std::vector<std::string>* written) {
  if (!force && fs::exists(dst)) return true;
  std::string body;
  if (!gen_util::ReadFile(tpl, &body)) return true;  // missing template -> skip
  if (subst_id) body = gen_util::ReplaceAll(body, "{{ID}}", id);
  if (force && fs::exists(dst)) written->push_back(dst.string());
  return gen_util::WriteFile(dst, body);
}

// Copy every regular file under tpl_sub into dst_sub preserving relative
// paths. force overwrites; otherwise only missing files. A missing source
// directory is a no-op (success).
bool CopyTree(const fs::path& tpl_sub, const fs::path& dst_sub, bool force,
               std::vector<std::string>* written) {
  if (!fs::exists(tpl_sub)) return true;
  for (const auto& ent : fs::recursive_directory_iterator(tpl_sub)) {
    if (!ent.is_regular_file()) continue;
    const fs::path rel = fs::relative(ent.path(), tpl_sub);
    const fs::path dst = dst_sub / rel;
    if (!force && fs::exists(dst)) continue;
    if (force && fs::exists(dst) && written != nullptr)
      written->push_back(dst.string());
    std::string body;
    if (!gen_util::ReadFile(ent.path(), &body)) return false;
    if (!gen_util::WriteFile(dst, body)) return false;
  }
  return true;
}

}  // namespace

// Materialize the effective template view as base ∪ overlay.
bool MergeTemplateDir(const std::string& base, const std::string& overlay,
                      const std::string& merged) {
  std::error_code ec;
  if (!fs::is_directory(overlay, ec)) return false;
  if (!CopyTree(base, merged, true, nullptr)) return false;
  if (!CopyTree(overlay, merged, true, nullptr)) return false;
  return true;
}

bool Scaffold(const std::string& root, const Graph& g,
              const std::string& update, bool force,
              const std::string& tpl_dir) {
  TemplateCatalog cat;
  if (!LoadCatalog(tpl_dir, &cat)) return false;
  const fs::path tpl_src = fs::path(tpl_dir) / "src";
  const fs::path r(root);
  std::vector<std::string> overwritten;

  const bool do_nodes = (update == "nodes" || update == "all");
  const bool do_app = (update == "app" || update == "all");
  const bool do_support_mp = (update == "all");

  if (do_nodes) {
    for (const auto& n : g.nodes) {
      if (!ScaffoldNode(r, g, n, force, cat, &overwritten)) return false;
    }
  }

  if (do_app) {
    if (!CopyTpl(tpl_src / "main.cpp", r / "src" / "main.cpp", force,
                 false, "", &overwritten)) return false;
    if (!CopyTpl(tpl_src / "CMakeLists.txt", r / "CMakeLists.txt", force,
                 false, "", &overwritten)) return false;
    if (!CopyTree(tpl_src / "include", r / "include", force, &overwritten))
      return false;
    if (!CopyTree(fs::path(tpl_dir) / "tools", r / "tools", force, &overwritten))
      return false;
    if (!CopyTree(fs::path(tpl_dir) / "config", r / "config", force, &overwritten))
      return false;
  }

  if (do_support_mp) {
    if (!CopyTree(tpl_src / "support", r / "src" / "support", force, &overwritten))
      return false;
    if (!CopyTree(tpl_src / "mp", r / "src" / "mp", force, &overwritten))
      return false;
  }

  if (force && !overwritten.empty()) {
    std::fprintf(stderr, "gen_graph: --force overwrote:\n");
    for (const auto& p : overwritten) std::fprintf(stderr, "  %s\n", p.c_str());
  }
  return true;
}
