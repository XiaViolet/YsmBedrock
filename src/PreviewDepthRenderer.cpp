#include "PreviewDepthRenderer.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace ysm {
namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT hr, char const* op) {
    if (FAILED(hr)) throw std::runtime_error(std::string("Preview ") + op + " failed (" + std::to_string(hr) + ")");
}

constexpr char shader[] = R"(
Texture2D skin : register(t0);
SamplerState atlasSampler : register(s0);
struct Vertex { float3 p : POSITION; float2 uv : TEXCOORD0; uint argb : COLOR0; };
struct Pixel { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 tint : COLOR0; };
Pixel transform(Vertex v) {
    Pixel o;
    o.p = float4(v.p.x * 2 - 1, 1 - v.p.y * 2, v.p.z, 1);
    o.uv = v.uv;
    uint c = v.argb;
    o.tint = float4((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24) / 255.0;
    return o;
}
float4 opaque(Pixel p) : SV_Target {
    float4 c = skin.Sample(atlasSampler, p.uv) * p.tint;
    if (c.a < 0.9999) discard;
    return c;
}
float4 translucent(Pixel p) : SV_Target {
    float4 c = skin.Sample(atlasSampler, p.uv) * p.tint;
    if (c.a <= 0 || c.a >= 0.9999) discard;
    return c;
}
)";
ComPtr<ID3DBlob> compile(char const* entry, char const* profile) {
    ComPtr<ID3DBlob> bytecode, error;
    auto hr = D3DCompile(shader, sizeof(shader)-1, "menu-preview", nullptr, nullptr, entry,
                         profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &error);
    if (FAILED(hr) && error) throw std::runtime_error(static_cast<char const*>(error->GetBufferPointer()));
    check(hr, entry);
    return bytecode;
}

D3D11_TEXTURE2D_DESC textureDesc(unsigned width, unsigned height, DXGI_FORMAT format, UINT bind) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.SampleDesc.Count = 1; desc.Format = format; desc.BindFlags = bind;
    return desc;
}
}

