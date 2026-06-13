#include "core/version.h"

namespace br::core {

const char* core_version() noexcept {
    // Single source of the human-readable version banner. The numeric triple
    // in version.h is the machine-checkable form.
    return "0.0.0";
}

}  // namespace br::core
