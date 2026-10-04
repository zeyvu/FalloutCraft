// FalloutCraft: Minecraft's blocks as part of Fallout's own world, in its renderer.
//
// Fallout 4 renders "deferred": every object first writes what it is made of into a set of
// screen-sized textures, the G-buffer (here: albedo sRGB, normal RG16, two RGBA8 material targets,
// another sRGB target, motion RG16F), plus depth and stencil; then Fallout lights all of it at
// once (sun with its shadow maps, lamps, ambient occlusion, reflections, fog) and post-processes it.
// So the solid blocks are written into that G-buffer at the end of Fallout's geometry pass, and
// Fallout shades them like its own objects.
//
// What Fallout writes isn't documented, so it's measured while playing:
//  - its passes are watched (its immediate context's OMSetRenderTargets): the G-buffer pass is the
//    one with 4+ screen-sized targets and depth; the blocks go in when Fallout moves on from it;
//  - every few seconds the pixels at the centre of the screen are copied out of the G-buffer and
//    compared with what is really there (Fallout's collision straight ahead: the surface normal):
//    that tells which of the usual normal encodings Fallout uses;
//  - a real Fallout surface that faces up (the ground) gives the blocks' values for the material
//    targets and the stencil, and the albedo target's alpha.
// Until the encoding is known for sure, the blocks are drawn at Present as before.
// Data\F4SE\Plugins\SkyCraft.ini [Render]: bGBuffer=0 turns it off, iGBufferNormal=N forces an encoding.

