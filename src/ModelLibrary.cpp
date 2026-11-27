#include "Localization.h"
#include "ModelLibrary.h"
#include "LegacyBundle.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <cerrno>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <share.h>
#include <Windows.h>
#include <sys/stat.h>

namespace ysm {
namespace {
std::mutex loadedDirectoryMutex;
std::map<std::filesystem::path, std::string> loadedDirectoryKeys;
std::string utf8(std::filesystem::path const& path) {
    auto value = path.u8string();
    return {reinterpret_cast<char const*>(value.data()), value.size()};
}
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
void note(std::vector<std::string>& messages, std::string message) {
    if (messages.size() < 128) messages.push_back(std::move(message));
}
std::string text(Json const& object, char const* key) {
    auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}
std::optional<ModelFileStamp> fileStamp(std::filesystem::path const& path) {
    std::error_code error;
    auto modified = std::filesystem::last_write_time(path, error);
    if (error) return {};
    auto size = std::filesystem::file_size(path, error);
    if (error) return {};
    return ModelFileStamp{modified, size};
}
std::string authorName(Json const& metadata) {
    auto authors = metadata.find("authors");
    return authors != metadata.end() && authors->is_array() && !authors->empty()
        ? text(authors->front(), "name") : std::string{};
}
void displayMetadata(Json& row, std::string const& name, std::string const& author) {
    if (!name.empty()) row["name"] = name;
    if (!author.empty()) row["author"] = author;
}
}
ModelLibrary::ModelLibrary(std::filesystem::path models, std::filesystem::path selection)
    : directory(std::move(models)), preferenceFile(std::move(selection)) {
    try {
        if (!std::filesystem::exists(preferenceFile)) return;
        std::ifstream stream(preferenceFile, std::ios::binary | std::ios::ate);
        if (!stream || stream.tellg() < 0 || stream.tellg() > 1024 * 1024)
            throw std::runtime_error("Selection file exceeds 1 MiB or is unreadable");
        stream.seekg(0);
        auto input = Json::parse(stream);
        if (!input.is_object()) throw std::runtime_error("Selection must be an object");
        if (input.contains("textures") && (!input["textures"].is_object() || input["textures"].size() > 1024))
            throw std::runtime_error("Saved texture selections must be an object with at most 1024 sources");
        if (input.contains("legacy_textures") && (!input["legacy_textures"].is_object() || input["legacy_textures"].size() > 1024))
            throw std::runtime_error("Legacy texture selections must be an object with at most 1024 models");
        preferences = std::move(input);
    } catch (std::exception const& error) {
        note(preferenceDiagnostics, std::string("Cannot read saved selection: ") + error.what());
    }
}
std::vector<std::filesystem::path> ModelLibrary::localArchiveFiles(std::filesystem::path const& library) {
    std::vector<std::filesystem::path> archives;
    std::vector<std::string> diagnostics;
    for (auto& entry : scan(library, diagnostics))
        if (!entry.directory) archives.push_back(std::move(entry.path));
    return archives;
}
std::vector<std::filesystem::path> ModelLibrary::loadedDirectories(std::filesystem::path const& library, std::string const& key) {
    auto root = std::filesystem::weakly_canonical(library);
    std::vector<std::filesystem::path> found;
    std::scoped_lock lock(loadedDirectoryMutex);
    for (auto const& [path, id] : loadedDirectoryKeys) {
        if (id != key) continue;
        auto relative = path.lexically_relative(root);
        if (!relative.empty() && relative != "." && !relative.is_absolute() && *relative.begin() != "..") found.push_back(path);
    }
    return found;
}
std::vector<ModelLibrary::Entry> ModelLibrary::scan(std::filesystem::path const& directory, std::vector<std::string>& messages) {
    std::vector<Entry> result;
    if (!std::filesystem::exists(directory)) return result;
    auto root = std::filesystem::canonical(directory);
    std::size_t visited = 0;
    auto visit = [&](auto&& self, std::filesystem::directory_entry const& item, unsigned depth) -> void {
        if (++visited > 4096) throw std::runtime_error("Model directory entry budget exceeded (4096)");
        auto relativeName = item.path().lexically_relative(directory).generic_u8string();
        std::string source(reinterpret_cast<char const*>(relativeName.data()), relativeName.size());
        bool folder;
        try {
            auto resolved = std::filesystem::canonical(item.path());
            auto relative = resolved.lexically_relative(root);
            if (item.is_symlink() || relative.empty() || *relative.begin() == "..") {
                note(messages, "Skipping linked or external model source: " + source);
                return;
            }
            folder = item.is_directory();
            auto extension = lower(utf8(item.path().extension()));
            if (!folder && (!item.is_regular_file() || (extension != ".zip" && extension != ".ysm"))) return;
        } catch (std::exception const& error) {
            note(messages, "Cannot inspect model source " + source + ": " + error.what());
            return;
        }
        Entry entry{item.path(), std::move(source), utf8(item.path().stem()), folder};
        if (folder && !ModelPack::isSourceDirectory(item.path())) {
            if (depth == 16) {
                note(messages, "Model directory nesting limit reached: " + entry.source);
                return;
            }
            auto begin = result.size();
            for (auto const& child : std::filesystem::directory_iterator(item.path(), std::filesystem::directory_options::skip_permission_denied))
                self(self, child, depth + 1);
             
             
            if (depth == 1 && result.size() == begin + 1 && result.back().directory) {
                result.back() = std::move(entry);
                return;
            }
            if (depth > 1 || result.size() != begin) return;
        }
        result.push_back(std::move(entry));
        if (result.size() > 1024) throw std::runtime_error("Model source budget exceeded (1024)");
    };
    for (auto const& item : std::filesystem::directory_iterator(directory)) visit(visit, item, 1);
    std::sort(result.begin(), result.end(), [](Entry const& a, Entry const& b) {
        auto left = lower(a.source), right = lower(b.source);
        return left == right ? a.source < b.source : left < right;
    });
    return result;
}
ModelChoice ModelLibrary::prepare(Preparation mode, std::function<void(ModelPack const&)> const& validate, std::stop_token stop, std::string const& target, std::optional<ModelPack> imported) const {
    ModelChoice result;
    result.diagnostics = preferenceDiagnostics;
    if (stop.stop_requested()) { result.cancelled = true; return result; }
    try {
        auto entries = scan(directory, result.diagnostics);
        for (auto const& [source, shared] : sharedModels)
            entries.push_back({{}, source, source, false});
        std::map<std::string, ModelFileStamp> legacyV3Stamps;
        for (auto const& entry : entries) {
            if (stop.stop_requested()) { result.cancelled = true; return result; }
            if (auto source = sharedModels.find(entry.source); source != sharedModels.end()) {
                result.catalog.push_back({{"source", entry.source}, {"id", entry.id}, {"kind", "server"},
                    {"name", source->second.title}, {"author", gameText("服务器", "Server")}, {"status", "unloaded"}});
                continue;
            }
            auto kind = entry.directory ? "folder" : lower(utf8(entry.path.extension())).substr(1);
            Json row{{"source", entry.source}, {"id", entry.id}, {"kind", kind}, {"status", "unloaded"}};
            try {
                 
                 
                if (kind == "ysm" && isLegacyV3(entry.path)) {
                    if (auto stamp = fileStamp(entry.path)) {
                        legacyV3Stamps.emplace(entry.source, *stamp);
                        auto cached = archiveMetadata.find(entry.source);
                        if (cached != archiveMetadata.end() && cached->second.stamp == *stamp) {
                            displayMetadata(row, cached->second.name, cached->second.author);
                            row["languages"] = cached->second.languages;
                            result.archiveMetadata.emplace(entry.source, cached->second);
                        } else result.pendingCards.emplace(entry.source, *stamp);
                    }
                } else {
                    auto info = ModelPack::uiInfo(entry.path);
                    auto const& metadata = info.at("metadata");
                    displayMetadata(row, text(metadata, "name"), authorName(metadata));
                    row["languages"] = std::move(info.at("languages"));
                }
            } catch (std::exception const&) {   }
            result.catalog.push_back(std::move(row));
        }
        if (mode == Preparation::Catalog) return result;
        bool advance = mode == Preparation::Next;
        if (entries.empty()) { note(result.diagnostics, "No YSM source packages found"); return result; }
        auto preferred = text(preferences, "source");
        if (!preferred.empty()) {
            if (std::none_of(entries.begin(), entries.end(), [&](auto const& entry) { return entry.source == preferred; })) {
                note(result.diagnostics, "Saved model source is unavailable: " + preferred);
                preferred.clear();
            }
        } else {
            auto legacy = text(preferences, "model");
            std::vector<std::size_t> matches;
            for (std::size_t i = 0; i < entries.size(); ++i) if (entries[i].id == legacy) matches.push_back(i);
            if (matches.size() == 1) preferred = entries[matches.front()].source;
            else if (!legacy.empty()) note(result.diagnostics, matches.empty() ? "Saved model is unavailable: " + legacy : "Ambiguous legacy model selection: " + legacy);
        }
        std::vector<std::size_t> order;
        if (!target.empty()) {
            for (std::size_t i = 0; i < entries.size(); ++i) if (entries[i].source == target) order.push_back(i);
            if (order.empty()) { note(result.diagnostics, "Requested model source is unavailable: " + target); return result; }
        } else if (advance) {
            auto found = std::find_if(entries.begin(), entries.end(), [&](auto const& entry) { return entry.source == selectedSource; });
            std::size_t start = found == entries.end() ? 0 : (static_cast<std::size_t>(found - entries.begin()) + 1) % entries.size();
            for (std::size_t n = 0; n < entries.size(); ++n) {
                auto i = (start + n) % entries.size();
                if (entries[i].source != selectedSource) order.push_back(i);
            }
        } else {
            for (std::size_t i = 0; i < entries.size(); ++i) if (entries[i].source == preferred) order.push_back(i);
            for (std::size_t i = 0; i < entries.size(); ++i) if (entries[i].source != preferred) order.push_back(i);
        }
        for (auto index : order) {
            if (stop.stop_requested()) { result.cancelled = true; return result; }
            auto const& entry = entries[index];
            try {
                auto shared = sharedModels.find(entry.source);
                auto pack = imported && entry.source == target ? std::move(*imported)
                    : shared == sharedModels.end() ? ModelPack::load(entry.path) : shared->second.load();
                auto wanted = entry.source == preferred ? text(preferences, "texture") : std::string{};
                if (auto legacy = preferences.find("legacy_textures"); legacy != preferences.end()) {
                    auto value = legacy->find(entry.id);
                    if (value != legacy->end() && value->is_string()
                        && std::count_if(entries.begin(), entries.end(), [&](auto const& other) { return other.id == entry.id; }) == 1)
                        wanted = value->get<std::string>();
                }
                if (auto map = preferences.find("textures"); map != preferences.end() && map->is_object()) {
                    auto value = map->find(entry.source);
                    if (value != map->end() && value->is_string()) wanted = value->get<std::string>();
                }
                if (!wanted.empty() && !pack.selectTexture(wanted)) note(result.diagnostics, "Saved texture unavailable for " + entry.source + ": " + wanted);
                if (validate) validate(pack);
                if (stop.stop_requested()) { result.cancelled = true; return result; }
                auto const& metadata = pack.descriptor.at("metadata");
                auto name = text(metadata, "name"), author = authorName(metadata);
                auto& row = result.catalog[index];
                row["status"] = "ready";
                row["name"] = entry.id;
                displayMetadata(row, name, author);
                auto languages = pack.catalogLanguages();
                row["languages"] = languages;
                if (auto stamp = legacyV3Stamps.find(entry.source);
                    stamp != legacyV3Stamps.end() && fileStamp(entry.path) == stamp->second)
                    result.archiveMetadata.insert_or_assign(entry.source, CachedModelMetadata{stamp->second, name, author, std::move(languages)});
                result.source = entry.source;
                result.pendingCards.erase(entry.source);
                pack.librarySource = entry.source;
                if (entry.directory) {
                    std::error_code ec;
                    auto path = std::filesystem::weakly_canonical(entry.path, ec);
                    if (!ec) {
                        std::scoped_lock lock(loadedDirectoryMutex);
                        if (loadedDirectoryKeys.size() >= 512 && !loadedDirectoryKeys.contains(path))
                            loadedDirectoryKeys.erase(loadedDirectoryKeys.begin());
                        loadedDirectoryKeys[path] = pack.localContentKey;
                    }
                }
                result.pack = std::move(pack);
                return result;
            } catch (std::exception const& error) {
                result.archiveMetadata.erase(entry.source);
                result.pendingCards.erase(entry.source);
                result.catalog[index]["status"] = "invalid";
                result.catalog[index]["error"] = error.what();
                note(result.diagnostics, "Cannot load " + entry.source + ": " + error.what());
            }
        }
        note(result.diagnostics, advance ? "No other usable model found; current selection retained" : "No usable model found");
    } catch (std::exception const& error) {
        note(result.diagnostics, error.what());
    }
    return result;
}
ModelChoice ModelLibrary::initial(std::function<void(ModelPack const&)> validate, std::stop_token stop) const {
    return prepare(Preparation::Select, validate, stop);
}
ModelChoice ModelLibrary::next(std::function<void(ModelPack const&)> validate, std::stop_token stop) const {
    return prepare(Preparation::Next, validate, stop);
}
ModelChoice ModelLibrary::select(std::string const& source, std::function<void(ModelPack const&)> validate, std::stop_token stop) const {
    if (source.empty()) throw std::runtime_error("Empty model selection");
    return prepare(Preparation::Select, validate, stop, source);
}
ModelChoice ModelLibrary::browse(std::string const& source, std::stop_token stop) const {
    if (source.empty()) return prepare(Preparation::Catalog, {}, stop);
    return select(source, {}, stop);
}
ModelChoice ModelLibrary::importSource(std::filesystem::path const& input, std::stop_token stop) const {
    namespace fs = std::filesystem;
    if (input.empty()) throw std::runtime_error(gameText("请选择模型文件或源文件夹", "Select a model file or source folder"));
    fs::create_directories(directory);
    auto source = fs::canonical(input);
    auto root = fs::canonical(directory);
    bool folder = fs::is_directory(source);
    auto extension = lower(utf8(source.extension()));
    if (!folder && (!fs::is_regular_file(source) || (extension != ".zip" && extension != ".ysm")))
        throw std::runtime_error(gameText("支持 ZIP、YSM 和模型源文件夹", "Supported: ZIP, YSM and model source folders"));
    if (source.filename().empty())
        throw std::runtime_error(gameText("请选择具体模型", "Select a specific model"));
     
    for (auto parent = root; folder; parent = parent.parent_path()) {
        if (fs::equivalent(source, parent)) throw std::runtime_error(gameText("不能把模型库或其上级目录导入自身", "Cannot import the model library or its parent into itself"));
        if (parent == parent.root_path()) break;
    }
    auto validate = [&](fs::path const& path) {
        if (stop.stop_requested()) throw std::runtime_error(gameText("已取消导入", "Import cancelled"));
        if (folder && !ModelPack::isSourceDirectory(path)) {
            std::vector<std::string> messages;
             
            ModelLibrary staged(path.parent_path(), {});
            auto entries = scan(staged.directory, messages);
            if (std::none_of(entries.begin(), entries.end(), [&](auto const& entry) { return entry.path == path; }))
                throw std::runtime_error(gameText("请选择单个模型的源文件夹", "Select a single model source folder"));
        }
        return ModelPack::load(path);
    };
    if (fs::equivalent(source.parent_path(), root)) {
        return prepare(Preparation::Select, {}, stop, utf8(source.filename()), validate(source));
    }
    static std::atomic_uint64_t sequence{};
    fs::path stage;
    do {
        stage = root.parent_path() / (".ysm-import-" + std::to_string(_getpid()) + "-" + std::to_string(++sequence));
    } while (!fs::create_directory(stage));
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); }
    } cleanup{stage};
    auto copy = stage / source.filename();
    auto check = [&] { if (stop.stop_requested()) throw std::runtime_error(gameText("已取消导入", "Import cancelled")); };
    auto regularEntry = [](fs::path const& path) {
        auto attrs = GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error(gameText("无法读取模型或模型包含链接目录", "Cannot read model or it contains a linked directory"));
    };
    check();
    regularEntry(input);
    if (folder) {
        fs::create_directory(copy);
        for (auto const& entry : fs::recursive_directory_iterator(source)) {
            check(); regularEntry(entry.path());
            auto target = copy / entry.path().lexically_relative(source);
            if (entry.is_directory()) fs::create_directory(target);
            else if (entry.is_regular_file()) fs::copy_file(entry.path(), target);
            else throw std::runtime_error(gameText("模型目录包含无法导入的文件", "Model folder contains files that cannot be imported"));
        }
    } else fs::copy_file(source, copy);
    auto imported = validate(copy);
    check();
    auto name = source.filename();
    for (unsigned n = 2;; ++n) {
        auto dest = root / name;
        if (MoveFileExW(copy.c_str(), dest.c_str(), MOVEFILE_WRITE_THROUGH)) {
            imported.id = utf8(name.stem());
            return prepare(Preparation::Select, {}, stop, utf8(name), std::move(imported));
        }
        auto code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS && !fs::exists(dest))
            throw std::system_error(code, std::system_category(), "Cannot publish imported model");
        auto suffix = L" (" + std::to_wstring(n) + L")";
        name = folder ? source.filename().wstring() + suffix
                      : source.stem().wstring() + suffix + source.extension().wstring();
        check();
    }
}
void ModelLibrary::accept(ModelChoice const& choice) {
    if (!choice.pack || choice.source.empty() || choice.cancelled) throw std::runtime_error("Cannot accept an empty or cancelled model choice");
    if (std::none_of(choice.catalog.begin(), choice.catalog.end(), [&](auto const& entry) {
        return text(entry, "source") == choice.source && text(entry, "status") == "ready";
    })) throw std::runtime_error("Model choice has no prepared source");
    auto next = *this;
    next.selectedSource = choice.source;
    next.observe(choice);
    next.rememberTexture(*choice.pack);
    *this = std::move(next);
}
void ModelLibrary::observe(ModelChoice const& choice) {
    catalog = choice.catalog;
    archiveMetadata = choice.archiveMetadata;
    pendingCards = choice.pendingCards;
    diagnostics = choice.diagnostics;
}
std::optional<ModelCardRequest> ModelLibrary::takeUnreadCard() {
    if (pendingCards.empty()) return {};
    auto entry = pendingCards.extract(pendingCards.begin());
    return ModelCardRequest{entry.key(), directory / std::filesystem::u8path(entry.key()), entry.mapped()};
}
std::optional<CachedModelMetadata> ModelLibrary::readCard(ModelCardRequest const& request) {
    if (std::filesystem::is_symlink(request.path) || fileStamp(request.path) != request.stamp) return {};
    auto info = ModelPack::uiInfo(request.path);
    if (fileStamp(request.path) != request.stamp) return {};
    auto const& metadata = info.at("metadata");
    return CachedModelMetadata{request.stamp, text(metadata, "name"), authorName(metadata), std::move(info.at("languages"))};
}
void ModelLibrary::applyCard(ModelCardRequest const& request, CachedModelMetadata metadata) {
    if (fileStamp(request.path) != metadata.stamp) return;
    for (auto& row : catalog) {
        if (row.value("source", "") != request.source || row.value("status", "") != "unloaded") continue;
        displayMetadata(row, metadata.name, metadata.author);
        row["languages"] = metadata.languages;
        archiveMetadata.insert_or_assign(request.source, std::move(metadata));
        pendingCards.erase(request.source);
        break;
    }
}
void ModelLibrary::rememberTexture(ModelPack const& pack) {
    if (selectedSource.empty()) throw std::runtime_error("No selected model source");
    if (selectedSource.starts_with("server:")) return;
    auto next = preferences;
    if (!next.contains("textures")) next["textures"] = Json::object();
    auto retain = [&](Json& map, std::string const& key, std::string const& value) {
        if (map.contains(key)) return;
        if (map.size() >= 1024) throw std::runtime_error("Saved texture source budget exceeded (1024)");
        map[key] = value;
    };
     
     
    auto oldTexture = text(next, "texture"), oldSource = text(next, "source"), oldModel = text(next, "model");
    if (!oldTexture.empty()) {
        if (oldSource.empty() && !oldModel.empty()) {
            std::vector<std::string> matches;
            for (auto const& entry : catalog) if (text(entry, "id") == oldModel) matches.push_back(text(entry, "source"));
            if (matches.size() == 1) oldSource = matches.front();
            else {
                if (!next.contains("legacy_textures")) next["legacy_textures"] = Json::object();
                retain(next["legacy_textures"], oldModel, oldTexture);
            }
        }
        if (!oldSource.empty()) retain(next["textures"], oldSource, oldTexture);
    }
    retain(next["textures"], selectedSource, pack.currentTexture().name);
    next["source"] = selectedSource;
    next["model"] = pack.id;
    next["texture"] = pack.currentTexture().name;
    next["textures"][selectedSource] = pack.currentTexture().name;
    preferences = std::move(next);
}
void ModelLibrary::save() const {
    if (selectedSource.empty()) throw std::runtime_error("No selected model source to save");
    if (selectedSource.starts_with("server:")) return;
    auto bytes = preferences.dump(2) + '\n';
    if (bytes.size() > 1024 * 1024) throw std::runtime_error("Selection file exceeds 1 MiB");
    auto parent = preferenceFile.parent_path();
    if (parent.empty()) parent = ".";
    std::filesystem::create_directories(parent);
    static std::atomic<unsigned long long> sequence = 0;
    std::filesystem::path temporary;
    int descriptor = -1;
    for (int attempt = 0; attempt < 32; ++attempt) {
        temporary = parent / (".ysm-selection-" + std::to_string(_getpid()) + "-" + std::to_string(sequence++) + ".tmp");
        auto error = _wsopen_s(&descriptor, temporary.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
                              _SH_DENYRW, _S_IREAD | _S_IWRITE);
        if (!error) break;
        if (error != EEXIST) throw std::runtime_error("Cannot create temporary selection file");
    }
    if (descriptor < 0) throw std::runtime_error("Cannot reserve temporary selection file");
    try {
        if (_write(descriptor, bytes.data(), static_cast<unsigned>(bytes.size())) != static_cast<int>(bytes.size()) || _commit(descriptor))
            throw std::runtime_error("Cannot write selection file");
        auto closed = _close(descriptor);
        descriptor = -1;
        if (closed) throw std::runtime_error("Cannot close selection file");
        std::filesystem::rename(temporary, preferenceFile);
    } catch (...) {
        if (descriptor >= 0) _close(descriptor);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
Json ModelLibrary::report() const {
    return {{"selected_source", selectedSource}, {"catalog", catalog}, {"diagnostics", diagnostics}, {"preferences", preferences}};
}
}
