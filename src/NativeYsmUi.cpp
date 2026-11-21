#include "NativeYsmUi.h"
#include "PortraitCadence.h"
#include "NativeMenuPreview.h"
#include "NativePortraitParrots.h"
#include "NativeShoulderParrot.h"
#include "mc/client/player/LocalPlayer.h"
#include "AnimationRuntime.h"
#include "UiPointer.h"
#include "NativeMenuTouch.h"
#include "UiTouchDelivery.h"
#include "UiMesh.h"
#include "UiMouseCapture.h"
#include "UiTextAtlas.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/gui/GuiData.h"
#include "mc/client/gui/ScreenSizeData.h"
#include "mc/client/gui/TextMeasureData.h"
#include "mc/client/gui/CaretMeasureData.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/client/renderer/TextureGroup.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTexture.h"
#include "mc/deps/core_graphics/ImageBuffer.h"
#include "mc/deps/core/image/Image.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/gui/ScreenRenderer.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/common/client/renderer/helpers/MeshHelpers.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/core/renderer/RenderMaterialInfo.h"
#include "mc/deps/renderer/ShaderColor.h"
#include <Windows.h>
#include <mutex>
#include <future>
#include <chrono>
#include <cstring>
#include <array>
#include <cmath>
#include <utility>
#include <fstream>

