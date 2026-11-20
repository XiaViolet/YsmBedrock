#include "ModelPack.h"
#include "ModelResourceLimits.h"
#include "DefaultAnimations.h"
#include "ContentHash.h"
#include "Molang.h"
#include "LegacyBundle.h"
#include "JavaVanillaInputs.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <cmath>
#include <cctype>
#include <functional>
#include <sstream>
#include <miniz.h>

namespace ysm {
static constexpr std::size_t MaxTotal = 256 * 1024 * 1024;
// Full-resolution player, PBR and replaced-entity textures share this budget.
// Keep encoded resources and decoded audio bounded by their existing limits.
static constexpr std::size_t MaxLoadedModel = 512 * 1024 * 1024;

// YSM 2.6.5 merges its loaded player groups in this order before filling
// defaults. Verified with conflicting pairs and both manifest key orders.
// fp_arm is loaded separately and never competes with body actions.
static constexpr char const* PlayerAnimationGroups[]{
    "fp_arm", "slashblade", "carryon", "tlm", "irons_spell_books", "immersive_melodies",
    "extra", "tac", "main", "swem", "arm", "parcool",
};

static std::filesystem::path utf8Path(std::string const& text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<char8_t const*>(text.data()), text.size()));
}

static std::string safeRelative(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name.front() == '/' || name.find(':') != name.npos || name.find('\0') != name.npos)
        throw std::runtime_error("Invalid resource path");
    for (auto const& part : utf8Path(name))
        if (part == "..") throw std::runtime_error("Resource path escapes model package");
    return name;
}

bool ModelPack::isSourceDirectory(std::filesystem::path const& path) {
    return std::filesystem::is_regular_file(path / "ysm.json")
        || (std::filesystem::is_regular_file(path / "main.json") && std::filesystem::is_regular_file(path / "arm.json"));
}

