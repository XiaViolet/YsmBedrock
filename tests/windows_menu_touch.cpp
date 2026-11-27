 
 
#include <Windows.h>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "UiTouchDelivery.h"
namespace {
TOUCHINPUT fixture{};
bool closedHandle = false;
BOOL fixtureType(UINT32 id, POINTER_INPUT_TYPE* type) {
    *type = id == 9 ? PT_MOUSE : PT_TOUCH;
    return TRUE;
}
BOOL fixtureRead(HTOUCHINPUT, UINT count, PTOUCHINPUT result, int size) {
    if (count != 1 || size != sizeof(TOUCHINPUT)) return FALSE;
    *result = fixture;
    return TRUE;
}
BOOL fixtureClose(HTOUCHINPUT) { closedHandle = true; return TRUE; }
}
#define GetPointerType fixtureType
#define GetTouchInputInfo fixtureRead
#define CloseTouchInputHandle fixtureClose
#undef WM_POINTERDOWN
#undef WM_POINTERUPDATE
#undef WM_POINTERUP
#undef WM_POINTERCAPTURECHANGED
#undef WM_TOUCH
#define WM_POINTERDOWN (WM_APP+1)
#define WM_POINTERUPDATE (WM_APP+2)
#define WM_POINTERUP (WM_APP+3)
#define WM_POINTERCAPTURECHANGED (WM_APP+4)
#define WM_TOUCH (WM_APP+5)
#include "NativeMenuTouch.h"
#undef GetPointerType
#undef GetTouchInputInfo
#undef CloseTouchInputHandle

