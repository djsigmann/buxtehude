#include "validate.hpp"

namespace buxtehude {

bool ValidateJSON(const json& j, const ValidationSeries& tests)
{
    for (auto& [ptr, pred] : tests) {
        if (!j.contains(ptr)) return false;
        if (!pred) continue;
        if (!pred(j[ptr])) return false;
    }

    return true;
}

}