class Source {
    mz_zip_archive zip{};
    bool archive = false;
    std::optional<LegacyBundle> legacy;
    std::shared_ptr<LegacyFiles const> decodedV3;
    std::filesystem::path directory;
    std::string prefix;
    std::size_t total = 0;
    std::size_t resourceTotal = 0;
    std::map<std::string, std::shared_ptr<TextureImage const>> images;
    std::map<std::string, std::string> resourceHashes;
    std::map<std::string, mz_uint> zipFiles;
    bool roomInput = false;
    std::size_t roomJsonNodes{}, roomBones{}, roomPrimitives{};
    int zipIndex(std::string const& name) {
        auto path = prefix + name;
        if (auto found = zipFiles.find(path); found != zipFiles.end()) return static_cast<int>(found->second);
        // Older packages sometimes refer to a member with different casing.
        return mz_zip_reader_locate_file(&zip, path.c_str(), nullptr, 0);
    }
    std::filesystem::path directoryPath(std::string const& name) const {
        auto path = std::filesystem::weakly_canonical(directory / utf8Path(name));
        auto relative = path.lexically_relative(directory);
        if (relative.empty() || *relative.begin() == "..") throw std::runtime_error("Resource escapes model directory");
        return path;
    }
    void selectRoot(std::vector<std::string> const& paths) {
        std::set<std::string> members(paths.begin(), paths.end());
        std::vector<std::string> candidates;
        std::vector<std::string> rawRoots;
        for (auto const& name : paths) {
            if (name == "ysm.json" || name.ends_with("/ysm.json")) candidates.push_back(name);
            if (name == "main.json" || name.ends_with("/main.json")) {
                auto parent = name.substr(0, name.size() - 9);
                if (members.contains(parent + "arm.json")) rawRoots.push_back(name);
            }
        }
        if (candidates.empty()) candidates = std::move(rawRoots);
        if (candidates.size() != 1) throw std::runtime_error("Expected exactly one source model in archive (ysm.json or main.json and arm.json)");
        safeRelative(candidates.front());
        auto slash = candidates.front().find_last_of('/');
        if (slash != std::string::npos) prefix = candidates.front().substr(0, slash + 1);
    }
public:
    explicit Source(std::filesystem::path const& path, bool room = false) : roomInput(room) {
        if (std::filesystem::is_directory(path)) {
            directory = std::filesystem::weakly_canonical(path);
            if (!ModelPack::isSourceDirectory(directory)) {
                std::vector<std::filesystem::path> candidates;
                for (auto item = std::filesystem::recursive_directory_iterator(directory);
                    item != std::filesystem::recursive_directory_iterator(); ++item) {
                    if (!item->is_directory() || item->is_symlink() || !ModelPack::isSourceDirectory(item->path())) continue;
                    candidates.push_back(item->path());
                    item.disable_recursion_pending();
                }
                if (candidates.size() != 1) throw std::runtime_error("Expected exactly one source model (ysm.json or main.json and arm.json)");
                directory = candidates.front();
            }
        } else if (_wcsicmp(path.extension().c_str(), L".ysm") == 0) {
            if (isLegacyV3(path)) {
                decodedV3 = readLegacyV3(path);
                if (!decodedV3->contains("ysm.json")) throw std::runtime_error("Incomplete YSM v3 player model");
            } else {
                legacy.emplace(path);
                selectRoot(legacy->paths());
            }
        } else {
            // miniz accepts a FILE*, which preserves Windows Unicode paths.
            FILE* file = nullptr;
            if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
                throw std::runtime_error("Cannot open model archive");
            if (!mz_zip_reader_init_cfile(&zip, file, 0, 0)) {
                fclose(file);
                throw std::runtime_error("Invalid ZIP model archive");
            }
            archive = true;
            try {
                std::vector<std::string> paths;
                if (room && mz_zip_reader_get_num_files(&zip) > 4096) throw std::runtime_error("Room ZIP has too many entries");
                std::uint64_t expanded{};
                std::set<std::filesystem::path> seen;
                for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
                    mz_zip_archive_file_stat info{};
                    if (room) {
                        auto size = mz_zip_reader_get_filename(&zip, i, nullptr, 0);
                        if (!mz_zip_reader_file_stat(&zip, i, &info) || size < 2 || size > 1025
                            || info.m_is_encrypted || info.m_uncomp_size > MaxModelResourceFile
                            || info.m_uncomp_size > MaxTotal - expanded)
                            throw std::runtime_error("Invalid or oversized room ZIP entry");
                        expanded += info.m_uncomp_size;
                        std::string name(size, '\0');
                        if (mz_zip_reader_get_filename(&zip, i, name.data(), size) != size)
                            throw std::runtime_error("Cannot read room ZIP entry");
                        name.pop_back();
                        name = safeRelative(std::move(name));
                        std::ranges::transform(name, name.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
                        auto pathName = utf8Path(name).lexically_normal();
                        if (!seen.insert(pathName).second)
                            throw std::runtime_error("Duplicate room ZIP entry");
                        static std::set<std::wstring> const programs{L".exe", L".dll", L".bat", L".cmd", L".sys", L".ps1",
                            L".com", L".lnk", L".msi", L".vbs", L".scr", L".js", L".jar", L".mcaddon", L".mcpack"};
                        if (!info.m_is_directory && programs.contains(pathName.extension().wstring()))
                            throw std::runtime_error("Program or add-on inside room ZIP");
                    }
                    if (mz_zip_reader_file_stat(&zip, i, &info) && !info.m_is_directory) {
                        paths.emplace_back(info.m_filename);
                        if (paths.back().size() + 1 == sizeof(info.m_filename)) {
                            auto size = mz_zip_reader_get_filename(&zip, i, nullptr, 0);
                            if (!size) throw std::runtime_error("Cannot read ZIP member name");
                            paths.back().resize(size);
                            if (mz_zip_reader_get_filename(&zip, i, paths.back().data(), size) != size)
                                throw std::runtime_error("Cannot read ZIP member name");
                            paths.back().pop_back();
                        }
                        zipFiles.emplace(paths.back(), i);
                    }
                }
                selectRoot(paths);
            } catch (...) {
                mz_zip_reader_end(&zip);
                fclose(file);
                archive = false;
                throw;
            }
        }
    }
    ~Source() {
        if (archive) {
            auto* file = mz_zip_get_cfile(&zip);
            mz_zip_reader_end(&zip);
            if (file) fclose(file);
        }
    }
    Source(Source const&) = delete;
    Source& operator=(Source const&) = delete;
    std::vector<unsigned char> read(std::string const& input) {
        auto name = safeRelative(input);
        std::vector<unsigned char> bytes;
        if (decodedV3) {
            auto entry = decodedV3->find(name);
            if (entry == decodedV3->end()) throw std::runtime_error("Missing resource: " + name);
            bytes = entry->second;
        } else if (legacy) bytes = legacy->read(prefix + name, MaxModelResourceFile);
        else if (archive) {
            auto index = zipIndex(name);
            if (index < 0) throw std::runtime_error("Missing resource: " + name);
            mz_zip_archive_file_stat info{};
            if (!mz_zip_reader_file_stat(&zip, index, &info) || info.m_uncomp_size > MaxModelResourceFile)
                throw std::runtime_error("Resource too large or unreadable: " + name);
            bytes.resize(static_cast<std::size_t>(info.m_uncomp_size));
            if (!mz_zip_reader_extract_to_mem(&zip, index, bytes.data(), bytes.size(), 0))
                throw std::runtime_error("Cannot extract resource: " + name);
        } else {
            std::ifstream stream(directoryPath(name), std::ios::binary | std::ios::ate);
            if (!stream) throw std::runtime_error("Missing resource: " + name);
            auto size = stream.tellg();
            if (size < 0 || size > static_cast<std::streamoff>(MaxModelResourceFile)) throw std::runtime_error("Invalid resource size");
            bytes.resize(static_cast<std::size_t>(size));
            stream.seekg(0);
            if (!stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
                throw std::runtime_error("Cannot read resource: " + name);
        }
        if (bytes.size() > MaxTotal - resourceTotal || bytes.size() > MaxLoadedModel - total)
            throw std::runtime_error("Model package exceeds memory limit");
        resourceTotal += bytes.size();
        total += bytes.size();
        auto normalized = utf8Path(name).lexically_normal().generic_u8string();
        std::string resource(reinterpret_cast<char const*>(normalized.data()), normalized.size());
        auto digest = contentSha256(bytes);
        auto [found, inserted] = resourceHashes.emplace(resource, digest);
        if (!inserted && found->second != digest) throw std::runtime_error("Source resource changed while loading: " + resource);
        return bytes;
    }
    bool contains(std::string const& input) {
        auto name = safeRelative(input);
        if (decodedV3) return decodedV3->contains(name);
        if (legacy) return legacy->contains(prefix + name);
        if (archive) {
            auto index = zipIndex(name);
            return index >= 0 && !mz_zip_reader_is_file_a_directory(&zip, index);
        }
        return std::filesystem::is_regular_file(directoryPath(name));
    }
    bool packedV3() const { return static_cast<bool>(decodedV3); }
    void recordDirectoryMembers(std::map<std::string, std::filesystem::path>& out) const {
        out.clear();
        if (directory.empty()) return;
        for (auto const& [name, digest] : resourceHashes) out.emplace(name, directoryPath(name));
    }
    std::string localContentKey() const {
        // Length framing, sorted relative names and hashes of every consumed
        // asset make repacked ZIPs/folders equivalent, without coupling state
        // to selected texture, source filename or native rendering cache keys.
        std::string payload = "YsmBedrock.SourceContent.1\n";
        for (auto const& [name, digest] : resourceHashes)
            payload += std::to_string(name.size()) + ":" + name + ":" + digest + "\n";
        return "local1_" + contentSha256(std::span<unsigned char const>(
            reinterpret_cast<unsigned char const*>(payload.data()), payload.size()));
    }
    Json json(std::string const& path) {
        auto bytes = read(path);
        if (decodedV3) return Json::from_cbor(bytes);
        if (roomInput) return Json::parse(bytes.begin(), bytes.end(), [&](int depth, Json::parse_event_t, Json& value) {
            if (depth > 128 || ++roomJsonNodes > 6000000
                || (value.is_string() && value.get_ref<std::string const&>().size() > (1u << 20)))
                throw std::runtime_error("Room model JSON exceeds complexity limit");
            return true;
        }, true, true);
        return Json::parse(bytes.begin(), bytes.end(), nullptr, true, true);
    }
    Json geometry(std::string const& path) {
        auto document = json(path);
        if (roomInput) {
            for (auto const& model : document.at("minecraft:geometry")) {
                auto const& bones = model.at("bones");
                roomBones += bones.size();
                if (roomBones > 32768) throw std::runtime_error("Room model exceeds bone limit");
                std::map<std::string, std::string> parents;
                for (auto const& bone : bones) {
                    auto const& name = bone.at("name").get_ref<std::string const&>();
                    if (name.size() > 512) throw std::runtime_error("Room model bone name exceeds limit");
                    parents.emplace(name, bone.value("parent", ""));
                    if (auto cubes = bone.find("cubes"); cubes != bone.end()) roomPrimitives += cubes->size();
                    if (auto mesh = bone.find("poly_mesh"); mesh != bone.end())
                        for (auto key : {"positions", "normals", "uvs", "polys"})
                            if (auto member = mesh->find(key); member != mesh->end()) roomPrimitives += member->size();
                    if (roomPrimitives > 1000000) throw std::runtime_error("Room model exceeds geometry limit");
                }
                for (auto const& [name, parent] : parents) {
                    auto next = parents.find(parent);
                    for (unsigned depth = 1; next != parents.end() && !next->first.empty(); ++depth) {
                        if (depth > 256) throw std::runtime_error("Room model bone hierarchy too deep");
                        next = parents.find(next->second);
                    }
                }
            }
        }
        if (decodedV3) {
            // The V3 reader returns the original, top-origin Java vertex UVs.
            // Bedrock poly_mesh samples from the bottom of the texture.
            for (auto& model : document.at("minecraft:geometry")) {
                for (auto& bone : model.at("bones")) {
                    auto mesh = bone.find("poly_mesh");
                    if (mesh == bone.end()) continue;
                    for (auto& uv : mesh->at("uvs"))
                        uv[1] = 1.0 - uv.at(1).get<double>();
                }
            }
        }
        return document;
    }
    std::shared_ptr<TextureImage const> image(std::string const& path) {
        auto normalized = utf8Path(safeRelative(path)).lexically_normal().generic_u8string();
        std::string name(reinterpret_cast<char const*>(normalized.data()), normalized.size());
        if (auto found = images.find(name); found != images.end()) return found->second;
        auto bytes = read(name);
        TextureImage texture;
        if (decodedV3 && name.ends_with(".ysmrgba")) {
            if (bytes.size() < 8) throw std::runtime_error("Truncated YSM v3 image");
            auto read32 = [&](std::size_t at) {
                std::uint32_t result = 0;
                for (unsigned i = 0; i < 4; ++i) result |= std::uint32_t(bytes[at + i]) << (8 * i);
                return result;
            };
            texture.path = name;
            texture.width = read32(0);
            texture.height = read32(4);
            auto rgbaSize = std::uint64_t(texture.width) * texture.height * 4;
            if (!texture.width || !texture.height || texture.width > 4096 || texture.height > 4096
                || rgbaSize != bytes.size() - 8 || rgbaSize > MaxLoadedModel - total)
                throw std::runtime_error("Invalid YSM v3 image dimensions");
            texture.rgba.assign(bytes.begin() + 8, bytes.end());
            texture.format = "RGBA";
        } else texture = decodeTexture(name, std::move(bytes), MaxLoadedModel - total);
        auto image = std::make_shared<TextureImage const>(std::move(texture));
        total += image->rgba.size();
        images.emplace(name, image);
        return image;
    }
    std::string const& digestOf(std::string const& path) const {
        auto key = utf8Path(path).lexically_normal().generic_u8string();
        return resourceHashes.at(std::string(reinterpret_cast<char const*>(key.data()), key.size()));
    }
    std::vector<std::string> filesIn(std::string folder, std::string_view suffix, bool recursive = true) {
        if (!folder.empty()) folder = safeRelative(std::move(folder));
        while (folder.ends_with('/')) folder.pop_back();
        auto start = folder.empty() ? std::string{} : folder + "/";
        std::vector<std::string> candidates;
        auto append = [&](std::string name) {
            if (!recursive && name.find('/', start.size()) != std::string::npos) return;
            if (!suffix.empty()) {
                if (name.size() < suffix.size() || !std::equal(suffix.rbegin(), suffix.rend(), name.rbegin(),
                    [](char a, unsigned char b) { return a == std::tolower(b); })) return;
            }
            if (candidates.size() >= 4096) throw std::runtime_error("Too many resource files in " + folder);
            candidates.push_back(safeRelative(std::move(name)));
        };
        if (decodedV3) {
            for (auto const& [name, bytes] : *decodedV3)
                if (name.starts_with(start)) append(name);
        } else if (legacy) {
            for (auto const& name : legacy->paths())
                if (name.starts_with(prefix + start)) append(name.substr(prefix.size()));
        } else if (archive) {
            for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
                mz_zip_archive_file_stat info{};
                if (!mz_zip_reader_file_stat(&zip, i, &info) || info.m_is_directory) continue;
                std::string name = info.m_filename;
                if (name.starts_with(prefix + start)) append(name.substr(prefix.size()));
            }
        } else {
            auto root = std::filesystem::weakly_canonical(directory / utf8Path(folder));
            auto relative = root.lexically_relative(directory);
            if (relative.empty() || *relative.begin() == "..") throw std::runtime_error("Resource directory escapes model package");
            if (!std::filesystem::exists(root)) return {};
            if (!std::filesystem::is_directory(root)) throw std::runtime_error("Invalid resource directory: " + folder);
            auto appendFile = [&](auto const& file) {
                if (!file.is_regular_file()) return;
                auto name = file.path().lexically_relative(directory).generic_u8string();
                append(std::string(reinterpret_cast<char const*>(name.data()), name.size()));
            };
            if (recursive) for (auto const& file : std::filesystem::recursive_directory_iterator(root)) appendFile(file);
            else for (auto const& file : std::filesystem::directory_iterator(root)) appendFile(file);
        }
        struct Directory { std::vector<std::string> files; std::set<std::string> children; };
        std::map<std::string, Directory> tree;
        for (auto const& path : candidates) {
            auto relative = path.substr(start.size());
            auto parent = std::string{};
            std::size_t offset = 0;
            while (true) {
                auto separator = relative.find('/', offset);
                if (separator == std::string::npos) { tree[parent].files.push_back(path); break; }
                auto child = relative.substr(0, separator);
                tree[parent].children.insert(child);
                parent = child;
                offset = separator + 1;
            }
        }
        std::vector<std::string> result;
        std::function<void(std::string const&, unsigned)> visit = [&](auto const& directoryName, unsigned depth) {
            if (depth > 128) throw std::runtime_error("Resource directory nesting limit exceeded");
            auto& current = tree[directoryName];
            std::sort(current.files.begin(), current.files.end());
            result.insert(result.end(), current.files.begin(), current.files.end());
            for (auto const& child : current.children) visit(child, depth + 1);
        };
        visit("", 0);
        return result;
    }
};

