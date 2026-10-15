#include <algorithm>
// HUD split. See framecap.h.
#include "framecap.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <cmath>
#include <d3dcompiler.h>
#include "MinHook.h"
#include "log.h"

namespace wdvr::framecap {

static volatile bool g_hooked;

using SetRTFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIdxFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawInstFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawIdxInstFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using BlendFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);

static SetRTFn o_setrt; static DrawFn o_draw; static DrawIdxFn o_drawidx; static DrawInstFn o_drawinst;
static DrawIdxInstFn o_drawidxinst; static BlendFn o_blend;

// ---------------------------------------------------------------- HUD split
// The game draws the finished picture into one full-screen texture with a single
// full-screen draw, then draws the HUD on top of it, then copies that texture to
// the screen. We let the first draw through and send the HUD draws to our own
// see-through texture instead, so the headset gets a clean picture and the HUD
// can be shown on a floating panel.
static bool  g_hud_enabled;
static void* g_bb;                      // backbuffer (this frame)
static void* g_last_rt_with_draws;      // last render target that got draws
static void* g_cand; static int g_cand_frames;
static void* g_target;                  // the full-screen texture the HUD is drawn into
static bool  g_in_target, g_first_ok, g_redirecting, g_redirected_frame, g_cur_has_draw;
static int   g_target_draws;
static bool  g_composited;
static void* g_cur_rt;
static ID3D11DepthStencilView* g_target_dsv;
static ID3D11Texture2D* g_hud_tex; static ID3D11RenderTargetView* g_hud_rtv; static ID3D11ShaderResourceView* g_hud_srv;
static UINT g_hud_w, g_hud_h; static DXGI_FORMAT g_hud_rtv_fmt;
static struct { ID3D11BlendState* game; ID3D11BlendState* ours; } g_blend_map[64]; static int g_blend_n;
static unsigned g_hud_frames;
static char g_hud_status[160] = "off";

static struct { DXGI_FORMAT f; ID3D11RenderTargetView* v; } g_hud_views[4]; static int g_hud_nviews;
static bool  g_in_present;              // our own drawing (Present hook) - not the game's
static float g_world_rect[4] = { 0.25f, 0.05f, 0.75f, 0.95f };  // HUD inside this stays in the 3D picture
static bool  g_world_center = true;
static bool  g_excl_on; static float g_excl[4];      // e.g. the phone: never in the world
static bool  g_excl2_on; static float g_excl2[4];    // the game's crosshair (screen centre): never in the world (0.16.6)
// rect r minus the box ex -> up to 4 rectangles appended to out
static void cut_rect(const D3D11_RECT& r, LONG ex0, LONG ey0, LONG ex1, LONG ey1, D3D11_RECT* out, int& n) {
    if (ex1 <= r.left || ex0 >= r.right || ey1 <= r.top || ey0 >= r.bottom) { out[n++] = r; return; }
    if (ey0 > r.top) out[n++] = { r.left, r.top, r.right, ey0 };
    if (ey1 < r.bottom) out[n++] = { r.left, ey1, r.right, r.bottom };
    LONG t = ey0 > r.top ? ey0 : r.top, b = ey1 < r.bottom ? ey1 : r.bottom;
    if (ex0 > r.left) out[n++] = { r.left, t, ex0, b };
    if (ex1 < r.right) out[n++] = { ex1, t, r.right, b };
}
static DXGI_FORMAT g_hud_view_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;

static void release_hud_tex() {
    for (int i = 0; i < g_hud_nviews; i++) if (g_hud_views[i].v) g_hud_views[i].v->Release();
    g_hud_nviews = 0; g_hud_rtv = nullptr;
    if (g_hud_srv) { g_hud_srv->Release(); g_hud_srv = nullptr; }
    if (g_hud_tex) { g_hud_tex->Release(); g_hud_tex = nullptr; }
}

// Our see-through HUD texture (same size as the game's), with a render view in
// whatever format the game is using right now (it switches between sRGB and plain).
static bool ensure_hud_target(ID3D11DeviceContext* c, ID3D11RenderTargetView* game_rtv) {
    ID3D11Resource* r = nullptr; game_rtv->GetResource(&r);
    ID3D11Texture2D* t = nullptr;
    if (r) { r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); }
    if (!t) return false;
    D3D11_TEXTURE2D_DESC td; t->GetDesc(&td); t->Release();
    D3D11_RENDER_TARGET_VIEW_DESC rd; game_rtv->GetDesc(&rd);
    if (g_hud_tex && (g_hud_w != td.Width || g_hud_h != td.Height)) release_hud_tex();
    ID3D11Device* dev = nullptr; c->GetDevice(&dev);
    if (!dev) return false;
    bool ok = true;
    if (!g_hud_tex) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = td.Width; d.Height = td.Height; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS; d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g_hud_tex)) &&
             SUCCEEDED(dev->CreateShaderResourceView(g_hud_tex, &sv, &g_hud_srv));
        log("HUD: own texture %ux%u (game texture format %u): %s", td.Width, td.Height, td.Format, ok ? "ok" : "FAILED");
        g_hud_w = td.Width; g_hud_h = td.Height;
    }
    g_hud_rtv = nullptr;
    for (int i = 0; ok && i < g_hud_nviews; i++) if (g_hud_views[i].f == rd.Format) g_hud_rtv = g_hud_views[i].v;
    if (ok && !g_hud_rtv && g_hud_nviews < 4) {
        D3D11_RENDER_TARGET_VIEW_DESC rv = {}; rv.Format = rd.Format; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11RenderTargetView* v = nullptr;
        if (SUCCEEDED(dev->CreateRenderTargetView(g_hud_tex, &rv, &v)) && v) {
            g_hud_views[g_hud_nviews].f = rd.Format; g_hud_views[g_hud_nviews].v = v; g_hud_nviews++;
            g_hud_rtv = v;
            log("HUD: view for format %u ready", rd.Format);
        }
    }
    dev->Release();
    if (!ok || !g_hud_rtv) { log("HUD: could not create our texture - split off"); g_hud_enabled = false; release_hud_tex(); return false; }
    g_hud_rtv_fmt = rd.Format;
    return true;
}

