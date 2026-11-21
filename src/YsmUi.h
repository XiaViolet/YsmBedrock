#pragma once
#include "ModelPack.h"
#include "ModelPreview.h"
#include "Molang.h"
#include <cstdint>
#include <deque>

namespace ysm {
// Source-authored routes, compiled once per installation. UI text is never
// interpreted as a program and stale screens cannot address a new installation.
class ModelUiBindings {
    struct Form {
        std::string type;
        MolangProgram read, write;
        std::vector<MolangProgram> labels;
        double min = 0, max = 100, step = 1;
    };
    std::map<std::string, Form> forms;
    std::set<std::string> formVariables;
public:
    explicit ModelUiBindings(Json const& descriptor = Json::object());
    // Only scalar variables explicitly writable by this model's config forms
    // may be restored by a remote wearer snapshot.
    std::map<std::string,float> snapshotScalars(MolangContext const&) const;
    bool applyScalars(MolangContext&, std::map<std::string,float> const& previous, std::map<std::string,float> const& next) const;
    Json observe(MolangContext const& context) const;
    bool act(MolangContext& context, std::string const& route, double value, int label = -1) const;
    std::set<std::string> roamingFields() const;
};

struct UiRect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x+w && py < y+h; }
};
struct UiPaint { UiRect rect; std::uint32_t color; std::string text; float scale = 1; bool centered = false; float radius = 0; int wheelSector = -1; };
struct UiCommand { std::string kind, key; double value = 0; int label = -1; std::uint64_t generation = 0; };
struct LibraryPreview {
    std::string source, name, animation;
    Json languages = Json::object();
    std::vector<std::string> clips;
    std::size_t texture = 0;
    bool fixedRotation = false;
};
struct UiSnapshot {
    std::uint64_t generation = 0;
    std::string source, texture, message, locale{"en_us"};
    Json descriptor = Json::object(), language = Json::object(), catalog = Json::array();
    Json values = Json::object(), settings = Json::object();
    Json players = Json::array(), room = Json::object();
    std::vector<std::string> textures, clips;
    std::shared_ptr<LibraryPreview const> browsing;
    std::string browseLoading;
    bool enabled = false, loading = false, debugOverlay = false;
};
struct UiScene {
    std::vector<UiPaint> paint;
    std::optional<UiRect> preview;
    PreviewCamera previewCamera;
    std::string previewClip;
    bool animatePreview = false, libraryPreview = false, livePlayerPreview = false;
    std::size_t overlayStart = static_cast<std::size_t>(-1);
};
// Renderer-independent state machine, shared with the executable verifier.
class YsmUi {
public:
    enum class Page { Closed, Models, Textures, Info, Settings, Preview, Roulette, Forms, Players, Import };
private:
    struct Hit { UiRect rect; std::string id; };
    struct Range { UiRect rect; std::string id, route; double min, max, step; bool setting; };
    struct WheelEntry { std::string key, label, description, form; };
    struct PreviewDrag { int button; float x, y, side; };
    UiSnapshot data;
    Page page = Page::Closed;
    std::vector<Hit> hits;
    std::vector<Range> ranges;
    std::optional<Range> dragging;
    std::optional<UiRect> previewArea;
    std::optional<PreviewDrag> previewDrag;
    std::string focusId;
    std::deque<UiCommand> commands;
    std::vector<std::pair<std::string, std::size_t>> categories{{"", 0}};
    std::string formButton, search, previewClip;
    bool searchFocused = false, favoritesOnly = false;
    std::size_t listPage = 0, texturePage = 0, formPage = 0, infoPage = 0, previewPage = 0, radioPage = 0;
    std::size_t playerPage = 0;
    bool observersOpen = false;
    float mouseX = -1, mouseY = -1;
    PreviewCamera previewCamera;
    UiRect wheelBounds;
    int wheelHovered = -1;
    std::vector<WheelEntry> wheelEntries() const;
    std::string local(std::string const& key, std::string fallback) const;
    void command(std::string kind, std::string key = {}, double value = 0, int label = -1);
    void activate(std::string const& id);
    void chooseWheel(std::size_t slot);
    void back();
    void setRange(Range const& range, double value);
public:
    void update(UiSnapshot value);
    void open(bool wheel = false);
    void close();
    bool isOpen() const { return page != Page::Closed; }
    bool isWheel() const { return page == Page::Roulette || page == Page::Forms; }
    bool blocksMovement() const;
    Page currentPage() const { return page; }
    bool key(int virtualKey, bool down, bool control = false, std::string text = {}, bool shift = false);
    bool mouse(int button, int value, float x, float y);
    void cancelPointer();
    UiScene draw(float width, float height, float hotbarTop = -1.f);
    std::vector<UiCommand> takeCommands();
    Json report() const;
    static Json defaultSettings();
};
}