void validateGeometry(Json const& document, std::size_t& boneCount, std::size_t& cubeCount) {
    auto const& geometry = document.at("minecraft:geometry").at(0);
    auto description = geometry.value("description", Json::object());
    if (!description.is_object()) throw std::runtime_error("Invalid geometry description");
    for (auto key : {"texture_width", "texture_height"}) {
        auto value = description.value(key, Json(64));
        if (!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>() < 0)
            throw std::runtime_error("Invalid geometry texture dimensions");
    }
    auto const& bones = geometry.at("bones");
    std::map<std::string, std::string> parents;
    for (auto const& bone : bones) {
        auto name = bone.at("name").get<std::string>();
        if (name.empty() || parents.contains(name)) throw std::runtime_error("Duplicate or empty bone: " + name);
        parents[name] = bone.value("parent", "");
        if (auto cubes = bone.find("cubes"); cubes != bone.end()) cubeCount += cubes->size();
    }
    for (auto const& [name, parent] : parents) {
        std::set<std::string> visited;
        auto current = name;
        while (!current.empty()) {
            if (!visited.insert(current).second) throw std::runtime_error("Bone hierarchy cycle: " + name);
            auto found = parents.find(current);
            if (found == parents.end()) throw std::runtime_error("Missing parent bone: " + current);
            current = found->second;
        }
    }
    boneCount += bones.size();
}

