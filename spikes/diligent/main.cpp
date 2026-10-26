// SPDX-License-Identifier: Apache-2.0
//
// P0.3 spike: Diligent (D3D12 / Vulkan) + SDL3 + Dear ImGui (docking, multi-viewport)
// + one compute dispatch. Throw-away code; findings are in docs/spikes/diligent.md.
//
//   forge3d-spike-diligent [--rhi=d3d12|vulkan] [--frames N] [--hidden] [--capture out.png]
//                          [--no-viewports] [--no-vsync] [--validation]

#include "f3d/core/log.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include "DeviceContext.h"
#include "ImGuiDiligentRenderer.hpp"
#include "ImGuiImplDiligent.hpp"
#include "MapHelper.hpp"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "SwapChain.h"
#if F3D_DILIGENT_D3D12
#include "EngineFactoryD3D12.h"
#endif
#if F3D_DILIGENT_VULKAN
#include "EngineFactoryVk.h"
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Diligent;

namespace {

enum class Backend { D3D12, Vulkan };

struct Options {
    Backend backend =
#if F3D_DILIGENT_D3D12
        Backend::D3D12;
#else
        Backend::Vulkan;
#endif
    int frames = -1;
    bool hidden = false;
    bool viewports = true;
    bool validation = false;
    bool vsync = true;
    std::string capture;
};

std::optional<Options> parse_options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--rhi=d3d12") {
            o.backend = Backend::D3D12;
        } else if (a == "--rhi=vulkan") {
            o.backend = Backend::Vulkan;
        } else if (a == "--frames" && i + 1 < argc) {
            const std::string_view v = argv[++i];
            if (std::from_chars(v.data(), v.data() + v.size(), o.frames).ec != std::errc{}) {
                return std::nullopt;
            }
        } else if (a == "--hidden") {
            o.hidden = true;
        } else if (a == "--no-viewports") {
            o.viewports = false;
        } else if (a == "--no-vsync") {
            o.vsync = false;
        } else if (a == "--validation") {
            o.validation = true;
        } else if (a == "--capture" && i + 1 < argc) {
            o.capture = argv[++i];
        } else {
            F3D_LOG_ERROR("unknown argument: {}", a);
            return std::nullopt;
        }
    }
    return o;
}

const char* yes_no(DEVICE_FEATURE_STATE s) {
    switch (s) {
    case DEVICE_FEATURE_STATE_ENABLED:
        return "yes";
    case DEVICE_FEATURE_STATE_OPTIONAL:
        return "optional";
    default:
        return "no";
    }
}

std::optional<NativeWindow> native_window(SDL_Window* window) {
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
#if defined(_WIN32)
    void* hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!hwnd) {
        return std::nullopt;
    }
    return NativeWindow{Win32NativeWindow{hwnd}};
#elif defined(__linux__)
    LinuxNativeWindow w;
    w.pDisplay = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    w.WindowId = static_cast<Uint32>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    if (!w.pDisplay || w.WindowId == 0) {
        return std::nullopt;
    }
    return NativeWindow{w};
#else
    (void)props;
    return std::nullopt;
#endif
}

/// Device, immediate context and the factory used to create swapchains.
struct Gpu {
    Backend backend = Backend::D3D12;
    RefCntAutoPtr<IRenderDevice> device;
    RefCntAutoPtr<IDeviceContext> context;
    IEngineFactory* factory = nullptr;

    bool create(const Options& o) {
        backend = o.backend;
        DeviceFeatures features(DEVICE_FEATURE_STATE_OPTIONAL); // enable everything the adapter offers
        switch (o.backend) {
        case Backend::D3D12: {
#if F3D_DILIGENT_D3D12
            IEngineFactoryD3D12* f = GetEngineFactoryD3D12();
            EngineD3D12CreateInfo ci;
            ci.EnableValidation = o.validation;
            ci.Features = features;
            f->CreateDeviceAndContextsD3D12(ci, &device, &context);
            factory = f;
#endif
            break;
        }
        case Backend::Vulkan: {
#if F3D_DILIGENT_VULKAN
            IEngineFactoryVk* f = GetEngineFactoryVk();
            EngineVkCreateInfo ci;
            ci.EnableValidation = o.validation;
            ci.Features = features;
            f->CreateDeviceAndContextsVk(ci, &device, &context);
            factory = f;
#endif
            break;
        }
        }
        return device && context;
    }