// Put the middle part of the HUD (crosshair, markers over cameras and people,
// prompts - things that sit on top of the world) back into the picture, so they
// line up with the world in the headset. The edges go on the floating panel.
static ID3D11VertexShader* g_cmp_vs; static ID3D11PixelShader* g_cmp_ps;
static ID3D11BlendState* g_cmp_bs; static ID3D11RasterizerState* g_cmp_rs; static ID3D11SamplerState* g_cmp_smp;
static ID3D11DepthStencilState* g_cmp_ds;
static ID3D11RenderTargetView* g_cmp_rtv; static void* g_cmp_rtv_res; static DXGI_FORMAT g_cmp_rtv_fmt;
static bool g_cmp_failed;

static bool cmp_init(ID3D11Device* dev) {
    if (g_cmp_vs) return true;
    if (g_cmp_failed) return false;
    static const char* hlsl =
        "struct VO { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };"
        "VO vs(uint id : SV_VertexID) { VO o; float2 uv = float2((id << 1) & 2, id & 2); o.uv = uv;"
        "  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }"
        "Texture2D t : register(t0); SamplerState s : register(s0);"
        "float4 ps(VO i) : SV_TARGET { return t.Sample(s, i.uv); }";
    ID3DBlob *vb = nullptr, *pb = nullptr;
    bool ok = SUCCEEDED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vb, nullptr)) &&
              SUCCEEDED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &pb, nullptr));
    ok = ok && SUCCEEDED(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_cmp_vs)) &&
         SUCCEEDED(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_cmp_ps));
    if (vb) vb->Release(); if (pb) pb->Release();
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.ScissorEnable = TRUE; rd.DepthClipEnable = TRUE;
    D3D11_SAMPLER_DESC sd = {}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    D3D11_DEPTH_STENCIL_DESC dd = {};   // no depth, no stencil
    ok = ok && SUCCEEDED(dev->CreateBlendState(&bd, &g_cmp_bs)) && SUCCEEDED(dev->CreateRasterizerState(&rd, &g_cmp_rs)) &&
         SUCCEEDED(dev->CreateSamplerState(&sd, &g_cmp_smp)) && SUCCEEDED(dev->CreateDepthStencilState(&dd, &g_cmp_ds));
    if (!ok) { g_cmp_failed = true; log("HUD: could not build the in-world HUD shader - everything goes on the panel"); }
    return ok;
}