static Json legacyManifest(Source& source) {
    Json animations = Json::object();
    for (auto group : PlayerAnimationGroups) {
        auto filename = std::string(group) + ".animation.json";
        if (source.contains(filename)) animations[group] = std::move(filename);
    }
    Json textures = Json::array();
    bool arrowTexture = false;
    for (auto const& path : source.filesIn("", ".png", false)) {
        if (path == "arrow.png") arrowTexture = true;
        else textures.push_back(path);
    }
    Json player = {{"model", {{"main", "main.json"}, {"arm", "arm.json"}}},
        {"animation", std::move(animations)}, {"texture", std::move(textures)}};
    Json files = {{"player", std::move(player)}};
    if (source.contains("arrow.json") != arrowTexture) throw std::runtime_error("Legacy arrow model requires arrow.json and arrow.png");
    if (arrowTexture) {
        Json arrow = {{"match", {"minecraft:arrow"}}, {"model", "arrow.json"}, {"texture", "arrow.png"}};
        if (source.contains("arrow.animation.json")) arrow["animation"] = "arrow.animation.json";
        files["arrow"] = std::move(arrow);
    }
    Json metadata = Json::object();
    Json properties = Json::object();
    if (source.contains("info.json")) {
        auto info = source.json("info.json");
        if (!info.is_null() && !info.is_object()) throw std::runtime_error("Invalid legacy info.json");
        if (info.is_object()) {
            for (auto field : {"name", "tips"}) if (info.contains(field) && info[field].is_string()) metadata[field] = info[field];
            auto license = info.find("license");
            metadata["license"] = {{"type", license != info.end() && license->is_string() ? *license : Json("All Rights Reserved")}};
            auto authors = info.value("authors", Json::array());
            if (authors.is_array()) for (auto const& author : authors) if (author.is_string())
                metadata["authors"].push_back({{"name", author}});
            auto names = info.value("extra_animation_names", Json::array());
            if (names.is_array()) for (std::size_t i = 0; i < names.size(); ++i) if (names[i].is_string())
                properties["extra_animation"]["extra" + std::to_string(i)] = names[i];
            if (info.contains("free") && info["free"].is_boolean()) properties["free"] = info["free"];
        }
    }
    return {{"spec", 2}, {"metadata", std::move(metadata)}, {"properties", std::move(properties)}, {"files", std::move(files)}};
}

static Json readManifest(Source& source) {
    auto descriptor = source.contains("ysm.json") ? source.json("ysm.json") : legacyManifest(source);
    if (!descriptor.is_object()) throw std::runtime_error("Invalid model manifest");
    for (auto field : {"metadata", "properties"}) {
        if (!descriptor.contains(field) || descriptor[field].is_null()) descriptor[field] = Json::object();
        if (!descriptor[field].is_object()) throw std::runtime_error(std::string("Invalid model ") + field);
    }
    return descriptor;
}

static Json readModelLanguages(Source& source, Json const& descriptor, std::vector<std::string>* warnings = nullptr) {
    Json languages = Json::object();
    auto folder = std::string("lang");
    auto files = descriptor.find("files");
    if (files != descriptor.end() && files->is_object()) {
        auto path = files->find("language_path");
        if (path != files->end() && path->is_string()) {
            auto const& configured = path->get_ref<std::string const&>();
            if (configured.find_first_not_of(" \t\r\n") != configured.npos) folder = configured;
        }
    }
    std::replace(folder.begin(), folder.end(), '\\', '/');
    folder.erase(0, std::min(folder.size(), folder.find_first_not_of('/')));
    while (folder.ends_with('/')) folder.pop_back();
    auto report = [&](std::string const& path, std::exception const& error) {
        if (warnings && warnings->size() < 128) warnings->push_back("Language file " + path + ": " + error.what());
    };
    try {
        auto prefixSize = folder.empty() ? 0 : folder.size() + 1;
        for (auto const& path : source.filesIn(folder, ".json")) {
            try {
                auto strings = source.json(path);
                if (!strings.is_object()) throw std::runtime_error("Expected an object of translation keys");
                Json entries = Json::object();
                for (auto const& [key, value] : strings.items()) if (value.is_string()) entries[key] = value;
                auto locale = path.substr(prefixSize);
                locale.resize(locale.size() - 5);
                languages[locale] = std::move(entries);
            } catch (std::exception const& error) { report(path, error); }
        }
    } catch (std::exception const& error) { report(folder, error); }
    return languages;
}

