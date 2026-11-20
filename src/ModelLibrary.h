#pragma once
#include "ModelPack.h"
#include <functional>
#include <optional>
#include <stop_token>

namespace ysm {
struct ModelFileStamp {
    std::filesystem::file_time_type modified;
    std::uintmax_t size;
    bool operator==(ModelFileStamp const&) const = default;
};
struct CachedModelMetadata {
    ModelFileStamp stamp;
    std::string name, author;
    Json languages;
};
struct ModelCardRequest {
    std::string source;
    std::filesystem::path path;
    ModelFileStamp stamp;
};
struct ModelChoice {
    std::optional<ModelPack> pack;
    std::string source;
    Json catalog = Json::array();
    std::map<std::string, CachedModelMetadata> archiveMetadata;
    std::map<std::string, ModelFileStamp> pendingCards;
    std::vector<std::string> diagnostics;
    bool cancelled = false;
};
struct SharedModelSource {
    std::string title;
    std::function<ModelPack()> load;
};
using SharedModelCatalog = std::map<std::string, SharedModelSource>;
// Preparation owns only CPU data and never changes the current selection or
// preference file. The host accepts a choice after its native publication.
class ModelLibrary {
    enum class Preparation { Select, Next, Catalog };
    struct Entry { std::filesystem::path path; std::string source, id; bool directory; };
    std::filesystem::path directory, preferenceFile;
    Json preferences = Json::object(), catalog = Json::array();
    std::vector<std::string> preferenceDiagnostics, diagnostics;
    std::string selectedSource;
    std::map<std::string, CachedModelMetadata> archiveMetadata;
    std::map<std::string, ModelFileStamp> pendingCards;
    SharedModelCatalog sharedModels;
    static std::vector<Entry> scan(std::filesystem::path const& directory, std::vector<std::string>& messages);
    ModelChoice prepare(Preparation mode, std::function<void(ModelPack const&)> const& validate, std::stop_token stop, std::string const& target = {}, std::optional<ModelPack> imported = {}) const;
public:
    // Private lookup inputs. These paths are never included in a room advertisement.
    static std::vector<std::filesystem::path> localArchiveFiles(std::filesystem::path const& library);
    static std::vector<std::filesystem::path> loadedDirectories(std::filesystem::path const& library, std::string const& key);
    ModelLibrary(std::filesystem::path models, std::filesystem::path selection);
    void shared(SharedModelCatalog sources) { sharedModels = std::move(sources); }
    ModelChoice initial(std::function<void(ModelPack const&)> validate = {}, std::stop_token stop = {}) const;
    ModelChoice next(std::function<void(ModelPack const&)> validate = {}, std::stop_token stop = {}) const;
    ModelChoice select(std::string const& source, std::function<void(ModelPack const&)> validate = {}, std::stop_token stop = {}) const;
    // With no preview target, refresh only the catalog and its deferred card reads.
    ModelChoice browse(std::string const& source, std::stop_token stop = {}) const;
    // Validate a private copy before publishing it in the local catalog.
    ModelChoice importSource(std::filesystem::path const& source, std::stop_token stop = {}) const;
    void accept(ModelChoice const& choice);
    void observe(ModelChoice const& choice);
    std::optional<ModelCardRequest> takeUnreadCard();
    static std::optional<CachedModelMetadata> readCard(ModelCardRequest const& request);
    void applyCard(ModelCardRequest const& request, CachedModelMetadata metadata);
    void rememberTexture(ModelPack const& pack);
    void save() const;
    std::filesystem::path const& root() const { return directory; }
    std::string const& source() const { return selectedSource; }
    Json report() const;
};
}