static void composite_center(ID3D11DeviceContext* c) {
    if (!g_world_center || !g_hud_srv || !g_target) return;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev);
    if (!dev) return;
    if (!cmp_init(dev)) { dev->Release(); return; }
    if (!g_cmp_rtv || g_cmp_rtv_res != g_target || g_cmp_rtv_fmt != g_hud_view_fmt) {
        if (g_cmp_rtv) { g_cmp_rtv->Release(); g_cmp_rtv = nullptr; }
        D3D11_RENDER_TARGET_VIEW_DESC rv = {}; rv.Format = g_hud_view_fmt; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        dev->CreateRenderTargetView((ID3D11Resource*)g_target, &rv, &g_cmp_rtv);
        g_cmp_rtv_res = g_target; g_cmp_rtv_fmt = g_hud_view_fmt;
    }
    dev->Release();
    if (!g_cmp_rtv) return;
    // save state
    ID3D11RenderTargetView* o_rtv = nullptr; ID3D11DepthStencilView* o_dsv = nullptr; c->OMGetRenderTargets(1, &o_rtv, &o_dsv);
    UINT nvp = 1; D3D11_VIEWPORT o_vp; c->RSGetViewports(&nvp, &o_vp);
    UINT nsc = 1; D3D11_RECT o_sc = {}; c->RSGetScissorRects(&nsc, &o_sc);
    ID3D11VertexShader* o_vs = nullptr; c->VSGetShader(&o_vs, nullptr, nullptr);
    ID3D11PixelShader* o_ps2 = nullptr; c->PSGetShader(&o_ps2, nullptr, nullptr);
    ID3D11GeometryShader* o_gs = nullptr; c->GSGetShader(&o_gs, nullptr, nullptr);
    ID3D11ShaderResourceView* o_srv = nullptr; c->PSGetShaderResources(0, 1, &o_srv);
    ID3D11SamplerState* o_smp = nullptr; c->PSGetSamplers(0, 1, &o_smp);
    ID3D11InputLayout* o_il = nullptr; c->IAGetInputLayout(&o_il);
    D3D11_PRIMITIVE_TOPOLOGY o_top; c->IAGetPrimitiveTopology(&o_top);
    ID3D11BlendState* o_bs = nullptr; float o_bf[4]; UINT o_mask; c->OMGetBlendState(&o_bs, o_bf, &o_mask);
    ID3D11RasterizerState* o_rs = nullptr; c->RSGetState(&o_rs);
    ID3D11DepthStencilState* o_ds = nullptr; UINT o_ref; c->OMGetDepthStencilState(&o_ds, &o_ref);

    D3D11_VIEWPORT vp = { 0, 0, (float)g_hud_w, (float)g_hud_h, 0, 1 };
    D3D11_RECT sc = { (LONG)(g_world_rect[0] * g_hud_w), (LONG)(g_world_rect[1] * g_hud_h),
                      (LONG)(g_world_rect[2] * g_hud_w), (LONG)(g_world_rect[3] * g_hud_h) };
    // The middle area minus the excluded areas (the phone, the game's crosshair), as up to 16 rectangles.
    D3D11_RECT parts[16]; int np = 0;
    {
        D3D11_RECT a[4]; int na = 0;
        if (g_excl_on) cut_rect(sc, (LONG)(g_excl[0] * g_hud_w), (LONG)(g_excl[1] * g_hud_h), (LONG)(g_excl[2] * g_hud_w), (LONG)(g_excl[3] * g_hud_h), a, na);
        else a[na++] = sc;
        for (int i = 0; i < na; i++) {
            if (g_excl2_on) cut_rect(a[i], (LONG)(g_excl2[0] * g_hud_w), (LONG)(g_excl2[1] * g_hud_h), (LONG)(g_excl2[2] * g_hud_w), (LONG)(g_excl2[3] * g_hud_h), parts, np);
            else parts[np++] = a[i];
        }
    }
    o_setrt(c, 1, &g_cmp_rtv, nullptr);
    c->RSSetViewports(1, &vp); c->RSSetScissorRects(1, &sc);
    c->VSSetShader(g_cmp_vs, nullptr, 0); c->PSSetShader(g_cmp_ps, nullptr, 0); c->GSSetShader(nullptr, nullptr, 0);
    c->PSSetShaderResources(0, 1, &g_hud_srv); c->PSSetSamplers(0, 1, &g_cmp_smp);
    c->IASetInputLayout(nullptr); c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const float bf[4] = { 0, 0, 0, 0 };
    o_blend(c, g_cmp_bs, bf, 0xFFFFFFFF); c->RSSetState(g_cmp_rs); c->OMSetDepthStencilState(g_cmp_ds, 0);
    for (int i = 0; i < np; i++) { c->RSSetScissorRects(1, &parts[i]); o_draw(c, 3, 0); }
    // restore
    ID3D11ShaderResourceView* none = nullptr; c->PSSetShaderResources(0, 1, &none);
    o_setrt(c, 1, &o_rtv, o_dsv);
    c->RSSetViewports(1, &o_vp); if (nsc) c->RSSetScissorRects(1, &o_sc);
    c->VSSetShader(o_vs, nullptr, 0); c->PSSetShader(o_ps2, nullptr, 0); c->GSSetShader(o_gs, nullptr, 0);
    c->PSSetShaderResources(0, 1, &o_srv); c->PSSetSamplers(0, 1, &o_smp);
    c->IASetInputLayout(o_il); c->IASetPrimitiveTopology(o_top);
    o_blend(c, o_bs, o_bf, o_mask); c->RSSetState(o_rs); c->OMSetDepthStencilState(o_ds, o_ref);
    for (IUnknown* u : { (IUnknown*)o_rtv, (IUnknown*)o_dsv, (IUnknown*)o_vs, (IUnknown*)o_ps2, (IUnknown*)o_gs, (IUnknown*)o_srv,
                         (IUnknown*)o_smp, (IUnknown*)o_il, (IUnknown*)o_bs, (IUnknown*)o_rs, (IUnknown*)o_ds })
        if (u) u->Release();
}

// Same blend as the game's, but also builds up coverage in alpha so the panel is see-through.
static ID3D11BlendState* hud_blend(ID3D11DeviceContext* c, ID3D11BlendState* game) {
    if (!game) return nullptr;
    for (int i = 0; i < g_blend_n; i++) if (g_blend_map[i].game == game) return g_blend_map[i].ours;
    D3D11_BLEND_DESC d; game->GetDesc(&d);
    for (auto& rt : d.RenderTarget) {
        if (rt.BlendEnable) { rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD; }
        rt.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    }
    ID3D11Device* dev = nullptr; c->GetDevice(&dev);
    ID3D11BlendState* ours = nullptr;
    if (dev) { dev->CreateBlendState(&d, &ours); dev->Release(); }
    if (!ours) return game;
    if (g_blend_n < 64) { g_blend_map[g_blend_n].game = game; g_blend_map[g_blend_n].ours = ours; g_blend_n++; }
    return ours;
}