std::string modelLocale(std::string locale) {
    for (auto& c : locale) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == '-') c = '_';
    }
    return locale;
}

Json const& modelLanguage(Json const& languages, std::string_view locale) {
    static Json const empty = Json::object();
    auto selected = languages.find(locale);
    if (selected != languages.end() && selected->is_object()) return *selected;
    // YSM 2.6.5 selects a whole language file. A missing key in an
    // existing locale falls back to the manifest field in the caller.
    auto fallback = languages.find("en_us");
    return fallback != languages.end() && fallback->is_object() ? *fallback : empty;
}

static Json catalogTranslations(Json const& languages) {
    Json result = Json::object();
    for (auto const& [locale, strings] : languages.items()) {
        Json fields = Json::object();
        for (auto const& [key, value] : strings.items())
            if (key == "metadata.name" || key == "metadata.tips" || key.starts_with("metadata.authors.")) fields[key] = value;
        result[locale] = std::move(fields);
    }
    return result;
}

Json ModelPack::uiInfo(std::filesystem::path const& source) {
    Source input(source);
    auto manifest = readManifest(input);
    return {{"metadata", manifest.at("metadata")}, {"languages", catalogTranslations(readModelLanguages(input, manifest))}};
}

Json ModelPack::catalogLanguages() const { return catalogTranslations(languages); }

ModelPack ModelPack::load(std::filesystem::path const& source) {
    return loadImpl(source, false, nullptr);
}

ModelPack ModelPack::loadRoom(std::filesystem::path const& source,
    std::map<std::string, std::filesystem::path>* directoryMembers) {
    return loadImpl(source, true, directoryMembers);
}