namespace ysm {
namespace {
std::string utf8(std::wstring const& w) {
    if(w.empty())return {};auto n=WideCharToMultiByte(CP_UTF8,0,w.data(),static_cast<int>(w.size()),nullptr,0,nullptr,nullptr);
    std::string out(n,0);WideCharToMultiByte(CP_UTF8,0,w.data(),static_cast<int>(w.size()),out.data(),n,nullptr,nullptr);return out;
}
std::string typedText(int key,bool control) {
    if(control&&key=='V') {
        std::wstring value;
        if(OpenClipboard(nullptr)){if(auto data=GetClipboardData(CF_UNICODETEXT))if(auto* p=static_cast<wchar_t const*>(GlobalLock(data))){std::size_t n=0;while(n<256&&p[n])++n;value.assign(p,n);GlobalUnlock(data);}CloseClipboard();}
        return utf8(value);
    }
    if(control)return {};
    BYTE state[256]{};if(!GetKeyboardState(state))return {};state[key]|=0x80;
    wchar_t value[8]{};auto count=ToUnicodeEx(key,MapVirtualKeyW(key,MAPVK_VK_TO_VSC),state,value,8,4,GetKeyboardLayout(0));
    if(count<=0)return {};std::wstring text;for(int i=0;i<count;++i)if(value[i]>=32)text+=value[i];return utf8(text);
}
struct ClientPointer { UiPoint position; float width,height; };
HWND foregroundClientWindow() {
    HWND window=GetForegroundWindow();DWORD process=0;GetWindowThreadProcessId(window,&process);
    return process==GetCurrentProcessId()?window:nullptr;
}
bool isTouchMessage() {
    INPUT_MESSAGE_SOURCE source{};
    if (GetCurrentInputMessageSource(&source) && source.deviceType == IMDT_TOUCH) return true;
    // Windows identifies touch-promoted mouse messages with the tablet
    // signature and the touch bit (the same signature without it is a pen).
    return (static_cast<ULONG_PTR>(GetMessageExtraInfo()) & 0xffffff80u) == 0xff515780u;
}
std::optional<ClientPointer> clientPointer(HWND window, bool touch = false) {
    if (!window) return {};
    RECT area{};POINT point{};
    if (touch) {
        auto pos = GetMessagePos();
        point = {static_cast<short>(LOWORD(pos)),static_cast<short>(HIWORD(pos))};
    } else if (!GetCursorPos(&point)) return {};
    if(!GetClientRect(window,&area)||!ScreenToClient(window,&point)||area.right<=0||area.bottom<=0)return {};
    return ClientPointer{{static_cast<float>(point.x),static_cast<float>(point.y)},static_cast<float>(area.right),static_cast<float>(area.bottom)};
}
void restoreMouse(IClientInstance* client,bool restore) {
    if(restore&&client&&client->getLocalPlayer()&&client->getTopScreenName()=="hud_screen"&&foregroundClientWindow())client->grabMouse();
}
std::size_t drawMesh(ScreenContext& screen,std::span<UiVertex const> vertices) {
    if(vertices.empty())return 0;
    auto& material=ScreenRenderer::mScreenMaterials()[static_cast<int>(ScreenRenderer::ScreenMaterial::FillGradient)];
    if(!material.mRenderMaterialInfoPtr||!material.mRenderMaterialInfoPtr->mPtr)return 0;
    Tessellator& tessellator=screen.tessellator;ShaderColor& shader=screen.currentShaderColor;
    struct Restore {ShaderColor& shader;mce::Color previous;~Restore(){*shader.color=previous;shader.dirty=true;}} restore{shader,*shader.color};
    *shader.color=mce::Color(255,255,255);shader.dirty=true;
    OffscreenCaptureDescription capture;std::size_t draws=0;
    for(std::size_t start=0;start<vertices.size();start+=uiBatchVertices){auto count=std::min(uiBatchVertices,vertices.size()-start);
        tessellator.begin({},mce::PrimitiveMode::TriangleList,static_cast<int>(count),false);
        for(auto const& vertex:vertices.subspan(start,count)){
            auto color=vertex.color;tessellator.color(((color>>16)&255)/255.f,((color>>8)&255)/255.f,(color&255)/255.f,((color>>24)&255)/255.f);
            tessellator.vertex(vertex.x,vertex.y,0.f);
        }
        MeshHelpers::renderMeshImmediately(screen,tessellator,material,capture);++draws;
    }return draws;
}
}
struct NativeYsmUi::Impl {
    mutable std::mutex mutex;
    YsmUi ui;
    std::uint64_t menuSession = 0;
    bool menuReady = false;
    std::vector<int> openingKeys;
    UiFrameGate frames;
    UiMouseCapture mouseCapture;
    std::atomic<std::uint64_t> releaseRetries=0;
    std::shared_ptr<ModelPreview const> preview, candidate;
    std::weak_ptr<ModelPreview const> lastRenderer;
    using MenuFrame = std::variant<TextureImage, PreviewMesh>;
    NativeMenuPreview nativePreview;
    std::optional<PreviewMesh> meshFrame;
    std::uint64_t meshRevision = 0;
    bool nativeEnabled = true, lastNative = false;
    std::string nativeFailure;
    std::weak_ptr<ModelPreview const> failedRenderer;
    std::size_t failedTexture = 0;
    struct CandidateFrame {
        std::shared_ptr<ModelPreview const> owner;
        std::string clip;
        std::size_t index;
        bool native, live;
        std::future<MenuFrame> pixels;
    };
    std::optional<CandidateFrame> candidateFrame;
    bool primeCandidate = false;
    std::size_t candidateTexture = 0;
    bool capture=false;
    std::array<bool,4> gameButtons{},uiButtons{};
    std::size_t textureIndex=0;
    float width=0,height=0;
    UiPointer pointer;
    bool forceRelative = false;
    std::filesystem::path diagnostics;
    std::filesystem::path touchDiagnostics;
    std::filesystem::path mouseDiagnostics;
    struct MouseTrace {
        unsigned moves = 0, deltas = 0, gameChanges = 0, desktopChanges = 0, buttons = 0;
        std::optional<UiPoint> game, desktop;
        std::chrono::steady_clock::time_point nextWrite{};
        static void observe(std::optional<UiPoint>& last, unsigned& changes, UiPoint point) {
            if (last && (last->x != point.x || last->y != point.y)) ++changes;
            last = point;
        }
    } mouseTrace;
    HWND touchWindow{};
    bool promotedTouch = false, deliveredTouch = false;
    int reportedTouchFlags = -1;
    void reportTouch() {
        if (touchDiagnostics.empty()) return;
        auto info = NativeMenuTouch::stats();
        int flags = int(info.attached) | (int(info.pointerMessages != 0) << 1) |
            (int(info.touchMessages != 0) << 2) | (int(promotedTouch) << 3) | (int(deliveredTouch) << 4) |
            (int(info.pointerLookupFailed) << 5) | (int(info.touchReadFailed) << 6);
        if (flags == reportedTouchFlags) return;
        reportedTouchFlags = flags;
        std::ofstream(touchDiagnostics) << Json{{"session",menuSession},{"attached",info.attached},
            {"attach_error",info.attachError},{"wm_pointer_seen",info.pointerMessages!=0},
            {"wm_touch_seen",info.touchMessages!=0},{"promoted_mouse_seen",promotedTouch},
            {"native_contact_delivered",deliveredTouch},{"pointer_lookup_failed",info.pointerLookupFailed},
            {"touch_read_failed",info.touchReadFailed}}.dump(2);
    }
    std::uint64_t renderedFrames=0,skippedViews=0;
    std::chrono::steady_clock::time_point reportAt{};
    bool retireTexture=false;
    std::string lastClip;
    bool lastAnimatePreview=false;
    bool portrait = false;
    PortraitCadence portraitCadence;
    bool lastLive = false, lastLibrary = false;
    NativePortraitParrots portraitParrots;
    bool debugOverlay = false;
    float portraitX = .12f, portraitY = .82f, portraitSize = .4f, portraitYaw = 0;
    PreviewCamera lastCamera;
    float previewFrameScale = 1.f;
    std::size_t lastTextureIndex=static_cast<std::size_t>(-1);
    std::chrono::steady_clock::time_point previewStarted{};
    std::chrono::steady_clock::time_point opened=std::chrono::steady_clock::now(),lastPreview{};
    std::weak_ptr<mce::TextureGroup> group;
    std::unique_ptr<ResourceLocation> location;
    std::shared_ptr<BedrockTextureData const> texture;
    std::uint64_t imageId=0;
    UiTextAtlas textAtlas;
    std::uint64_t textKey=0;
    std::weak_ptr<mce::TextureGroup> textGroup;
    std::unique_ptr<ResourceLocation> textLocation;
    std::shared_ptr<BedrockTextureData const> textTexture;
    float textDensity=1;
    void clearTextTexture() {
        textTexture.reset();if(textLocation)if(auto owner=textGroup.lock())owner->unloadTexture(*textLocation,false);
        textLocation.reset();textGroup.reset();textKey=0;
    }
    void clearTexture() {
        meshFrame.reset();nativePreview.reset();
        texture.reset();if(location)if(auto owner=group.lock())owner->unloadTexture(*location,false);location.reset();group.reset();
    }
    void uploadPreview(IClientInstance& client, TextureImage const& bitmap) {
        auto textures = client.getTextureGroup();
        if (!textures) return;
        mce::Image image(bitmap.width, bitmap.height, 1, mce::ImageFormat::RGBA8Unorm, mce::ImageUsage::Data);
        image.resizeImageBytesToFitImageDescription();
        std::memcpy(image.mImageBytes.data(), bitmap.rgba.data(), bitmap.rgba.size());
        image.mAlphaUsage = mce::AlphaUsage::Transparent;
        auto key = std::make_unique<ResourceLocation>(Core::PathView("textures/ysm_ui/preview_" + std::to_string(++imageId)));
        auto uploaded = textures->uploadTexture(*key, cg::ImageBuffer(std::move(image))).mBedrockTextureData;
        clearTexture();
        texture = std::move(uploaded);
        location = std::move(key);
        group = textures;
        previewFrameScale = static_cast<float>(bitmap.width) / ModelPreview::BodyFramePixels;
    }
    bool closeUi() {
        mouseCapture.close();
        NativeMenuTouch::close();
        touchWindow = nullptr;
        ui.close();pointer.reset();retireTexture=true;
        return std::exchange(capture,false);
    }
};
NativeYsmUi::NativeYsmUi():impl(std::make_unique<Impl>()){}
NativeYsmUi::~NativeYsmUi(){impl->clearTexture();impl->clearTextTexture();}
std::optional<std::uint64_t> NativeYsmUi::updateHud(bool enabled, bool debugOverlay, Json const& settings,
    std::shared_ptr<ModelPreview const> preview, std::size_t textureIndex) {
    std::scoped_lock lock(impl->mutex);
    impl->textureIndex = textureIndex;
    if(impl->preview!=preview){impl->retireTexture=true;impl->lastPreview={};impl->lastTextureIndex=static_cast<std::size_t>(-1);impl->opened=std::chrono::steady_clock::now();}
    bool portrait = enabled && settings.value("show_hud_avatar", false);
    if (portrait != impl->portrait) {
        impl->retireTexture = true;
        if (!portrait) impl->portraitParrots = {};
    }
    bool native = settings.value("native_menu_preview", true);
    if (native != impl->nativeEnabled) { impl->failedRenderer.reset(); impl->nativeFailure.clear(); }
    impl->nativeEnabled = native;
    impl->portraitCadence.rate(settings.value("hud_fps",30));
    impl->portrait = portrait;
    impl->debugOverlay = debugOverlay;
    bool forceRelative = settings.value("relative_menu_pointer", false);
    if (std::exchange(impl->forceRelative, forceRelative) != forceRelative) impl->pointer.reset();
    if (portrait) {
        impl->portraitX = settings.value("hud_x", 12) / 100.0f;
        impl->portraitY = settings.value("hud_y", 82) / 100.0f;
        impl->portraitSize = settings.value("hud_size", 40) / 100.0f;
        impl->portraitYaw = settings.value("hud_yaw", 50) * 3.6f - 180.0f;
    }
    impl->preview = std::move(preview);
    if (impl->ui.isOpen()) return impl->menuSession;
    impl->candidate.reset();
    // Destroy an async result only after completion. A running future can block this tick.
    if (impl->candidateFrame && impl->candidateFrame->pixels.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        impl->candidateFrame.reset();
    return std::nullopt;
}
void NativeYsmUi::updateMenu(IClientInstance& client, std::uint64_t session, UiSnapshot snapshot, std::shared_ptr<ModelPreview const> candidate) {
    std::unique_lock lock(impl->mutex);
    if (!impl->ui.isOpen() || session != impl->menuSession) return;
    impl->candidateTexture = snapshot.browsing ? snapshot.browsing->texture : 0;
    if (impl->candidateFrame && impl->candidateFrame->owner != candidate && impl->candidateFrame->owner != impl->preview &&
        impl->candidateFrame->pixels.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        impl->candidateFrame.reset();
    impl->candidate = std::move(candidate);
    impl->ui.update(std::move(snapshot));
    impl->menuReady = true;
    for (int code : impl->openingKeys) {
        if (!impl->ui.isOpen()) break;
        impl->ui.key(code, true, false);
    }
    impl->openingKeys.clear();
    if (!impl->ui.isOpen()) {
        auto restore = impl->closeUi();
        lock.unlock();
        restoreMouse(&client, restore);
    }
}
void NativeYsmUi::toggle(IClientInstance& client,bool wheel) {
    bool restore=false,opening=false;
    if(!client.getLocalPlayer()||client.getTopScreenName()!="hud_screen")return;
    {std::scoped_lock lock(impl->mutex);
        if(impl->ui.isOpen())restore=impl->closeUi();
        else {impl->capture=client.getMouseGrabbed();impl->pointer.reset();impl->opened=std::chrono::steady_clock::now();++impl->menuSession;impl->menuReady=false;impl->openingKeys.clear();impl->ui.open(wheel);impl->mouseCapture.open(&client);opening=true;
            impl->promotedTouch = impl->deliveredTouch = false;
            impl->reportedTouchFlags = -1;
            impl->mouseTrace = {};
            // Install on the render thread once there is a visible menu. A
            // hook installed by a transient input worker dies with that worker.
            impl->touchWindow = nullptr;
        }
    }
    // Mouse capture may synchronously publish another input event. Never hold
    // the UI state lock across either native capture call.
    if(opening)client.releaseMouse();else restoreMouse(&client,restore);
}
void NativeYsmUi::close(IClientInstance* client){bool restore;{std::scoped_lock lock(impl->mutex);restore=impl->closeUi();}restoreMouse(client,restore);}
void NativeYsmUi::diagnostics(std::filesystem::path const& directory, bool debug) {
    std::scoped_lock lock(impl->mutex);
    impl->diagnostics = debug ? directory/"ui-input-status.json" : std::filesystem::path{};
    impl->touchDiagnostics = directory/"ui-touch-status.json";
    if (debug || std::filesystem::exists(directory/"mouse-diagnostics.enable"))
        impl->mouseDiagnostics = directory/"ui-mouse-status.json";
    else impl->mouseDiagnostics.clear();
}
bool NativeYsmUi::isOpen()const{std::scoped_lock lock(impl->mutex);return impl->ui.isOpen();}
bool NativeYsmUi::showsModels() const {
    std::scoped_lock lock(impl->mutex);
    return impl->ui.currentPage() == YsmUi::Page::Models;
}
bool NativeYsmUi::suppressMouseCapture(IClientInstance const& client){return impl->mouseCapture.suppress(&client);}
void NativeYsmUi::maintainMouse(IClientInstance& client) {
    NativeMenuTouch::retireIfIdle();
    // Catch capture acquired through a focus/input-mode transition before its
    // ordinary grabMouse request. Native calls always happen outside ui.mutex.
    if(impl->mouseCapture.owns(&client)&&client.getMouseGrabbed()) {
        ++impl->releaseRetries;client.releaseMouse();
    }
}
void NativeYsmUi::retireLibraryPreview() {
    std::optional<Impl::CandidateFrame> retired;
    {
        std::scoped_lock lock(impl->mutex);
        retired = std::exchange(impl->candidateFrame, std::nullopt);
        impl->candidate.reset();
    }
    if (retired) retired->pixels.wait();
}
void NativeYsmUi::beginFrame(){impl->frames.begin();}
bool NativeYsmUi::key(IClientInstance& client,int code,bool down,bool control) {
    bool restore=false,consumed=false;
    {std::scoped_lock lock(impl->mutex);if(!impl->ui.isOpen())return false;
        if (!impl->menuReady && down && ((code >= '1' && code <= '8') || code == VK_PRIOR || code == VK_NEXT)) {
            if (impl->openingKeys.size() < 16) impl->openingKeys.push_back(code);
            return true;
        }
        consumed=impl->ui.key(code,down,control,down?typedText(code,control):std::string{},(GetKeyState(VK_SHIFT)&0x8000)!=0);
        if(!impl->ui.isOpen())restore=impl->closeUi();
    }restoreMouse(&client,restore);return consumed;
}
bool NativeYsmUi::mouse(IClientInstance& client,int button,int value,float x,float y,float dx,float dy) {
    std::unique_lock lock(impl->mutex);
    bool touch = (impl->ui.isOpen() || NativeMenuTouch::waitingForRelease()) && isTouchMessage();
    if (touch) {
        auto pos = GetMessagePos();
        POINT physical{static_cast<short>(LOWORD(pos)),static_cast<short>(HIWORD(pos))};
        if (NativeMenuTouch::alreadyHandled(physical,DWORD(GetMessageTime()))) return true;
    }
    if(button>0&&button<4) {
        if(!value&&impl->gameButtons[button]) {impl->gameButtons[button]=false;return false;}
        if(!impl->ui.isOpen()) {
            if(!value&&impl->uiButtons[button]){impl->uiButtons[button]=false;return true;}
            impl->gameButtons[button]=value!=0;return false;
        }
        impl->uiButtons[button]=value!=0;
    }
    if(!impl->ui.isOpen())return false;
    if (!touch && !impl->mouseDiagnostics.empty()) {
        auto& trace = impl->mouseTrace;
        if (button == 0) {
            ++trace.moves;
            if (dx || dy) ++trace.deltas;
            trace.observe(trace.game,trace.gameChanges,{x,y});
        } else ++trace.buttons;
    }
    if (!impl->menuReady) return true;
    auto window = foregroundClientWindow();
    if (!window) return true;
    auto gui=client.getGuiData();auto const& size=*gui->mScreenSizeData;
    auto pw=size.clientScreenSize->x,ph=size.clientScreenSize->y;
    std::optional<UiPoint> native;
    if (touch) { impl->promotedTouch = true; impl->reportTouch(); }
    if(auto position=clientPointer(window,touch)){native=position->position;pw=position->width;ph=position->height;}
    auto point=touch ? impl->pointer.contact(native.value_or(UiPoint{x,y}),pw,ph,impl->width,impl->height)
                     : impl->pointer.feed(button,x,y,native,pw,ph,impl->width,impl->height,{dx,dy},impl->forceRelative);
    if(!point)return true;
    const bool traceClick = button == 1 && value && !impl->diagnostics.empty();
    Json before;
    if (traceClick) before = impl->ui.report();
    auto consumed=impl->ui.mouse(button,value,point->x,point->y);bool restore=false;
    if(traceClick) {
        try {std::ofstream(impl->diagnostics)<<Json{{"raw",{x,y}},{"pointer",{point->x,point->y}},{"client_pixels",{pw,ph}},{"gui_size",{impl->width,impl->height}},{"window_cursor",native.has_value()},{"before",before},{"after",impl->ui.report()}}.dump(2);}catch(std::exception const&){}
    }
    if(!impl->ui.isOpen())restore=impl->closeUi();lock.unlock();restoreMouse(&client,restore);return consumed;
}
std::vector<UiCommand> NativeYsmUi::takeCommands(){std::scoped_lock lock(impl->mutex);return impl->ui.takeCommands();}
void NativeYsmUi::render(MinecraftUIRenderContext& context, AnimationRuntime const& animation) {
    std::unique_lock lock(impl->mutex);
    if(impl->retireTexture){impl->clearTexture();impl->clearTextTexture();impl->retireTexture=false;}
    bool hudOnly = !impl->ui.isOpen();
    if (!hudOnly && !impl->menuReady) return;
    bool portraitOnly = hudOnly && impl->portrait && impl->preview;
    if (hudOnly && !portraitOnly && !impl->debugOverlay) return;
    if(!impl->frames.claim()){++impl->skippedViews;return;}
    IClientInstance& client=context.mClient;
    if(!client.getLocalPlayer()||client.getTopScreenName()!="hud_screen"){
        if (hudOnly) return;
        auto restore=impl->closeUi();lock.unlock();restoreMouse(&client,restore);return;
    }
    auto gui=client.getGuiData();auto const& size=*gui->mScreenSizeData;
    impl->width=size.clientUIScreenSize->x;impl->height=size.clientUIScreenSize->y;
    if(!std::isfinite(impl->width)||!std::isfinite(impl->height)||impl->width<=0||impl->height<=0)return;
    if(!hudOnly) {
        auto window = foregroundClientWindow();
        if (impl->touchWindow && window != impl->touchWindow) {
            impl->pointer.reset();
            impl->ui.cancelPointer();
            NativeMenuTouch::focusLost();
            NativeMenuTouch::close();
            impl->touchWindow = nullptr;
        }
        if (auto position=clientPointer(window)) {
            impl->textDensity=std::min(position->width/impl->width,position->height/impl->height);
            if (!impl->mouseDiagnostics.empty())
                impl->mouseTrace.observe(impl->mouseTrace.desktop,impl->mouseTrace.desktopChanges,position->position);
            if (!impl->pointer.usesTouch())
                if(auto point=impl->pointer.frame(position->position,position->width,position->height,impl->width,impl->height))
                    impl->ui.mouse(0,0,point->x,point->y);
        }
        if (window && !impl->touchWindow) {
            impl->touchWindow = window;
            NativeMenuTouch::open(window);
        }
        auto& trace = impl->mouseTrace;
        auto now = std::chrono::steady_clock::now();
        if (!impl->mouseDiagnostics.empty() && now >= trace.nextWrite) {
            trace.nextWrite = now + std::chrono::seconds(1);
            std::ofstream(impl->mouseDiagnostics) << Json{{"session",impl->menuSession},{"foreground",window!=nullptr},
                {"game_mouse_grabbed",client.getMouseGrabbed()},{"forced_compatibility",impl->forceRelative},
                {"pointer_source",impl->pointer.source()},{"game_motion_events",trace.moves},{"game_nonzero_deltas",trace.deltas},
                {"game_position_changes",trace.gameChanges},{"desktop_position_changes",trace.desktopChanges},
                {"game_button_events",trace.buttons}}.dump(2);
        }
    }
    UiScene scene;
    if (portraitOnly) {
        auto side = std::min(impl->width, impl->height) * (.08f + .30f * impl->portraitSize);
        scene.preview = UiRect{(impl->width - side) * impl->portraitX, (impl->height - side) * impl->portraitY, side, side};
        scene.previewCamera = PreviewCamera{impl->portraitYaw, 0};
        scene.animatePreview = true;
    } else if (!hudOnly) {
        auto const& bar = *gui->mHUDHotbarRectangle;
        scene = impl->ui.draw(impl->width, impl->height, bar._y0);
        auto touches = NativeMenuTouch::take();
        impl->deliveredTouch |= deliverMenuTouches(touches,impl->ui,impl->pointer,impl->width,impl->height);
        impl->reportTouch();
        if (!impl->ui.isOpen()) {
            auto restore = impl->closeUi();
            lock.unlock(); restoreMouse(&client,restore); return;
        }
        if (!touches.empty()) scene = impl->ui.draw(impl->width,impl->height,bar._y0);
    }
    if (hudOnly && impl->debugOverlay) {
        auto lines = animation.debugLines();
        if (!lines.empty()) {
            float panelWidth = std::min(180.0f, std::max(0.0f, impl->width - 12));
            scene.paint.push_back({{6,6,panelWidth,6 + 12.0f * lines.size()},0xdf15352d,{},1,false,4});
            for (std::size_t row = 0; row < lines.size(); ++row)
                scene.paint.push_back({{12,10 + 12.0f * row,panelWidth - 10,12},0xffe6f5eb,std::move(lines[row]),.88f});
        }
    }
    auto split=std::min(scene.overlayStart,scene.paint.size());
    auto mainMesh=buildUiMesh(std::span<UiPaint const>(scene.paint).first(split));
    auto cursorMesh=buildUiMesh(std::span<UiPaint const>(scene.paint).subspan(split));
    context.saveCurrentClippingRectangle();context.setFullClippingRectangle();
    struct RestoreClip {MinecraftUIRenderContext& context;~RestoreClip(){context.restoreSavedClippingRectangle();}} restoreClip{context};
    ScreenContext& screen=context.mScreenContext;
    auto nativeDraws=drawMesh(screen,mainMesh);
    auto textPaint=std::span<UiPaint const>(scene.paint).first(split);
    auto textKey=uiTextKey(textPaint,impl->textDensity);
    if(!textPaint.empty() && (textKey!=impl->textKey||!impl->textTexture)) {
        auto atlas=rasterizeUiText(textPaint,impl->textDensity);auto group=client.getTextureGroup();
        if(group) {
            mce::Image image(atlas.image.width,atlas.image.height,1,mce::ImageFormat::RGBA8Unorm,mce::ImageUsage::Data);
            image.resizeImageBytesToFitImageDescription();std::memcpy(image.mImageBytes.data(),atlas.image.rgba.data(),atlas.image.rgba.size());image.mAlphaUsage=mce::AlphaUsage::Transparent;
            auto location=std::make_unique<ResourceLocation>(Core::PathView("textures/ysm_ui/type_"+std::to_string(++impl->imageId)));
            auto& upload=group->uploadTexture(*location,cg::ImageBuffer(std::move(image)));auto texture=upload.mBedrockTextureData;
            impl->clearTextTexture();impl->textTexture=texture;impl->textLocation=std::move(location);impl->textGroup=group;impl->textAtlas=std::move(atlas);impl->textKey=textKey;
        }
    }
    if(!textPaint.empty() && impl->textTexture) {
        for(auto const& q:impl->textAtlas.quads)context.drawImage(*impl->textTexture->mClientTexture,{q.rect.x,q.rect.y},{q.rect.w,q.rect.h},{q.uv[0],q.uv[1]},{q.uv[2],q.uv[3]},false);
        context.flushImages(mce::Color(255,255,255),1.f,HashedString("ui_textured_and_glcolor"));
    }
    auto const& renderer = scene.libraryPreview ? impl->candidate : impl->preview;
    auto textureIndex = scene.libraryPreview ? impl->candidateTexture : impl->textureIndex;
    std::string_view previewBackend = "none";
    if(scene.preview && renderer) {
        bool native = !hudOnly && impl->nativeEnabled &&
            (impl->failedRenderer.lock() != renderer || impl->failedTexture != textureIndex);
        bool liveMode = portraitOnly || scene.livePlayerPreview;
        bool backendChanged = impl->lastNative != native || impl->lastLive != liveMode || impl->lastLibrary != scene.libraryPreview;
        impl->lastLive=liveMode;
        impl->lastLibrary=scene.libraryPreview;
        bool hasFrame = native ? impl->meshFrame.has_value() : bool(impl->texture);
        auto fallback = [&](std::exception const& error) {
            impl->failedRenderer = renderer;
            impl->failedTexture = textureIndex;
            impl->nativeFailure = error.what();
            impl->clearTexture();
        };
        impl->lastNative = native;
        auto now=std::chrono::steady_clock::now();
        bool changedClip=impl->lastRenderer.lock()!=renderer || impl->lastClip!=scene.previewClip||impl->lastTextureIndex!=textureIndex
            ||impl->lastAnimatePreview!=scene.animatePreview||!hasFrame||backendChanged;
        bool changedView=impl->lastCamera!=scene.previewCamera;
        auto elapsed=now-impl->lastPreview;
        if (scene.libraryPreview || scene.livePlayerPreview) {
            auto changed = impl->lastRenderer.lock() != renderer || impl->lastClip != scene.previewClip
                || impl->lastTextureIndex != textureIndex || !impl->lastAnimatePreview || backendChanged;
            if (changed) {
                impl->clearTexture();
                impl->primeCandidate = true;
                impl->previewStarted = now;
                impl->lastPreview = {};
                impl->portraitCadence.reset();
            }
            if (impl->candidateFrame && impl->candidateFrame->pixels.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto completed = std::move(*impl->candidateFrame);
                impl->candidateFrame.reset();
                if (completed.owner == renderer && completed.clip == scene.previewClip && completed.index == textureIndex &&
                    completed.native == native && completed.live == scene.livePlayerPreview && !impl->primeCandidate) {
                    if (native) {
                        try { impl->meshFrame = std::get<PreviewMesh>(completed.pixels.get()); ++impl->meshRevision; }
                        catch (std::exception const& error) { fallback(error); }
                    } else impl->uploadPreview(client, std::get<TextureImage>(completed.pixels.get()));
                }
            }
            bool due = scene.livePlayerPreview ? impl->portraitCadence.due(now) : elapsed>std::chrono::milliseconds(changedView ? 33 : 125);
            if (!impl->candidateFrame && (impl->primeCandidate || due)) {
                auto pose = scene.livePlayerPreview ? animation.previewPose() : nullptr;
                if (!scene.livePlayerPreview || pose) {
                    auto time = .12 + std::chrono::duration<double>(now - impl->previewStarted).count();
                    auto pixels = std::async(std::launch::async, [renderer, clip=scene.previewClip, camera=scene.previewCamera,
                                                                 textureIndex, time, native, pose, prime=impl->primeCandidate]() -> Impl::MenuFrame {
                        if (prime && !pose) renderer->samplePose(clip, 0, textureIndex);
                        if (native) return renderer->mesh(clip, time, camera, textureIndex, pose.get());
                        return renderer->render(clip, time, camera, textureIndex,!pose,pose.get());
                    });
                    impl->candidateFrame.emplace(Impl::CandidateFrame{renderer,scene.previewClip,textureIndex,native,scene.livePlayerPreview,std::move(pixels)});
                    impl->primeCandidate = false;
                    impl->lastPreview = now;
                    impl->lastCamera = scene.previewCamera;
                    if (scene.livePlayerPreview) impl->portraitCadence.submitted(now);
                }
            }
            impl->lastRenderer = renderer;
            impl->lastClip = scene.previewClip;
            impl->lastTextureIndex = textureIndex;
            impl->lastAnimatePreview = true;
        } else if(changedClip || (changedView && elapsed>std::chrono::milliseconds(33))
            || (scene.animatePreview && (portraitOnly ? impl->portraitCadence.due(now) : elapsed>std::chrono::milliseconds(125)))) {
            auto live = portraitOnly ? animation.previewPose() : std::shared_ptr<PreviewPose const>{};
            if (portraitOnly && !live) return;
            if(changedClip){impl->previewStarted=now;if(!portraitOnly)renderer->samplePose(scene.previewClip,0,textureIndex);}
            auto time=.12+std::chrono::duration<double>(now-impl->previewStarted).count();
            if (!scene.animatePreview) time=.12;
            PreviewShoulders shoulders;
            if (portraitOnly) {
                impl->portraitCadence.submitted(now);
                auto variants = shoulderParrotVariants(*client.getLocalPlayer());
                shoulders = impl->portraitParrots.get(client, variants);
            }
            if (native) {
                try { impl->meshFrame = renderer->mesh(scene.previewClip, time, scene.previewCamera, textureIndex); ++impl->meshRevision; }
                catch (std::exception const& error) { fallback(error); }
            } else {
                auto bitmap=renderer->render(scene.previewClip,time,scene.previewCamera,textureIndex,!portraitOnly,live.get(),shoulders);
                impl->uploadPreview(client,bitmap);
            }
            impl->lastRenderer=renderer;impl->lastPreview=now;
            impl->lastClip=scene.previewClip;impl->lastCamera=scene.previewCamera;impl->lastTextureIndex=textureIndex;
            impl->lastAnimatePreview=scene.animatePreview;
        }
        if (native && impl->meshFrame) {
            try {
                impl->nativePreview.draw(context, *scene.preview, *impl->meshFrame, impl->meshRevision, impl->textDensity);
                previewBackend = "native_depth_preview";
                impl->nativeFailure.clear();
            } catch (std::exception const& error) { fallback(error); }
        } else if(!native && impl->texture) {
            previewBackend = "bitmap";
            auto r=*scene.preview;auto side=std::min(r.w,r.h) * impl->previewFrameScale;
            context.drawImage(*impl->texture->mClientTexture,{r.x+(r.w-side)/2,r.y+(r.h-side)/2},{side,side},{0,0},{1,1},false);
            context.flushImages(mce::Color(255,255,255),1.f,HashedString("ui_textured_and_glcolor"));
        }
    }
    nativeDraws+=drawMesh(screen,cursorMesh);++impl->renderedFrames;
    auto now=std::chrono::steady_clock::now();
    if(now-impl->reportAt>std::chrono::seconds(1)&&!impl->diagnostics.empty()) {
        try {auto path=impl->diagnostics.parent_path()/"ui-render-status.json";
            std::ofstream(path)<<Json{{"frames",impl->renderedFrames},{"duplicate_views_skipped",impl->skippedViews},{"shape_draw_calls",nativeDraws},
                {"live_player_preview",scene.livePlayerPreview || portraitOnly},{"portrait_target_fps",impl->portraitCadence.rate()},{"preview_backend",previewBackend},{"preview_vertices",impl->meshFrame ? impl->meshFrame->vertices.size() : 0},
                {"native_preview_error",impl->nativeFailure},{"portrait_shoulders", impl->portraitParrots.report()}, {"mouse_grabbed",client.getMouseGrabbed()},{"menu_owns_mouse",impl->mouseCapture.owns(&client)},
                {"game_capture_requests_blocked",impl->mouseCapture.blockedCount()},{"release_retries",impl->releaseRetries.load()},
                {"menu_pointer_source",impl->pointer.source()},
                {"text_pixel_density",impl->textDensity},{"text_atlas_pixels",{impl->textAtlas.image.width,impl->textAtlas.image.height}},
                {"vertices",mainMesh.size()+cursorMesh.size()},{"legacy_rectangle_calls",0},{"gui_size",{impl->width,impl->height}},{"ui",impl->ui.report()}}.dump(2);
        }catch(std::exception const&){}impl->reportAt=now;
    }
}
}
