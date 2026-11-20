#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace ysm {
// Native orientation arrays can be rebuilt or reordered when skins change.
// Only the prepared model bindings have a stable lifetime.
template<class Binding>
class NativePoseLayout {
    struct Entry {
        std::string name;
        Binding const* binding = nullptr;
    };
    std::vector<Entry> entries;
public:
    template<class NameAt, class Resolve>
    bool bind(std::size_t count, NameAt nameAt, Resolve resolve) {
        entries.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            auto const& name = nameAt(i);
            auto& entry = entries[i];
            if (entry.binding && entry.name == name) continue;
            entry.binding = resolve(name);
            if (!entry.binding) return false;
            entry.name = name;
        }
        return true;
    }

    Binding const& operator[](std::size_t index) const { return *entries[index].binding; }
};
}
