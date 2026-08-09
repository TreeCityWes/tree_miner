// Single defining translation unit for the contract's to_string declarations.
// Owned by the integration lead; linked once into the miner and once into each test binary
// that needs it (journal and submit components must not define these).

#include "Types.h"

namespace treeminer {

const char* to_string(FindStatus s) {
    switch (s) {
        case FindStatus::Pending:             return "Pending";
        case FindStatus::Submitting:          return "Submitting";
        case FindStatus::AcceptedUnconfirmed: return "AcceptedUnconfirmed";
        case FindStatus::Acked:               return "Acked";
        case FindStatus::ParkedDifficulty:    return "ParkedDifficulty";
        case FindStatus::ParkedXuniWindow:    return "ParkedXuniWindow";
        case FindStatus::Quarantined:         return "Quarantined";
        case FindStatus::Dead:                return "Dead";
        case FindStatus::PermanentlyInvalid:  return "PermanentlyInvalid";
    }
    return "Unknown";
}

const char* to_string(FindKind k) {
    switch (k) {
        case FindKind::XEN11: return "XEN11";
        case FindKind::XUNI:  return "XUNI";
    }
    return "Unknown";
}

}  // namespace treeminer
