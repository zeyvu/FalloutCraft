// FalloutCraft Phase 2a: Minecraft's own client on screen. Minecraft renders its first-person
// hand, held item, hotbar, hearts, crosshair and every open screen (inventory, chat, crafting)
// into a shared-memory frame; this draws that frame over Fallout's, in Fallout's Present.
// Ported from the Skyrim plugin (skse/src/Overlay.cpp).

#include "fo_common.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#undef ERROR  // wingdi.h; clashes with REX::ERROR

#include "fo_blocks.h"

#include <cstring>

namespace skycraft
{
	namespace
	{
		using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
		PresentFn originalPresent = nullptr;

		ID3D11Device*             device = nullptr;
		ID3D11DeviceContext*      context = nullptr;
		ID3D11Texture2D*          texture = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		UINT                      texW = 0, texH = 0;
		ID3D11VertexShader*       vs = nullptr;
		ID3D11PixelShader*        ps = nullptr;
		ID3D11PixelShader*        psInvert = nullptr;
		ID3D11BlendState*         blend = nullptr;
		ID3D11BlendState*         invertBlend = nullptr;
		ID3D11SamplerState*       sampler = nullptr;
		ID3D11RasterizerState*    raster = nullptr;
		ID3D11DepthStencilState*  depth = nullptr;
		ID3D11Buffer*             params = nullptr;
		bool                      haveFrame = false;
		bool                      flipY = true;
		bool                      initFailed = false;

		struct alignas(16) Params
		{
			float cursor[2];
			float viewport[2];
			float cursorOn;
			float flipY;
			float pad[2];
			float invertRect[4];  // back buffer pixels: x0, y0, x1, y1 (empty: none)
		};

