// UI only. Mouse.cpp is the small raw HID example; Session.cpp owns its lifetime.
#include "Session.h"
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace VirtualMouse;
namespace {
constexpr float BaseWidth = 1120, BaseHeight = 790;
constexpr UINT Animation = 1;
const D2D1_COLOR_F Background = D2D1::ColorF(0x0C111B), Card = D2D1::ColorF(0x151E2D);
const D2D1_COLOR_F Border = D2D1::ColorF(0x29364B), White = D2D1::ColorF(0xF4F7FF);
const D2D1_COLOR_F Muted = D2D1::ColorF(0x9AAAC2), Blue = D2D1::ColorF(0x438BFF);
void Check(HRESULT value) { if (FAILED(value)) throw std::runtime_error("Windows graphics operation failed (HRESULT " + std::to_string(static_cast<unsigned long>(value)) + ")."); }
bool Inside(D2D1_RECT_F r, D2D1_POINT_2F p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }
std::wstring Field(HWND window) { wchar_t text[128]{}; GetWindowTextW(window, text, 128); return text; }
D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) { return {a.r + (b.r-a.r)*t, a.g + (b.g-a.g)*t, a.b + (b.b-a.b)*t, 1}; }
enum Action { Setup, Source, Retry, Copy, Left, Right, Middle, Back, Forward, SendX, SendY, MinusX, PlusX, MinusY, PlusY, WheelUp, WheelDown, PanLeft, PanRight };
struct Button { Action action; std::wstring text; D2D1_RECT_F box{}; float hover = 0, press = 0; };