static void start_redirect(ID3D11DeviceContext* c) {
    if (!g_redirected_frame) {
        const float zero[4] = { 0, 0, 0, 0 };
        c->ClearRenderTargetView(g_hud_rtv, zero);
        g_redirected_frame = true;
    }
    o_setrt(c, 1, &g_hud_rtv, g_target_dsv);
    g_hud_view_fmt = g_hud_rtv_fmt;
    ID3D11BlendState* b = nullptr; FLOAT f[4]; UINT m = 0;
    c->OMGetBlendState(&b, f, &m);
    if (b) { o_blend(c, hud_blend(c, b), f, m); b->Release(); }
    g_redirecting = true;
}

// Called before every draw.
static void before_draw(ID3D11DeviceContext* c, bool plain_draw, UINT n) {
    if (!g_hud_enabled || !g_in_target || g_in_present) return;
    if (g_target_draws == 0) g_first_ok = (plain_draw && (n == 3 || n == 4));
    else if (g_target_draws == 1 && g_first_ok && !g_redirecting) start_redirect(c);
    g_target_draws++;
}

static void* rtv_resource(ID3D11View* v) {
    if (!v) return nullptr;
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    if (r) r->Release();
    return r;
}

// Scene depth: the depth buffer with the most draws in a frame, at full resolution, stable for 30 frames.
static void* g_cur_depth;
static struct { void* res; int draws; } g_dc[8]; static int g_dcn;
static ID3D11Resource* g_depth; static void* g_depth_cand; static int g_depth_frames;
static void count_draw() {
    if (g_in_present || !g_cur_depth) return;
    for (int i = 0; i < g_dcn; i++) if (g_dc[i].res == g_cur_depth) { g_dc[i].draws++; return; }
    if (g_dcn < 8) { g_dc[g_dcn].res = g_cur_depth; g_dc[g_dcn].draws = 1; g_dcn++; }
}
ID3D11Resource* scene_depth() { return g_depth; }

static void STDMETHODCALLTYPE h_setrt(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* v, ID3D11DepthStencilView* d) {
    if (g_in_present) { o_setrt(c, n, v, d); return; }     // our own drawing
    g_cur_depth = d ? rtv_resource(d) : nullptr;
    void* res = (n && v) ? rtv_resource(v[0]) : nullptr;
    if (res && res == g_bb && g_redirected_frame && !g_composited) {   // HUD done, game is about to show the frame
        g_composited = true;
        composite_center(c);
    }
    if (res != g_cur_rt) {
        if (g_cur_has_draw && res == g_bb && g_cur_rt) g_last_rt_with_draws = g_cur_rt;   // the texture copied to the screen
        g_cur_rt = res; g_cur_has_draw = false;
    }
    g_in_target = g_hud_enabled && res && res == g_target && n >= 1;
    g_redirecting = false;
    if (g_in_target) {
        g_target_dsv = d;
        if (!ensure_hud_target(c, v[0])) g_in_target = false;
        else if (g_redirected_frame && g_first_ok) {     // the game re-bound it mid-HUD: stay on ours
            o_setrt(c, n, v, d);
            start_redirect(c);
            return;
        }
    }
    o_setrt(c, n, v, d);
}
static void STDMETHODCALLTYPE h_draw(ID3D11DeviceContext* c, UINT n, UINT s) { if (!g_in_present) g_cur_has_draw = true; count_draw(); before_draw(c, true, n); o_draw(c, n, s); }
static void STDMETHODCALLTYPE h_drawidx(ID3D11DeviceContext* c, UINT n, UINT s, INT b) { if (!g_in_present) g_cur_has_draw = true; count_draw(); before_draw(c, false, n); o_drawidx(c, n, s, b); }
static void STDMETHODCALLTYPE h_drawinst(ID3D11DeviceContext* c, UINT n, UINT i, UINT s, UINT si) { if (!g_in_present) g_cur_has_draw = true; count_draw(); before_draw(c, false, n); o_drawinst(c, n, i, s, si); }
static void STDMETHODCALLTYPE h_drawidxinst(ID3D11DeviceContext* c, UINT n, UINT i, UINT s, INT b, UINT si) { if (!g_in_present) g_cur_has_draw = true; count_draw(); before_draw(c, false, n); o_drawidxinst(c, n, i, s, b, si); }
static void STDMETHODCALLTYPE h_blend(ID3D11DeviceContext* c, ID3D11BlendState* b, const FLOAT f[4], UINT m) {
    if (g_redirecting && b && !g_in_present) b = hud_blend(c, b);
    o_blend(c, b, f, m);
}


// ---------------------------------------------------------------- frame ID
// Which camera request the frame reaching Present was drawn with. Each request's field of view carries a tiny tag
// (a few parts in 100000 - invisible); the game puts its projection numbers into constant buffers while drawing, so
// finding a tagged number there says exactly which request this frame is. Discovery: the (buffer size, offset, form)
// that holds a tagged number in nearly every frame; once found ("locked") only that spot is read.
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using UpdFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
static MapFn o_map; static UnmapFn o_unmap; static UpdFn o_upd;