ModelPack ModelPack::loadImpl(std::filesystem::path const& source, bool roomInput,
    std::map<std::string, std::filesystem::path>* directoryMembers) {
    Source input(source, roomInput);
    ModelPack result;
    result.descriptor = readManifest(input);
    if (result.descriptor.value("spec", 0) != 2) throw std::runtime_error("Only YSM spec 2 source packages are supported");
    auto& properties = result.descriptor.at("properties");
    if (input.packedV3()) result.legacyOriginVersion = static_cast<std::uint32_t>(
        std::stoul(properties.at("originVer").get<std::string>()));
    auto& wheel = properties["extra_animation"];
    if (wheel.is_null() || (wheel.is_object() && wheel.empty())) {
        wheel = Json::object();
        for (int i = 0; i < 8; ++i) wheel["extra" + std::to_string(i)] = "";
    }
    auto const& player = result.descriptor.at("files").at("player");
    result.mainGeometry = input.geometry(player.at("model").at("main"));
    result.armGeometry = input.geometry(player.at("model").at("arm"));
    auto stem = source.stem().u8string();
    result.id = std::string(reinterpret_cast<char const*>(stem.data()), stem.size());
    validateGeometry(result.mainGeometry, result.boneCount, result.cubeCount);
    std::size_t armBones = 0, armCubes = 0;
    validateGeometry(result.armGeometry, armBones, armCubes);
    auto const geometryDescription = result.mainGeometry.at("minecraft:geometry").front().value("description", Json::object());
    auto scale = [&](char const* name, char const* legacyName) {
        auto value = properties.value(name, Json(0.7));
        if (value == 0.7 && geometryDescription.is_object() && geometryDescription.contains(legacyName))
            value = geometryDescription.at(legacyName);
        if (!value.is_number()) throw std::runtime_error("Invalid model scale");
        auto number = value.get<double>();
        if (!std::isfinite(number) || number <= 0 || number > 16) throw std::runtime_error("Invalid model scale");
        return number;
    };
    result.widthScale = scale("width_scale", "ysm_width_scale");
    result.heightScale = scale("height_scale", "ysm_height_scale");
    auto const& textures = player.at("texture");
    if (!textures.is_array() || textures.empty()) throw std::runtime_error("Missing player texture");
    auto blank = [](std::string const& value) { return value.find_first_not_of(" \t\r\n\f\v") == value.npos; };
    auto readTexture = [&](Json const& item, std::string const& fixedName) {
        if (!item.is_string() && !item.is_object()) throw std::runtime_error("Expected string or object for player texture");
        auto uv = item.is_string() ? item.get<std::string>() : item.value("uv", Json("")).get<std::string>();
        if (blank(uv)) throw std::runtime_error("Missing texture uv");
        ModelTexture texture;
        texture.uv = input.image(uv);
        auto basename = texture.uv->path.substr(texture.uv->path.find_last_of('/') + 1);
        texture.name = fixedName.empty() ? basename.substr(0, basename.find_last_of('.')) : fixedName;
        auto channel = [&](char const* name) -> std::shared_ptr<TextureImage const> {
            if (!item.is_object() || !item.contains(name) || item[name].is_null()) return {};
            if (!item[name].is_string()) throw std::runtime_error(std::string("Invalid texture ") + name);
            auto path = item[name].get<std::string>();
            return blank(path) ? nullptr : input.image(path);
        };
        texture.normal = channel("normal");
        texture.specular = channel("specular");
        return texture;
    };
    for (auto const& item : textures) {
        auto texture = readTexture(item, "");
        auto found = std::find_if(result.textures.begin(), result.textures.end(), [&](auto const& entry) { return entry.name == texture.name; });
        if (found == result.textures.end()) result.textures.push_back(std::move(texture));
        else *found = std::move(texture); // Upstream's texture-name map keeps the last declaration.
    }
    auto wanted = properties.value("default_texture", "");
    if (!wanted.empty() && !result.selectTexture(wanted))
        result.warnings.push_back("Default texture not found; using " + result.currentTexture().name + ": " + wanted);
    if (std::any_of(result.textures.begin(), result.textures.end(), [](auto const& t) { return t.normal || t.specular; })) {
        if (input.contains("texture.properties")) {
            auto bytes = input.read("texture.properties");
            std::istringstream lines(std::string(bytes.begin(), bytes.end()));
            for (std::string line; std::getline(lines, line);) {
                auto start = line.find_first_not_of(" \t\r");
                if (start == line.npos || line[start] == '#' || line[start] == '!') continue;
                auto split = line.find_first_of("=: \t", start);
                if (line.substr(start, split - start) != "format" || split == line.npos) continue;
                auto value = line.find_first_not_of("=: \t", split);
                auto format = value == line.npos ? "" : line.substr(value, line.find_last_not_of(" \t\r") - value + 1);
                result.pbrEncoding = format == "lab-pbr" || format == "lab-pbr/1.3" ? PbrEncoding::LabPbr13 : PbrEncoding::OldPbr;
                if (format.starts_with("lab-pbr/") && format != "lab-pbr/1.3")
                    result.warnings.push_back("Unrecognized PBR version " + format + "; using oldPBR mapping");
            }
        }
    }
    result.animations = Json::object();
    auto animations = player.value("animation", Json::object());
    if (!animations.is_object() && !animations.is_null()) throw std::runtime_error("Invalid player animation file map");
    for (auto const& [group, path] : animations.items()) {
        if (std::ranges::find(PlayerAnimationGroups, group) == std::end(PlayerAnimationGroups)) {
            auto name = path.get<std::string>();
            if (!input.contains(name)) throw std::runtime_error("Missing resource: " + name);
            continue;
        }
        auto document = input.json(path);
        result.animationCount += document.at("animations").size();
        result.animations[group] = std::move(document);
        if (group != "main" && group != "arm" && group != "fp_arm" && group != "extra")
            result.warnings.push_back("Java mod animation group not connected: " + group);
    }
    if (!result.animations.contains("main")) result.animations["main"] = {{"animations", Json::object()}};
    auto controllerFiles = player.value("animation_controllers", Json::array());
    if (controllerFiles.is_null()) controllerFiles = Json::array();
    if (!controllerFiles.is_array()) throw std::runtime_error("Invalid animation controller file list");
    for (auto const& path : controllerFiles) {
        auto document = input.json(path.get<std::string>());
        auto const& definitions = document.at("animation_controllers");
        if (!definitions.is_object()) throw std::runtime_error("Invalid animation controller document");
        for (auto const& [name, definition] : definitions.items()) {
            if (result.controllers["animation_controllers"].contains(name))
                throw std::runtime_error("Duplicate animation controller: " + name);
            result.controllers["animation_controllers"][name] = definition;
        }
    }
    auto readTargets = [&](ReplacedModelKind kind, char const* section, bool legacyArrow = false) {
        auto const& files = result.descriptor.at("files");
        auto entries = files.value(section, Json());
        if (entries.is_null()) return;
        Json list = Json::array();
        if (legacyArrow) list.push_back(entries);
        else if (entries.is_array()) list = std::move(entries);
        else if (entries.is_object()) {
            for (auto const& [id, entry] : entries.items()) {
                auto selected = entry;
                if (!selected.is_null()) {
                    if (!selected.is_object()) throw std::runtime_error("Invalid replaced model entry");
                    if (!selected.contains("match") || selected["match"].is_null()) selected["match"] = Json::array();
                    if (!selected["match"].is_array()) throw std::runtime_error("Invalid replaced model match list");
                    selected["match"].push_back(id);
                }
                list.push_back(std::move(selected));
            }
        } else throw std::runtime_error("Invalid replaced model list");
        if (list.size() > 512 || result.targets.size() + list.size() > 1024)
            throw std::runtime_error("Too many replaced model targets");
        std::size_t offset = 0;
        if (legacyArrow) {
            auto declared = files.value("projectiles", Json());
            if (declared.is_array() || declared.is_object()) offset = declared.size();
        }
        for (std::size_t i = 0; i < list.size(); ++i) {
            auto const& entry = list[i];
            if (entry.is_null()) continue;
            if (!entry.is_object()) throw std::runtime_error("Invalid replaced model entry");
            auto matches = entry.value("match", Json::array());
            if (legacyArrow && !entry.contains("match")) {
                matches = Json::array({"minecraft:arrow"});
                result.warnings.push_back("Legacy arrow without match uses minecraft:arrow");
            }
            if (matches.is_null() || (matches.is_array() && matches.empty())) continue;
            if (!matches.is_array() || matches.size() > 4096) throw std::runtime_error("Invalid replaced model match list");
            ReplacedModelTarget target;
            target.kind = kind;
            target.id = std::string(replacedModelKindName(kind)) + "/" + std::to_string(10000 + offset + i).substr(1);
            for (auto const& match : matches) {
                if (!match.is_string()) throw std::runtime_error("Invalid replaced model match");
                target.matches.push_back(match.get<std::string>());
            }
            target.geometry = input.geometry(entry.at("model").get<std::string>());
            validateGeometry(target.geometry, target.boneCount, target.cubeCount);
            auto resource = [&](char const* name) {
                auto found = entry.find(name);
                if (found == entry.end() || found->is_null()) return std::string{};
                if (!found->is_string()) throw std::runtime_error(std::string("Invalid replaced model ") + name);
                auto value = found->get<std::string>();
                return blank(value) ? std::string{} : value;
            };
            if (auto path = resource("animation"); !path.empty()) {
                target.animations = input.json(path);
                if (!target.animations.at("animations").is_object()) throw std::runtime_error("Invalid replaced animation document");
            }
            if (auto path = resource("controller"); !path.empty()) {
                target.controllers = input.json(path);
                if (!target.controllers.at("animation_controllers").is_object()) throw std::runtime_error("Invalid replaced controller document");
            }
            auto const& animations = target.animations.at("animations");
            auto const& controllers = target.controllers.at("animation_controllers");
            bool arrowEntry = kind == ReplacedModelKind::Vehicle && (animations.contains("air") || animations.contains("fire"))
                && std::all_of(target.matches.begin(), target.matches.end(), [](auto const& match) {
                    auto id = canonicalRegistryId(match);
                    return id == "minecraft:arrow" || id == "minecraft:spectral_arrow" || javaEntityId(id) == "minecraft:trident";
                })
                && controllers.empty();
            if (arrowEntry) {
                // Some old manifests place an arrow-model definition in the
                // vehicle map. Its projectile clips need the projectile stages.
                target.kind = ReplacedModelKind::Projectile;
                target.id = "projectile/" + std::to_string(11000 + i).substr(1);
                result.warnings.push_back("Recovered projectile from vehicle model entry: " + target.matches.front());
            }
            target.texture = readTexture(entry.at("texture"), "default");
            result.targets.push_back(std::move(target));
        }
    };
    readTargets(ReplacedModelKind::Projectile, "projectiles");
    readTargets(ReplacedModelKind::Projectile, "arrow", true);
    readTargets(ReplacedModelKind::Vehicle, "vehicles");
    auto functionPath = result.descriptor.at("files").value("function_path", "functions");
    if (functionPath.find_first_not_of(" \t\r\n") == std::string::npos) functionPath = "functions";
    while (functionPath.ends_with('/') || functionPath.ends_with('\\')) functionPath.pop_back();
    std::replace(functionPath.begin(), functionPath.end(), '\\', '/');
    auto normalizedFunctions = utf8Path(safeRelative(functionPath)).lexically_normal().generic_u8string();
    functionPath.assign(reinterpret_cast<char const*>(normalizedFunctions.data()), normalizedFunctions.size());
    if (functionPath == ".") functionPath.clear();
    std::size_t functionBytes = 0;
    for (auto const& path : input.filesIn(functionPath, ".molang")) {
        auto bytes = input.read(path);
        functionBytes += bytes.size();
        if (functionBytes > 16 * 1024 * 1024) throw std::runtime_error("Molang function library budget exceeded");
        auto name = path.substr(functionPath.empty() ? 0 : functionPath.size() + 1);
        name.resize(name.size() - 7);
        if (result.functions.contains(name)) throw std::runtime_error("Duplicate Molang function file: " + name);
        result.functions[name] = std::string(bytes.begin(), bytes.end());
    }
    auto soundPath = [](Json const& section) {
        auto it = section.find("sound_path");
        if (it == section.end() || it->is_null()) return std::string{};
        auto path = it->get<std::string>();
        return path.find_first_not_of(" \t\r\n") == path.npos ? std::string{} : path;
    };
    auto folder = soundPath(result.descriptor.at("files"));
    if (folder.empty()) folder = soundPath(player);
    if (folder.empty()) folder = "sounds";
    auto normalized = utf8Path(safeRelative(folder)).lexically_normal().generic_u8string();
    folder.assign(reinterpret_cast<char const*>(normalized.data()), normalized.size());
    while (folder.ends_with('/')) folder.pop_back();
    if (folder == ".") folder.clear();
    std::map<std::string, std::shared_ptr<ModelAudio const>> byDigest;
    std::size_t decodedSoundBytes = 0;
    for (auto const& path : input.filesIn(folder, "")) {
        auto bytes = input.read(path);
        auto const& digest = input.digestOf(path);
        auto& clip = byDigest[digest];
        if (!clip) {
            std::optional<AudioInfo> info;
            try { info = inspectAudio(bytes); }
            catch (std::runtime_error const& error) { throw std::runtime_error("Invalid raw sound " + path + ": " + error.what()); }
            if (!info) { result.warnings.push_back("Unknown audio: " + path); continue; }
            auto audio = std::make_shared<ModelAudio>(ModelAudio{*info, std::move(bytes), {}});
            if (info->opus) {
                audio->wave = decodeOpusWave(*audio, MaxTotal - decodedSoundBytes);
                decodedSoundBytes += audio->wave.size();
            }
            clip = std::move(audio);
        }
        auto name = path.substr(folder.empty() ? 0 : folder.size()+1);
        if (auto dot = name.rfind('.'); dot != name.npos) name.resize(dot);
        if (!result.sounds.emplace(name, clip).second) throw std::runtime_error("Duplicate raw sound name: " + name);
    }
    result.localContentKey = input.localContentKey();
    // Menu decoration is optional and has no bearing on existing animation
    // state keys. Load it after core resources, using the same asset budget.
    auto readDecoration = [&](char const* key) -> std::shared_ptr<TextureImage const> {
        auto found = properties.find(key);
        if (found == properties.end() || !found->is_string() || blank(found->get_ref<std::string const&>())) return {};
        try { return input.image(found->get_ref<std::string const&>()); }
        catch (std::exception const& error) { result.warnings.push_back(std::string(key) + ": " + error.what()); }
        return {};
    };
    result.previewBackground = readDecoration("gui_background");
    result.previewForeground = readDecoration("gui_foreground");
    // Presentation-only data is intentionally outside the animation/Roaming
    // content identity. Missing translations do not invalidate an author pack.
    result.languages = readModelLanguages(input, result.descriptor, &result.warnings);
    // Only models using per-bone surfaces need separate native surface passes.
    // Inspect compiled calls, so a quoted name or comment cannot opt a model in.
    auto visualCalls = [&](auto&& self, Json const& value) -> void {
        if (result.boneTransparency) return;
        if (value.is_string()) {
            try {
                MolangProgram program(value.get<std::string>());
                result.individualBoneMaterials |= program.hasBoneVisualActions();
                result.boneTransparency = program.hasBoneTransparency();
            } catch (std::exception const&) { /* The animation compiler reports invalid expressions. */ }
        }
        if (value.is_structured()) for (auto const& item : value) self(self, item);
    };
    for (auto const* file : {&result.animations, &result.controllers, &result.functions})
        visualCalls(visualCalls, *file);
    for (auto const* geometry : {&result.mainGeometry, &result.armGeometry})
        for (auto const& bone : geometry->at("minecraft:geometry").at(0).at("bones"))
            if (bone.at("name").get<std::string>().starts_with("ysmGlow")) result.individualBoneMaterials = true;
    for (auto& target : result.targets) {
        auto inspect = [&](auto&& self, Json const& value) -> void {
            if (value.is_string()) try {
                MolangProgram program(value.get<std::string>());
                target.individualBoneMaterials |= program.hasBoneVisualActions();
                target.boneTransparency |= program.hasBoneTransparency();
            } catch (std::exception const&) { /* Reported by the target animation compiler. */ }
            if (value.is_structured()) for (auto const& item : value) self(self, item);
        };
        for (auto const* value : {&target.animations, &target.controllers, &result.functions}) inspect(inspect, *value);
        for (auto const& bone : target.geometry.at("minecraft:geometry").at(0).at("bones"))
            target.individualBoneMaterials |= bone.at("name").get<std::string>().starts_with("ysmGlow");
    }
    result.geometryHash = result.hashRenderGeometry();
    if (directoryMembers) input.recordDirectoryMembers(*directoryMembers);
    return result;
}