#include "fo_common.h"
#include "fo_blocks.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef ERROR
#include <d3d11.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace skycraft::Inject
{
	namespace
	{
		using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
		OMSetRenderTargetsFn origOMSet = nullptr;

		ID3D11DeviceContext* immediate = nullptr;
		ID3D11Device*        device = nullptr;
		bool                 enabled = true;
		int                  forcedEncoding = -1;
		UINT                 screenW = 0, screenH = 0;

		struct Bind
		{
			int                     rts{ 0 };
			ID3D11RenderTargetView* rtv[8]{};
			ID3D11Resource*         res[8]{};
			DXGI_FORMAT             fmt[8]{};
			bool                    screen[8]{};
			ID3D11DepthStencilView* dsv{ nullptr };
			ID3D11Resource*         dsRes{ nullptr };
			bool                    depth{ false };
		};
		bool IsGBuffer(const Bind& b) { return b.rts >= 4 && b.depth && b.screen[0] && b.screen[1]; }

		// This frame.
		std::vector<Bind> binds;
		bool              drawnThisFrame = false;
		bool              logFrame = false;

		// From the previous frame.
		int lastGBufferBind = -1;

		// ---- measuring -----------------------------------------------------------------------
		struct Truth
		{
			bool  valid{ false };
			float dist{ 0 };
			float n[3]{}, right[3]{}, up[3]{}, fwd[3]{};
		};
		std::mutex truthLock;
		Truth      truth;

		struct Probe
		{
			ID3D11Texture2D* staging{ nullptr };
			DXGI_FORMAT      fmt{};
			bool             pending{ false };
		};
		std::array<Probe, 8> probes{};
		ID3D11Texture2D*     depthStaging = nullptr;
		DXGI_FORMAT          depthFmt{};
		bool                 depthPending = false;
		int                  probeAge = 0;
		Truth                truthAtCopy;
		bool                 sampleNow = false;
		int                  samples = 0;
		ULONGLONG            sampleAt = 0;

		// What was learnt.
		constexpr int kEncodings = 14;
		double        encErr[kEncodings]{};
		int           encSamples = 0;
		int           encoding = -1;     // -1: not known yet
		bool          haveTemplate = false;
		float         templ[4][4]{};
		float         albedoAlpha = 1.0f;
		unsigned      stencil = 0;
		int           drawnFrames = 0, presentFrames = 0;
		ULONGLONG     statsAt = 0;

		std::string FormatName(DXGI_FORMAT a_f)
		{
			switch (a_f) {
			case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8_SRGB";
			case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
			case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
			case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
			case DXGI_FORMAT_R16G16_FLOAT: return "RG16F";
			case DXGI_FORMAT_R16G16_UNORM: return "RG16";
			default: return std::format("fmt{}", static_cast<int>(a_f));
			}
		}

		bool ScreenSized(ID3D11Resource* a_res)
		{
			if (!a_res) {
				return false;
			}
			D3D11_RESOURCE_DIMENSION dim{};
			a_res->GetType(&dim);
			if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
				return false;
			}
			D3D11_TEXTURE2D_DESC td{};
			static_cast<ID3D11Texture2D*>(a_res)->GetDesc(&td);
			return td.Width == screenW && td.Height == screenH;
		}

		float Half(std::uint16_t h)
		{
			const std::uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
			const float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? 65504.0f : std::ldexp(float(m | 1024), int(e) - 25);
			return s ? -v : v;
		}

		// A pixel as 4 floats, the way a shader writing that target would have to output it.
		void DecodePixel(DXGI_FORMAT a_f, const std::uint8_t* a_px, float a_out[4])
		{
			a_out[0] = a_out[1] = a_out[2] = 0.0f;
			a_out[3] = 1.0f;
			std::uint32_t u = 0;
			std::memcpy(&u, a_px, 4);
			auto srgbToLinear = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
			switch (a_f) {
			case DXGI_FORMAT_R8G8B8A8_UNORM:
				for (int k = 0; k < 4; ++k) {
					a_out[k] = a_px[k] / 255.0f;
				}
				break;
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				for (int k = 0; k < 3; ++k) {
					a_out[k] = srgbToLinear(a_px[k] / 255.0f);
				}
				a_out[3] = a_px[3] / 255.0f;
				break;
			case DXGI_FORMAT_R10G10B10A2_UNORM:
				a_out[0] = (u & 1023) / 1023.0f, a_out[1] = ((u >> 10) & 1023) / 1023.0f, a_out[2] = ((u >> 20) & 1023) / 1023.0f, a_out[3] = (u >> 30) / 3.0f;
				break;
			case DXGI_FORMAT_R16G16_UNORM:
				a_out[0] = (u & 0xFFFF) / 65535.0f, a_out[1] = (u >> 16) / 65535.0f;
				break;
			case DXGI_FORMAT_R16G16_FLOAT:
				a_out[0] = Half(std::uint16_t(u & 0xFFFF)), a_out[1] = Half(std::uint16_t(u >> 16));
				break;
			default:
				break;
			}
		}

		std::uint32_t BytesPerPixel(DXGI_FORMAT a_f)
		{
			switch (a_f) {
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R32G8X24_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return 8;
			case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
			default: return 4;
			}
		}

		// The encodings tried (the shader's EncodeNormal must match): bits 0-2 kind, bit 3 view y flipped.
		bool Encode(int a_id, const Truth& t, float a_out[2])
		{
			float v[3] = { 0, 0, 0 };
			for (int k = 0; k < 3; ++k) {
				v[0] += t.n[k] * t.right[k], v[1] += t.n[k] * t.up[k], v[2] += t.n[k] * t.fwd[k];
			}
			if (a_id & 8) {
				v[1] = -v[1];
			}
			auto oct = [&](float x, float y, float z) {
				const float s = std::fabs(x) + std::fabs(y) + std::fabs(z);
				float       ox = x / s, oy = y / s;
				if (z / s < 0) {
					const float tx = (1 - std::fabs(oy)) * (ox >= 0 ? 1.0f : -1.0f), ty = (1 - std::fabs(ox)) * (oy >= 0 ? 1.0f : -1.0f);
					ox = tx, oy = ty;
				}
				a_out[0] = ox * 0.5f + 0.5f, a_out[1] = oy * 0.5f + 0.5f;
			};
			switch (a_id & 7) {
			case 0: {
				const float z = -v[2], f = std::sqrt(8 * z + 8);
				if (f < 1e-4f) return false;
				a_out[0] = v[0] / f + 0.5f, a_out[1] = v[1] / f + 0.5f;
				return true;
			}
			case 1: {
				const float f = std::sqrt(8 * v[2] + 8);
				if (f < 1e-4f) return false;
				a_out[0] = v[0] / f + 0.5f, a_out[1] = v[1] / f + 0.5f;
				return true;
			}
			case 2: oct(v[0], v[1], -v[2]); return true;
			case 3: oct(t.n[0], t.n[1], t.n[2]); return true;
			case 4: a_out[0] = v[0] * 0.5f + 0.5f, a_out[1] = v[1] * 0.5f + 0.5f; return true;
			case 5: a_out[0] = t.n[0] * 0.5f + 0.5f, a_out[1] = t.n[1] * 0.5f + 0.5f; return true;
			default: return false;
			}
		}
		const int kIds[kEncodings] = { 0, 1, 2, 3, 4, 5, 8, 9, 10, 12, 0, 0, 0, 0 };
		constexpr int kUsed = 10;

		void CopyCentre(ID3D11DeviceContext* a_ctx, const Bind& a_bind)
		{
			for (int i = 0; i < a_bind.rts && i < 8; ++i) {
				auto* tex = static_cast<ID3D11Texture2D*>(a_bind.res[i]);
				if (!tex || !a_bind.screen[i]) {
					continue;
				}
				D3D11_TEXTURE2D_DESC td{};
				tex->GetDesc(&td);
				auto& p = probes[i];
				if (p.staging && p.fmt != td.Format) {
					p.staging->Release();
					p.staging = nullptr;
				}
				if (!p.staging) {
					D3D11_TEXTURE2D_DESC sd{};
					sd.Width = 3, sd.Height = 3, sd.MipLevels = 1, sd.ArraySize = 1;
					sd.Format = td.Format;
					sd.SampleDesc.Count = 1;
					sd.Usage = D3D11_USAGE_STAGING;
					sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					if (FAILED(device->CreateTexture2D(&sd, nullptr, &p.staging))) {
						p.staging = nullptr;
						continue;
					}
					p.fmt = td.Format;
				}
				D3D11_BOX box{ td.Width / 2 - 1, td.Height / 2 - 1, 0, td.Width / 2 + 2, td.Height / 2 + 2, 1 };
				a_ctx->CopySubresourceRegion(p.staging, 0, 0, 0, 0, tex, 0, &box);
				p.pending = true;
			}
			// Depth-stencil can only be copied whole.
			if (auto* ds = static_cast<ID3D11Texture2D*>(a_bind.dsRes)) {
				D3D11_TEXTURE2D_DESC td{};
				ds->GetDesc(&td);
				if (depthStaging && depthFmt != td.Format) {
					depthStaging->Release();
					depthStaging = nullptr;
				}
				if (!depthStaging && td.SampleDesc.Count == 1) {
					D3D11_TEXTURE2D_DESC sd = td;
					sd.Usage = D3D11_USAGE_STAGING;
					sd.BindFlags = 0;
					sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					sd.MiscFlags = 0;
					if (SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &depthStaging))) {
						depthFmt = td.Format;
					} else {
						depthStaging = nullptr;
					}
				}
				if (depthStaging) {
					a_ctx->CopyResource(depthStaging, ds);
					depthPending = true;
				}
			}
			probeAge = 0;
			std::scoped_lock guard(truthLock);
			truthAtCopy = truth;
		}

		void ReadProbes(ID3D11DeviceContext* a_ctx)
		{
			float       px[8][4]{};
			bool        got[8]{};
			std::string line;
			for (int i = 0; i < 8; ++i) {
				auto& p = probes[i];
				if (!p.pending || !p.staging) {
					continue;
				}
				D3D11_MAPPED_SUBRESOURCE m{};
				if (a_ctx->Map(p.staging, 0, D3D11_MAP_READ, 0, &m) != S_OK) {
					continue;
				}
				DecodePixel(p.fmt, static_cast<const std::uint8_t*>(m.pData) + m.RowPitch + BytesPerPixel(p.fmt), px[i]);
				a_ctx->Unmap(p.staging, 0);
				p.pending = false;
				got[i] = true;
				line += std::format(" | RT{} {} ({:.3f} {:.3f} {:.3f} {:.3f})", i, FormatName(p.fmt), px[i][0], px[i][1], px[i][2], px[i][3]);
			}
			int st = -1;
			if (depthPending && depthStaging) {
				D3D11_MAPPED_SUBRESOURCE m{};
				if (a_ctx->Map(depthStaging, 0, D3D11_MAP_READ, 0, &m) == S_OK) {
					const auto* base = static_cast<const std::uint8_t*>(m.pData) + std::size_t(screenH / 2) * m.RowPitch + std::size_t(screenW / 2) * BytesPerPixel(depthFmt);
					if (depthFmt == DXGI_FORMAT_R24G8_TYPELESS || depthFmt == DXGI_FORMAT_D24_UNORM_S8_UINT) {
						st = base[3];
					} else if (depthFmt == DXGI_FORMAT_R32G8X24_TYPELESS || depthFmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
						st = base[4];
					}
					a_ctx->Unmap(depthStaging, 0);
				}
				depthPending = false;
			}
			if (!got[1]) {
				return;
			}
			++samples;
			const Truth t = truthAtCopy;
			std::string verdict;
			if (t.valid && t.dist < 6000.0f) {
				// Which encoding turns the real normal into what Fallout wrote?
				int   best = -1;
				float bestErr = 1e9f;
				for (int k = 0; k < kUsed; ++k) {
					float e[2];
					if (!Encode(kIds[k], t, e)) {
						continue;
					}
					const float err = std::fabs(e[0] - px[1][0]) + std::fabs(e[1] - px[1][1]);
					encErr[k] += err;
					if (err < bestErr) {
						bestErr = err, best = k;
					}
				}
				++encSamples;
				verdict = std::format("best encoding this time {} (error {:.3f})", best >= 0 ? kIds[best] : -1, bestErr);
				// A surface facing up (the ground): its material values are the blocks' ones.
				if (t.n[2] > 0.85f) {
					for (int r = 2; r < 6; ++r) {
						if (got[r]) {
							std::memcpy(templ[r - 2], px[r], sizeof(templ[r - 2]));
						}
					}
					if (got[0]) {
						albedoAlpha = px[0][3];
					}
					if (st >= 0) {
						stencil = unsigned(st);
					}
					haveTemplate = true;
				}
			} else {
				verdict = "nothing measurable straight ahead";
			}
			// Settle on the encoding with the lowest average error, once it's clearly the one.
			if (forcedEncoding >= 0) {
				encoding = forcedEncoding;
			} else if (encSamples >= 3) {
				int   best = 0, second = -1;
				for (int k = 1; k < kUsed; ++k) {
					if (encErr[k] < encErr[best]) {
						second = best, best = k;
					} else if (second < 0 || encErr[k] < encErr[second]) {
						second = k;
					}
				}
				const double avg = encErr[best] / encSamples, avg2 = second >= 0 ? encErr[second] / encSamples : 9.0;
				const int    chosen = (avg < 0.06 && avg2 > avg * 2.0) ? kIds[best] : -1;
				if (chosen != encoding) {
					encoding = chosen;
					REX::INFO("gbuffer: {}", chosen >= 0 ? std::format("Fallout's normal encoding is {} (average error {:.3f}, next best {:.3f}); the blocks go into its G-buffer now", chosen, avg, avg2)
					                                       : std::format("no normal encoding fits clearly yet (best {:.3f}, next {:.3f})", avg, avg2));
				}
			}
			if (samples <= 12 || samples % 10 == 0) {
				REX::INFO("gbuffer sample {}: centre{}{} | truth: {} | {}", samples, line, st >= 0 ? std::format(" | stencil {}", st) : std::string(),
					t.valid ? std::format("{:.0f} units, normal ({:.2f} {:.2f} {:.2f})", t.dist, t.n[0], t.n[1], t.n[2]) : std::string("-"), verdict);
			}
		}

		Bind MakeBind(UINT a_n, ID3D11RenderTargetView* const* a_rtvs, ID3D11DepthStencilView* a_dsv)
		{
			Bind b{};
			for (UINT i = 0; i < a_n && i < 8; ++i) {
				if (!a_rtvs || !a_rtvs[i]) {
					continue;
				}
				ID3D11Resource* r = nullptr;
				a_rtvs[i]->GetResource(&r);
				r->Release();
				D3D11_RENDER_TARGET_VIEW_DESC vd{};
				a_rtvs[i]->GetDesc(&vd);
				b.rtv[i] = a_rtvs[i];
				b.res[i] = r;
				b.fmt[i] = vd.Format;
				b.screen[i] = ScreenSized(r);
				b.rts = int(i) + 1;
			}
			if (a_dsv) {
				ID3D11Resource* r = nullptr;
				a_dsv->GetResource(&r);
				r->Release();
				b.dsv = a_dsv;
				b.dsRes = r;
				b.depth = ScreenSized(r);
			}
			return b;
		}

		void STDMETHODCALLTYPE OMSetRenderTargetsHook(ID3D11DeviceContext* a_ctx, UINT a_n, ID3D11RenderTargetView* const* a_rtvs, ID3D11DepthStencilView* a_dsv)
		{
			if (a_ctx == immediate && enabled) {
				// Fallout is moving on from its last G-buffer pass: its world's geometry is all in.
				if (lastGBufferBind >= 0 && int(binds.size()) == lastGBufferBind + 1 && IsGBuffer(binds.back())) {
					const Bind& gb = binds.back();
					if (sampleNow) {
						CopyCentre(a_ctx, gb);
						sampleNow = false;
					}
					if (encoding >= 0 && haveTemplate && !drawnThisFrame) {
						Blocks::GBufferTarget target{};
						target.count = unsigned(gb.rts);
						std::copy(std::begin(gb.rtv), std::end(gb.rtv), target.rtvs);
						target.dsv = gb.dsv;
						target.stencil = stencil;
						target.encoding = encoding;
						target.albedoAlpha = albedoAlpha;
						std::memcpy(target.templ, templ, sizeof(templ));
						drawnThisFrame = Blocks::DrawGBuffer(a_ctx, target);
					}
				}
				if (binds.size() < 4000) {
					binds.push_back(MakeBind(a_n, a_rtvs, a_dsv));
				}
			}
			origOMSet(a_ctx, a_n, a_rtvs, a_dsv);
		}

		void Patch(void** a_vtbl, int a_index, void* a_hook, void** a_orig)
		{
			DWORD old = 0;
			if (::VirtualProtect(&a_vtbl[a_index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
				*a_orig = a_vtbl[a_index];
				a_vtbl[a_index] = a_hook;
				::VirtualProtect(&a_vtbl[a_index], sizeof(void*), old, &old);
			}
		}
	}

	void Install(::ID3D11DeviceContext* a_context)
	{
		if (immediate || !a_context) {
			return;
		}
		wchar_t path[MAX_PATH]{};
		::GetModuleFileNameW(nullptr, path, MAX_PATH);
		if (auto* slash = std::wcsrchr(path, L'\\')) {
			*(slash + 1) = 0;
		}
		::wcscat_s(path, MAX_PATH, L"Data\\F4SE\\Plugins\\SkyCraft.ini");
		enabled = ::GetPrivateProfileIntW(L"Render", L"bGBuffer", 1, path) != 0;
		forcedEncoding = static_cast<int>(::GetPrivateProfileIntW(L"Render", L"iGBufferNormal", -1, path));
		immediate = a_context;
		a_context->GetDevice(&device);
		if (device) {
			device->Release();  // the context keeps it alive
		}
		if (!enabled) {
			REX::INFO("gbuffer: off (SkyCraft.ini bGBuffer=0)");
			return;
		}
		auto** vtbl = *reinterpret_cast<void***>(a_context);
		Patch(vtbl, 33, reinterpret_cast<void*>(&OMSetRenderTargetsHook), reinterpret_cast<void**>(&origOMSet));
		REX::INFO("gbuffer: watching Fallout's passes; measuring its G-buffer (stand still now and then, looking at the ground and at walls)");
	}

	void SetTruth(std::string)
	{
	}

	void SetTruthNormal(bool a_valid, float a_dist, const float a_n[3], const float a_right[3], const float a_up[3], const float a_fwd[3])
	{
		std::scoped_lock guard(truthLock);
		truth.valid = a_valid;
		if (!a_valid) {
			return;
		}
		truth.dist = a_dist;
		for (int k = 0; k < 3; ++k) {
			truth.n[k] = a_n[k], truth.right[k] = a_right[k], truth.up[k] = a_up[k], truth.fwd[k] = a_fwd[k];
		}
	}

	bool InjectedThisFrame()
	{
		return false;  // the HDR-target path is retired; see GBufferThisFrame
	}

	bool GBufferThisFrame()
	{
		return drawnThisFrame;
	}

	void EndFrame(std::uint32_t a_width, std::uint32_t a_height)
	{
		if (!enabled || !immediate) {
			return;
		}
		if (a_width != screenW || a_height != screenH) {
			screenW = a_width, screenH = a_height;
			lastGBufferBind = -1;
		}
		int last = -1;
		for (int i = 0; i < int(binds.size()); ++i) {
			if (IsGBuffer(binds[i])) {
				last = i;
			}
		}
		if (logFrame) {
			logFrame = false;
			std::string line;
			for (int i = 0; i < int(binds.size()) && i < 60; ++i) {
				const auto& b = binds[i];
				if (b.rts < 2) {
					continue;
				}
				line += std::format("\n  pass {:3}:{}{}", i, b.depth ? " depth" : "", IsGBuffer(b) ? (i == last ? "  <- G-buffer (blocks go in after this)" : "  G-buffer") : "");
				for (int r = 0; r < b.rts; ++r) {
					line += std::format(" | RT{} {}", r, FormatName(b.fmt[r]));
				}
			}
			REX::INFO("gbuffer: Fallout's passes with several targets:{}", line);
		}
		lastGBufferBind = last;
		drawnThisFrame ? ++drawnFrames : ++presentFrames;
		if (++probeAge == 3) {
			ReadProbes(immediate);
		}
		const auto now = ::GetTickCount64();
		const ULONGLONG every = encoding >= 0 && haveTemplate ? 20000 : 2000;
		if (now - sampleAt > every && last >= 0) {
			sampleAt = now;
			sampleNow = true;
		}
		if (now - statsAt > 15000) {
			statsAt = now;
			logFrame = samples < 3;
			REX::INFO("gbuffer: last 15 s: blocks in Fallout's G-buffer in {} frames, drawn over the frame in {}", drawnFrames, presentFrames);
			drawnFrames = presentFrames = 0;
		}
		binds.clear();
		drawnThisFrame = false;
	}
}