static const float kTagTol = 1.2e-5f;                 // relative; neighbouring tags differ by >= 5e-5
static const char* kFormName[6] = { "1/tan", "1/tan/aspect", "1/tan*aspect", "tan", "tan*aspect", "tan/aspect" };
struct Cand { float v; unsigned idx; int form; };
static Cand g_cands[2][64]; static int g_ncand[2]; static volatile LONG g_cand_cur;
static struct { unsigned idx; float fov; bool ok; } g_tag_ring[8];
static volatile ULONGLONG g_tag_t;
struct Key { UINT size, off; int form, hits; unsigned last; };
static Key g_keys[128]; static int g_nkeys;
static Key g_lock[4]; static volatile int g_nlock;
static unsigned g_fid_frame; static int g_disc_frames, g_disc_rounds, g_miss_run;
static volatile LONG g_dec_has; static volatile unsigned g_dec_idx;
static bool g_shown_has; static unsigned g_shown_idx;
static SRWLOCK g_fid_lock = SRWLOCK_INIT;
static struct { void* res; void* p; UINT size; } g_maps[32]; static int g_nmaps;
static char g_fid_status[200] = "off";
static volatile ULONGLONG g_fid_rest_until;           // not found after several tries: stop searching for a while (it costs time)
// Probe: the spot that held the tag at the last lock (the main camera buffer) is read every frame even while the tag
// is missing, so the log can say what field of view the game actually drew with (zoom factor / blend / its own).
static Key g_probe; static volatile float g_probe_v; static volatile ULONGLONG g_probe_t; static float g_aspect = 1.7778f;

void frame_tag(unsigned idx, float fov, float aspect) {
    g_tag_ring[idx & 7] = { idx, fov, true };
    if (aspect > 0.1f) g_aspect = aspect;
    int w = (int)(g_cand_cur ^ 1), n = 0;
    Cand* c = g_cands[w];
    for (auto& r : g_tag_ring) {
        if (!r.ok || r.idx > idx || idx - r.idx >= 8) continue;
        float t = tanf(r.fov * 0.5f);
        if (!(t > 1e-4f && t < 1e4f)) continue;
        float ct = 1.0f / t, v[6] = { ct, ct / aspect, ct * aspect, t, t * aspect, t / aspect };
        for (int f = 0; f < 6 && n < 64; f++) c[n++] = { v[f], r.idx, f };
    }
    for (int i = 1; i < n; i++) { Cand x = c[i]; int j = i - 1; while (j >= 0 && c[j].v > x.v) { c[j + 1] = c[j]; j--; } c[j + 1] = x; }
    g_ncand[w] = n;
    InterlockedExchange(&g_cand_cur, w);
    g_tag_t = GetTickCount64();
}

// The one candidate within tolerance of v (none, or two or more = no match).
// Rotation check (0.16.4): the decoded frame's constant buffer also holds the view rotation. Which request's rotation
// is it? (0 = the same request as the FOV tag / position = right; +1 = one request newer = the rotation arrives sooner.)
static struct { unsigned idx; float m[9]; bool ok; } g_rot_ring[8];
static volatile LONG g_rot_hist[7], g_rot_none;
void rot_tag(unsigned idx, const float m[9]) { auto& r = g_rot_ring[idx & 7]; r.ok = false; memcpy(r.m, m, sizeof r.m); r.idx = idx; r.ok = true; }
static void rot_check(const float* f, UINT cnt, unsigned dec) {
    int best = -1, bc = 0, sc = 0;
    for (int k = 0; k < 8; k++) {
        const auto& r = g_rot_ring[k];
        if (!r.ok) continue;
        int d = (int)(r.idx - dec);
        if (d < -3 || d > 3) continue;
        int c = 0;
        for (int j = 0; j < 9; j++) {
            float a = fabsf(r.m[j]);
            if (a < 0.02f) continue;
            for (UINT i = 0; i < cnt; i++) if (fabsf(fabsf(f[i]) - a) < 2e-6f) { c++; break; }
        }
        if (c > bc) { sc = bc; bc = c; best = d; } else if (c > sc) sc = c;
    }
    if (best >= -3 && bc >= 5 && bc > sc) InterlockedIncrement(&g_rot_hist[best + 3]); else InterlockedIncrement(&g_rot_none);
}
void rot_report(char* out, size_t n) {
    LONG h[7], none = InterlockedExchange(&g_rot_none, 0), tot = none;
    for (int i = 0; i < 7; i++) { h[i] = InterlockedExchange(&g_rot_hist[i], 0); tot += h[i]; }
    if (!tot) { snprintf(out, n, "rotation not checked"); return; }
    snprintf(out, n, "rotation from request -1 / same / +1 / +2: %.0f%% / %.0f%% / %.0f%% / %.0f%%, unclear %.0f%%",
             100.0 * h[2] / tot, 100.0 * h[3] / tot, 100.0 * h[4] / tot, 100.0 * h[5] / tot, 100.0 * none / tot);
}