Json ModelPack::bodyAnimations() const {
    auto result = animations.at("main");
    result["ysm_merge_multiline_expr"] = descriptor.at("properties").value("merge_multiline_expr", false);
    result["ysm_legacy_molang"] = true;
    result["ysm_origin_version"] = legacyOriginVersion ? Json(*legacyOriginVersion)
        : Json(descriptor.at("properties").value("originVer", ""));
    result["ysm_functions"] = functions;
    auto& body = result["animations"];
    body = Json::object();
    for (std::string_view group : PlayerAnimationGroups) {
        if (group == "fp_arm") continue;
        auto document = animations.find(group);
        if (document == animations.end()) continue;
        for (auto const& [name, clip] : document->at("animations").items()) body[name] = clip;
    }
    return withDefaultAnimations(std::move(result), false, mainGeometry);
}
Json ModelPack::firstPersonAnimations() const {
    auto result = animations.value("fp_arm", Json{{"animations", Json::object()}});
    result["ysm_merge_multiline_expr"] = descriptor.at("properties").value("merge_multiline_expr", false);
    result["ysm_legacy_molang"] = true;
    result["ysm_origin_version"] = descriptor.value("properties", Json::object()).value("originVer", "");
    result["ysm_functions"] = functions;
    return withDefaultAnimations(std::move(result), true);
}