class Window {
    HWND hwnd_ = nullptr, x_ = nullptr, y_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = CreateSolidBrush(RGB(12,17,27));
    Session session_;
    std::thread shutdown_;
    Snapshot state_;
    bool preview_ = false, closing_ = false, reconnecting_ = false, animation_ = false;
    int hover_ = -1, down_ = -1, focus_ = -1;
    ULONGLONG tick_ = 0;
    float width_ = BaseWidth, height_ = BaseHeight, dpi_ = 96, pulse_ = 0;
    std::wstring localError_;
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> writing_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    std::map<int, ComPtr<IDWriteTextFormat>> fonts_;
    std::vector<Button> buttons_ = {
        {Setup,L"Driver setup"},{Source,L"GitHub  \u2197"},{Retry,L"Reconnect"},{Copy,L"Copy details"},
        {Left,L"Left click"},{Right,L"Right click"},{Middle,L"Middle click"},{Back,L"Side: back"},{Forward,L"Side: forward"},
        {SendX,L"Send X  \u2192"},{SendY,L"Send Y  \u2192"},
        {MinusX,L"X  -200"},{PlusX,L"X  +200"},{MinusY,L"Y  -200"},{PlusY,L"Y  +200"},
        {WheelUp,L"Wheel +1"},{WheelDown,L"Wheel -1"},{PanLeft,L"Pan -1"},{PanRight,L"Pan +1"}
    };
    D2D1_RECT_F xBox_{}, yBox_{};
    bool Enabled(const Button& b) const { return !closing_ && !reconnecting_ && (b.action <= Copy ? (b.action != Retry || !state_.starting) : state_.ready && (b.action > Forward || !state_.click_pending)); }
    float Scale() const { return dpi_/96; }
    void Animate() { if (!animation_) { tick_ = GetTickCount64(); SetTimer(hwnd_,Animation,16,nullptr); animation_ = true; } InvalidateRect(hwnd_,nullptr,FALSE); }
    void Refresh() {
        if (!preview_) state_ = session_.Read();
        EnableWindow(x_,state_.ready && !closing_ && !reconnecting_); EnableWindow(y_,state_.ready && !closing_ && !reconnecting_);
        pulse_ = 1; Animate();
    }
    int Hit(D2D1_POINT_2F point) const { for (std::size_t i = 0; i < buttons_.size(); ++i) if (Enabled(buttons_[i]) && Inside(buttons_[i].box,point)) return static_cast<int>(i); return -1; }
    void Layout() {
        RECT client{}; GetClientRect(hwnd_,&client); dpi_ = static_cast<float>(GetDpiForWindow(hwnd_));
        width_ = client.right/Scale(); height_ = client.bottom/Scale();
        const float half=(width_-88)/2, right=56+half;
        auto place=[&](Action a,float x,float y,float w,float h=42) { buttons_[static_cast<std::size_t>(a)].box={x,y,x+w,y+h}; };
        place(Setup,width_-280,34,132); place(Source,width_-136,34,104);
        place(Retry,width_-298,159,126); place(Copy,width_-160,159,108);
        const float button=(half-64)/3;
        place(Left,52,331,button,54); place(Right,64+button,331,button,54); place(Middle,76+2*button,331,button,54);
        place(Back,52,397,button*1.5f+6,54); place(Forward,64+button*1.5f,397,button*1.5f+6,54);
        place(SendX,width_-180,326,128); place(SendY,width_-180,385,128);
        xBox_={right+86,326,width_-196,368}; yBox_={right+86,385,width_-196,427};
        const float quick=(half-76)/4;
        for(int i=0;i<4;++i) place(static_cast<Action>(MinusX+i),right+20+i*(quick+12),469,quick,42);
        const float scroll=(half-76)/4;
        for(int i=0;i<4;++i) place(static_cast<Action>(WheelUp+i),52+i*(scroll+12),608,scroll,38);
        auto edit=[&](HWND window,D2D1_RECT_F r) { MoveWindow(window,static_cast<int>((r.left+12)*Scale()),static_cast<int>((r.top+9)*Scale()),static_cast<int>((r.right-r.left-24)*Scale()),static_cast<int>(25*Scale()),TRUE); };
        HFONT previous=editFont_;
        editFont_=CreateFontW(-static_cast<int>(17*Scale()),0,0,0,FW_MEDIUM,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        for(HWND field:{x_,y_}) SendMessageW(field,WM_SETFONT,reinterpret_cast<WPARAM>(editFont_),TRUE);
        if(previous)DeleteObject(previous);
        edit(x_,xBox_); edit(y_,yBox_);
        if(target_) { Check(target_->Resize(D2D1::SizeU(static_cast<UINT>(client.right),static_cast<UINT>(client.bottom)))); target_->SetDpi(dpi_,dpi_); }
        InvalidateRect(hwnd_,nullptr,FALSE);
    }
    IDWriteTextFormat* Font(float size,bool bold,bool mono=false) {
        int key=static_cast<int>(size)*4+(bold?1:0)+(mono?2:0); auto found=fonts_.find(key);
        if(found==fonts_.end()) { ComPtr<IDWriteTextFormat> format; Check(writing_->CreateTextFormat(mono?L"Consolas":L"Segoe UI",nullptr,bold?DWRITE_FONT_WEIGHT_SEMI_BOLD:DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"en-us",&format)); format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); found=fonts_.emplace(key,std::move(format)).first; }
        return found->second.Get();
    }
    void Draw(ID2D1RenderTarget* target) {
        ComPtr<ID2D1SolidColorBrush> ink; Check(target->CreateSolidColorBrush(White,&ink));
        auto fill=[&](D2D1_RECT_F box,D2D1_COLOR_F color,float radius=14) { ink->SetColor(color); target->FillRoundedRectangle(D2D1::RoundedRect(box,radius,radius),ink.Get()); };
        auto outline=[&](D2D1_RECT_F box,D2D1_COLOR_F color,float radius=14,float stroke=1) { ink->SetColor(color); target->DrawRoundedRectangle(D2D1::RoundedRect(box,radius,radius),ink.Get(),stroke); };
        auto text=[&](const std::wstring& value,D2D1_RECT_F box,float size=14,D2D1_COLOR_F color=White,bool bold=false,bool center=false,bool mono=false) {
            auto format=Font(size,bold,mono); format->SetTextAlignment(center?DWRITE_TEXT_ALIGNMENT_CENTER:DWRITE_TEXT_ALIGNMENT_LEADING); ink->SetColor(color);
            target->DrawTextW(value.c_str(),static_cast<UINT32>(value.size()),format,box,ink.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };
        target->Clear(Background);
        const float half=(width_-88)/2,right=56+half;
        fill({32,34,74,76},Blue,12);
        ink->SetColor(White); target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(45,44,61,66),7,7),ink.Get(),1.7f);
        target->DrawLine({53,44},{53,52},ink.Get(),1.7f);
        text(L"Virtual mouse input",{90,29,width_-308,62},26,White,true);
        text(L"A small C++ example powered by Vdevice",{91,66,width_-308,88},13,Muted);
        ink->SetColor(Border);target->DrawLine({32,112},{width_-32,112},ink.Get());
        fill({32,136,width_-32,226},Card); outline({32,136,width_-32,226},Border);
        ink->SetColor(Blue);target->FillEllipse(D2D1::Ellipse({58,171},4+pulse_*1.2f,4+pulse_*1.2f),ink.Get());
        const std::wstring title=preview_?L"Offline preview":reconnecting_?L"Disconnecting virtual mouse...":state_.starting?L"Connecting virtual mouse...":state_.ready?L"Virtual mouse ready":L"Virtual mouse unavailable";
        text(title,{73,149,width_-320,181},20,White,true);
        std::wstring sub=preview_?L"Interface preview only. No virtual device or input was created.":state_.ready?L"5 buttons  /  16-bit relative X/Y  /  USB HID":L"Use Driver setup if Vdevice is missing, then reconnect.";
        text(sub,{73,185,width_-320,208},13,Muted);
        fill({32,246,32+half,541},Card); outline({32,246,32+half,541},Border);
        fill({right,246,width_-32,541},Card); outline({right,246,width_-32,541},Border);
        text(L"Click controls",{52,262,half,292},20,White,true);
        text(L"Choose a button, then move to your target.",{52,294,half+12,315},13,Muted);
        text(state_.click_pending?L"Click armed \u2014 move outside this window now.":L"Clicks fire in 1.2 s. Press + release, never held.",{52,469,half+10,510},13,state_.click_pending?Blue:Muted);
        text(L"Relative motion",{right+20,262,width_-52,292},20,White,true);
        text(L"Positive X = right. Positive Y = down.",{right+20,294,width_-52,315},13,Muted);
        text(L"X",{right+20,326,right+70,368},17,Muted,true); text(L"Y",{right+20,385,right+70,427},17,Muted,true);
        for(auto box:{xBox_,yBox_}) { fill(box,Background,9); outline(box,Border,9); }
        text(Field(x_),{xBox_.left+12,xBox_.top,xBox_.right-12,xBox_.bottom},17);
        text(Field(y_),{yBox_.left+12,yBox_.top,yBox_.right-12,yBox_.bottom},17);
        text(L"Signed HID units, from -32768 to 32767",{right+20,434,width_-52,456},12,Muted);
        fill({32,561,32+half,668},Card); outline({32,561,32+half,668},Border);
        fill({right,561,width_-32,668},Card); outline({right,561,width_-32,668},Border);
        text(L"Scroll",{52,574,half,598},16,White,true);
        text(L"LAST ACCEPTED REPORT",{right+20,572,width_-200,596},12,Muted,true);
        text(std::to_wstring(state_.reports)+L" reports",{width_-202,572,width_-52,596},12,Blue,true);
        const auto& last=state_.last;
        std::wstring line=L"x = "+std::to_wstring(last.x)+L"    y = "+std::to_wstring(last.y)+L"    buttons = "+std::to_wstring(last.buttons);
        text(line,{right+20,601,width_-52,625},14,White,false,false,true);
        text(L"wheel = "+std::to_wstring(last.wheel)+L"    pan = "+std::to_wstring(last.pan),{right+20,630,width_-52,650},12,Muted,false,false,true);
        for(std::size_t i=0;i<buttons_.size();++i) {
            auto& b=buttons_[i]; bool enabled=Enabled(b); bool primary=b.action==SendX||b.action==SendY;
            D2D1_COLOR_F base=primary?D2D1::ColorF(0x2565D9):D2D1::ColorF(0x1B283C);
            auto color=enabled?Mix(base,D2D1::ColorF(0x3568AB),b.hover*.6f):Mix(Background,Card,.7f);
            color=Mix(color,Blue,b.press*.3f); fill(b.box,color,9);
            outline(b.box,static_cast<int>(i)==focus_?Blue:Mix(Border,Blue,b.hover*.65f),9,static_cast<int>(i)==focus_?1.8f:1);
            text(b.text,b.box,13,enabled?White:Muted,primary,true);
        }
        std::wstring detail=localError_.empty()?state_.detail:localError_;
        if(preview_&&detail.empty())detail=L"Real HID reports through Vdevice. X/Y are counts, not guaranteed screen pixels.";
        text(detail,{32,688,width_-32,735},13,localError_.empty()?Muted:White);
        ink->SetColor(Border);target->DrawLine({32,height_-38},{width_-32,height_-38},ink.Get());
        std::wstring demo=L"VDEVICE DEMO  /  1 hour per Windows restart";
        if(state_.trial_remaining_100ns)demo+=L"  /  "+std::to_wstring(state_.trial_remaining_100ns/10000000/60)+L" min remaining";
        text(demo,{32,height_-32,width_-265,height_-7},11,Muted,true);
        text(L"Device disconnects on close",{width_-250,height_-32,width_-32,height_-7},11,Muted);
    }
    void Paint() {
        PAINTSTRUCT paint{}; BeginPaint(hwnd_,&paint);
        bool recreate=false;
        try {
            if(!target_) { RECT r{};GetClientRect(hwnd_,&r);auto properties=D2D1::HwndRenderTargetProperties(hwnd_,D2D1::SizeU(static_cast<UINT>(r.right),static_cast<UINT>(r.bottom)));HRESULT result=factory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),properties,&target_);if(FAILED(result))Check(factory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE),properties,&target_));target_->SetDpi(dpi_,dpi_); }
            target_->BeginDraw();Draw(target_.Get());HRESULT result=target_->EndDraw();if(result==D2DERR_RECREATE_TARGET){target_.Reset();recreate=true;}else Check(result);
        } catch(const std::exception&) { target_.Reset();FillRect(paint.hdc,&paint.rcPaint,editBrush_);SetTextColor(paint.hdc,RGB(244,247,255));SetBkMode(paint.hdc,TRANSPARENT);RECT bounds{32,32,1000,180};DrawTextW(paint.hdc,L"Graphics initialization failed. Close the app and check your Windows display driver.",-1,&bounds,DT_WORDBREAK); }
        EndPaint(hwnd_,&paint);
        if(recreate)InvalidateRect(hwnd_,nullptr,FALSE);
    }
    void CopyDetails() {
        std::wostringstream out;out<<L"Virtual Mouse Input 1.0.0\r\nVdevice demo\r\n"<<state_.title<<L"\r\n"<<state_.detail<<L"\r\nWindows error: "<<state_.error<<L"\r\nReports accepted: "<<state_.reports<<L"\r\n"<<localError_;
        const auto value=out.str(); const auto bytes=(value.size()+1)*sizeof(wchar_t);
        HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);if(!memory){localError_=L"Cannot allocate clipboard data.";return;}
        void* data=GlobalLock(memory);if(!data){GlobalFree(memory);return;}memcpy(data,value.c_str(),bytes);GlobalUnlock(memory);
        if(OpenClipboard(hwnd_)){if(EmptyClipboard()&&SetClipboardData(CF_UNICODETEXT,memory))memory=nullptr;CloseClipboard();}if(memory)GlobalFree(memory);
    }
    void Send(State state) {
        if(preview_) { state_.last=state; ++state_.reports; pulse_=1; } else session_.Send(state);
    }
    void Disconnect(bool close) {
        closing_|=close;if(reconnecting_)return;
        reconnecting_=true;localError_=L"Disconnecting the virtual mouse...";EnableWindow(x_,FALSE);EnableWindow(y_,FALSE);Animate();
        try{shutdown_=std::thread([this]{session_.Stop();PostMessageW(hwnd_,WM_APP+2,0,0);});}
        catch(...){closing_=false;reconnecting_=false;throw;}
    }
    void Execute(Action action) {
        localError_.clear();
        if(action==SendX||action==SendY) {
            int value=0;if(!ParseMotion(Field(action==SendX?x_:y_),value)){localError_=L"Enter a whole number from -32768 to 32767. Nothing was sent.";Animate();return;}
            State state;if(action==SendX)state.x=value;else state.y=value;Send(state);
        } else if(action>=Left&&action<=Forward) {
            if(preview_){localError_=L"Offline preview: no click was sent.";}else session_.Click(static_cast<std::uint8_t>(1u<<(action-Left)));
        } else if(action>=MinusX) {
            State state;switch(action){case MinusX:state.x=-200;break;case PlusX:state.x=200;break;case MinusY:state.y=-200;break;case PlusY:state.y=200;break;case WheelUp:state.wheel=1;break;case WheelDown:state.wheel=-1;break;case PanLeft:state.pan=-1;break;case PanRight:state.pan=1;break;default:break;}Send(state);
        } else if(action==Copy) { CopyDetails(); }
        else if(preview_) { localError_=L"Offline preview: external actions and driver access are disabled."; }
        else if(action==Retry) { Disconnect(false); }
        else if(action==Source) ShellExecuteW(hwnd_,L"open",L"https://github.com/TitanCSA/Virtual-mouse-input",nullptr,nullptr,SW_SHOWNORMAL);
        else if(action==Setup) {
            wchar_t path[32768]{}; GetModuleFileNameW(nullptr,path,32768);
            const auto root=std::filesystem::path(path).parent_path(); auto setup=root/L"Driver"/L"VdeviceSetup.exe";
            if(!std::filesystem::is_regular_file(setup))setup=root/L"VdeviceSetup.exe";
            if(std::filesystem::is_regular_file(setup)) { auto result=ShellExecuteW(hwnd_,L"open",setup.c_str(),nullptr,root.c_str(),SW_SHOWNORMAL);if(reinterpret_cast<INT_PTR>(result)<=32)localError_=L"Driver setup could not start. Open VdeviceSetup.exe manually."; }
            else { ShellExecuteW(hwnd_,L"open",L"https://github.com/TitanCSA/Vdevice/releases/latest",nullptr,nullptr,SW_SHOWNORMAL);localError_=L"Download and install VdeviceSetup.exe, then click Reconnect."; }
        }
        Animate();
    }
    void NextFocus(bool reverse) {
        std::vector<int> order;for(std::size_t i=0;i<buttons_.size();++i){if(i==SendX&&IsWindowEnabled(x_))order.push_back(-2);if(i==SendY&&IsWindowEnabled(y_))order.push_back(-3);if(Enabled(buttons_[i]))order.push_back(static_cast<int>(i));}
        if(order.empty())return;
        int current=GetFocus()==x_?-2:GetFocus()==y_?-3:focus_;auto found=std::find(order.begin(),order.end(),current);
        auto index=found==order.end()?0:static_cast<int>(found-order.begin())+(reverse?-1:1);index=(index+static_cast<int>(order.size()))%static_cast<int>(order.size());focus_=order[static_cast<std::size_t>(index)];
        SetFocus(focus_==-2?x_:focus_==-3?y_:hwnd_);Animate();
    }
    LRESULT Handle(UINT message,WPARAM w,LPARAM l) {
        switch(message) {
        case WM_CREATE: {
            x_=CreateWindowExW(0,L"EDIT",L"200",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,0,0,1,1,hwnd_,reinterpret_cast<HMENU>(101),nullptr,nullptr);
            y_=CreateWindowExW(0,L"EDIT",L"200",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,0,0,1,1,hwnd_,reinterpret_cast<HMENU>(102),nullptr,nullptr);
            for(HWND field:{x_,y_})SendMessageW(field,EM_SETLIMITTEXT,12,0);
            Layout();if(!preview_)session_.Start(hwnd_);Refresh();return 0;
        }
        case WM_APP+1: Refresh();return 0;
        case WM_SIZE: if(x_&&w!=SIZE_MINIMIZED)Layout();return 0;
        case WM_DPICHANGED: {auto r=reinterpret_cast<RECT*>(l);SetWindowPos(hwnd_,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
        case WM_GETMINMAXINFO: { auto limits=reinterpret_cast<MINMAXINFO*>(l);limits->ptMinTrackSize={static_cast<LONG>(1050*Scale()),static_cast<LONG>(830*Scale())};return 0; }
        case WM_PAINT: Paint();return 0;
        case WM_ERASEBKGND: return 1;
        case WM_CTLCOLOREDIT: SetTextColor(reinterpret_cast<HDC>(w),RGB(244,247,255));SetBkColor(reinterpret_cast<HDC>(w),RGB(12,17,27));return reinterpret_cast<LRESULT>(editBrush_);
        case WM_COMMAND: if(HIWORD(w)==EN_CHANGE)InvalidateRect(hwnd_,nullptr,FALSE);return 0;
        case WM_MOUSEMOVE: {auto point=D2D1::Point2F(GET_X_LPARAM(l)/Scale(),GET_Y_LPARAM(l)/Scale());int hit=Hit(point);if(hit!=hover_){hover_=hit;Animate();}TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd_,0};TrackMouseEvent(&track);return 0;}
        case WM_MOUSELEAVE: hover_=-1;Animate();return 0;
        case WM_SETCURSOR: if(LOWORD(l)==HTCLIENT){POINT p{};GetCursorPos(&p);ScreenToClient(hwnd_,&p);SetCursor(LoadCursorW(nullptr,Hit({p.x/Scale(),p.y/Scale()})>=0?IDC_HAND:IDC_ARROW));return TRUE;}break;
        case WM_LBUTTONDOWN: {down_=Hit({GET_X_LPARAM(l)/Scale(),GET_Y_LPARAM(l)/Scale()});if(down_>=0){focus_=down_;SetFocus(hwnd_);SetCapture(hwnd_);buttons_[static_cast<std::size_t>(down_)].press=1;Animate();}return 0;}
        case WM_LBUTTONUP: {int down=down_;down_=-1;ReleaseCapture();int hit=Hit({GET_X_LPARAM(l)/Scale(),GET_Y_LPARAM(l)/Scale()});if(down>=0&&down==hit)Execute(buttons_[static_cast<std::size_t>(down)].action);Animate();return 0;}
        case WM_CAPTURECHANGED: down_=-1;Animate();return 0;
        case WM_KEYDOWN: if((w==VK_RETURN||w==VK_SPACE)&&focus_>=0&&Enabled(buttons_[static_cast<std::size_t>(focus_)])){Execute(buttons_[static_cast<std::size_t>(focus_)].action);return 0;}break;
        case WM_TIMER: if(w==Animation){ULONGLONG now=GetTickCount64();float step=1-std::exp(-static_cast<float>(now-tick_)/70);tick_=now;bool moving=false;for(std::size_t i=0;i<buttons_.size();++i){auto& b=buttons_[i];float desired=static_cast<int>(i)==hover_?1.0f:0.0f;b.hover+=(desired-b.hover)*step;b.press*=1-step;if(std::abs(b.hover-desired)<.008f)b.hover=desired;else moving=true;if(b.press<.008f)b.press=0;else moving=true;}pulse_*=1-step;if(pulse_<.008f)pulse_=0;else moving=true;if(!moving){KillTimer(hwnd_,Animation);animation_=false;}InvalidateRect(hwnd_,nullptr,FALSE);return 0;}break;
        case WM_CLOSE: Disconnect(true);return 0;
        case WM_APP+2: if(reconnecting_){if(shutdown_.joinable())shutdown_.join();if(closing_)DestroyWindow(hwnd_);else{reconnecting_=false;localError_.clear();if(!preview_)session_.Start(hwnd_);Refresh();}}return 0;
        case WM_DESTROY: KillTimer(hwnd_,Animation);PostQuitMessage(0);return 0;
        }
        return DefWindowProcW(hwnd_,message,w,l);
    }
    static LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM w,LPARAM l) noexcept {
        auto self=reinterpret_cast<Window*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);self->hwnd_=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        try{return self?self->Handle(message,w,l):DefWindowProcW(window,message,w,l);}
        catch(const std::exception&){if(self){self->localError_=L"A Windows operation failed. Close and reopen the application.";InvalidateRect(window,nullptr,FALSE);}return message==WM_CREATE?-1:0;}
    }
