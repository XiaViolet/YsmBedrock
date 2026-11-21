#pragma once
#include "UiTouchQueue.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <mutex>

namespace ysm {
// Windows GameCore can deliver a touch without generating a LeviLamina mouse
// event. Listen on the game window's own thread; never call game/UI code there.
class NativeMenuTouch {
    struct State {
        std::mutex mutex;
        HWND window{};
        bool open = false;
        UiTouchQueue input;
        DWORD lastTime = 0;
        POINT lastPoint{};
        bool seen = false;
        unsigned pointers = 0, touches = 0;
        bool pointerLookupFailed = false, touchReadFailed = false;
    };
    static State& state() { static State data; return data; }
    // Only lifecycle methods take this lock. The Windows callback cannot wait
    // on a thread that's installing/removing its hook.
    inline static std::mutex lifecycle;
    inline static HHOOK hook{};
    inline static DWORD thread{};
    inline static DWORD error{};
    inline static std::atomic<bool> retiring = false;
    static bool contact(State& s, std::uint32_t id, bool primary, TouchPhase phase, POINT screen, DWORD time) {
        RECT area{};
        POINT pixel = screen;
        bool coordinates = GetClientRect(s.window, &area) && area.right > 0 && area.bottom > 0 &&
            ScreenToClient(s.window, &pixel);
        if (!coordinates && phase == TouchPhase::Down) return false;
        UiPoint point{};
        if (coordinates) point = {float(pixel.x)/area.right, float(pixel.y)/area.bottom};
        else phase = TouchPhase::Cancel;
        if (!s.input.receive(id, primary, phase, point)) return false;
        if (primary || !s.seen) {
            s.lastPoint = screen;
            s.lastTime = time;
            s.seen = true;
        }
        return true;
    }
    static bool pointer(State& s, MSG const& msg) {
        auto id = GET_POINTERID_WPARAM(msg.wParam);
        if (!s.input.owns(id)) {
            if (!s.open || msg.message != WM_POINTERDOWN) return false;
            POINTER_INPUT_TYPE type{};
            if (!GetPointerType(id, &type)) {
                POINTER_INFO info{};
                if (GetPointerInfo(id,&info)) type=info.pointerType;
                else {
                    s.pointerLookupFailed=true;
                    INPUT_MESSAGE_SOURCE source{};
                    if (GetCurrentInputMessageSource(&source) && source.deviceType==IMDT_TOUCH) type=PT_TOUCH;
                }
            }
            if (type != PT_TOUCH) return false;
        }
        TouchPhase phase = TouchPhase::Move;
        if (msg.message == WM_POINTERCAPTURECHANGED || IS_POINTER_CANCELED_WPARAM(msg.wParam))
            phase = TouchPhase::Cancel;
        else if (msg.message == WM_POINTERDOWN) phase = TouchPhase::Down;
        else if (msg.message == WM_POINTERUP) phase = TouchPhase::Up;
        POINT screen{static_cast<short>(LOWORD(msg.lParam)), static_cast<short>(HIWORD(msg.lParam))};
        if (!contact(s, id, IS_POINTER_PRIMARY_WPARAM(msg.wParam), phase, screen, msg.time)) return false;
        ++s.pointers;
        return true;
    }
    static bool classic(State& s, MSG const& msg) {
        UINT count = LOWORD(msg.wParam);
        std::array<TOUCHINPUT, 32> items;
        auto handle = reinterpret_cast<HTOUCHINPUT>(msg.lParam);
        if (!count || count > items.size()) return false;
        if (!GetTouchInputInfo(handle,count,items.data(),sizeof(TOUCHINPUT))) {
            s.touchReadFailed=true;
            return false;
        }
        // WM_TOUCH is one indivisible message. Pass a mixed batch through if it
        // contains a contact that had already started in the game.
        for (UINT i = 0; i < count; ++i) {
            auto const& t = items[i];
            if ((t.dwFlags & TOUCHEVENTF_PEN) ||
                (!s.input.owns(t.dwID) && !(s.open && (t.dwFlags & TOUCHEVENTF_DOWN)))) return false;
        }
        for (UINT i = 0; i < count; ++i) {
            auto const& t = items[i];
            auto phase = t.dwFlags & TOUCHEVENTF_UP ? TouchPhase::Up :
                t.dwFlags & TOUCHEVENTF_DOWN ? TouchPhase::Down : TouchPhase::Move;
            contact(s, t.dwID, t.dwFlags & TOUCHEVENTF_PRIMARY, phase,
                {TOUCH_COORD_TO_PIXEL(t.x), TOUCH_COORD_TO_PIXEL(t.y)}, msg.time);
        }
        CloseTouchInputHandle(handle);
        ++s.touches;
        return true;
    }
    static bool handle(MSG const& msg) {
        bool native = msg.message == WM_POINTERDOWN || msg.message == WM_POINTERUPDATE ||
            msg.message == WM_POINTERUP || msg.message == WM_POINTERCAPTURECHANGED;
        bool mouse = msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST;
        if (!native && !mouse && msg.message != WM_TOUCH) return false;
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (!s.window || (s.window != msg.hwnd && !IsChild(s.window,msg.hwnd))) return false;
        if (mouse) {
            if (!matchesContact(s,msg.pt,msg.time)) return false;
            INPUT_MESSAGE_SOURCE source{};
            return (GetCurrentInputMessageSource(&source) && source.deviceType == IMDT_TOUCH) ||
                (static_cast<ULONG_PTR>(GetMessageExtraInfo()) & 0xffffff80u) == 0xff515780u;
        }
        if (!s.open && !s.input.hasContacts()) return false;
        return native ? pointer(s,msg) : classic(s,msg);
    }
    static bool matchesContact(State const& s, POINT pt, DWORD time) {
        return s.seen && DWORD(time-s.lastTime)<=120 &&
            std::abs(pt.x-s.lastPoint.x)<=4 && std::abs(pt.y-s.lastPoint.y)<=4;
    }
    static LRESULT CALLBACK callback(int code, WPARAM removed, LPARAM data) noexcept {
        if (code == HC_ACTION && removed == PM_REMOVE) {
            auto& msg = *reinterpret_cast<MSG*>(data);
            try {
                if (handle(msg)) {
                    msg.message = WM_NULL;
                    msg.wParam = 0;
                    msg.lParam = 0;
                }
            } catch (...) { /* Let the window handle unconsumed input. */ }
        }
        return CallNextHookEx(nullptr, code, removed, data);
    }
public:
    static bool waitingForRelease() { return retiring.load(); }
    // Some input backends promote a contact after Windows has dispatched it.
    // Call only for a positively identified touch-promoted mouse message.
    static bool alreadyHandled(POINT pt,DWORD time) {
        auto& s=state();
        std::lock_guard lock(s.mutex);
        return matchesContact(s,pt,time);
    }
    static void open(HWND window) {
        std::lock_guard guard(lifecycle);
        DWORD pid{};
        auto target = GetWindowThreadProcessId(window,&pid);
        if (!target || pid != GetCurrentProcessId()) return;
        retiring.store(false);
        if (hook && thread != target) { UnhookWindowsHookEx(hook); hook=nullptr; }
        if (!hook) {
            hook=SetWindowsHookExW(WH_GETMESSAGE,callback,nullptr,target);
            error=hook ? 0 : GetLastError();
            thread=target;
        }
        auto& s=state();
        std::lock_guard lock(s.mutex);
        if (s.window != window) s.input.focusLost();
        s.window=window;
        s.open=true;
        s.seen=false;
        s.pointers=s.touches=0;
        s.pointerLookupFailed=s.touchReadFailed=false;
        s.input.setOpen(true);
    }
    static void close() {
        auto& s=state();
        std::lock_guard lock(s.mutex);
        s.open=false;
        s.input.setOpen(false);
        retiring.store(true);
    }
    static void retireIfIdle() {
        if (!retiring.load()) return;
        std::lock_guard guard(lifecycle);
        auto& s=state();
        {
            std::lock_guard lock(s.mutex);
            if (s.open) return;
            // Wait for the consumed press to finish and for its compatibility
            // mouse message. A lost up must not retain the hook indefinitely.
            DWORD age = GetTickCount()-s.lastTime;
            if (s.seen && (age<=120 || (s.input.hasContacts() && age<10000))) return;
            s.window=nullptr;
            s.input.focusLost();
            retiring.store(false);
        }
        if (hook) { UnhookWindowsHookEx(hook); hook=nullptr; }
    }
    static void focusLost() {
        auto& s=state();
        std::lock_guard lock(s.mutex);
        s.input.focusLost();
        s.seen=false;
    }
    static void shutdown() {
        std::lock_guard guard(lifecycle);
        retiring.store(false);
        if (hook) { UnhookWindowsHookEx(hook); hook=nullptr; }
        auto& s=state();
        std::lock_guard lock(s.mutex);
        s.window=nullptr; s.open=false; s.seen=false;
        s.input.setOpen(false); s.input.focusLost();
    }
    struct Stats { bool attached; DWORD attachError; unsigned pointerMessages, touchMessages; bool pointerLookupFailed, touchReadFailed; };
    static Stats stats() {
        std::lock_guard guard(lifecycle);
        auto& s=state(); std::lock_guard lock(s.mutex);
        return {hook!=nullptr,error,s.pointers,s.touches,s.pointerLookupFailed,s.touchReadFailed};
    }
    static std::vector<MenuTouchSample> take() {
        auto& s=state(); std::lock_guard lock(s.mutex);
        return s.input.take();
    }
};
}
