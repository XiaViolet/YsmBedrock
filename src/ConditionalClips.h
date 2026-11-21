#pragma once
#include <algorithm>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ysm {
// Index condition groups in author declaration order. Multiple tags can
// match one item, so sorting the candidates would change the selected clip.
class ConditionalClips {
    std::map<std::string, std::vector<std::string>> groups;
public:
    void add(std::string const& clip) {
        auto split = clip.find_first_of("$#");
        if (split == std::string::npos) return;
        auto& tags = groups[clip.substr(0, split)];
        if (clip[split] == '#') tags.push_back(clip);
    }
    bool contains(std::string const& group) const { return groups.contains(group); }
    std::string matchingTag(std::string const& group, std::span<std::string const> tags) const {
        if (tags.empty()) return {};
        auto found = groups.find(group);
        if (found == groups.end()) return {};
        for (auto const& clip : found->second) {
            auto tag = std::string_view(clip).substr(group.size() + 1);
            if (std::find(tags.begin(), tags.end(), tag) != tags.end()) return clip;
        }
        return {};
    }
};
}