struct PreviewDepthRenderer::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> opaqueShader, translucentShader;
    ComPtr<ID3D11DepthStencilState> writeDepth, readDepth;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vertices;
    UINT capacity = 0;
    ComPtr<ID3D11Texture2D> color, readback;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11DepthStencilView> depth;
    ComPtr<ID3D11ShaderResourceView> atlas;
    std::weak_ptr<TextureImage const> lastSkin;
    unsigned side = 0;
    bool skinTranslucent = false;

    Impl() {
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                &device, nullptr, &ctx), "D3D11 device");
        auto vs = compile("transform", "vs_4_0");
        check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertexShader), "vertex shader");
        auto ps = [&](char const* name, ID3D11PixelShader** output) {
            auto code = compile(name, "ps_4_0");
            check(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, output), name);
        };
        ps("opaque", opaqueShader.GetAddressOf());
        ps("translucent", translucentShader.GetAddressOf());
        D3D11_INPUT_ELEMENT_DESC fields[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(PreviewMeshVertex,x), D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(PreviewMeshVertex,u), D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32_UINT, 0, offsetof(PreviewMeshVertex,color), D3D11_INPUT_PER_VERTEX_DATA, 0}
        };
        check(device->CreateInputLayout(fields, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &layout), "vertex layout");
        ctx->IASetInputLayout(layout.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vertexShader.Get(), nullptr, 0);

        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> raster;
        check(device->CreateRasterizerState(&rs, &raster), "rasterizer");
        ctx->RSSetState(raster.Get());
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER; sd.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        check(device->CreateSamplerState(&sd, &sampler), "skin sampler");
        ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = TRUE; ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        check(device->CreateDepthStencilState(&ds, &writeDepth), "opaque depth state");
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        check(device->CreateDepthStencilState(&ds, &readDepth), "transparent depth state");
        D3D11_BLEND_DESC bd{};
        auto& b = bd.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D11_BLEND_SRC_ALPHA; b.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; b.BlendOp = D3D11_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D11_BLEND_ONE; b.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        check(device->CreateBlendState(&bd, &blend), "transparent blend");
    }

    void resize(unsigned size) {
        if (size == side) return;
        if (!size || size > 1024) throw std::runtime_error("Invalid preview target size");
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        auto desc = textureDesc(size, size, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
        check(device->CreateTexture2D(&desc, nullptr, color.ReleaseAndGetAddressOf()), "color surface");
        check(device->CreateRenderTargetView(color.Get(), nullptr, target.ReleaseAndGetAddressOf()), "color target");
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&desc, nullptr, readback.ReleaseAndGetAddressOf()), "readback");
        desc = textureDesc(size, size, DXGI_FORMAT_D32_FLOAT, D3D11_BIND_DEPTH_STENCIL);
        ComPtr<ID3D11Texture2D> z;
        check(device->CreateTexture2D(&desc, nullptr, &z), "depth surface");
        check(device->CreateDepthStencilView(z.Get(), nullptr, depth.ReleaseAndGetAddressOf()), "depth target");
        D3D11_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
        ctx->RSSetViewports(1, &viewport);
        side = size;
    }

    void bindSkin(std::shared_ptr<TextureImage const> const& skin) {
        if (!skin || skin->width == 0 || skin->height == 0 ||
            skin->rgba.size() != size_t(skin->width) * skin->height * 4)
            throw std::runtime_error("Invalid preview skin");
        if (lastSkin.lock() == skin) return;
        skinTranslucent=false;
        for(std::size_t i=3;i<skin->rgba.size();i+=4) if(skin->rgba[i]>0 && skin->rgba[i]<255){skinTranslucent=true;break;}
        auto desc = textureDesc(skin->width, skin->height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        D3D11_SUBRESOURCE_DATA source{skin->rgba.data(), skin->width * 4, 0};
        ComPtr<ID3D11Texture2D> texture;
        check(device->CreateTexture2D(&desc, &source, &texture), "skin texture");
        check(device->CreateShaderResourceView(texture.Get(), nullptr, atlas.ReleaseAndGetAddressOf()), "skin view");
        ctx->PSSetShaderResources(0, 1, atlas.GetAddressOf());
        lastSkin = skin;
    }

    void draw(std::span<PreviewMeshVertex const> data) {
        if (data.empty()) return;
        if (data.size_bytes() > std::numeric_limits<UINT>::max()) throw std::runtime_error("Preview mesh too large");
        UINT bytes = static_cast<UINT>(data.size_bytes());
        if (bytes > capacity) {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = static_cast<UINT>(std::min<std::uint64_t>(std::numeric_limits<UINT>::max(), std::max<std::uint64_t>(bytes, std::uint64_t(capacity) + capacity / 2 + 4096))); desc.Usage = D3D11_USAGE_DYNAMIC; desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            check(device->CreateBuffer(&desc, nullptr, vertices.ReleaseAndGetAddressOf()), "vertex buffer");
            capacity = desc.ByteWidth;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(ctx->Map(vertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "vertex upload");
        std::memcpy(mapped.pData, data.data(), bytes);
        ctx->Unmap(vertices.Get(), 0);
        UINT stride = sizeof(PreviewMeshVertex), offset = 0;
        ctx->IASetVertexBuffers(0, 1, vertices.GetAddressOf(), &stride, &offset);
        ctx->OMSetDepthStencilState(writeDepth.Get(), 0);
        ctx->OMSetBlendState(nullptr, nullptr, UINT_MAX);
        ctx->PSSetShader(opaqueShader.Get(), nullptr, 0);
        ctx->Draw(static_cast<UINT>(data.size()), 0);
        if (skinTranslucent || std::any_of(data.begin(),data.end(),[](auto const& vertex){auto alpha=vertex.color>>24;return alpha>0 && alpha<255;})) {
            ctx->OMSetDepthStencilState(readDepth.Get(), 0);
            ctx->OMSetBlendState(blend.Get(), nullptr, UINT_MAX);
            ctx->PSSetShader(translucentShader.Get(), nullptr, 0);
            ctx->Draw(static_cast<UINT>(data.size()), 0);
        }
    }
};

PreviewDepthRenderer::PreviewDepthRenderer() : impl(std::make_unique<Impl>()) {}
PreviewDepthRenderer::~PreviewDepthRenderer() = default;
void PreviewDepthRenderer::clearFrame() {
    auto& g = *impl;
    if (!g.side && !g.atlas && !g.vertices && !g.color) return;
    ID3D11ShaderResourceView* emptyAtlas = nullptr;
    g.ctx->PSSetShaderResources(0, 1, &emptyAtlas);
    ID3D11Buffer* emptyBuffer = nullptr;
    UINT zero = 0;
    g.ctx->IASetVertexBuffers(0, 1, &emptyBuffer, &zero, &zero);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g.atlas.Reset();
    g.lastSkin.reset();
    g.vertices.Reset();
    g.capacity = 0;
    g.target.Reset();
    g.depth.Reset();
    g.color.Reset();
    g.readback.Reset();
    g.side = 0;
}
TextureImage PreviewDepthRenderer::render(PreviewMesh const& mesh, unsigned side) {
    auto& g = *impl;
    g.resize(side);
    g.bindSkin(mesh.skin);
    g.ctx->OMSetRenderTargets(1, g.target.GetAddressOf(), g.depth.Get());
    constexpr float clear[4]{};
    g.ctx->ClearRenderTargetView(g.target.Get(), clear);
    g.ctx->ClearDepthStencilView(g.depth.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    g.draw(mesh.vertices);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g.ctx->CopyResource(g.readback.Get(), g.color.Get());
    TextureImage out;
    out.width = out.height = side;
    out.rgba.resize(size_t(side) * side * 4);
    D3D11_MAPPED_SUBRESOURCE pixels{};
    check(g.ctx->Map(g.readback.Get(), 0, D3D11_MAP_READ, 0, &pixels), "frame readback");
    for (UINT row = 0; row < side; ++row) {
        auto src = static_cast<unsigned char const*>(pixels.pData) + size_t(row) * pixels.RowPitch;
        std::memcpy(out.rgba.data() + size_t(row) * side * 4, src, size_t(side) * 4);
    }
    g.ctx->Unmap(g.readback.Get(), 0);
    // The game composites straight-alpha UI textures; the offscreen blend accumulated premultiplied RGB.
    for (size_t i = 0; i < out.rgba.size(); i += 4) {
        unsigned a = out.rgba[i+3];
        if (a && a != 255) for (size_t c = 0; c < 3; ++c)
            out.rgba[i+c] = static_cast<unsigned char>(std::min(255u, (out.rgba[i+c]*255u+a/2)/a));
    }
    return out;
}
}
