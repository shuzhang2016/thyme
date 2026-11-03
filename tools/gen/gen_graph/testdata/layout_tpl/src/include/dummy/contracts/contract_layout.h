#pragma once
// ABI-09 fixture: minimal generated-style contract layout table for the GEN-08
// layout/size test. The real gen_contracts emitter writes the same shape with
// offsetof/sizeof static_asserts; here Reading is a one-field POD.
#include "dummy/contracts/stub_types.h"

#include "thyme/record/record_schema.h"

#include <cstddef>

namespace thyme_contract_layout {

inline constexpr thyme::schema::FieldLayout kReadingFields[] = {
    {"value", "uint32", 0u, 4u, 1u, 4u},
};

inline constexpr thyme::schema::ContractLayout kContracts[] = {
    {"Reading", 4u, 1u, 4u, kReadingFields},
};

inline constexpr std::size_t kContractCount =
    sizeof(kContracts) / sizeof(kContracts[0]);

}  // namespace thyme_contract_layout
