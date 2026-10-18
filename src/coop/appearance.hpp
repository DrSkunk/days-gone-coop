#pragma once

#include "menu.hpp"

namespace coop {

class Appearance {
public:
    void Apply(ue::UObject* component, const MenuSettings& settings);
    ue::UObject* BaseMaterial(ue::UObject* component, int slot, ue::UObject* current) const;
    void Clear() { entries_.clear(); }

private:
    struct Entry {
        ue::UObject* component = nullptr;
        ue::UObject* mesh = nullptr;
        int palette = -1;
        bool visible = true;
        std::vector<ue::UObject*> originals;
        std::vector<ue::UObject*> applied;
    };
    std::vector<Entry> entries_;
};

} // namespace coop