Json ModelPack::renderGeometry() const {
    auto result = mainGeometry;
    auto& bones = result.at("minecraft:geometry").at(0).at("bones");
    std::set<std::string> names;
    for (auto const& bone : bones) names.insert(bone.at("name").get<std::string>());
    std::string root = "__ysm_bedrock_scale_root";
    while (names.contains(root)) root += "_";
    for (auto& bone : bones) if (!bone.contains("parent")) bone["parent"] = root;
    bones.push_back(Json{{"name", root}, {"pivot", {0, 0, 0}}});
    return result;
}

std::uint64_t ModelPack::hashRenderGeometry() const {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : renderGeometry().dump()) { hash ^= byte; hash *= 1099511628211ull; }
    for (unsigned char byte : armGeometry.dump()) { hash ^= byte; hash *= 1099511628211ull; }
    return hash;
}

std::string ModelPack::renderKey() const {
    // Geometry is immutable after loading. Texture changes only mix the
    // selected pixels and material into the prepared cache identity.
    auto hash = geometryHash;
    for (unsigned char byte : currentTexture().uv->encoded) { hash ^= byte; hash *= 1099511628211ull; }
    if (currentTexture().normal || currentTexture().specular) {
        auto mix = [&](unsigned char byte) { hash ^= byte; hash *= 1099511628211ull; };
        mix(static_cast<unsigned char>(pbrEncoding));
        for (auto const& image : {currentTexture().normal, currentTexture().specular}) {
            auto length = image ? static_cast<uint64_t>(image->encoded.size()) : 0;
            for (unsigned shift = 0; shift < 64; shift += 8) mix(static_cast<unsigned char>(length >> shift));
            if (image) for (auto byte : image->encoded) mix(byte);
        }
    }
    return (legacyOriginVersion ? "v9_" : "v8_") + std::to_string(hash)
        + (individualBoneMaterials ? "_vis" : "") + (boneTransparency ? "_opacity" : "");
}

Json ModelPack::summary() const {
    auto images = Json::array();
    auto image = [](std::shared_ptr<TextureImage const> const& data) -> Json {
        if (!data) return nullptr;
        return {{"path", data->path}, {"width", data->width}, {"height", data->height},
            {"encoded_bytes", data->encoded.size()}, {"rgba_bytes", data->rgba.size()}, {"format", data->format}};
    };
    for (auto const& texture : textures) {
        Json variant = {{"name", texture.name}, {"uv", image(texture.uv)},
            {"normal", image(texture.normal)}, {"specular", image(texture.specular)}};
        if (texture.normal) variant["normal"]["bedrock_first_pixel"] =
            convertPbrTexel(texture.normal->rgba.data(), pbrEncoding, PbrLayer::Normal);
        if (texture.specular) variant["specular"]["mers_first_pixel"] =
            convertPbrTexel(texture.specular->rgba.data(), pbrEncoding, PbrLayer::Material);
        images.push_back(std::move(variant));
    }
    Json audio = Json::object();
    for (auto const& [name, clip] : sounds) {
        auto const& info = clip->info;
        audio[name] = {{"codec", info.opus ? "opus" : "vorbis"}, {"channels", info.channels},
            {"sample_rate", info.rate}, {"frames", info.frames}, {"pre_skip", info.preSkip},
            {"output_gain", info.gain}, {"encoded_bytes", clip->encoded.size()}, {"decoded_wave_bytes", clip->wave.size()}};
    }
    auto replacements = Json::array();
    for (auto const& target : targets) replacements.push_back(target.summary());
    return Json{{"name", descriptor.value("metadata", Json::object()).value("name", "Unnamed")}, {"sounds", std::move(audio)},
        {"replaced_targets", std::move(replacements)},
        {"spec", 2}, {"main_bones", boneCount}, {"main_cubes", cubeCount},
        {"animation_clips", animationCount}, {"animation_controllers", controllers.at("animation_controllers").size()}, {"functions", functions.size()},
        {"texture_bytes", currentTexture().uv->encoded.size()}, {"texture_count", textures.size()},
        {"selected_texture", currentTexture().name}, {"textures", std::move(images)}, {"render_key", renderKey()},
        {"local_content_key", localContentKey},
        {"pbr_encoding", pbrEncoding == PbrEncoding::LabPbr13 ? "lab-pbr/1.3" : "oldPBR"},
        {"individual_bone_materials", individualBoneMaterials},
        {"warnings", warnings}};
}
bool ModelPack::selectTexture(std::string const& name) {
    for (std::size_t i = 0; i < textures.size(); ++i) if (textures[i].name == name) { selectedTexture = i; return true; }
    return false;
}
}