static const Cand* tag_lookup(float v, const Cand* c, int n) {
    int lo = 0, hi = n;
    while (lo < hi) { int m = (lo + hi) / 2; if (c[m].v < v) lo = m + 1; else hi = m; }
    const Cand* best = nullptr; int found = 0;
    for (int i = lo - 1; i <= lo; i++) {
        if (i < 0 || i >= n) continue;
        if (fabsf(c[i].v - v) <= kTagTol * c[i].v) { best = &c[i]; found++; }
    }
    return found == 1 ? best : nullptr;
}

static void tag_scan(const void* data, UINT size) {
    if (!data || size < 16 || size > 65536 || GetTickCount64() - g_tag_t > 500) return;
    if (g_probe.size == size && g_probe.off + 4 <= size) { g_probe_v = ((const float*)data)[g_probe.off / 4]; g_probe_t = GetTickCount64(); }
    if (!g_nlock && GetTickCount64() < g_fid_rest_until) return;
    int cur = (int)g_cand_cur; const Cand* c = g_cands[cur]; int n = g_ncand[cur];
    if (n <= 0) return;
    const float* f = (const float*)data;
    int nl = g_nlock;
    if (nl > 0) {
        for (int k = 0; k < nl; k++) {
            const Key& key = g_lock[k];
            if (key.size != size || key.off + 4 > size) continue;
            const Cand* m = tag_lookup(f[key.off / 4], c, n);
            if (m && m->form == key.form) { g_dec_idx = m->idx; InterlockedExchange(&g_dec_has, 1); if (size >= 256) rot_check(f, size / 4, m->idx); return; }
        }
        return;
    }
    float lo = c[0].v * (1 - kTagTol), hi = c[n - 1].v * (1 + kTagTol);
    UINT cnt = size / 4;
    for (UINT i = 0; i < cnt; i++) {
        float v = f[i];
        if (!(v >= lo && v <= hi)) continue;
        const Cand* m = tag_lookup(v, c, n);
        if (!m) continue;
        AcquireSRWLockExclusive(&g_fid_lock);
        int e = -1;
        for (int k = 0; k < g_nkeys; k++) if (g_keys[k].size == size && g_keys[k].off == i * 4 && g_keys[k].form == m->form) { e = k; break; }
        if (e < 0) {
            if (g_nkeys < 128) e = g_nkeys++;
            else {   // full: replace the weakest key that hasn't hit lately
                for (int k = 0; k < 128; k++) if (g_fid_frame - g_keys[k].last > 2 && (e < 0 || g_keys[k].hits < g_keys[e].hits)) e = k;
            }
            if (e >= 0) g_keys[e] = { size, i * 4, m->form, 0, g_fid_frame - 1 };
        }
        if (e >= 0 && g_keys[e].last != g_fid_frame) { g_keys[e].hits++; g_keys[e].last = g_fid_frame; }
        ReleaseSRWLockExclusive(&g_fid_lock);
    }
}

static bool is_cbuffer(ID3D11Resource* r, UINT* size) {
    if (!r) return false;
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    D3D11_BUFFER_DESC bd; ((ID3D11Buffer*)r)->GetDesc(&bd);
    if (!(bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return false;
    *size = bd.ByteWidth;
    return true;
}

static HRESULT STDMETHODCALLTYPE h_map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT fl, D3D11_MAPPED_SUBRESOURCE* m) {
    HRESULT hr = o_map(c, r, sub, type, fl, m);
    UINT size;
    if (SUCCEEDED(hr) && m && m->pData && !g_in_present && type != D3D11_MAP_READ && GetTickCount64() - g_tag_t < 500 &&
        (g_nlock || GetTickCount64() >= g_fid_rest_until) && is_cbuffer(r, &size)) {
        AcquireSRWLockExclusive(&g_fid_lock);
        int i = 0;
        for (; i < g_nmaps; i++) if (g_maps[i].res == r) break;
        if (i == g_nmaps && g_nmaps < 32) g_nmaps++;
        if (i < 32) g_maps[i] = { r, m->pData, size };
        ReleaseSRWLockExclusive(&g_fid_lock);
    }
    return hr;
}
static void STDMETHODCALLTYPE h_unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    if (g_nmaps) {
        void* p = nullptr; UINT size = 0;
        AcquireSRWLockExclusive(&g_fid_lock);
        for (int i = 0; i < g_nmaps; i++) if (g_maps[i].res == r) { p = g_maps[i].p; size = g_maps[i].size; g_maps[i] = g_maps[--g_nmaps]; break; }
        ReleaseSRWLockExclusive(&g_fid_lock);
        if (p) tag_scan(p, size);         // the game has written it: read before handing it back
    }
    o_unmap(c, r, sub);
}
static void STDMETHODCALLTYPE h_upd(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src, UINT rp, UINT dp) {
    UINT size;
    if (!g_in_present && src && !box && GetTickCount64() - g_tag_t < 500 && is_cbuffer(r, &size)) tag_scan(src, size);
    o_upd(c, r, sub, box, src, rp, dp);
}