		constexpr char kShader[] = R"(
cbuffer Params : register(b0) { float2 cursor; float2 viewport; float cursorOn; float flipY; float2 pad; float4 invertRect; };
Texture2D overlay : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = uv;
	return o;
}
bool InInvertRect(float2 p) { return all(p >= invertRect.xy) && all(p < invertRect.zw); }
float4 Overlay(float2 uv) {
	if (flipY > 0.5) uv.y = 1 - uv.y;
	return overlay.Sample(samp, uv);   // premultiplied alpha straight from Minecraft
}
// Minecraft's crosshair and attack indicator: drawn with Minecraft's invert blend (out = src * (1 - dst)
// + dst * (1 - src)) against Skyrim's picture, in a pass of their own; left out of the main one.
float4 PSInvert(VSOut i) : SV_Target {
	if (!InInvertRect(i.pos.xy)) discard;
	return float4(Overlay(i.uv).rgb, 0);
}
float4 PSMain(VSOut i) : SV_Target {
	float2 uv = i.uv;
	if (InInvertRect(i.pos.xy)) return 0;
	float4 c = Overlay(uv);
	if (cursorOn > 0.5) {
		float2 p = i.pos.xy - cursor;
		if (p.x >= 0 && p.y >= 0 && p.y < 18 && p.x <= p.y * 0.6) {
			bool edge = p.x < 1.5 || p.x > p.y * 0.6 - 1.5 || p.y > 16.5;
			c = float4(edge ? float3(0, 0, 0) : float3(1, 1, 1), 1);
		}
	}
	return c;
}
)";

		template <class T>
		void SafeRelease(T*& a_ptr)
		{
			if (a_ptr) {
				a_ptr->Release();
				a_ptr = nullptr;
			}
		}

		bool Compile(const char* a_entry, const char* a_target, ID3DBlob** a_out)
		{
			ID3DBlob* errors = nullptr;
			const auto hr = D3DCompile(kShader, sizeof(kShader) - 1, "skycraft_overlay", nullptr, nullptr, a_entry, a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, a_out, &errors);
			if (FAILED(hr)) {
				REX::ERROR("overlay shader {} failed: {}", a_entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
				SafeRelease(errors);
				return false;
			}
			SafeRelease(errors);
			return true;
		}

		bool InitResources(IDXGISwapChain* a_swapChain)
		{
			if (device) {
				return true;
			}
			if (initFailed || FAILED(a_swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) {
				initFailed = true;
				return false;
			}
			device->GetImmediateContext(&context);

			ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *psInvertBlob = nullptr;
			if (!Compile("VSMain", "vs_5_0", &vsBlob) || !Compile("PSMain", "ps_5_0", &psBlob) || !Compile("PSInvert", "ps_5_0", &psInvertBlob)) {
				initFailed = true;
				return false;
			}
			device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
			device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
			device->CreatePixelShader(psInvertBlob->GetBufferPointer(), psInvertBlob->GetBufferSize(), nullptr, &psInvert);
			SafeRelease(vsBlob);
			SafeRelease(psBlob);
			SafeRelease(psInvertBlob);

			D3D11_BLEND_DESC bd{};
			bd.RenderTarget[0].BlendEnable = TRUE;
			bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
			bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			device->CreateBlendState(&bd, &blend);
			// Minecraft's BlendFunction.INVERT; the back buffer's alpha is left alone.
			bd.RenderTarget[0].SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
			bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_COLOR;
			bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
			bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
			device->CreateBlendState(&bd, &invertBlend);

			D3D11_SAMPLER_DESC sd{};
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.MaxLOD = D3D11_FLOAT32_MAX;
			device->CreateSamplerState(&sd, &sampler);

			D3D11_RASTERIZER_DESC rd{};
			rd.FillMode = D3D11_FILL_SOLID;
			rd.CullMode = D3D11_CULL_NONE;
			rd.DepthClipEnable = TRUE;
			device->CreateRasterizerState(&rd, &raster);

			D3D11_DEPTH_STENCIL_DESC dd{};
			dd.DepthEnable = FALSE;
			dd.StencilEnable = FALSE;
			device->CreateDepthStencilState(&dd, &depth);

			D3D11_BUFFER_DESC cbd{};
			cbd.ByteWidth = sizeof(Params);
			cbd.Usage = D3D11_USAGE_DYNAMIC;
			cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			device->CreateBuffer(&cbd, nullptr, &params);

			const bool ok = vs && ps && psInvert && blend && invertBlend && sampler && raster && depth && params;
			REX::INFO("overlay renderer {}", ok ? "ready" : "failed to initialize");
			initFailed = !ok;
			return ok;
		}

		bool EnsureTexture(UINT a_w, UINT a_h)
		{
			if (texture && texW == a_w && texH == a_h) {
				return true;
			}
			SafeRelease(srv);
			SafeRelease(texture);
			D3D11_TEXTURE2D_DESC td{};
			td.Width = a_w;
			td.Height = a_h;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DYNAMIC;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(device->CreateTexture2D(&td, nullptr, &texture)) || FAILED(device->CreateShaderResourceView(texture, nullptr, &srv))) {
				REX::ERROR("overlay texture {}x{} creation failed", a_w, a_h);
				return false;
			}
			texW = a_w;
			texH = a_h;
			REX::INFO("overlay texture {}x{}", a_w, a_h);
			return true;
		}

		void UploadLatestFrame()
		{
			if (!link::AcquireOverlayFrame()) {
				return;
			}
			const auto* hdr = link::FrontHeader();
			if (hdr->width == 0 || hdr->height == 0 || hdr->width > proto::kMaxOverlayW || hdr->height > proto::kMaxOverlayH) {
				return;
			}
			if (!EnsureTexture(hdr->width, hdr->height)) {
				return;
			}
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(context->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				return;
			}
			const auto* src = link::FrontPixels();
			const auto  rowBytes = hdr->width * 4;
			auto*       dst = static_cast<std::uint8_t*>(mapped.pData);
			if (mapped.RowPitch == rowBytes) {
				std::memcpy(dst, src, std::size_t(rowBytes) * hdr->height);
			} else {
				for (UINT y = 0; y < hdr->height; ++y) {
					std::memcpy(dst + std::size_t(y) * mapped.RowPitch, src + std::size_t(y) * rowBytes, rowBytes);
				}
			}
			context->Unmap(texture, 0);
			flipY = (hdr->flags & 1) != 0;
			haveFrame = true;
		}

		void DrawOverlay(IDXGISwapChain* a_swapChain)
		{
			auto& st = State();
			DXGI_SWAP_CHAIN_DESC desc{};
			if (SUCCEEDED(a_swapChain->GetDesc(&desc))) {
				st.viewportW = static_cast<int>(desc.BufferDesc.Width);
				st.viewportH = static_cast<int>(desc.BufferDesc.Height);
			}
			if (!link::IsOpen() || !link::MinecraftAlive() || !st.mcInWorld || !InitResources(a_swapChain)) {
				return;
			}
			UploadLatestFrame();
			if (!haveFrame || st.falloutMenuOpen || !st.minecraftOwnsPlayer) {  // also while Minecraft settles after a teleport
				return;
			}

			ID3D11Texture2D* backBuffer = nullptr;
			if (FAILED(a_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
				return;
			}
			ID3D11RenderTargetView* rtv = nullptr;
			const auto              hr = device->CreateRenderTargetView(backBuffer, nullptr, &rtv);
			D3D11_TEXTURE2D_DESC    bbDesc{};
			backBuffer->GetDesc(&bbDesc);
			SafeRelease(backBuffer);
			if (FAILED(hr)) {
				static bool logged = false;
				if (!logged) {
					logged = true;
					REX::ERROR("overlay: back buffer RTV failed (format {})", static_cast<int>(bbDesc.Format));
				}
				return;
			}

			// Save the pipeline state we touch.
			ID3D11RenderTargetView*   oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView*   oldDsv = nullptr;
			ID3D11BlendState*         oldBlend = nullptr;
			float                     oldFactor[4]{};
			UINT                      oldMask = 0;
			ID3D11RasterizerState*    oldRaster = nullptr;
			ID3D11DepthStencilState*  oldDepth = nullptr;
			UINT                      oldStencil = 0;
			D3D11_VIEWPORT            oldVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT                      oldVpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			D3D11_PRIMITIVE_TOPOLOGY  oldTopo{};
			ID3D11InputLayout*        oldLayout = nullptr;
			ID3D11VertexShader*       oldVs = nullptr;
			ID3D11PixelShader*        oldPs = nullptr;
			ID3D11ShaderResourceView* oldSrv = nullptr;
			ID3D11SamplerState*       oldSampler = nullptr;
			ID3D11Buffer*             oldCb = nullptr;
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
			context->OMGetBlendState(&oldBlend, oldFactor, &oldMask);
			context->RSGetState(&oldRaster);
			context->OMGetDepthStencilState(&oldDepth, &oldStencil);
			context->RSGetViewports(&oldVpCount, oldVps);
			context->IAGetPrimitiveTopology(&oldTopo);
			context->IAGetInputLayout(&oldLayout);
			context->VSGetShader(&oldVs, nullptr, nullptr);
			context->PSGetShader(&oldPs, nullptr, nullptr);
			context->PSGetShaderResources(0, 1, &oldSrv);
			context->PSGetSamplers(0, 1, &oldSampler);
			context->PSGetConstantBuffers(0, 1, &oldCb);

			bool                     invert = false;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(params, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				auto* p = static_cast<Params*>(mapped.pData);
				// The cursor lives in overlay pixels; scale if the overlay and back buffer differ.
				const float sx = texW ? float(bbDesc.Width) / float(texW) : 1.0f;
				const float sy = texH ? float(bbDesc.Height) / float(texH) : 1.0f;
				p->cursor[0] = st.cursorX * sx;
				p->cursor[1] = st.cursorY * sy;
				p->viewport[0] = float(bbDesc.Width);
				p->viewport[1] = float(bbDesc.Height);
				p->cursorOn = st.mcScreenOpen ? 1.0f : 0.0f;
				p->flipY = flipY ? 1.0f : 0.0f;
				// Around the screen centre, where Minecraft puts the crosshair (15 GUI pixels) and the
				// attack indicator under it (16 x 16 from 9 GUI pixels below the centre).
				const int   scale = st.mcGuiScale;
				const float g = float(scale) * sx;
				const float cx = float(bbDesc.Width) * 0.5f, cy = float(bbDesc.Height) * 0.5f;
				invert = st.mcCrosshair && scale > 0;
				p->invertRect[0] = invert ? cx - 12.0f * g : 0.0f;
				p->invertRect[1] = invert ? cy - 12.0f * g : 0.0f;
				p->invertRect[2] = invert ? cx + 12.0f * g : 0.0f;
				p->invertRect[3] = invert ? cy + 28.0f * g : 0.0f;
				context->Unmap(params, 0);
			}

			D3D11_VIEWPORT vp{ 0, 0, float(bbDesc.Width), float(bbDesc.Height), 0, 1 };
			const float    factor[4]{};
			context->OMSetRenderTargets(1, &rtv, nullptr);
			context->OMSetBlendState(blend, factor, 0xFFFFFFFF);
			context->OMSetDepthStencilState(depth, 0);
			context->RSSetState(raster);
			context->RSSetViewports(1, &vp);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->IASetInputLayout(nullptr);
			context->VSSetShader(vs, nullptr, 0);
			context->PSSetShader(ps, nullptr, 0);
			context->PSSetShaderResources(0, 1, &srv);
			context->PSSetSamplers(0, 1, &sampler);
			context->PSSetConstantBuffers(0, 1, &params);
			context->Draw(3, 0);
			if (invert) {
				context->OMSetBlendState(invertBlend, factor, 0xFFFFFFFF);
				context->PSSetShader(psInvert, nullptr, 0);
				context->Draw(3, 0);
			}

			// Restore.
			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
			context->OMSetBlendState(oldBlend, oldFactor, oldMask);
			context->OMSetDepthStencilState(oldDepth, oldStencil);
			context->RSSetState(oldRaster);
			context->RSSetViewports(oldVpCount, oldVps);
			context->IASetPrimitiveTopology(oldTopo);
			context->IASetInputLayout(oldLayout);
			context->VSSetShader(oldVs, nullptr, 0);
			context->PSSetShader(oldPs, nullptr, 0);
			context->PSSetShaderResources(0, 1, &oldSrv);
			context->PSSetSamplers(0, 1, &oldSampler);
			context->PSSetConstantBuffers(0, 1, &oldCb);
			for (auto*& r : oldRtv) {
				SafeRelease(r);
			}
			SafeRelease(oldDsv);
			SafeRelease(oldBlend);
			SafeRelease(oldRaster);
			SafeRelease(oldDepth);
			SafeRelease(oldLayout);
			SafeRelease(oldVs);
			SafeRelease(oldPs);
			SafeRelease(oldSrv);
			SafeRelease(oldSampler);
			SafeRelease(oldCb);
			SafeRelease(rtv);
		}

		HRESULT WINAPI PresentHook(IDXGISwapChain* a_swapChain, UINT a_sync, UINT a_flags)
		{
			// Present runs even while the game is paused (menus, loading), unlike the player update.
			link::Heartbeat();
			try {
				Game::KeepFirstPersonHidden();
				// Minecraft's blocks go under its hand and HUD.
				if (link::IsOpen() && InitResources(a_swapChain)) {
					Blocks::Draw(device, context, a_swapChain);
				}
				DrawOverlay(a_swapChain);
			} catch (...) {
			}
			return originalPresent(a_swapChain, a_sync, a_flags);
		}
	}

	namespace Overlay
	{
		void Install()
		{
			if (originalPresent) {
				return;
			}
			auto* data = RE::BSGraphics::GetRendererData();
			auto* swapChain = data ? reinterpret_cast<IDXGISwapChain*>(data->renderWindow[0].swapChain) : nullptr;
			if (!swapChain) {
				return;  // the renderer isn't up yet; tried again later
			}
			auto** vtable = *reinterpret_cast<void***>(swapChain);
			DWORD  oldProtect = 0;
			::VirtualProtect(&vtable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
			originalPresent = reinterpret_cast<PresentFn>(vtable[8]);
			vtable[8] = reinterpret_cast<void*>(&PresentHook);
			::VirtualProtect(&vtable[8], sizeof(void*), oldProtect, &oldProtect);
			REX::INFO("overlay: Present hooked");
		}
	}
}