public:
    explicit Window(bool preview):preview_(preview) {
        Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory_.GetAddressOf()));Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(writing_.GetAddressOf())));
        if(preview_){state_.ready=true;state_.starting=false;state_.last.x=200;state_.last.y=-75;state_.reports=12;}
    }
    ~Window(){if(shutdown_.joinable())shutdown_.join();session_.Stop();if(hwnd_&&IsWindow(hwnd_))DestroyWindow(hwnd_);if(editFont_)DeleteObject(editFont_);if(editBrush_)DeleteObject(editBrush_);}
    void Create(bool visible) {
        WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=Procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.hIcon=LoadIconW(cls.hInstance,MAKEINTRESOURCEW(101));cls.hIconSm=cls.hIcon;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.lpszClassName=L"TitanCSA.VirtualMouseInput";cls.style=CS_HREDRAW|CS_VREDRAW;RegisterClassExW(&cls);
        UINT dpi=GetDpiForSystem();RECT bounds{0,0,static_cast<LONG>(BaseWidth*dpi/96),static_cast<LONG>(BaseHeight*dpi/96)};AdjustWindowRectExForDpi(&bounds,WS_OVERLAPPEDWINDOW,FALSE,0,dpi);
        hwnd_=CreateWindowExW(0,cls.lpszClassName,L"Virtual Mouse Input | Vdevice",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,bounds.right-bounds.left,bounds.bottom-bounds.top,nullptr,nullptr,cls.hInstance,this);
        if(!hwnd_)throw std::runtime_error("Cannot create the application window.");
        BOOL dark=TRUE;DwmSetWindowAttribute(hwnd_,20,&dark,sizeof(dark));
        if(visible){ShowWindow(hwnd_,SW_SHOWNORMAL);UpdateWindow(hwnd_);}
    }
    int Run(){MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){if(message.message==WM_KEYDOWN&&message.wParam==VK_TAB){NextFocus((GetKeyState(VK_SHIFT)&0x8000)!=0);continue;}if(message.message==WM_KEYDOWN&&message.wParam==VK_RETURN&&(message.hwnd==x_||message.hwnd==y_)){Execute(message.hwnd==x_?SendX:SendY);continue;}if(message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE){PostMessageW(hwnd_,WM_CLOSE,0,0);continue;}TranslateMessage(&message);DispatchMessageW(&message);}return static_cast<int>(message.wParam);}
    void Screenshot(const wchar_t* path) {
        width_=BaseWidth;height_=BaseHeight;Layout();
        ComPtr<IWICImagingFactory> imaging;Check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&imaging)));
        ComPtr<IWICBitmap> bitmap;Check(imaging->CreateBitmap(static_cast<UINT>(width_),static_cast<UINT>(height_),GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,&bitmap));
        ComPtr<ID2D1RenderTarget> target;Check(factory_->CreateWicBitmapRenderTarget(bitmap.Get(),D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96),&target));
        target->BeginDraw();Draw(target.Get());Check(target->EndDraw());
        ComPtr<IWICStream> stream;Check(imaging->CreateStream(&stream));Check(stream->InitializeFromFilename(path,GENERIC_WRITE));
        ComPtr<IWICBitmapEncoder> encoder;Check(imaging->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));Check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
        ComPtr<IWICBitmapFrameEncode> frame;Check(encoder->CreateNewFrame(&frame,nullptr));Check(frame->Initialize(nullptr));Check(frame->SetSize(static_cast<UINT>(width_),static_cast<UINT>(height_)));WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;Check(frame->SetPixelFormat(&format));Check(frame->WriteSource(bitmap.Get(),nullptr));Check(frame->Commit());Check(encoder->Commit());
    }
    void SelfTest(const wchar_t* path) {
        int checks=0;auto expect=[&](bool condition){++checks;if(!condition)throw std::runtime_error("Offline UI regression failed.");};
        for(const auto& b:buttons_){expect(b.box.left>=0&&b.box.top>=0&&b.box.right<=width_&&b.box.bottom<height_-70);auto p=D2D1::Point2F((b.box.left+b.box.right)/2,(b.box.top+b.box.bottom)/2);expect(Hit(p)==static_cast<int>(b.action));}
        auto count=state_.reports;SetWindowTextW(x_,L"-485");Execute(SendX);expect(state_.last.x==-485&&state_.last.y==0&&state_.reports==count+1);
        SetWindowTextW(y_,L"32768");Execute(SendY);expect(state_.reports==count+1&&!localError_.empty());
        SetWindowTextW(y_,L"300");Execute(SendY);expect(state_.last.x==0&&state_.last.y==300&&localError_.empty());
        Execute(PlusX);expect(state_.last.x==200);Execute(MinusY);expect(state_.last.y==-200);Execute(WheelDown);expect(state_.last.wheel==-1);Execute(PanRight);expect(state_.last.pan==1);
        Execute(Left);expect(!state_.click_pending);NextFocus(false);expect(GetFocus()!=nullptr);
        Handle(WM_APP+2,0,0);expect(!reconnecting_);
        Disconnect(true);for(const auto& button:buttons_)expect(!Enabled(button));
        MSG message{};auto deadline=GetTickCount64()+5000;while(IsWindow(hwnd_)&&GetTickCount64()<deadline){while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}if(IsWindow(hwnd_))MsgWaitForMultipleObjects(0,nullptr,FALSE,50,QS_ALLINPUT);}
        expect(!IsWindow(hwnd_));
        std::ofstream out{std::filesystem::path(path)};out<<"{\"passed\":true,\"checks\":"<<checks<<",\"driverOpened\":false,\"inputsSent\":false}\n";if(!out)throw std::runtime_error("Cannot save UI test result.");
    }
};