// Once per Present (start): what the frame now being shown was, and discovery / lock bookkeeping.
static void fid_frame() {
    g_shown_has = InterlockedExchange(&g_dec_has, 0) != 0;
    g_shown_idx = g_dec_idx;
    g_fid_frame++;
    if (GetTickCount64() - g_tag_t > 500) { if (g_nlock) snprintf(g_fid_status, sizeof g_fid_status, "locked (paused: no headset frames)"); return; }
    if (g_nlock) {
        if (g_shown_has) g_miss_run = 0;
        else if (++g_miss_run > 120) {
            g_nlock = 0; g_miss_run = 0; g_disc_frames = 0;
            AcquireSRWLockExclusive(&g_fid_lock); g_nkeys = 0; ReleaseSRWLockExclusive(&g_fid_lock);
            log("Frame ID: lost (no tagged frame for 120 frames) - searching again");
        }
        return;
    }
    if (GetTickCount64() < g_fid_rest_until) return;
    if (++g_disc_frames < 90) { snprintf(g_fid_status, sizeof g_fid_status, "searching (%d/90)", g_disc_frames); return; }
    AcquireSRWLockExclusive(&g_fid_lock);
    Key found[4]; int nf = 0; int best = -1;
    for (int k = 0; k < g_nkeys; k++) {
        if (best < 0 || g_keys[k].hits > g_keys[best].hits) best = k;
        if (g_keys[k].hits >= 80 && nf < 4) found[nf++] = g_keys[k];
    }
    Key b = best >= 0 ? g_keys[best] : Key{};
    g_nkeys = 0;
    ReleaseSRWLockExclusive(&g_fid_lock);
    g_disc_frames = 0; g_disc_rounds++;
    if (nf) {
        // main camera buffer first (the largest one; 0.16.2 relocked onto a small 192-byte buffer first)
        std::sort(found, found + nf, [](const Key& a, const Key& b) { return a.hits != b.hits ? a.hits > b.hits : a.size > b.size; });
        if (!g_probe.size || g_probe.size < found[0].size) g_probe = found[0];
        for (int k = 0; k < nf; k++) {
            g_lock[k] = found[k];
            log("Frame ID: locked - %s in a %u-byte constant buffer at +%u (%d of 90 frames)", kFormName[found[k].form], found[k].size, found[k].off, found[k].hits);
        }
        g_nlock = nf; g_miss_run = 0;
        snprintf(g_fid_status, sizeof g_fid_status, "locked (%s, %u B +%u)", kFormName[found[0].form], found[0].size, found[0].off);
    } else {
        if (g_disc_rounds <= 5 || g_disc_rounds % 20 == 0)
            log("Frame ID: not found yet (best: %s, %u B +%u, %d of 90 frames) - frame order is guessed meanwhile",
                best >= 0 ? kFormName[b.form] : "-", b.size, b.off, b.hits);
        snprintf(g_fid_status, sizeof g_fid_status, "not found yet (best %d/90) - guessing frame order", b.hits);
        if (g_disc_rounds % 6 == 0) {      // ~9 s of searching without a result: rest a minute, then try again
            g_fid_rest_until = GetTickCount64() + 60000;
            snprintf(g_fid_status, sizeof g_fid_status, "not found (best %d/90) - guessing frame order, retrying in a minute", b.hits);
        }
    }
}

int frame_shown(unsigned* idx) {
    if (!g_nlock) return -1;
    if (!g_shown_has) return 0;
    *idx = g_shown_idx;
    return 1;
}
bool frame_id_locked() { return g_nlock > 0; }
bool frame_id_missing() { return g_nlock == 0 || g_miss_run > 10; }
bool probe_fov(float* deg, char* where, size_t n) {
    if (!g_probe.size || GetTickCount64() - g_probe_t > 300) return false;
    float v = g_probe_v, a = g_aspect, t;
    switch (g_probe.form) {
        case 0: t = 1.0f / v; break;       case 1: t = 1.0f / (v * a); break; case 2: t = a / v; break;
        case 3: t = v; break;              case 4: t = v / a; break;          default: t = v * a; break;
    }
    if (!(t > 1e-4f && t < 1e4f)) return false;
    *deg = 2.0f * atanf(t) * 57.2957795f;
    if (where) snprintf(where, n, "%s, %u B +%u", kFormName[g_probe.form], g_probe.size, g_probe.off);
    return true;
}
const char* frame_id_status() { return g_fid_status; }

static bool hook(void** vt, int idx, void* fn, void** orig) {
    return MH_CreateHook(vt[idx], fn, orig) == MH_OK && MH_EnableHook(vt[idx]) == MH_OK;
}

static bool install(ID3D11DeviceContext* ctx) {
    if (g_hooked) return true;
    if (!ctx) return false;
    void** vt = *(void***)ctx;
    bool ok = hook(vt, 33, (void*)h_setrt, (void**)&o_setrt) && hook(vt, 13, (void*)h_draw, (void**)&o_draw) &&
              hook(vt, 12, (void*)h_drawidx, (void**)&o_drawidx) && hook(vt, 21, (void*)h_drawinst, (void**)&o_drawinst) &&
              hook(vt, 20, (void*)h_drawidxinst, (void**)&o_drawidxinst) && hook(vt, 35, (void*)h_blend, (void**)&o_blend) &&
              hook(vt, 14, (void*)h_map, (void**)&o_map) && hook(vt, 15, (void*)h_unmap, (void**)&o_unmap) &&
              hook(vt, 48, (void*)h_upd, (void**)&o_upd);
    log("HUD: draw hooks %s", ok ? "installed" : "FAILED");
    g_hooked = ok;
    return ok;
}

