#include "overlay.hpp"
#include "log.hpp"

#include <windows.h>
#include <d3d11.h>
#include <imgui.h>
#include <backends/imgui_impl_dx11.h>

#include <atomic>
#include <mutex>

namespace coop {
namespace {
using PresentFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
PresentFn originalPresent = nullptr;
ResizeFn originalResize = nullptr;
std::mutex frameMutex, renderMutex;
OverlayFrame frame;
std::atomic<bool> ready = false;
HWND dummyWindow = nullptr;
IDXGISwapChain* activeChain = nullptr;
ID3D11Device* device = nullptr;
ID3D11DeviceContext* context = nullptr;
ID3D11RenderTargetView* target = nullptr;
ImGuiContext* imguiContext = nullptr;

void ReleaseTarget() {
    if (target) target->Release();
    target = nullptr;
}

HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* chain, UINT interval, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) {
        std::lock_guard lock(renderMutex);
        DXGI_SWAP_CHAIN_DESC desc{};
        if (SUCCEEDED(chain->GetDesc(&desc)) && desc.OutputWindow != dummyWindow && IsWindowVisible(desc.OutputWindow)) {
            if (!activeChain) {
                HRESULT result = chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device));
                if (SUCCEEDED(result)) {
                    device->GetImmediateContext(&context);
                    imguiContext = ImGui::CreateContext();
                    auto& io = ImGui::GetIO();
                    io.IniFilename = nullptr;
                    io.LogFilename = nullptr;
                    if (ImGui_ImplDX11_Init(device, context)) {
                        activeChain = chain;
                        ready = true;
                        Log("overlay: Direct3D 11 renderer ready");
                    } else {
                        Log("overlay: Direct3D 11 renderer initialization failed");
                        ImGui::DestroyContext(imguiContext);
                        imguiContext = nullptr;
                        context->Release();
                        context = nullptr;
                        device->Release();
                        device = nullptr;
                    }
                } else Log("overlay: GetDevice failed (0x%lx)", static_cast<unsigned long>(result));
            }
            if (chain == activeChain) {
                OverlayFrame current;
                {
                    std::lock_guard frameLock(frameMutex);
                    current = frame;
                }
                if (!current.primitives.empty() && current.width > 0 && current.height > 0) {
                    ID3D11Texture2D* buffer = nullptr;
                    D3D11_TEXTURE2D_DESC texture{};
                    HRESULT result = chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer));
                    if (SUCCEEDED(result)) {
                        buffer->GetDesc(&texture);
                        if (!target) result = device->CreateRenderTargetView(buffer, nullptr, &target);
                        buffer->Release();
                    }
                    if (SUCCEEDED(result) && target) {
                        ImGui::SetCurrentContext(imguiContext);
                        auto& io = ImGui::GetIO();
                        io.DisplaySize = ImVec2(static_cast<float>(texture.Width), static_cast<float>(texture.Height));
                        io.DeltaTime = 1.f / 60.f;
                        ImGui_ImplDX11_NewFrame();
                        ImGui::NewFrame();
                        auto draw = ImGui::GetForegroundDrawList();
                        float sx = io.DisplaySize.x / current.width, sy = io.DisplaySize.y / current.height;
                        for (const auto& primitive : current.primitives) {
                            auto c = primitive.color;
                            ImU32 color = ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a));
                            ImVec2 at(primitive.x * sx, primitive.y * sy);
                            if (primitive.text.empty())
                                draw->AddRectFilled(at, ImVec2(at.x + primitive.width * sx, at.y + primitive.height * sy), color);
                            else draw->AddText(nullptr, primitive.fontSize * sy, at, color, primitive.text.c_str());
                        }
                        ImGui::Render();
                        ID3D11RenderTargetView* previous[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
                        ID3D11DepthStencilView* depth = nullptr;
                        context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, previous, &depth);
                        context->OMSetRenderTargets(1, &target, nullptr);
                        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                        context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, previous, depth);
                        for (auto view : previous) if (view) view->Release();
                        if (depth) depth->Release();
                    } else Log("overlay: render target unavailable (0x%lx)", static_cast<unsigned long>(result));
                }
            }
        }
    }
    return originalPresent(chain, interval, flags);
}

HRESULT STDMETHODCALLTYPE Resize(IDXGISwapChain* chain, UINT count, UINT width, UINT height,
                                  DXGI_FORMAT format, UINT flags) {
    std::lock_guard lock(renderMutex);
    if (chain == activeChain) ReleaseTarget();
    return originalResize(chain, count, width, height, format, flags);
}

DWORD WINAPI InstallRenderer(void*) {
    dummyWindow = CreateWindowExW(0, L"STATIC", L"DaysGoneCoop renderer setup", WS_POPUP, 0, 0, 1, 1,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!dummyWindow) {
        Log("overlay: setup window failed (error %lu)", GetLastError());
        return 1;
    }
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = desc.BufferDesc.Height = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = dummyWindow;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    IDXGISwapChain* chain = nullptr;
    ID3D11Device* temporaryDevice = nullptr;
    ID3D11DeviceContext* temporaryContext = nullptr;
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &desc, &chain, &temporaryDevice, nullptr, &temporaryContext);
    if (SUCCEEDED(result)) {
        auto table = *reinterpret_cast<void***>(chain);
        DWORD protection = 0;
        if (VirtualProtect(table + 8, 6 * sizeof(void*), PAGE_READWRITE, &protection)) {
            originalPresent = reinterpret_cast<PresentFn>(table[8]);
            originalResize = reinterpret_cast<ResizeFn>(table[13]);
            InterlockedExchangePointer(table + 8, reinterpret_cast<void*>(&Present));
            InterlockedExchangePointer(table + 13, reinterpret_cast<void*>(&Resize));
            DWORD ignored = 0;
            if (!VirtualProtect(table + 8, 6 * sizeof(void*), protection, &ignored))
                Log("overlay: could not restore vtable protection (error %lu)", GetLastError());
            Log("overlay: swap-chain callbacks installed");
        } else Log("overlay: swap-chain vtable protection failed (error %lu)", GetLastError());
        temporaryContext->Release();
        temporaryDevice->Release();
        chain->Release();
    } else Log("overlay: setup device failed (0x%lx)", static_cast<unsigned long>(result));
    DestroyWindow(dummyWindow);
    return SUCCEEDED(result) ? 0 : 1;
}
} // namespace

void StartOverlay() {
    HANDLE thread = CreateThread(nullptr, 0, InstallRenderer, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else Log("overlay: setup thread failed (error %lu)", GetLastError());
}
bool OverlayReady() { return ready; }
void PublishOverlay(OverlayFrame value) {
    std::lock_guard lock(frameMutex);
    frame = std::move(value);
}
} // namespace coop