    RefCntAutoPtr<ISwapChain> create_swapchain(SDL_Window* window, Uint32 width, Uint32 height) {
        RefCntAutoPtr<ISwapChain> swapchain;
        const auto native = native_window(window);
        if (!native) {
            F3D_LOG_ERROR("no native window handle");
            return swapchain;
        }
        SwapChainDesc desc;
        desc.Width = width;
        desc.Height = height;
        desc.ColorBufferFormat = TEX_FORMAT_RGBA8_UNORM_SRGB;
        desc.DepthBufferFormat = TEX_FORMAT_UNKNOWN;
        desc.Usage = SWAP_CHAIN_USAGE_RENDER_TARGET | SWAP_CHAIN_USAGE_COPY_SOURCE;
        switch (backend) {
        case Backend::D3D12:
#if F3D_DILIGENT_D3D12
            static_cast<IEngineFactoryD3D12*>(factory)->CreateSwapChainD3D12(device, context, desc,
                                                                             FullScreenModeDesc{}, *native, &swapchain);
#endif
            break;
        case Backend::Vulkan:
#if F3D_DILIGENT_VULKAN
            static_cast<IEngineFactoryVk*>(factory)->CreateSwapChainVk(device, context, desc, *native, &swapchain);
#endif
            break;
        }
        return swapchain;
    }
};

void print_capabilities(IRenderDevice* device) {
    const GraphicsAdapterInfo& adapter = device->GetAdapterInfo();
    const RenderDeviceInfo& info = device->GetDeviceInfo();
    const DeviceFeatures& f = info.Features;
    const char* type = adapter.Type == ADAPTER_TYPE_DISCRETE     ? "discrete"
                       : adapter.Type == ADAPTER_TYPE_INTEGRATED ? "integrated"
                       : adapter.Type == ADAPTER_TYPE_SOFTWARE   ? "software"
                                                                 : "unknown";
    F3D_LOG_INFO("Adapter: {} ({}), API {}.{}", adapter.Description, type, info.APIVersion.Major,
                 info.APIVersion.Minor);
    F3D_LOG_INFO("  compute shaders:     {}", yes_no(f.ComputeShaders));
    F3D_LOG_INFO("  bindless resources:  {}", yes_no(f.BindlessResources));
    F3D_LOG_INFO("  ray tracing:         {}", yes_no(f.RayTracing));
    const auto rt = adapter.RayTracing.CapFlags;
    F3D_LOG_INFO("    standalone shaders {} | inline (RayQuery) {} | indirect {} | max recursion {}",
                 (rt & RAY_TRACING_CAP_FLAG_STANDALONE_SHADERS) != 0,
                 (rt & RAY_TRACING_CAP_FLAG_INLINE_RAY_TRACING) != 0,
                 (rt & RAY_TRACING_CAP_FLAG_INDIRECT_RAY_TRACING) != 0, adapter.RayTracing.MaxRecursionDepth);
    F3D_LOG_INFO("  mesh shaders:        {}", yes_no(f.MeshShaders));
    F3D_LOG_INFO("  wave ops:            {}", yes_no(f.WaveOp));
    F3D_LOG_INFO("  shader float16:      {}", yes_no(f.ShaderFloat16));
    F3D_LOG_INFO("  BC7 textures:        {}",
                 device->GetTextureFormatInfo(TEX_FORMAT_BC7_UNORM).Supported ? "yes" : "no");
    F3D_LOG_INFO("  device-local memory: {} MB", adapter.Memory.LocalMemory / (1024 * 1024));
}