bool Administrator(){SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY;PSID sid=nullptr;BOOL member=FALSE;if(AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&sid)){CheckTokenMembership(nullptr,sid,&member);FreeSid(sid);}return member!=FALSE;}
}

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    int count=0;wchar_t** args=CommandLineToArgvW(GetCommandLineW(),&count);if(!args)return 1;
    int result=0;const bool preview=count==2&&std::wstring(args[1])==L"--preview";
    const bool screenshot=count==3&&std::wstring(args[1])==L"--screenshot";
    const bool test=count==3&&std::wstring(args[1])==L"--ui-self-test";
    if(count>1&&!preview&&!screenshot&&!test&&!(count==2&&std::wstring(args[1])==L"--elevated")){MessageBoxW(nullptr,L"Usage: VirtualMouseInput.exe [--preview | --screenshot file.png | --ui-self-test result.json]",L"Virtual Mouse Input",MB_OK);LocalFree(args);return 87;}
    if(!preview&&!screenshot&&!test&&!Administrator()){
        wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);auto code=reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"runas",path,L"--elevated",nullptr,SW_SHOWNORMAL));
        if(code<=32)MessageBoxW(nullptr,L"Administrator permission is required to access Vdevice. Open the app again and accept the prompt.",L"Virtual Mouse Input",MB_ICONINFORMATION);LocalFree(args);return code>32?0:ERROR_CANCELLED;
    }
    HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    HANDLE instance=nullptr;
    if(!preview&&!screenshot&&!test){
        instance=CreateMutexW(nullptr,FALSE,L"Local\\TitanCSA.VirtualMouseInput");DWORD lockError=GetLastError();
        if(!instance){auto detail=L"Cannot create the single-instance lock. Windows error "+std::to_wstring(lockError)+L".";MessageBoxW(nullptr,detail.c_str(),L"Virtual Mouse Input",MB_ICONERROR);if(SUCCEEDED(com))CoUninitialize();LocalFree(args);return static_cast<int>(lockError);}
        if(lockError==ERROR_ALREADY_EXISTS){CloseHandle(instance);HWND existing=FindWindowW(L"TitanCSA.VirtualMouseInput",nullptr);if(existing){ShowWindow(existing,SW_RESTORE);SetForegroundWindow(existing);}else MessageBoxW(nullptr,L"Virtual Mouse Input is already running in this Windows session.",L"Virtual Mouse Input",MB_ICONINFORMATION);if(SUCCEEDED(com))CoUninitialize();LocalFree(args);return 0;}
    }
    try {Check(com);Window window(preview||screenshot||test);window.Create(!screenshot&&!test);if(screenshot)window.Screenshot(args[2]);else if(test)window.SelfTest(args[2]);else result=window.Run();}
    catch(const std::exception& error){if(!screenshot&&!test)MessageBoxA(nullptr,error.what(),"Virtual Mouse Input",MB_ICONERROR);else OutputDebugStringA(error.what());result=1;}
    if(instance)CloseHandle(instance);if(SUCCEEDED(com))CoUninitialize();LocalFree(args);return result;
}