void hooks_install(ID3D11DeviceContext* ctx) { if (!g_hooked) install(ctx); }

void hud_enable(ID3D11DeviceContext* ctx, bool on) {
    if (on == g_hud_enabled) return;
    if (on && !install(ctx)) return;
    g_hud_enabled = on;
    g_in_target = g_redirecting = false;
    log("HUD split %s", on ? "ON" : "off");
}
bool hud_active() { return g_hud_enabled && g_target && g_hud_srv && g_hud_frames > 0; }
ID3D11Texture2D* hud_texture() { return g_hud_tex; }
ID3D11ShaderResourceView* hud_srv() { return g_hud_srv; }
const char* hud_status() { return g_hud_status; }
void hud_exclude_area(bool on, float x0, float y0, float x1, float y1) {
    g_excl_on = on; g_excl[0] = x0; g_excl[1] = y0; g_excl[2] = x1; g_excl[3] = y1;
}
void hud_exclude_area2(bool on, float x0, float y0, float x1, float y1) {
    g_excl2_on = on; g_excl2[0] = x0; g_excl2[1] = y0; g_excl2[2] = x1; g_excl2[3] = y1;
}
void hud_world_area(bool on, float x0, float y0, float x1, float y1) {
    g_world_center = on;
    g_world_rect[0] = x0; g_world_rect[1] = y0; g_world_rect[2] = x1; g_world_rect[3] = y1;
}

void on_present_end() { g_in_present = false; }

static void depth_frame(ID3D11Resource* bb) {
    D3D11_TEXTURE2D_DESC bd = {};
    { ID3D11Texture2D* b = nullptr; bb->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&b); if (b) { b->GetDesc(&bd); b->Release(); } }
    int best = -1;
    for (int i = 0; i < g_dcn; i++) {
        if (g_dc[i].draws <= 50 || (best >= 0 && g_dc[i].draws <= g_dc[best].draws)) continue;
        ID3D11Texture2D* t = nullptr;
        ((ID3D11Resource*)g_dc[i].res)->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t);
        if (!t) continue;
        D3D11_TEXTURE2D_DESC td; t->GetDesc(&td); t->Release();
        if (td.Width == bd.Width && td.Height == bd.Height && td.SampleDesc.Count == 1) best = i;   // full screen only (not shadow maps)
    }
    void* cand = best >= 0 ? g_dc[best].res : nullptr;
    g_dcn = 0;
    if (cand && cand == g_depth_cand) { if (g_depth_frames < 1000) g_depth_frames++; } else { g_depth_cand = cand; g_depth_frames = 0; }
    if (g_depth_frames == 30 && cand != (void*)g_depth) {
        if (g_depth) g_depth->Release();
        g_depth = (ID3D11Resource*)cand; g_depth->AddRef();
        ID3D11Texture2D* t = nullptr; g_depth->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t);
        D3D11_TEXTURE2D_DESC td = {}; if (t) { t->GetDesc(&td); t->Release(); }
        log("Depth: scene depth buffer %p %ux%u format %u", cand, td.Width, td.Height, td.Format);
    }
}

static void hud_frame(ID3D11Resource* bb) {
    depth_frame(bb);
    // Learn which texture is copied to the screen (stable for 30 frames = the target).
    void* cand = g_last_rt_with_draws;
    if (cand && cand == g_cand) { if (g_cand_frames < 1000) g_cand_frames++; } else { g_cand = cand; g_cand_frames = 0; }
    if (g_cand_frames >= 30 && g_cand != g_target) {
        g_target = g_cand;
        log("HUD: the game draws its HUD into %p (copied to the screen at the end of each frame)", g_target);
    }
    if (g_hud_enabled) {
        if (g_redirected_frame) g_hud_frames++;
        snprintf(g_hud_status, sizeof g_hud_status, "%s", !g_target ? "learning..." : g_redirected_frame ? "HUD split working" :
                 g_first_ok ? "no HUD this frame" : "target found but the first draw isn't full-screen - not splitting");
        static bool told = false;
        if (g_target && !g_first_ok && g_target_draws > 0 && !told) { told = true; log("HUD: first draw into the target is not a full-screen draw - split disabled for safety"); }
        if (!g_redirected_frame && g_hud_rtv && g_hud_frames) {    // no HUD drawn this frame: keep the panel empty
            ID3D11Device* dev = nullptr; g_hud_rtv->GetDevice(&dev);
            ID3D11DeviceContext* c = nullptr; if (dev) { dev->GetImmediateContext(&c); dev->Release(); }
            if (c) { const float z[4] = { 0, 0, 0, 0 }; c->ClearRenderTargetView(g_hud_rtv, z); c->Release(); }
        }
    }
    g_redirected_frame = false; g_redirecting = false; g_in_target = false; g_target_draws = 0; g_composited = false;
    g_bb = bb; g_cur_rt = nullptr; g_cur_has_draw = false; g_last_rt_with_draws = nullptr;
}

void on_present_begin(ID3D11Resource* bb) {
    g_in_present = true;
    if (g_hooked) { fid_frame(); hud_frame(bb); }
}

} // namespace wdvr::framecap
