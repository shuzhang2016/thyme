#pragma once
#include <cstdint>
#include <string>
#include <vector>
struct PoolCat { std::string type, prefix; bool index_pipe; };
struct KnownCat {
  std::string type, cls, member, ctor, flags;
};
struct ContractCat { std::string type, channel; bool is_out; };
struct IntegrationHintCat {
  std::string type;       // node type (matches graph.yaml nodes[].type)
  std::string sdk_label;  // e.g. "J3 Vision SDK"
  std::string checklist;  // semicolon-separated manual steps
};
struct ShmKindCat { std::string cpp_type, kind; };
// GEN-08: schema-derived bare payload size per contract type (FrameHeader
// excluded), loaded from contract_sizes.txt (written by gen_contracts). Used
// for the generation-time 64 KB Frame transport check and the generated
// framed-wire-size assert. Absent file = no schema knowledge (compile-time
// asserts in the generated app still apply).
struct ContractSizeCat {
  std::string cpp_type;
  std::uint32_t bytes = 0;
};
// One field from gen_contracts' contract_layout.h. Offsets are inside the
// bare payload; the recorded sample is Frame<T>, so the chassis emitter adds
// the aligned payload start.
struct LayoutFieldCat {
  std::string name;
  std::string type;
  std::uint32_t offset = 0;
  std::uint32_t size = 0;
  std::uint32_t count = 1;
};
struct ContractLayoutCat {
  std::string cpp_type;
  std::uint32_t payload_bytes = 0;
  std::uint32_t align = 1;
  std::vector<LayoutFieldCat> fields;
};
// Extra shared buffer available to nodes beyond pool/ring edges (catalog
// driven; replaces the hardcoded chassis/arctrl pair).
struct ExtraBufCat {
  std::string name;         // member/local name, e.g. "chassis"
  std::string cpp_type;     // e.g. "CanInChassisBuffer"
  std::string read_types;   // csv node types that may read it (latest slot)
  std::string write_types;  // csv node types that may write it
};
struct TemplateCatalog {
  std::string banner{"generated graph"};
  std::string contracts_include{"contracts/types.h"};
  // Include path of the gen_contracts JSON header (GEN-04). Empty = the
  // project has no contracts_json.h; viz_ser.h then uses the generic base64
  // envelope fallback. Loaded from contracts_json.txt (missing = absent;
  // empty content = derived as contracts_json.h next to contracts_include).
  std::string contracts_json_include;
  // ABI-09: include path of the gen_contracts field-layout table, loaded from
  // contracts_layout.txt (written by gen_contracts alongside
  // contract_sizes.txt). Empty = the project has no layout table; the
  // generated record table then declares the record manifest but no contract
  // lookup, and the recorder's sidecar says "layout unavailable".
  std::string contracts_layout_include;
  std::string mp_includes;  // raw text, may be empty
  std::string hz_comment;    // raw text from hz_comment.txt, may be empty
  std::vector<ExtraBufCat> extras;
  std::vector<PoolCat> pools;
  std::vector<KnownCat> known;
  std::vector<ContractCat> contracts;
  std::vector<IntegrationHintCat> integration_hints;
  std::vector<ShmKindCat> shm_kinds;
  std::vector<ContractSizeCat> contract_sizes;
  // Parsed from the header named by contracts_layout.txt when that file is
  // inside the template tree. Empty when the include path is absent or the
  // header is not shipped next to the template.
  std::vector<ContractLayoutCat> contract_layouts;
};
bool LoadCatalog(const std::string& tpl_dir, TemplateCatalog* out);
const PoolCat* FindPool(const TemplateCatalog& c, const std::string& pool);
const KnownCat* FindKnown(const TemplateCatalog& c, const std::string& type);
const IntegrationHintCat* FindIntegrationHint(const TemplateCatalog& c,
                                              const std::string& type);
// GEN-08: schema size row for a contract cpp_type (nullptr when the template
// has no contract_sizes.txt row for it). Header-inline so validation-only
// test targets can link validate.cpp without the catalog translation unit.
inline const ContractSizeCat* FindContractSize(const TemplateCatalog& c,
                                               const std::string& cpp_type) {
  for (const auto& s : c.contract_sizes)
    if (s.cpp_type == cpp_type) return &s;
  return nullptr;
}
inline const ContractLayoutCat* FindContractLayout(const TemplateCatalog& c,
                                                   const std::string& cpp_type) {
  for (const auto& row : c.contract_layouts)
    if (row.cpp_type == cpp_type) return &row;
  return nullptr;
}
bool HasInc(const std::string& tpl_dir, const std::string& type);  // <type>_generated.h.inc
// True if `type` appears in the csv list (ExtraBufCat read_types/write_types).
bool ExtraBufListed(const std::string& csv, const std::string& type);