// --- Compute gradient -------------------------------------------------------

constexpr const char* kGradientCS = R"(
cbuffer Constants
{
    float4 g_Params; // x = time
};
RWTexture2D<float4> g_Output;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    g_Output.GetDimensions(w, h);
    if (id.x >= w || id.y >= h)
        return;
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    float  t  = 0.5 + 0.5 * sin(g_Params.x + uv.x * 6.2831853);
    g_Output[id.xy] = float4(uv.x, uv.y, t, 1.0);
}
)";

struct ComputeGradient {
    static constexpr Uint32 kSize = 256;
    RefCntAutoPtr<IPipelineState> pso;
    RefCntAutoPtr<IShaderResourceBinding> srb;
    RefCntAutoPtr<IBuffer> constants;
    RefCntAutoPtr<ITexture> texture;

    bool create(IRenderDevice* device) {
        ShaderCreateInfo sci;
        sci.SourceLanguage = SHADER_SOURCE_LANGUAGE_HLSL;
        sci.Desc.ShaderType = SHADER_TYPE_COMPUTE;
        sci.Desc.Name = "Gradient CS";
        sci.Desc.UseCombinedTextureSamplers = true;
        sci.EntryPoint = "main";
        sci.Source = kGradientCS;
        RefCntAutoPtr<IShader> cs;
        device->CreateShader(sci, &cs);
        if (!cs) {
            return false;
        }

        ComputePipelineStateCreateInfo pci;
        pci.PSODesc.Name = "Gradient PSO";
        pci.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
        pci.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
        pci.pCS = cs;
        device->CreateComputePipelineState(pci, &pso);
        if (!pso) {
            return false;
        }

        BufferDesc bd;
        bd.Name = "Gradient constants";
        bd.Size = sizeof(float) * 4;
        bd.Usage = USAGE_DYNAMIC;
        bd.BindFlags = BIND_UNIFORM_BUFFER;
        bd.CPUAccessFlags = CPU_ACCESS_WRITE;
        device->CreateBuffer(bd, nullptr, &constants);

        // RGBA32F: glslang's HLSL front end gives an untyped float4 storage image
        // the rgba32f format, so the texture format must match for Vulkan.
        TextureDesc td;
        td.Name = "Gradient texture";
        td.Type = RESOURCE_DIM_TEX_2D;
        td.Width = kSize;
        td.Height = kSize;
        td.Format = TEX_FORMAT_RGBA32_FLOAT;
        td.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
        device->CreateTexture(td, nullptr, &texture);
        if (!constants || !texture) {
            return false;
        }

        pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "Constants")->Set(constants);
        pso->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "g_Output")
            ->Set(texture->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
        pso->CreateShaderResourceBinding(&srb, true);
        return srb != nullptr;
    }

    void dispatch(IDeviceContext* ctx, float time) {
        {
            MapHelper<float> data(ctx, constants, MAP_WRITE, MAP_FLAG_DISCARD);
            data[0] = time;
            data[1] = data[2] = data[3] = 0.0f;
        }
        ctx->SetPipelineState(pso);
        ctx->CommitShaderResources(srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        DispatchComputeAttribs attribs;
        attribs.ThreadGroupCountX = (kSize + 7) / 8;
        attribs.ThreadGroupCountY = (kSize + 7) / 8;
        ctx->DispatchCompute(attribs);
    }

    [[nodiscard]] ImTextureID imgui_id() const {
        return reinterpret_cast<ImTextureID>(texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    }
};

// --- Capture --------------------------------------------------------------------

bool capture_backbuffer(IRenderDevice* device, IDeviceContext* ctx, ISwapChain* swapchain, const std::string& path) {
    ITexture* backbuffer = swapchain->GetCurrentBackBufferRTV()->GetTexture();
    TextureDesc desc = backbuffer->GetDesc();
    desc.Name = "Capture staging";
    desc.Usage = USAGE_STAGING;
    desc.BindFlags = BIND_NONE;
    desc.CPUAccessFlags = CPU_ACCESS_READ;
    desc.MiscFlags = MISC_TEXTURE_FLAG_NONE;
    RefCntAutoPtr<ITexture> staging;
    device->CreateTexture(desc, nullptr, &staging);
    if (!staging) {
        return false;
    }
    CopyTextureAttribs copy(backbuffer, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, staging,
                            RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    ctx->CopyTexture(copy);
    ctx->WaitForIdle();

    MappedTextureSubresource mapped;
    ctx->MapTextureSubresource(staging, 0, 0, MAP_READ, MAP_FLAG_DO_NOT_WAIT, nullptr, mapped);
    if (!mapped.pData) {
        return false;
    }
    const bool bgra = desc.Format == TEX_FORMAT_BGRA8_UNORM || desc.Format == TEX_FORMAT_BGRA8_UNORM_SRGB;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
    for (Uint32 y = 0; y < desc.Height; ++y) {
        const auto* src = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(mapped.Stride) * y;
        std::uint8_t* dst = pixels.data() + static_cast<std::size_t>(desc.Width) * 4 * y;
        for (Uint32 x = 0; x < desc.Width; ++x) {
            dst[x * 4 + 0] = src[x * 4 + (bgra ? 2 : 0)];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + (bgra ? 0 : 2)];
            dst[x * 4 + 3] = 255;
        }
    }
    ctx->UnmapTextureSubresource(staging, 0, 0);
    const int ok = stbi_write_png(path.c_str(), static_cast<int>(desc.Width), static_cast<int>(desc.Height), 4,
                                  pixels.data(), static_cast<int>(desc.Width) * 4);
    return ok != 0;
}

// --- ImGui multi-viewport renderer hooks -------------------------------------
// Diligent's ImGui renderer only draws the main viewport. These hooks give each
// secondary ImGui viewport (an SDL window created by imgui_impl_sdl3) its own
// Diligent swapchain and reuse the renderer to draw its draw data.

class ImGuiBackend : public ImGuiImplDiligent {
public:
    using ImGuiImplDiligent::ImGuiImplDiligent;
    ImGuiDiligentRenderer& renderer() { return *m_pRenderer; }
};

struct ViewportRenderer {
    Gpu* gpu = nullptr;
    ImGuiBackend* imgui = nullptr;
    Uint32 main_width = 0;
    Uint32 main_height = 0;
    std::string capture_prefix; ///< non-empty: capture each secondary viewport this frame
    int captured = 0;
};
ViewportRenderer g_viewports;

struct ViewportData {
    RefCntAutoPtr<ISwapChain> swapchain;
};

void install_viewport_hooks() {
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Renderer_CreateWindow = [](ImGuiViewport* vp) {
        // imgui_impl_sdl3 stores the SDL window ID in PlatformHandle.
        SDL_Window* window =
            SDL_GetWindowFromID(static_cast<SDL_WindowID>(reinterpret_cast<std::uintptr_t>(vp->PlatformHandle)));
        auto* data = IM_NEW(ViewportData)();
        data->swapchain =
            g_viewports.gpu->create_swapchain(window, static_cast<Uint32>(vp->Size.x), static_cast<Uint32>(vp->Size.y));
        vp->RendererUserData = data;
        F3D_LOG_INFO("ImGui viewport window created ({}x{}): swapchain {}", vp->Size.x, vp->Size.y,
                     data->swapchain ? "ok" : "FAILED");
    };
    pio.Renderer_DestroyWindow = [](ImGuiViewport* vp) {
        if (auto* data = static_cast<ViewportData*>(vp->RendererUserData)) {
            g_viewports.gpu->context->Flush();
            g_viewports.gpu->context->WaitForIdle();
            IM_DELETE(data);
        }
        vp->RendererUserData = nullptr;
    };
    pio.Renderer_SetWindowSize = [](ImGuiViewport* vp, ImVec2 size) {
        if (auto* data = static_cast<ViewportData*>(vp->RendererUserData); data && data->swapchain) {
            data->swapchain->Resize(static_cast<Uint32>(size.x), static_cast<Uint32>(size.y));
        }
    };
    pio.Renderer_RenderWindow = [](ImGuiViewport* vp, void*) {
        auto* data = static_cast<ViewportData*>(vp->RendererUserData);
        if (!data || !data->swapchain) {
            return;
        }
        IDeviceContext* ctx = g_viewports.gpu->context;
        const SwapChainDesc& sc = data->swapchain->GetDesc();
        ITextureView* rtv = data->swapchain->GetCurrentBackBufferRTV();
        ctx->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        const float clear[4] = {0.f, 0.f, 0.f, 1.f};
        ctx->ClearRenderTarget(rtv, clear, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        // The renderer takes the target size from NewFrame(); switch it for this window.
        ImGuiDiligentRenderer& r = g_viewports.imgui->renderer();
        r.NewFrame(sc.Width, sc.Height, SURFACE_TRANSFORM_IDENTITY);
        const ImGuiDiligentRenderTargetDesc rt{sc.ColorBufferFormat, sc.DepthBufferFormat};
        r.RenderDrawData(ctx, vp->DrawData, &rt);
        r.NewFrame(g_viewports.main_width, g_viewports.main_height, SURFACE_TRANSFORM_IDENTITY);
        if (!g_viewports.capture_prefix.empty()) {
            const std::string path =
                g_viewports.capture_prefix + "-viewport" + std::to_string(g_viewports.captured++) + ".png";
            if (capture_backbuffer(g_viewports.gpu->device, ctx, data->swapchain, path)) {
                F3D_LOG_INFO("Captured secondary viewport {}", path);
            }
        }
    };
    pio.Renderer_SwapBuffers = [](ImGuiViewport* vp, void*) {
        if (auto* data = static_cast<ViewportData*>(vp->RendererUserData); data && data->swapchain) {
            data->swapchain->Present(0);
        }
    };
}

} // namespace

int main(int argc, char** argv) {
    f3d::core::log::init();
    const auto parsed = parse_options(argc, argv);
    if (!parsed) {
        F3D_LOG_ERROR("usage: forge3d-spike-diligent [--rhi=d3d12|vulkan] [--frames N] [--hidden] "
                      "[--capture out.png] [--no-viewports] [--validation]");
        return 2;
    }
    const Options opt = *parsed;
    const char* backend_name = opt.backend == Backend::D3D12 ? "D3D12" : "Vulkan";

#if defined(__linux__)
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11"); // Diligent's Vulkan surface needs an X11 window
#endif
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        F3D_LOG_ERROR("SDL_Init: {}", SDL_GetError());
        return 1;
    }
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (opt.hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    SDL_Window* window = SDL_CreateWindow("forge3d Diligent spike", 1280, 720, flags);
    if (!window) {
        F3D_LOG_ERROR("SDL_CreateWindow: {}", SDL_GetError());
        return 1;
    }
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);

    Gpu gpu;
    if (!gpu.create(opt)) {
        F3D_LOG_ERROR("{}: device creation failed", backend_name);
        return 1;
    }
    F3D_LOG_INFO("Backend: {}", backend_name);
    print_capabilities(gpu.device);

    RefCntAutoPtr<ISwapChain> swapchain =
        gpu.create_swapchain(window, static_cast<Uint32>(pw), static_cast<Uint32>(ph));
    if (!swapchain) {
        F3D_LOG_ERROR("{}: swapchain creation failed", backend_name);
        return 1;
    }

    ComputeGradient gradient;
    if (!gradient.create(gpu.device)) {
        F3D_LOG_ERROR("compute pipeline creation failed");
        return 1;
    }

    // ImGui: Diligent renderer (ImGuiImplDiligent creates the ImGui context) + SDL3 platform backend.
    const SwapChainDesc& scd = swapchain->GetDesc();
    ImGuiDiligentCreateInfo ici(gpu.device, scd);
    ImGuiBackend imgui(ici);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    if (opt.viewports) {
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
    }
    ImGui_ImplSDL3_InitForOther(window);
    g_viewports = {&gpu, &imgui, scd.Width, scd.Height};
    if (opt.viewports) {
        install_viewport_hooks();
    }

    bool running = true;
    bool demo_open = true;
    int frame = 0;
    const Uint64 start_ticks = SDL_GetTicks();
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT ||
                (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(window))) {
                running = false;
            }
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && e.window.windowID == SDL_GetWindowID(window)) {
                swapchain->Resize(static_cast<Uint32>(e.window.data1), static_cast<Uint32>(e.window.data2));
            }
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        const float time = static_cast<float>(SDL_GetTicks() - start_ticks) / 1000.0f;
        gradient.dispatch(gpu.context, time);

        const SwapChainDesc& sc = swapchain->GetDesc();
        g_viewports.main_width = sc.Width;
        g_viewports.main_height = sc.Height;
        ImGui_ImplSDL3_NewFrame();
        imgui.NewFrame(sc.Width, sc.Height, sc.PreTransform);
        ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
        // With multi-viewport enabled ImGui positions are desktop coordinates.
        const ImVec2 origin = ImGui::GetMainViewport()->WorkPos;
        ImGui::ShowDemoWindow(&demo_open); // places itself at origin + (650, 20)
        ImGui::SetNextWindowPos(ImVec2(origin.x + 40, origin.y + 60), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(300, 340), ImGuiCond_FirstUseEver);
        ImGui::Begin("Compute gradient");
        ImGui::Text("%s, frame %d", backend_name, frame);
        ImGui::Image(gradient.imgui_id(), ImVec2(256, 256));
        ImGui::End();
        if (opt.viewports) {
            // Deliberately outside the main window: ImGui gives it its own OS window.
            ImGui::SetNextWindowPos(ImVec2(origin.x - 340, origin.y + 40), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(300, 120), ImGuiCond_FirstUseEver);
            ImGui::Begin("Floating viewport");
            ImGui::Text("Separate OS window + Diligent swapchain");
            ImGui::Text("%s, frame %d", backend_name, frame);
            ImGui::End();
        }

        ITextureView* rtv = swapchain->GetCurrentBackBufferRTV();
        gpu.context->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        const float clear[4] = {0.10f, 0.12f, 0.16f, 1.0f};
        gpu.context->ClearRenderTarget(rtv, clear, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        imgui.Render(gpu.context);

        ++frame;
        const bool last = opt.frames >= 0 && frame >= opt.frames;
        if (last && !opt.capture.empty()) {
            if (capture_backbuffer(gpu.device, gpu.context, swapchain, opt.capture)) {
                F3D_LOG_INFO("Captured {}", opt.capture);
            } else {
                F3D_LOG_ERROR("Capture failed");
                return 1;
            }
        }
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            if (last && !opt.capture.empty()) {
                const auto dot = opt.capture.rfind('.');
                g_viewports.capture_prefix = opt.capture.substr(0, dot);
            }
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            g_viewports.capture_prefix.clear();
        }
        swapchain->Present(opt.vsync ? 1 : 0);
        if (last) {
            running = false;
        }
    }

    gpu.context->Flush();
    gpu.context->WaitForIdle();
    ImGui_ImplSDL3_Shutdown();
    const double seconds = static_cast<double>(SDL_GetTicks() - start_ticks) / 1000.0;
    F3D_LOG_INFO("{}: ran {} frames in {:.2f} s ({:.2f} ms/frame)", backend_name, frame, seconds,
                 frame > 0 ? seconds * 1000.0 / frame : 0.0);
    // imgui (ImGuiImplDiligent) destroys the ImGui context in its destructor.
    return 0;
}