using namespace ysm;
static void check(bool ok, char const* what) { if (!ok) throw std::runtime_error(what); }
static UINT post(HWND window, UINT message, WPARAM flags, LPARAM position) {
    if (!PostMessageW(window,message,flags,position)) throw std::runtime_error(
        "PostMessage " + std::to_string(message) + ", Win32 error " + std::to_string(GetLastError()));
    MSG msg{};
    check(PeekMessageW(&msg,window,message,message,PM_NOREMOVE) && msg.message==message,"peek consumed a contact");
    check(PeekMessageW(&msg,window,message,message,PM_REMOVE),"missing message");
    return msg.message;
}
int main() try {
    HWND window=CreateWindowExW(0,L"STATIC",L"YSM touch test",WS_POPUP,50,60,1920,1080,
                                nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    HWND child=CreateWindowExW(0,L"STATIC",L"",WS_CHILD,100,80,100,100,window,nullptr,nullptr,nullptr);
    HWND other=CreateWindowExW(0,L"STATIC",L"",WS_POPUP,0,0,100,100,nullptr,nullptr,nullptr,nullptr);
    check(window && child && other,"hidden game windows");
    std::promise<void> installed;
    std::jthread installer([&](std::stop_token stop) {
        NativeMenuTouch::open(window);
        installed.set_value();
        while (!stop.stop_requested()) Sleep(5);
    });
    installed.get_future().wait();
    check(NativeMenuTouch::stats().attached,"same-process window thread hook failed");
    UiPointer cursor;
    YsmUi ui;
    UiSnapshot snapshot;
    snapshot.locale="zh_cn";
    snapshot.settings=YsmUi::defaultSettings();
    snapshot.descriptor={{"properties",{{"extra_animation",{{"wave","招手"}}}}}};
    ui.update(snapshot); ui.open(false);
    auto scene=ui.draw(960,540);
    auto label=[&](std::string_view name) {
        for(auto const& item:scene.paint) if(item.text==name) {
            POINT p{LONG(item.rect.x*2+item.rect.w),LONG(item.rect.y*2+item.rect.h)};
            ClientToScreen(window,&p); return p;
        }
        throw std::runtime_error("missing control");
    };
    auto sample=[&](HWND target,UINT msg,unsigned id,POINT p,bool primary=true) {
        UINT flags=primary ? POINTER_MESSAGE_FLAG_PRIMARY : 0;
        return post(target,msg,MAKEWPARAM(id,flags),MAKELPARAM(p.x,p.y));
    };
    auto deliver=[&] {
        auto batch=NativeMenuTouch::take();
        deliverMenuTouches(batch,ui,cursor,960,540);
        if(ui.isOpen()) scene=ui.draw(960,540);
        return batch.size();
    };
    POINT p=label("本地设置");
    check(sample(other,WM_POINTERDOWN,1,p)==WM_POINTERDOWN,"another window consumed");
    check(sample(window,WM_POINTERDOWN,9,p)==WM_POINTERDOWN,"physical mouse consumed as touch");
    check(sample(child,WM_POINTERDOWN,3,p)==WM_NULL,"touch down leaked to game");
    check(sample(window,WM_POINTERDOWN,5,{0,0},false)==WM_NULL,"second finger leaked");
    check(sample(window,WM_POINTERUP,5,{0,0},false)==WM_NULL,"second finger up leaked");
    check(sample(child,WM_POINTERUP,3,p)==WM_NULL,"touch up leaked");
    auto time=DWORD(GetMessageTime());
    check(NativeMenuTouch::alreadyHandled(p,time),"promoted click not recognized");
    check(!NativeMenuTouch::alreadyHandled({p.x+30,p.y},time) &&
          !NativeMenuTouch::alreadyHandled(p,time+121),"unrelated input suppressed");
    check(deliver()==2 && ui.currentPage()==YsmUi::Page::Settings,"tap missed settings");

    p=label("动作预览");
    fixture.dwID=4; fixture.x=p.x*100; fixture.y=p.y*100;
    for(auto flag:{TOUCHEVENTF_DOWN,TOUCHEVENTF_UP}) {
        fixture.dwFlags=flag|TOUCHEVENTF_PRIMARY; closedHandle=false;
        check(post(window,WM_TOUCH,1,123)==WM_NULL && closedHandle,"WM_TOUCH ownership");
    }
    check(deliver()==2 && ui.currentPage()==YsmUi::Page::Preview,"WM_TOUCH tap missed preview");
    check(scene.preview.has_value(),"preview missing");
    auto r=*scene.preview;
    POINT start{LONG(2*r.x+r.w),LONG(2*r.y+r.h)};
    ClientToScreen(window,&start);
    POINT moved{start.x+60,start.y-40};
    sample(window,WM_POINTERDOWN,3,start);
    sample(window,WM_POINTERUPDATE,3,moved);
    sample(window,WM_POINTERCAPTURECHANGED,3,moved);
    check(deliver()==3 && std::abs(ui.report()["preview_camera"][0].get<float>()-60)<.01f,"drag or capture loss");
    check(NativeMenuTouch::take().empty(),"poll replayed old tap");

    p=label("本地设置");
    sample(window,WM_POINTERDOWN,3,p);
    NativeMenuTouch::focusLost();
    check(deliver()==1 && ui.currentPage()==YsmUi::Page::Preview,"stale touch after focus loss");

    ui.open(true); scene=ui.draw(960,540); p=label("招手");
    sample(window,WM_POINTERDOWN,3,p);
    check(deliver()==1 && !ui.isOpen(),"wheel should close on action");
    NativeMenuTouch::close();
    check(sample(window,WM_POINTERUP,3,p)==WM_NULL,"last lift reached world");
    check(ui.takeCommands().size()==1,"duplicate wheel action");
    check(sample(window,WM_POINTERDOWN,7,p)==WM_POINTERDOWN,"world touch blocked with closed menu");
    NativeMenuTouch::shutdown();
    check(!NativeMenuTouch::stats().attached,"hook remained after shutdown");
    DestroyWindow(other); DestroyWindow(window);
    std::cout<<"PASS hidden Windows HWND, cross-thread registration, PM_NOREMOVE, native pointer and WM_TOUCH, "
        "hit targets, drag, capture cancellation, single action, world and foreign-window passthrough.\n"
        "WM_APP carriers and digitizer metadata fixtures. No physical touchscreen or game input injected.\n";
} catch (std::exception const& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    NativeMenuTouch::shutdown();
    return 1;
}
