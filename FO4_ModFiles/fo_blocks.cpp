// FalloutCraft Phase 4a: Minecraft's blocks in Fallout's world.
//
// Minecraft builds its own block meshes (its block renderer: models, tint, ambient occlusion,
// block and sky light) per 16x16x16 section and sends them with its texture atlas through the
// render ring. They are drawn here, in Present, with Fallout's own camera matrix, and hidden
// wherever Fallout's depth buffer says Fallout geometry is nearer.
//
// Fallout's depth convention isn't assumed: depth = A + B / w is solved from Fallout's own
// world->clip matrix every frame, so normal and reversed depth both work.
// A much smaller renderer than the Skyrim one (skse/src/WorldRender.cpp): no shadows, no Fallout
// lights, no entities yet; lighting is Minecraft's own (sky light x daylight, block light).

#include "fo_common.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#undef ERROR  // wingdi.h; clashes with REX::ERROR

#include "fo_blocks.h"

#include <atomic>
#include <format>
#include <string>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace skycraft::Blocks
{
	namespace
	{
		template <class T>
		void Release(T*& a_ptr)
		{
			if (a_ptr) {
				a_ptr->Release();
				a_ptr = nullptr;
			}
		}

		constexpr std::uint32_t kFlagCutout = 1u << 0;
		constexpr std::uint32_t kFlagTranslucent = 1u << 1;
		constexpr std::uint32_t kFlagUntextured = 1u << 2;  // outline: vertex colour only
		constexpr std::uint32_t kFlagNoMip = 1u << 3;       // items, arrows, cracks: full detail, like Minecraft
		constexpr std::uint32_t kFullSkyLight = 15u << 8;
		constexpr float         kPi = 3.14159265f;
		// Minecraft Direction ordinal + 1, in bits 4-6 (the shader's per-face shading).
		constexpr std::uint32_t kFaceDown = 1u << 4, kFaceUp = 2u << 4, kFaceNorth = 3u << 4, kFaceSouth = 4u << 4, kFaceWest = 5u << 4, kFaceEast = 6u << 4;

		constexpr char kShader[] = R"(
cbuffer Frame : register(b0)
{
	row_major float4x4 viewProj;  // camera-relative Fallout units -> clip
	float4 depthAB;               // x: A, y: B (depth = A + B / w), z: occlusion on, w: -
	float4 lighting;              // x: daylight 0..1, y: darkest light, z: brightness, w: -
	float4 camDug;                // camera in Minecraft blocks, relative to the dug-cell volume's corner; w: volume valid
	// FalloutCraft: Fallout's light (fo_shadow.cpp); sunDir.w = 1 outdoors (else Minecraft's own lighting)
	float4 sunDir;                // towards the sun, Fallout axes
	float4 sunColor;
	float4 ambient[6];            // Fallout's directional ambient: +X -X +Y -Y +Z -Z
	float4 fogParams;             // near, far (Fallout units), power, max (far 0: no fog)
	float4 fogNear;
	float4 fogFar;
	float4 hdr;                   // x: 1 = drawing into Fallout's HDR scene (linear light), y: exposure
	// FalloutCraft G-buffer (fo_inject.cpp): the camera's axes (Fallout world), the normal encoding
	// Fallout uses (x), the albedo target's alpha (y), and what a real Fallout surface wrote into the
	// other G-buffer targets (RT2..RT5), copied for the blocks.
	float4 camRight;
	float4 camUp;
	float4 camFwd;
	float4 gbEnc;
	float4 gbT[4];
};
cbuffer Object : register(b1)
{
	float4 offset;                // camera-relative Fallout position of the section's corner
};
Texture2D atlas : register(t0);
Texture2D<float> sceneDepth : register(t1);
Texture3D<uint> dugCells : register(t2);  // FalloutCraft: cells dug out of Fallout's world around the camera (x, y, z)
SamplerState atlasSampler : register(s0);

struct VSIn
{
	float3 pos : POSITION;    // Minecraft blocks, relative to the section corner
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;    // tint * ambient occlusion
	uint light : TEXCOORD1;   // block light | sky light << 8
	uint flags : TEXCOORD2;   // cutout, translucent, face normal
	float shadow : TEXCOORD4; // FalloutCraft: 1 sunlit, 0 in Fallout's shadow
};
struct VSOut
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
	float viewW : TEXCOORD1;
	nointerpolation uint flags : TEXCOORD2;
	float3 rel : TEXCOORD3;   // camera-relative Fallout position
	float3 lit : TEXCOORD5;   // light on it (colour)
};

VSOut VSMain(VSIn i)
{
	VSOut o;
	// Minecraft (x, y up, z south) -> Fallout (x east, y north, z up), 70 units per block.
	float3 g = float3(i.pos.x * 70.0, -i.pos.z * 70.0, i.pos.y * 70.0) + offset.xyz;
	o.pos = mul(viewProj, float4(g, 1.0));
	o.viewW = o.pos.w;
	o.rel = g;
	o.uv = i.uv;
	o.flags = i.flags;

	// Minecraft's lightmap, simplified: the brighter of sky light (scaled by daylight) and block
	// light, then its fixed per-face shading (top 1, bottom 0.5, north/south 0.8, east/west 0.6).
	float block = (i.light & 15) / 15.0;
	float sky = ((i.light >> 8) & 15) / 15.0;
	uint face = (i.flags >> 4) & 7;
	if (sunDir.w > 0.5) {
		// FalloutCraft: Fallout's light. Its directional ambient for this face plus its sun, unless
		// Fallout's world shadows the face; scaled by how open to the sky the block is. Torches and
		// other Minecraft lights still light it (warm), whichever is brighter.
		static const float3 kNormals[8] = { float3(0, 0, 1), float3(0, 0, -1), float3(0, 0, 1), float3(0, 1, 0),
			float3(0, -1, 0), float3(-1, 0, 0), float3(1, 0, 0), float3(0, 0, 1) };
		float3 n = kNormals[face];
		float3 amb = face == 0 ? (ambient[0].rgb + ambient[1].rgb + ambient[2].rgb + ambient[3].rgb + ambient[4].rgb + ambient[5].rgb) / 6.0
		           : (n.x > 0.5 ? ambient[0].rgb : n.x < -0.5 ? ambient[1].rgb : n.y > 0.5 ? ambient[2].rgb : n.y < -0.5 ? ambient[3].rgb
		              : n.z > 0.5 ? ambient[4].rgb : ambient[5].rgb);
		float ndl = face == 0 ? 0.6 : saturate(dot(n, sunDir.xyz));
		float3 skyLit = (amb + sunColor.rgb * ndl * i.shadow) * (sky * sky);
		float3 blockLit = block * block * float3(1.0, 0.82, 0.62);
		float3 lit = max(max(skyLit, blockLit), lighting.y * 0.5);
		if (hdr.x < 0.5) {
			// Drawn over Fallout's finished (tone-mapped) frame: its sun is HDR light, far over 1, and
			// would wash light textures out to white (skin, shirts, plushies facing the sun). Roll the
			// light off softly towards 1 instead, keeping its colour: at most a texture's own colour.
			float m = max(max(lit.r, lit.g), lit.b);
			if (m > 0.8) {
				lit *= (0.8 + 0.2 * (1.0 - exp(-(m - 0.8) / 0.2))) / m;
			}
		}
		o.color = i.color;
		o.lit = lit * lighting.z;
		return o;
	}
	float l = max(sky * lighting.x, block);
	l = lerp(lighting.y, 1.0, l * l * (3.0 - 2.0 * l));
	static const float kFace[8] = { 1.0, 0.5, 1.0, 0.8, 0.8, 0.6, 0.6, 1.0 };
	o.color = i.color;
	o.lit = l * kFace[face] * lighting.z;
	return o;
}

float4 Shade(VSOut i)
{
	if (depthAB.z > 0.5) {
		float d = sceneDepth.Load(int3(i.pos.xy, 0));
		float sceneW = depthAB.y / (d - depthAB.x);
		// Sky and anything unreadable count as infinitely far.
		// Depths at the near plane (cleared buffers, first-person geometry) say nothing about walls.
		if (sceneW > 20.0 && i.viewW > sceneW * 1.002 + 1.5) {
			// FalloutCraft: behind Fallout's surface. If that surface was dug out (Fallout still draws
			// its land there), the hole's Minecraft walls and floor show through it.
			bool hole = false;
			if (camDug.w > 0.5) {
				float3 surface = i.rel * (sceneW / i.viewW);  // where the ray meets Fallout's surface
				float3 dir = normalize(i.rel);
				[unroll] for (int k = 0; k < 2; ++k) {
					float3 f = surface + dir * (k == 0 ? 4.0 : 20.0);  // just past it
					float3 mc = camDug.xyz + float3(f.x, f.z, -f.y) / 70.0;
					int3 c = int3(floor(mc));
					if (all(c >= 0) && all(c < int3(64, 32, 64)) && dugCells.Load(int4(c, 0)) != 0) hole = true;
				}
			}
			if (!hole) discard;
		}
	}
	float4 tex = (i.flags & 4) != 0 ? float4(1, 1, 1, 1)
	           : (i.flags & 8) != 0 ? atlas.SampleLevel(atlasSampler, i.uv, 0)
	           : atlas.Sample(atlasSampler, i.uv);
	if ((i.flags & 1) != 0 && tex.a < 0.5) discard;
	float3 base = tex.rgb * i.color.rgb;
	if (hdr.x > 0.5) {
		base = pow(max(base, 0.0), 2.2);  // Fallout's scene is linear light; Minecraft's textures are sRGB
	}
	float3 rgb = base * i.lit * (hdr.x > 0.5 ? hdr.y : 1.0);
	if (sunDir.w > 0.5 && fogParams.y > fogParams.x) {
		// FalloutCraft: Fallout's fog, as its world gets it.
		float t = saturate((length(i.rel) - fogParams.x) / (fogParams.y - fogParams.x));
		float f = min(pow(t, fogParams.z), fogParams.w);
		rgb = lerp(rgb, lerp(fogNear.rgb, fogFar.rgb, t), f);
	}
	return float4(rgb, tex.a * i.color.a);
}

float4 PSOpaque(VSOut i) : SV_Target { float4 c = Shade(i); return float4(c.rgb, 1.0); }
float4 PSTranslucent(VSOut i) : SV_Target { return Shade(i); }

// ---- FalloutCraft: the blocks as Fallout geometry, written into Fallout's G-buffer ----
float2 OctWrap(float2 v) { return (1.0 - abs(v.yx)) * (v.xy >= 0.0 ? 1.0 : -1.0); }
float2 EncodeNormal(float3 nWorld, uint id)
{
	float3 v = float3(dot(nWorld, camRight.xyz), dot(nWorld, camUp.xyz), dot(nWorld, camFwd.xyz));  // x right, y up, z forward
	if ((id & 8) != 0) v.y = -v.y;
	uint kind = id & 7;
	if (kind == 0) { v.z = -v.z; return v.xy / sqrt(8.0 * v.z + 8.0) + 0.5; }   // spheremap, z towards the viewer
	if (kind == 1) { return v.xy / sqrt(8.0 * v.z + 8.0) + 0.5; }               // spheremap, z forward
	if (kind == 2) { v.z = -v.z; float3 o = v / (abs(v.x) + abs(v.y) + abs(v.z)); return (o.z >= 0.0 ? o.xy : OctWrap(o.xy)) * 0.5 + 0.5; }
	if (kind == 3) { float3 w = nWorld; float3 o = w / (abs(w.x) + abs(w.y) + abs(w.z)); return (o.z >= 0.0 ? o.xy : OctWrap(o.xy)) * 0.5 + 0.5; }
	if (kind == 4) { return v.xy * 0.5 + 0.5; }
	return nWorld.xy * 0.5 + 0.5;                                                  // 5: world xy
}

struct GBufferOut
{
	float4 c0 : SV_Target0;
	float4 c1 : SV_Target1;
	float4 c2 : SV_Target2;
	float4 c3 : SV_Target3;
	float4 c4 : SV_Target4;
	float4 c5 : SV_Target5;
};

GBufferOut PSGBuffer(VSOut i)
{
	float4 tex = (i.flags & 8) != 0 ? atlas.SampleLevel(atlasSampler, i.uv, 0) : atlas.Sample(atlasSampler, i.uv);
	if ((i.flags & 1) != 0 && tex.a < 0.5) discard;
	static const float3 kNormals[8] = { float3(0, 0, 1), float3(0, 0, -1), float3(0, 0, 1), float3(0, 1, 0),
		float3(0, -1, 0), float3(-1, 0, 0), float3(1, 0, 0), float3(0, 0, 1) };
	float3 n = kNormals[(i.flags >> 4) & 7];
	GBufferOut o;
	float3 albedo = pow(max(tex.rgb * i.color.rgb, 0.0), 2.2);  // linear: the target is sRGB
	o.c0 = float4(albedo, gbEnc.y);
	o.c1 = float4(EncodeNormal(n, (uint)gbEnc.x), 0, 1);
	o.c2 = gbT[0];
	o.c3 = gbT[1];
	o.c4 = gbT[2];
	o.c5 = gbT[3];
	return o;
}
)";

		struct alignas(16) FrameConstants
		{
			float viewProj[4][4];
			float depthAB[4];
			float lighting[4];
			float camDug[4];
			float sunDir[4];
			float sunColor[4];
			float ambient[6][4];
			float fogParams[4];
			float fogNear[4];
			float fogFar[4];
			float hdr[4];
			float camRight[4];
			float camUp[4];
			float camFwd[4];
			float gbEnc[4];
			float gbT[4][4];
		};

		struct alignas(16) ObjectConstants
		{
			float offset[4];
		};

		struct Section
		{
			ID3D11Buffer* vb{ nullptr };
			std::uint32_t opaque{ 0 };       // first the opaque/cutout vertices...
			std::uint32_t translucent{ 0 };  // ...then the translucent ones (water, stained glass)
			std::int32_t  sx{ 0 }, sy{ 0 }, sz{ 0 };
			ID3D11Buffer* shadowVb{ nullptr };  // FalloutCraft: per vertex, 1 sunlit / 0 shadowed (fo_shadow.cpp)
			std::uint64_t shadowVersion{ 0 };
		};

		ID3D11Device*             device = nullptr;
		ID3D11VertexShader*       vs = nullptr;
		ID3D11PixelShader*        psOpaque = nullptr;
		ID3D11PixelShader*        psTranslucent = nullptr;
		ID3D11InputLayout*        layout = nullptr;
		ID3D11Buffer*             frameCb = nullptr;
		ID3D11Buffer*             objectCb = nullptr;
		ID3D11SamplerState*       sampler = nullptr;
		ID3D11RasterizerState*    raster = nullptr;
		ID3D11BlendState*         opaqueBlend = nullptr;
		ID3D11BlendState*         alphaBlend = nullptr;
		ID3D11DepthStencilState*  depthWrite[2] = {};  // [reversed]
		ID3D11DepthStencilState*  depthTest[2] = {};
		ID3D11DepthStencilState*  depthGBuffer[2] = {};  // FalloutCraft: Fallout's depth + stencil
		ID3D11PixelShader*        psGBuffer = nullptr;
		bool                      skipOpaqueSections = false;  // this frame they went into the G-buffer
		ID3D11Texture2D*          ownDepth = nullptr;
		ID3D11DepthStencilView*   ownDsv = nullptr;
		UINT                      ownW = 0, ownH = 0;
		ID3D11Texture2D*          atlasTex = nullptr;
		ID3D11ShaderResourceView* atlasSrv = nullptr;
		bool                      atlasMipsStale = false;
		// FalloutCraft: dug cells around the camera, for seeing into holes through Fallout's land.
		ID3D11Texture3D*          dugTex = nullptr;
		ID3D11ShaderResourceView* dugSrv = nullptr;
		std::uint64_t             dugVersion = ~0ull;
		std::int32_t              dugOrigin[3]{ INT32_MIN, 0, 0 };
		bool                      dugAny = false;
		std::vector<std::uint8_t> dugScratch;
		ID3D11Buffer*             onesVb = nullptr;
		float                     hdrExposure = 1.0f;  // SkyCraft.ini [Render] fHDRExposure  // shadow stream for meshes (entities, the player): all lit
		std::vector<std::uint8_t> shadowLit;
		std::vector<float>        shadowScratch;
		bool                      initFailed = false;

		std::unordered_map<std::uint64_t, Section> sections;
		std::vector<proto::RenVertex>              scratch;

		// Per-frame things: dropped items and blocks, arrows, block-breaking cracks, the outline.
		proto::WorldEntities          entities{};
		std::vector<proto::RenVertex> dynVerts;
		ID3D11Buffer*                 dynVb = nullptr;
		UINT                          dynCapacity = 0;

		// Minecraft's entity renderer output: the player's body in third person (relative to the
		// feet) and every other entity and particle (relative to an origin), with their textures.
		struct EntityTexture
		{
			ID3D11Texture2D*          tex{ nullptr };
			ID3D11ShaderResourceView* srv{ nullptr };
		};
		std::unordered_map<std::uint32_t, EntityTexture> entityTextures;
		struct Mesh
		{
			std::vector<proto::RenBatch> batches;
			ID3D11Buffer*                vb{ nullptr };
			UINT                         capacity{ 0 };
			double                       origin[3]{};
		};
		Mesh avatar;
		Mesh scene;

		std::mutex                 ringMutex;  // the render ring has one consumer at a time
		std::atomic<std::uint64_t> lastDrainMs{ 0 };
		std::atomic<bool>          resendNeeded{ true };  // Minecraft sent its atlas before we could draw
		std::atomic<bool>          drawing{ false };

		// Diagnostics.
		bool          loggedDepth = false;
		std::uint64_t statsAt = 0;
		std::uint64_t receivedSections = 0;

		std::uint64_t Key(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z)
		{
			return (std::uint64_t(std::uint32_t(a_x) & 0x1FFFFF) << 42) | (std::uint64_t(std::uint32_t(a_y) & 0x1FFFFF) << 21) | (std::uint32_t(a_z) & 0x1FFFFF);
		}

		bool Compile(const char* a_entry, const char* a_target, ID3DBlob** a_out)
		{
			ID3DBlob*  errors = nullptr;
			const auto hr = D3DCompile(kShader, sizeof(kShader) - 1, "skycraft_blocks", nullptr, nullptr, a_entry, a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, a_out, &errors);
			if (FAILED(hr)) {
				REX::ERROR("blocks shader {} failed: {}", a_entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
				Release(errors);
				return false;
			}
			Release(errors);
			return true;
		}

		bool Init(ID3D11Device* a_device)
		{
			if (device) {
				return true;
			}
			if (initFailed) {
				return false;
			}
			ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *ptBlob = nullptr;
			if (!Compile("VSMain", "vs_5_0", &vsBlob) || !Compile("PSOpaque", "ps_5_0", &psBlob) || !Compile("PSTranslucent", "ps_5_0", &ptBlob)) {
				Release(vsBlob);
				Release(psBlob);
				initFailed = true;
				return false;
			}
			a_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
			a_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &psOpaque);
			a_device->CreatePixelShader(ptBlob->GetBufferPointer(), ptBlob->GetBufferSize(), nullptr, &psTranslucent);
			{
				ID3DBlob* gbBlob = nullptr;
				if (Compile("PSGBuffer", "ps_5_0", &gbBlob)) {
					a_device->CreatePixelShader(gbBlob->GetBufferPointer(), gbBlob->GetBufferSize(), nullptr, &psGBuffer);
					Release(gbBlob);
				}
				if (!psGBuffer) {
					REX::WARN("blocks: the G-buffer shader didn't compile; blocks stay drawn over Fallout's frame");
				}
			}
			const D3D11_INPUT_ELEMENT_DESC elements[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 2, DXGI_FORMAT_R32_UINT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 4, DXGI_FORMAT_R32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },  // shadow (stream 1)
			};
			a_device->CreateInputLayout(elements, 6, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout);
			Release(vsBlob);
			Release(psBlob);
			Release(ptBlob);

			D3D11_BUFFER_DESC cbd{};
			cbd.Usage = D3D11_USAGE_DYNAMIC;
			cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			cbd.ByteWidth = sizeof(FrameConstants);
			a_device->CreateBuffer(&cbd, nullptr, &frameCb);
			cbd.ByteWidth = sizeof(ObjectConstants);
			a_device->CreateBuffer(&cbd, nullptr, &objectCb);

			D3D11_SAMPLER_DESC sd{};
			sd.Filter = D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR;  // crisp pixels like Minecraft, no shimmer far away
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.MaxLOD = 4.0f;
			a_device->CreateSamplerState(&sd, &sampler);

			D3D11_RASTERIZER_DESC rd{};
			rd.FillMode = D3D11_FILL_SOLID;
			rd.CullMode = D3D11_CULL_NONE;  // Fallout's matrix may mirror the winding; Minecraft's quads are one-sided anyway
			rd.DepthClipEnable = TRUE;
			a_device->CreateRasterizerState(&rd, &raster);

			D3D11_BLEND_DESC bd{};
			bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
			a_device->CreateBlendState(&bd, &opaqueBlend);
			bd.RenderTarget[0].BlendEnable = TRUE;
			bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
			bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
			bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			a_device->CreateBlendState(&bd, &alphaBlend);

			for (int reversed = 0; reversed < 2; ++reversed) {
				D3D11_DEPTH_STENCIL_DESC dd{};
				dd.DepthEnable = TRUE;
				dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
				dd.DepthFunc = reversed ? D3D11_COMPARISON_GREATER_EQUAL : D3D11_COMPARISON_LESS_EQUAL;
				a_device->CreateDepthStencilState(&dd, &depthWrite[reversed]);
				dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
				a_device->CreateDepthStencilState(&dd, &depthTest[reversed]);
				// Into Fallout's own depth buffer, with the stencil value its ground has.
				dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
				dd.StencilEnable = TRUE;
				dd.StencilReadMask = 0xFF;
				dd.StencilWriteMask = 0xFF;
				dd.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS };
				dd.BackFace = dd.FrontFace;
				a_device->CreateDepthStencilState(&dd, &depthGBuffer[reversed]);
			}

			const bool ok = vs && psOpaque && psTranslucent && layout && frameCb && objectCb && sampler && raster && opaqueBlend && alphaBlend &&
			                depthWrite[0] && depthWrite[1] && depthTest[0] && depthTest[1];
			REX::INFO("block renderer {}", ok ? "ready" : "failed to initialize");
			initFailed = !ok;
			if (ok) {
				device = a_device;
			}
			return ok;
		}

		bool EnsureOwnDepth(UINT a_w, UINT a_h)
		{
			if (ownDsv && ownW == a_w && ownH == a_h) {
				return true;
			}
			Release(ownDsv);
			Release(ownDepth);
			D3D11_TEXTURE2D_DESC td{};
			td.Width = a_w;
			td.Height = a_h;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_D32_FLOAT;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			if (FAILED(device->CreateTexture2D(&td, nullptr, &ownDepth)) || FAILED(device->CreateDepthStencilView(ownDepth, nullptr, &ownDsv))) {
				Release(ownDepth);
				return false;
			}
			ownW = a_w;
			ownH = a_h;
			return true;
		}

		// ---- render ring messages ---------------------------------------------------------------

		void OnTexture(const std::uint8_t* a_data, std::uint32_t a_bytes)
		{
			if (a_bytes < sizeof(proto::RenTexture)) {
				return;
			}
			const auto* hdr = reinterpret_cast<const proto::RenTexture*>(a_data);
			if (!hdr->width || !hdr->height || hdr->width > 4096 || hdr->height > 4096 ||
				a_bytes < sizeof(proto::RenTexture) + std::uint64_t(hdr->width) * hdr->height * 4) {
				return;
			}
			auto& t = entityTextures[hdr->id];
			Release(t.srv);
			Release(t.tex);
			D3D11_TEXTURE2D_DESC td{};
			td.Width = hdr->width;
			td.Height = hdr->height;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_IMMUTABLE;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA init{ a_data + sizeof(proto::RenTexture), hdr->width * 4, 0 };
			if (FAILED(device->CreateTexture2D(&td, &init, &t.tex)) || FAILED(device->CreateShaderResourceView(t.tex, nullptr, &t.srv))) {
				Release(t.tex);
				entityTextures.erase(hdr->id);
				return;
			}
			REX::INFO("blocks: received Minecraft entity texture {} ({}x{})", hdr->id, hdr->width, hdr->height);
		}

		// RenAvatar / RenScene: batches + vertices into a mesh's dynamic vertex buffer.
		void OnMesh(ID3D11DeviceContext* a_context, Mesh& a_mesh, const std::uint8_t* a_data, std::uint32_t a_bytes, bool a_hasOrigin)
		{
			a_mesh.batches.clear();
			const std::size_t head = a_hasOrigin ? sizeof(proto::RenScene) : sizeof(proto::RenAvatar);
			if (a_bytes < head) {
				return;
			}
			std::uint32_t batchCount, vertexCount;
			if (a_hasOrigin) {
				const auto* hdr = reinterpret_cast<const proto::RenScene*>(a_data);
				a_mesh.origin[0] = hdr->originX;
				a_mesh.origin[1] = hdr->originY;
				a_mesh.origin[2] = hdr->originZ;
				batchCount = hdr->batchCount;
				vertexCount = hdr->vertexCount;
			} else {
				const auto* hdr = reinterpret_cast<const proto::RenAvatar*>(a_data);
				batchCount = hdr->batchCount;
				vertexCount = hdr->vertexCount;
			}
			const std::uint64_t need = head + std::uint64_t(batchCount) * sizeof(proto::RenBatch) + std::uint64_t(vertexCount) * sizeof(proto::RenVertex);
			if (batchCount == 0 || vertexCount == 0 || a_bytes < need) {
				return;
			}
			const auto* batches = reinterpret_cast<const proto::RenBatch*>(a_data + head);
			const auto* verts = reinterpret_cast<const proto::RenVertex*>(batches + batchCount);
			const UINT  bytes = vertexCount * sizeof(proto::RenVertex);
			if (bytes > a_mesh.capacity) {
				Release(a_mesh.vb);
				D3D11_BUFFER_DESC bd{};
				bd.ByteWidth = std::max<UINT>(bytes * 2, 256 * 1024);
				bd.Usage = D3D11_USAGE_DYNAMIC;
				bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
				bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				if (FAILED(device->CreateBuffer(&bd, nullptr, &a_mesh.vb))) {
					a_mesh.capacity = 0;
					return;
				}
				a_mesh.capacity = bd.ByteWidth;
			}
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(a_context->Map(a_mesh.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				return;
			}
			std::memcpy(mapped.pData, verts, bytes);
			a_context->Unmap(a_mesh.vb, 0);
			for (std::uint32_t b = 0; b < batchCount; ++b) {
				if (batches[b].first + batches[b].count <= vertexCount) {
					a_mesh.batches.push_back(batches[b]);
				}
			}
			static bool loggedAvatar = false, loggedScene = false;
			if (!(a_hasOrigin ? loggedScene : loggedAvatar)) {
				(a_hasOrigin ? loggedScene : loggedAvatar) = true;
				REX::INFO("blocks: {}: {} triangles in {} texture batches", a_hasOrigin ? "Minecraft entities and particles" : "third-person player model",
					vertexCount / 3, batchCount);
			}
		}

		void ClearSections()
		{
			for (auto& [key, s] : sections) {
				Release(s.vb);
				Release(s.shadowVb);
			}
			sections.clear();
			Shadows::Clear();
		}

		void OnAtlas(ID3D11DeviceContext* a_context, const std::uint8_t* a_data, std::uint32_t a_bytes)
		{
			if (a_bytes < sizeof(proto::RenAtlas)) {
				return;
			}
			const auto* hdr = reinterpret_cast<const proto::RenAtlas*>(a_data);
			if (!hdr->width || !hdr->height) {
				return;
			}
			// A big atlas (many mods' textures) comes in pieces: the first rows here, the rest as
			// kRenAtlasRegion strips right after.
			const std::uint64_t rowBytes = std::uint64_t(hdr->width) * 4;
			const auto          rows = static_cast<std::uint32_t>(std::min<std::uint64_t>(hdr->height, (a_bytes - sizeof(proto::RenAtlas)) / rowBytes));
			if (rows == 0) {
				return;
			}
			Release(atlasSrv);
			Release(atlasTex);
			D3D11_TEXTURE2D_DESC td{};
			td.Width = hdr->width;
			td.Height = hdr->height;
			td.MipLevels = 5;  // 16px sprites stay inside their own cell down to 1px
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
			if (FAILED(device->CreateTexture2D(&td, nullptr, &atlasTex)) || FAILED(device->CreateShaderResourceView(atlasTex, nullptr, &atlasSrv))) {
				REX::ERROR("blocks: atlas texture {}x{} failed", hdr->width, hdr->height);
				Release(atlasTex);
				return;
			}
			const D3D11_BOX box{ 0, 0, 0, hdr->width, rows, 1 };
			a_context->UpdateSubresource(atlasTex, 0, &box, a_data + sizeof(proto::RenAtlas), hdr->width * 4, 0);
			if (rows == hdr->height) {
				a_context->GenerateMips(atlasSrv);
			} else {
				atlasMipsStale = true;  // the rest arrives as regions
			}
			REX::INFO("blocks: received Minecraft's texture atlas {}x{} ({} rows in the first piece)", hdr->width, hdr->height, rows);
		}

		void OnAtlasRegion(ID3D11DeviceContext* a_context, const std::uint8_t* a_data, std::uint32_t a_bytes)
		{
			if (!atlasTex || a_bytes < sizeof(proto::RenAtlasRegion)) {
				return;
			}
			const auto*          hdr = reinterpret_cast<const proto::RenAtlasRegion*>(a_data);
			D3D11_TEXTURE2D_DESC td{};
			atlasTex->GetDesc(&td);
			if (!hdr->width || !hdr->height || hdr->x + hdr->width > td.Width || hdr->y + hdr->height > td.Height ||
				a_bytes < sizeof(proto::RenAtlasRegion) + std::uint64_t(hdr->width) * hdr->height * 4) {
				return;
			}
			const D3D11_BOX box{ hdr->x, hdr->y, 0, hdr->x + hdr->width, hdr->y + hdr->height, 1 };
			a_context->UpdateSubresource(atlasTex, 0, &box, a_data + sizeof(proto::RenAtlasRegion), hdr->width * 4, 0);
			atlasMipsStale = true;  // water, lava and fire animate
		}

		void OnSection(const std::uint8_t* a_data, std::uint32_t a_bytes)
		{
			if (a_bytes < sizeof(proto::RenSection)) {
				return;
			}
			const auto* hdr = reinterpret_cast<const proto::RenSection*>(a_data);
			const auto  key = Key(hdr->sx, hdr->sy, hdr->sz);
			if (auto it = sections.find(key); it != sections.end()) {
				Release(it->second.vb);
				Release(it->second.shadowVb);
				sections.erase(it);
			}
			Shadows::Remove(key);
			++receivedSections;
			const std::uint32_t count = hdr->vertexCount - hdr->vertexCount % 3;
			if (count == 0 || a_bytes < sizeof(proto::RenSection) + std::uint64_t(count) * sizeof(proto::RenVertex)) {
				return;  // an emptied section: just removed
			}
			const auto* src = reinterpret_cast<const proto::RenVertex*>(a_data + sizeof(proto::RenSection));
			scratch.clear();
			scratch.reserve(count);
			std::uint32_t opaque = 0;
			for (int pass = 0; pass < 2; ++pass) {
				for (std::uint32_t t = 0; t < count; t += 3) {
					const bool translucent = (src[t].flags & kFlagTranslucent) != 0;
					if (translucent == (pass == 1)) {
						scratch.insert(scratch.end(), src + t, src + t + 3);
					}
				}
				if (pass == 0) {
					opaque = static_cast<std::uint32_t>(scratch.size());
				}
			}
			Section s{};
			s.opaque = opaque;
			s.translucent = static_cast<std::uint32_t>(scratch.size()) - opaque;
			s.sx = hdr->sx;
			s.sy = hdr->sy;
			s.sz = hdr->sz;
			D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = static_cast<UINT>(scratch.size() * sizeof(proto::RenVertex));
			bd.Usage = D3D11_USAGE_IMMUTABLE;
			bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			D3D11_SUBRESOURCE_DATA init{ scratch.data(), 0, 0 };
			if (FAILED(device->CreateBuffer(&bd, &init, &s.vb))) {
				return;
			}
			// FalloutCraft: Fallout's shadows, per quad (Minecraft sends quads as 6 vertices).
			const std::uint32_t total = static_cast<std::uint32_t>(scratch.size());
			{
				std::vector<float> ones(total, 1.0f);
				D3D11_BUFFER_DESC  sbd{};
				sbd.ByteWidth = total * sizeof(float);
				sbd.Usage = D3D11_USAGE_DEFAULT;
				sbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
				D3D11_SUBRESOURCE_DATA sinit{ ones.data(), 0, 0 };
				device->CreateBuffer(&sbd, &sinit, &s.shadowVb);
			}
			if (s.shadowVb && opaque % 6 == 0 && total % 6 == 0) {
				static constexpr float kMcNormals[7][3] = { { 0, 1, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { -1, 0, 0 }, { 1, 0, 0 } };
				std::vector<float>        points;
				std::vector<std::uint8_t> faces;
				points.reserve(total / 2);
				faces.reserve(total / 6);
				for (std::uint32_t q = 0; q + 5 < total; q += 6) {
					float c[3] = { 0, 0, 0 };
					for (int k = 0; k < 6; ++k) {
						c[0] += scratch[q + k].x, c[1] += scratch[q + k].y, c[2] += scratch[q + k].z;
					}
					const std::uint32_t face = (scratch[q].flags >> 4) & 7;
					const auto&         nrm = kMcNormals[face < 7 ? face : 0];
					points.push_back(float(hdr->sx * 16) + c[0] / 6.0f + nrm[0] * 0.06f);
					points.push_back(float(hdr->sy * 16) + c[1] / 6.0f + nrm[1] * 0.06f);
					points.push_back(float(hdr->sz * 16) + c[2] / 6.0f + nrm[2] * 0.06f);
					faces.push_back(static_cast<std::uint8_t>(face));
				}
				Shadows::Submit(key, hdr->sx, hdr->sy, hdr->sz, std::move(points), std::move(faces));
			}
			sections[key] = s;
		}

		struct DrainContext
		{
			ID3D11DeviceContext* context;
		};

		void OnMessage(void* a_ctx, std::uint32_t a_type, const std::uint8_t* a_data, std::uint32_t a_bytes)
		{
			auto* ctx = static_cast<DrainContext*>(a_ctx);
			switch (a_type) {
			case proto::kRenAtlas:
				OnAtlas(ctx->context, a_data, a_bytes);
				break;
			case proto::kRenAtlasRegion:
				OnAtlasRegion(ctx->context, a_data, a_bytes);
				break;
			case proto::kRenSection:
				OnSection(a_data, a_bytes);
				break;
			case proto::kRenTexture:
				OnTexture(a_data, a_bytes);
				break;
			case proto::kRenAvatar:
				OnMesh(ctx->context, avatar, a_data, a_bytes, false);
				break;
			case proto::kRenScene:
				OnMesh(ctx->context, scene, a_data, a_bytes, true);
				break;
			case proto::kRenClearAll:
				ClearSections();
				BlockCollision::ClearAll();
				break;
			case proto::kRenSolids:
				BlockCollision::OnSolids(a_data, a_bytes);  // FalloutCraft: builds NPCs collide with
				break;
			case proto::kRenDug:
				Dig::OnDug(a_data, a_bytes);  // FalloutCraft: cells dug out of Fallout's world
				break;
			default:
				break;  // entities, avatar, lights, solids: later phases
			}
		}

		// ---- drawing -----------------------------------------------------------------------------

		struct StateBackup
		{
			ID3D11RenderTargetView*   rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView*   dsv = nullptr;
			ID3D11BlendState*         blend = nullptr;
			float                     factor[4]{};
			UINT                      mask = 0;
			ID3D11RasterizerState*    rs = nullptr;
			ID3D11DepthStencilState*  ds = nullptr;
			UINT                      stencil = 0;
			D3D11_VIEWPORT            vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT                      vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			D3D11_PRIMITIVE_TOPOLOGY  topo{};
			ID3D11InputLayout*        il = nullptr;
			ID3D11Buffer*             vb = nullptr;
			UINT                      stride = 0, offset = 0;
			ID3D11Buffer*             vb1 = nullptr;  // stream 1 (our shadow stream)
			UINT                      stride1 = 0, offset1 = 0;
			ID3D11VertexShader*       vsh = nullptr;
			ID3D11PixelShader*        psh = nullptr;
			ID3D11ShaderResourceView* srv[3]{};
			ID3D11SamplerState*       smp = nullptr;
			ID3D11Buffer*             vcb[2]{};
			ID3D11Buffer*             pcb[2]{};

			void Save(ID3D11DeviceContext* c)
			{
				c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
				c->OMGetBlendState(&blend, factor, &mask);
				c->RSGetState(&rs);
				c->OMGetDepthStencilState(&ds, &stencil);
				c->RSGetViewports(&vpCount, vps);
				c->IAGetPrimitiveTopology(&topo);
				c->IAGetInputLayout(&il);
				c->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
				c->IAGetVertexBuffers(1, 1, &vb1, &stride1, &offset1);
				c->VSGetShader(&vsh, nullptr, nullptr);
				c->PSGetShader(&psh, nullptr, nullptr);
				c->PSGetShaderResources(0, 3, srv);
				c->PSGetSamplers(0, 1, &smp);
				c->VSGetConstantBuffers(0, 2, vcb);
				c->PSGetConstantBuffers(0, 2, pcb);
			}

			void Restore(ID3D11DeviceContext* c)
			{
				c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
				c->OMSetBlendState(blend, factor, mask);
				c->RSSetState(rs);
				c->OMSetDepthStencilState(ds, stencil);
				c->RSSetViewports(vpCount, vps);
				c->IASetPrimitiveTopology(topo);
				c->IASetInputLayout(il);
				c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
				c->IASetVertexBuffers(1, 1, &vb1, &stride1, &offset1);
				c->VSSetShader(vsh, nullptr, 0);
				c->PSSetShader(psh, nullptr, 0);
				c->PSSetShaderResources(0, 3, srv);
				c->PSSetSamplers(0, 1, &smp);
				c->VSSetConstantBuffers(0, 2, vcb);
				c->PSSetConstantBuffers(0, 2, pcb);
				for (auto*& r : rtv) {
					Release(r);
				}
				Release(dsv);
				Release(blend);
				Release(rs);
				Release(ds);
				Release(il);
				Release(vb);
				Release(vb1);
				Release(vsh);
				Release(psh);
				Release(srv[0]);
				Release(srv[1]);
				Release(srv[2]);
				Release(smp);
				Release(vcb[0]);
				Release(vcb[1]);
				Release(pcb[0]);
				Release(pcb[1]);
			}
		};

		// Fallout's scene depth. Several depth targets have the back buffer's size, and by Present
		// some may hold something else (or be cleared), so each candidate is read back a few times
		// (9 points) and the one that looks like a scene - depths strictly between near and far - is
		// used. If none does, blocks are drawn without occlusion rather than not at all.
		// SkyCraft.ini [Render] iDepthTarget=N forces one (-1: automatic).
		struct Probe
		{
			ID3D11Texture2D* staging{ nullptr };
			bool             pending{ false };
			int              score{ 0 };   // geometry-like samples, summed over rounds
			int              rounds{ 0 };
		};
		Probe         probes[13];
		int           chosenDepth = -2;  // -2: still probing, -1: none usable
		std::uint64_t lastProbeMs = 0;
		int           probeRounds = 0;

		bool DecodeDepth(DXGI_FORMAT a_format, const std::uint8_t* a_px, float& a_out)
		{
			switch (a_format) {
			case DXGI_FORMAT_R24G8_TYPELESS:
			case DXGI_FORMAT_D24_UNORM_S8_UINT:
			case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
				{
					std::uint32_t v;
					std::memcpy(&v, a_px, 4);
					a_out = float(v & 0xFFFFFF) / 16777215.0f;
					return true;
				}
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT:
			case DXGI_FORMAT_R32_FLOAT:
			case DXGI_FORMAT_R32G8X24_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
				std::memcpy(&a_out, a_px, 4);
				return true;
			default:
				return false;
			}
		}

		std::uint32_t BytesPerPixel(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R32G8X24_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
				return 8;
			default:
				return 4;
			}
		}

		ID3D11ShaderResourceView* FindSceneDepth(ID3D11DeviceContext* a_context, UINT a_w, UINT a_h)
		{
			auto* data = RE::BSGraphics::GetRendererData();
			if (!data) {
				return nullptr;
			}
			auto srvOf = [&](int a_i) { return reinterpret_cast<ID3D11ShaderResourceView*>(data->depthStencilTargets[a_i].srViewDepth); };
			static const int forced = [] {
				wchar_t path[MAX_PATH]{};
				::GetModuleFileNameW(nullptr, path, MAX_PATH);  // ...\Fallout4.exe
				if (auto* slash = std::wcsrchr(path, L'\\')) {
					*(slash + 1) = 0;
				}
				::wcscat_s(path, MAX_PATH, L"Data\\F4SE\\Plugins\\SkyCraft.ini");
				return static_cast<int>(::GetPrivateProfileIntW(L"Render", L"iDepthTarget", -1, path));
			}();
			if (forced >= 0 && forced < 13) {
				if (!loggedDepth) {
					loggedDepth = true;
					REX::INFO("blocks: using Fallout depth target {} (forced in SkyCraft.ini)", forced);
				}
				return srvOf(forced);
			}

			const auto now = ::GetTickCount64();
			const bool deciding = chosenDepth == -2;
			if (now - lastProbeMs >= (deciding ? 700u : 10000u)) {
				lastProbeMs = now;
				std::string line;
				for (int i = 0; i < 13; ++i) {
					auto* tex = reinterpret_cast<ID3D11Texture2D*>(data->depthStencilTargets[i].texture);
					if (!tex || !srvOf(i)) {
						continue;
					}
					D3D11_TEXTURE2D_DESC td{};
					tex->GetDesc(&td);
					if (td.Width != a_w || td.Height != a_h || td.SampleDesc.Count != 1) {
						continue;
					}
					auto& p = probes[i];
					if (p.pending) {
						D3D11_MAPPED_SUBRESOURCE m{};
						if (a_context->Map(p.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) == S_OK) {
							const auto* base = static_cast<const std::uint8_t*>(m.pData);
							const auto  bpp = BytesPerPixel(td.Format);
							int         good = 0;
							line += std::format(" [{}]", i);
							for (int sy = 1; sy <= 3; ++sy) {
								for (int sx = 1; sx <= 3; ++sx) {
									const UINT x = td.Width * sx / 4, y = td.Height * sy / 4;
									float      d = -1.0f;
									if (DecodeDepth(td.Format, base + std::size_t(y) * m.RowPitch + std::size_t(x) * bpp, d)) {
										good += (d > 0.00001f && d < 0.99999f) ? 1 : 0;
										line += std::format(" {:.5f}", d);
									}
								}
							}
							a_context->Unmap(p.staging, 0);
							p.score += good;
							++p.rounds;
							p.pending = false;
						}
					}
					if (!p.staging) {
						D3D11_TEXTURE2D_DESC sd = td;
						sd.Usage = D3D11_USAGE_STAGING;
						sd.BindFlags = 0;
						sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
						sd.MiscFlags = 0;
						sd.MipLevels = 1;
						if (FAILED(device->CreateTexture2D(&sd, nullptr, &p.staging))) {
							p.staging = nullptr;
							continue;
						}
					}
					a_context->CopyResource(p.staging, tex);
					p.pending = true;
				}
				if (!line.empty()) {
					++probeRounds;
					if (deciding || probeRounds % 6 == 0) {
						REX::INFO("blocks: Fallout depth at Present (9 points each):{}", line);
					}
				}
				if (deciding && probeRounds >= 3) {
					int best = -1, bestScore = 0;
					for (int i = 0; i < 13; ++i) {
						if (probes[i].score > bestScore || (probes[i].score == bestScore && bestScore > 0 && i == 2)) {
							bestScore = probes[i].score;
							best = i;
						}
					}
					chosenDepth = best;
					REX::INFO("blocks: {}", best >= 0 ? std::format("Fallout's scene depth is target {}; blocks hide behind Fallout's walls", best) :
					                                    std::string("no Fallout depth target holds the scene at Present; blocks are drawn without hiding behind walls"));
				}
			}
			return chosenDepth >= 0 ? srvOf(chosenDepth) : nullptr;
		}

		float Daylight()
		{
			auto* calendar = RE::Calendar::GetSingleton();
			const float hour = calendar && calendar->gameHour ? calendar->gameHour->GetValue() : 12.0f;
			// Full day 7-17h, night 20-4h, a ramp in between.
			if (hour >= 7.0f && hour <= 17.0f) {
				return 1.0f;
			}
			if (hour >= 20.0f || hour <= 4.0f) {
				return 0.25f;
			}
			const float t = hour < 12.0f ? (hour - 4.0f) / 3.0f : (20.0f - hour) / 3.0f;
			return 0.25f + 0.75f * std::clamp(t, 0.0f, 1.0f);
		}

		// ---- per-frame geometry (ported from the Skyrim plugin's WorldRender.cpp) -----------------
		void Quad(std::vector<proto::RenVertex>& a_out, const float a_p[4][3], const float a_uv[4], std::uint32_t a_color, std::uint32_t a_flags)
		{
			const float uv[4][2] = { { a_uv[0], a_uv[1] }, { a_uv[2], a_uv[1] }, { a_uv[2], a_uv[3] }, { a_uv[0], a_uv[3] } };
			for (int k : { 0, 1, 2, 0, 2, 3 }) {
				a_out.push_back({ a_p[k][0], a_p[k][1], a_p[k][2], uv[k][0], uv[k][1], a_color, kFullSkyLight, a_flags });
			}
		}

		std::uint32_t Tint(std::uint32_t a_tint)
		{
			return a_tint ? (0xFF000000u | (a_tint & 0x00FFFFFFu)) : 0xFFFFFFFFu;
		}

		// An axis-aligned box, turned a_yaw about its vertical centre line; textures per face group.
		void Box(std::vector<proto::RenVertex>& a_out, const float a_min[3], const float a_size[3], float a_yaw, const float a_side[4], const float a_top[4],
			const float a_bottom[4], std::uint32_t a_topTint, std::uint32_t a_flags)
		{
			const float cx = a_min[0] + a_size[0] * 0.5f, cz = a_min[2] + a_size[2] * 0.5f;
			const float c = std::cos(a_yaw), s = std::sin(a_yaw);
			auto        corner = [&](int a_i, float a_out3[3]) {
				const float lx = ((a_i & 1) ? 0.5f : -0.5f) * a_size[0], lz = ((a_i & 4) ? 0.5f : -0.5f) * a_size[2];
				a_out3[0] = cx + lx * c - lz * s;
				a_out3[1] = a_min[1] + ((a_i & 2) ? a_size[1] : 0.0f);
				a_out3[2] = cz + lx * s + lz * c;
			};
			// corner bits: 1 = +x, 2 = +y, 4 = +z; each face TL, TR, BR, BL seen from outside
			static constexpr int           kFaces[6][4] = { { 6, 7, 5, 4 }, { 3, 2, 0, 1 }, { 7, 3, 1, 5 }, { 2, 6, 4, 0 }, { 2, 3, 7, 6 }, { 4, 5, 1, 0 } };
			static constexpr std::uint32_t kFaceFlags[6] = { kFaceSouth, kFaceNorth, kFaceEast, kFaceWest, kFaceUp, kFaceDown };
			for (int f = 0; f < 6; ++f) {
				float p[4][3];
				for (int k = 0; k < 4; ++k) {
					corner(kFaces[f][k], p[k]);
				}
				const float* uv = f == 4 ? a_top : f == 5 ? a_bottom : a_side;
				Quad(a_out, p, uv, f == 4 ? Tint(a_topTint) : 0xFFFFFFFFu, a_flags | kFaceFlags[f]);
			}
		}

		// Minecraft's arrow model, a bit smaller (it's chunky next to Fallout's people), or a trident's icon.
		void Arrow(std::vector<proto::RenVertex>& a_out, float px, float py, float pz, const float d[3], const float* a_uvSide, const float* a_uvBack, bool a_trident)
		{
			float s[3] = { d[2], 0.0f, -d[0] };
			float sl = std::sqrt(s[0] * s[0] + s[2] * s[2]);
			if (sl < 1e-3f) {
				s[0] = 1.0f, s[2] = 0.0f, sl = 1.0f;
			}
			s[0] /= sl, s[2] /= sl;
			const float     u[3] = { s[1] * d[2] - s[2] * d[1], s[2] * d[0] - s[0] * d[2], s[0] * d[1] - s[1] * d[0] };
			constexpr float r = 0.70710678f;
			const float     fins[2][3] = { { (u[0] + s[0]) * r, (u[1] + s[1]) * r, (u[2] + s[2]) * r }, { (u[0] - s[0]) * r, (u[1] - s[1]) * r, (u[2] - s[2]) * r } };
			auto            at = [&](float a_along, const float* a_q, float a_side, const float* a_q2, float a_side2, float a_o[3]) {
				for (int k = 0; k < 3; ++k) {
					a_o[k] = (k == 0 ? px : k == 1 ? py : pz) + d[k] * a_along + a_q[k] * a_side + (a_q2 ? a_q2[k] * a_side2 : 0.0f);
				}
			};
			const std::uint32_t flags = kFlagCutout | kFlagNoMip;
			if (!a_trident) {
				constexpr float k = 0.9f / 16.0f * 0.55f;
				for (const auto& q : fins) {
					float p[4][3];
					at(-12 * k, q, -2 * k, nullptr, 0, p[0]);
					at(4 * k, q, -2 * k, nullptr, 0, p[1]);
					at(4 * k, q, 2 * k, nullptr, 0, p[2]);
					at(-12 * k, q, 2 * k, nullptr, 0, p[3]);
					Quad(a_out, p, a_uvSide, 0xFFFFFFFFu, flags);
				}
				float p[4][3];
				at(-11 * k, fins[0], -2 * k, fins[1], -2 * k, p[0]);
				at(-11 * k, fins[0], 2 * k, fins[1], -2 * k, p[1]);
				at(-11 * k, fins[0], 2 * k, fins[1], 2 * k, p[2]);
				at(-11 * k, fins[0], -2 * k, fins[1], 2 * k, p[3]);
				Quad(a_out, p, a_uvBack, 0xFFFFFFFFu, flags);
			} else {
				constexpr float h = 0.9f;
				for (const auto& q : fins) {
					float p[4][3];
					at(0, q, h, nullptr, 0, p[0]);
					at(h, q, 0, nullptr, 0, p[1]);
					at(0, q, -h, nullptr, 0, p[2]);
					at(-h, q, 0, nullptr, 0, p[3]);
					Quad(a_out, p, a_uvSide, 0xFFFFFFFFu, flags);
				}
			}
		}

		// Builds this frame's dynamic vertices relative to a_o (Minecraft coords): solid things first,
		// then the cracks (blended over the blocks), then the outline (lines).
		bool BuildDynamic(const double a_o[3], UINT& a_solid, UINT& a_cracks, UINT& a_lines)
		{
			dynVerts.clear();
			static std::vector<proto::RenVertex> cracks;
			cracks.clear();
			for (std::uint32_t i = 0; i < entities.count; ++i) {
				const auto& e = entities.entities[i];
				const float px = float(e.x - a_o[0]), py = float(e.y - a_o[1]), pz = float(e.z - a_o[2]);
				switch (e.kind) {
				case proto::kWeBlock:
					{
						// A dropped block: a small cube spinning about its centre.
						const float sz = e.scale;
						const float mn[3] = { px - sz * 0.5f, py - sz * 0.5f, pz - sz * 0.5f };
						const float size[3] = { sz, sz, sz };
						Box(dynVerts, mn, size, e.yaw * kPi / 180.0f, e.uv[0], e.uv[1], e.uv[2], e.tint, kFlagCutout | kFlagNoMip);
						break;
					}
				case proto::kWeCrack:
					{
						// Breaking cracks over the block's box, a hair outside it so they win the depth test.
						constexpr float g = 0.002f;
						const float     mn[3] = { px - g, py - g, pz - g };
						const float     size[3] = { e.ext[0] + 2 * g, e.ext[1] + 2 * g, e.ext[2] + 2 * g };
						Box(cracks, mn, size, 0.0f, e.uv[0], e.uv[0], e.uv[0], 0, kFlagTranslucent | kFlagNoMip);
						break;
					}
				case proto::kWeArrow:
				case proto::kWeTrident:
					{
						const float yaw = e.yaw * kPi / 180.0f, pitch = e.pitch * kPi / 180.0f;
						const float d[3] = { std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
						Arrow(dynVerts, px, py, pz, d, e.uv[0], e.uv[1], e.kind == proto::kWeTrident);
						break;
					}
				case proto::kWeItem:
					{
						// A dropped item: a flat sprite turning about the vertical, seen from both sides.
						const float spin = e.yaw * kPi / 180.0f, half = e.scale * 0.5f;
						const float rx = std::cos(spin) * half, rz = std::sin(spin) * half;
						const float p[4][3] = { { px - rx, py + half, pz - rz }, { px + rx, py + half, pz + rz }, { px + rx, py - half, pz + rz }, { px - rx, py - half, pz - rz } };
						Quad(dynVerts, p, e.uv[0], 0xFFFFFFFFu, kFlagCutout | kFlagNoMip);
						break;
					}
				default:
					break;  // shadows: not geometry
				}
			}
			a_solid = static_cast<UINT>(dynVerts.size());
			dynVerts.insert(dynVerts.end(), cracks.begin(), cracks.end());
			a_cracks = static_cast<UINT>(cracks.size());
			if (entities.hasSelection) {
				// Minecraft's outline around the targeted block: black, 40%.
				constexpr float g = 0.002f;
				const float     lo[3] = { float(entities.selMin[0] - a_o[0]) - g, float(entities.selMin[1] - a_o[1]) - g, float(entities.selMin[2] - a_o[2]) - g };
				const float     hi[3] = { float(entities.selMax[0] - a_o[0]) + g, float(entities.selMax[1] - a_o[1]) + g, float(entities.selMax[2] - a_o[2]) + g };
				static constexpr int kEdges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
				for (const auto& edge : kEdges) {
					for (int k : edge) {
						dynVerts.push_back({ (k & 1) ? hi[0] : lo[0], (k & 2) ? hi[1] : lo[1], (k & 4) ? hi[2] : lo[2], 0, 0, 0x66000000u, kFullSkyLight, kFlagUntextured });
					}
				}
			}
			a_lines = static_cast<UINT>(dynVerts.size()) - a_solid - a_cracks;
			return !dynVerts.empty();
		}

		bool UploadDynamic(ID3D11DeviceContext* a_context)
		{
			const UINT bytes = static_cast<UINT>(dynVerts.size() * sizeof(proto::RenVertex));
			if (bytes > dynCapacity) {
				Release(dynVb);
				D3D11_BUFFER_DESC bd{};
				bd.ByteWidth = std::max<UINT>(bytes * 2, 64 * 1024);
				bd.Usage = D3D11_USAGE_DYNAMIC;
				bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
				bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				if (FAILED(device->CreateBuffer(&bd, nullptr, &dynVb))) {
					dynCapacity = 0;
					return false;
				}
				dynCapacity = bd.ByteWidth;
			}
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(a_context->Map(dynVb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				return false;
			}
			std::memcpy(mapped.pData, dynVerts.data(), bytes);
			a_context->Unmap(dynVb, 0);
			return true;
		}

		// a_target: draw into this texture (Fallout's HDR scene, fo_inject.cpp) instead of the back
		// buffer. Returns whether anything was drawn.
		bool Render(ID3D11DeviceContext* a_context, IDXGISwapChain* a_swapChain, ID3D11Texture2D* a_target = nullptr, const GBufferTarget* a_gb = nullptr)
		{
			auto* camera = RE::Main::WorldRootCamera();
			const bool haveEntities = link::ReadWorldEntities(entities) && (entities.count > 0 || entities.hasSelection);
			if (!camera || !atlasSrv || (sections.empty() && !haveEntities && scene.batches.empty() && avatar.batches.empty())) {
				return false;
			}
			const bool       hdrTarget = a_target != nullptr;
			ID3D11Texture2D* backBuffer = a_target;
			D3D11_TEXTURE2D_DESC bb{};
			ID3D11RenderTargetView* rtv = nullptr;
			if (a_gb) {
				// Into Fallout's G-buffer: its own targets and depth (fo_inject.cpp).
				if (!psGBuffer || a_gb->count == 0 || !a_gb->rtvs[0] || !a_gb->dsv) {
					return false;
				}
				ID3D11Resource* r = nullptr;
				a_gb->rtvs[0]->GetResource(&r);
				static_cast<ID3D11Texture2D*>(r)->GetDesc(&bb);
				r->Release();
			}
			if (!a_gb && !backBuffer) {
				if (!a_swapChain || FAILED(a_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
					return false;
				}
			}
			if (!a_gb) {
			backBuffer->GetDesc(&bb);
			D3D11_RENDER_TARGET_VIEW_DESC rvd{};
			rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			rvd.Format = bb.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS ? DXGI_FORMAT_R16G16B16A16_FLOAT
			           : bb.Format == DXGI_FORMAT_R32G32B32A32_TYPELESS ? DXGI_FORMAT_R32G32B32A32_FLOAT : bb.Format;
			const auto hr = device->CreateRenderTargetView(backBuffer, hdrTarget ? &rvd : nullptr, &rtv);
			if (!hdrTarget) {
				Release(backBuffer);
			}
			if (FAILED(hr) || !EnsureOwnDepth(bb.Width, bb.Height)) {
				Release(rtv);
				return false;
			}
			}

			// Fallout's world->clip matrix, re-based on the camera so the GPU only sees small numbers.
			const auto&    w2c = camera->worldToCam;
			const auto     cam = camera->world.translate;
			FrameConstants fc{};
			for (int r = 0; r < 4; ++r) {
				for (int c = 0; c < 3; ++c) {
					fc.viewProj[r][c] = w2c[r][c];
				}
				fc.viewProj[r][3] = float(double(w2c[r][3]) + double(w2c[r][0]) * cam.x + double(w2c[r][1]) * cam.y + double(w2c[r][2]) * cam.z);
			}
			// Depth convention, from two points straight ahead: depth = A + B / w.
			const float fwdLen = std::sqrt(w2c[3][0] * w2c[3][0] + w2c[3][1] * w2c[3][1] + w2c[3][2] * w2c[3][2]);
			bool        reversed = false;
			bool        occlusion = false;
			if (fwdLen > 1e-6f) {
				auto project = [&](float a_dist, float& a_depth, float& a_w) {
					const float p[3] = { w2c[3][0] / fwdLen * a_dist, w2c[3][1] / fwdLen * a_dist, w2c[3][2] / fwdLen * a_dist };
					const float z = fc.viewProj[2][0] * p[0] + fc.viewProj[2][1] * p[1] + fc.viewProj[2][2] * p[2] + fc.viewProj[2][3];
					a_w = fc.viewProj[3][0] * p[0] + fc.viewProj[3][1] * p[1] + fc.viewProj[3][2] * p[2] + fc.viewProj[3][3];
					a_depth = a_w != 0.0f ? z / a_w : 0.0f;
				};
				float d1, w1, d2, w2;
				project(200.0f, d1, w1);
				project(20000.0f, d2, w2);
				if (w1 > 0.0f && w2 > w1 && std::fabs(1.0f / w1 - 1.0f / w2) > 1e-12f) {
					const float B = (d1 - d2) / (1.0f / w1 - 1.0f / w2);
					const float A = d1 - B / w1;
					fc.depthAB[0] = A;
					fc.depthAB[1] = B;
					reversed = d2 < d1;
					occlusion = true;
				}
			}
			ID3D11ShaderResourceView* sceneDepth = occlusion ? FindSceneDepth(a_context, bb.Width, bb.Height) : nullptr;
			static bool loggedCamera = false;
			if (!loggedCamera) {
				loggedCamera = true;
				REX::INFO("blocks: camera world->clip rows ({:.4f} {:.4f} {:.4f} {:.1f}) ({:.4f} {:.4f} {:.4f} {:.1f}) ({:.4f} {:.4f} {:.4f} {:.1f}) ({:.4f} {:.4f} {:.4f} {:.1f}); depth = {:.6f} + {:.3f}/w, {} depth, scene depth {}",
					w2c[0][0], w2c[0][1], w2c[0][2], w2c[0][3], w2c[1][0], w2c[1][1], w2c[1][2], w2c[1][3], w2c[2][0], w2c[2][1], w2c[2][2], w2c[2][3],
					w2c[3][0], w2c[3][1], w2c[3][2], w2c[3][3], fc.depthAB[0], fc.depthAB[1], reversed ? "reversed" : "normal", sceneDepth ? "found" : "NOT found (blocks drawn over everything)");
			}
			fc.depthAB[2] = sceneDepth ? 1.0f : 0.0f;
			fc.lighting[0] = Daylight();
			fc.lighting[1] = 0.12f;
			fc.lighting[2] = 1.0f;
			fc.hdr[0] = hdrTarget ? 1.0f : 0.0f;
			fc.hdr[1] = hdrExposure;
			{
				// The camera in Minecraft blocks; the 64x32x64 volume follows it in 16-block steps.
				const double cx = cam.x / proto::kUnitsPerBlock + g_worldOffset.x.load(std::memory_order_relaxed);
				const double cy = cam.z / proto::kUnitsPerBlock;
				const double cz = -cam.y / proto::kUnitsPerBlock + g_worldOffset.z.load(std::memory_order_relaxed);
				const std::int32_t ox = std::int32_t(std::floor(cx / 16.0)) * 16 - 32, oy = std::int32_t(std::floor(cy / 16.0)) * 16 - 16,
								   oz = std::int32_t(std::floor(cz / 16.0)) * 16 - 32;
				const auto version = Dig::Version();
				if (version != dugVersion || ox != dugOrigin[0] || oy != dugOrigin[1] || oz != dugOrigin[2]) {
					dugVersion = version;
					dugOrigin[0] = ox, dugOrigin[1] = oy, dugOrigin[2] = oz;
					dugScratch.assign(64 * 32 * 64, 0);
					dugAny = Dig::FillVolume(Dig::CurrentWorld(), ox, oy, oz, 64, 32, 64, dugScratch.data());
					if (!dugTex) {
						D3D11_TEXTURE3D_DESC td{};
						td.Width = 64, td.Height = 32, td.Depth = 64, td.MipLevels = 1;
						td.Format = DXGI_FORMAT_R8_UINT;
						td.Usage = D3D11_USAGE_DEFAULT;
						td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
						if (SUCCEEDED(device->CreateTexture3D(&td, nullptr, &dugTex))) {
							device->CreateShaderResourceView(dugTex, nullptr, &dugSrv);
						}
					}
					if (dugTex) {
						a_context->UpdateSubresource(dugTex, 0, nullptr, dugScratch.data(), 64, 64 * 32);
					}
				}
				fc.camDug[0] = float(cx - ox), fc.camDug[1] = float(cy - oy), fc.camDug[2] = float(cz - oz);
				fc.camDug[3] = (dugAny && dugSrv) ? 1.0f : 0.0f;
			}
			{
				// FalloutCraft: Fallout's light now (fo_shadow.cpp; outdoors only).
				const auto light = Shadows::Current();
				if (light.valid) {
					for (int k = 0; k < 3; ++k) {
						fc.sunDir[k] = light.toSun[k];
						fc.sunColor[k] = light.sunColor[k];
						fc.fogNear[k] = light.fogNear[k];
						fc.fogFar[k] = light.fogFar[k];
						for (int a = 0; a < 6; ++a) {
							fc.ambient[a][k] = light.ambient[a][k];
						}
					}
					for (int k = 0; k < 4; ++k) {
						fc.fogParams[k] = light.fog[k];
					}
					fc.sunDir[3] = 1.0f;
					fc.lighting[2] = 0.9f;  // Fallout's light is a bit stronger than Minecraft's lightmap
				}
				// New shadow results for the sections.
				for (auto& [key, sec] : sections) {
					if (!sec.shadowVb || !Shadows::Fetch(key, sec.shadowVersion, shadowLit)) {
						continue;
					}
					const std::size_t total = std::size_t(sec.opaque) + sec.translucent;
					if (shadowLit.size() * 6 != total) {
						continue;
					}
					shadowScratch.resize(total);
					for (std::size_t q = 0; q < shadowLit.size(); ++q) {
						const float v = shadowLit[q] / 255.0f;
						for (int k = 0; k < 6; ++k) {
							shadowScratch[q * 6 + k] = v;
						}
					}
					a_context->UpdateSubresource(sec.shadowVb, 0, nullptr, shadowScratch.data(), 0, 0);
				}
				if (!onesVb) {
					std::vector<float> ones(1u << 20, 1.0f);
					D3D11_BUFFER_DESC  obd{};
					obd.ByteWidth = UINT(ones.size() * sizeof(float));
					obd.Usage = D3D11_USAGE_IMMUTABLE;
					obd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
					D3D11_SUBRESOURCE_DATA oinit{ ones.data(), 0, 0 };
					device->CreateBuffer(&obd, &oinit, &onesVb);
				}
			}

			if (a_gb) {
				// Fallout's depth buffer does the hiding; no holes trick (the land is in that depth).
				fc.depthAB[2] = 0.0f;
				fc.camDug[3] = 0.0f;
				auto axis = [&](int a_row, float* a_out) {
					const float l = std::sqrt(w2c[a_row][0] * w2c[a_row][0] + w2c[a_row][1] * w2c[a_row][1] + w2c[a_row][2] * w2c[a_row][2]);
					for (int k = 0; k < 3; ++k) {
						a_out[k] = l > 1e-6f ? w2c[a_row][k] / l : 0.0f;
					}
				};
				axis(0, fc.camRight);
				axis(1, fc.camUp);
				axis(3, fc.camFwd);
				fc.gbEnc[0] = float(a_gb->encoding);
				fc.gbEnc[1] = a_gb->albedoAlpha;
				std::memcpy(fc.gbT, a_gb->templ, sizeof(fc.gbT));
			}
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(a_context->Map(frameCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				Release(rtv);
				return false;
			}
			std::memcpy(mapped.pData, &fc, sizeof(fc));
			a_context->Unmap(frameCb, 0);

			StateBackup backup;
			backup.Save(a_context);

			const D3D11_VIEWPORT vp{ 0, 0, float(bb.Width), float(bb.Height), 0, 1 };
			const float          factor[4]{};
			if (a_gb) {
				a_context->OMSetRenderTargets(a_gb->count, a_gb->rtvs, a_gb->dsv);
			} else {
				a_context->ClearDepthStencilView(ownDsv, D3D11_CLEAR_DEPTH, reversed ? 0.0f : 1.0f, 0);
				a_context->OMSetRenderTargets(1, &rtv, ownDsv);
			}
			a_context->RSSetViewports(1, &vp);
			a_context->RSSetState(raster);
			a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			a_context->IASetInputLayout(layout);
			a_context->VSSetShader(vs, nullptr, 0);
			ID3D11Buffer* cbs[2] = { frameCb, objectCb };
			a_context->VSSetConstantBuffers(0, 2, cbs);
			a_context->PSSetConstantBuffers(0, 2, cbs);
			ID3D11ShaderResourceView* srvs[3] = { atlasSrv, sceneDepth, dugSrv };
			a_context->PSSetShaderResources(0, 3, srvs);
			a_context->PSSetSamplers(0, 1, &sampler);

			// A Minecraft point (blocks, in this world's place in Minecraft) relative to the camera,
			// in Fallout units (double precision here, float once small).
			const double offX = g_worldOffset.x.load(std::memory_order_relaxed), offZ = g_worldOffset.z.load(std::memory_order_relaxed);
			auto RelativeToCamera = [&](double a_x, double a_y, double a_z, float* a_out) {
				a_out[0] = float((a_x - offX) * proto::kUnitsPerBlock - double(cam.x));
				a_out[1] = float(-(a_z - offZ) * proto::kUnitsPerBlock - double(cam.y));
				a_out[2] = float(a_y * proto::kUnitsPerBlock - double(cam.z));
			};
			auto drawPass = [&](bool a_translucent) {
				if (!a_translucent && !a_gb && skipOpaqueSections) {
					return;  // they're in Fallout's world this frame (its G-buffer)
				}
				a_context->OMSetBlendState(a_translucent ? alphaBlend : opaqueBlend, factor, 0xFFFFFFFF);
				if (a_gb) {
					a_context->OMSetDepthStencilState(depthGBuffer[reversed], a_gb->stencil);
					a_context->PSSetShader(psGBuffer, nullptr, 0);
				} else {
					a_context->OMSetDepthStencilState(a_translucent ? depthTest[reversed] : depthWrite[reversed], 0);
					a_context->PSSetShader(a_translucent ? psTranslucent : psOpaque, nullptr, 0);
				}
				for (const auto& [key, s] : sections) {
					const std::uint32_t count = a_translucent ? s.translucent : s.opaque;
					if (!count || !s.vb) {
						continue;
					}
					// Section corner in Fallout units, relative to the camera (double precision here).
					ObjectConstants oc{};
					RelativeToCamera(double(s.sx) * 16.0, double(s.sy) * 16.0, double(s.sz) * 16.0, oc.offset);
					D3D11_MAPPED_SUBRESOURCE om{};
					if (FAILED(a_context->Map(objectCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &om))) {
						continue;
					}
					std::memcpy(om.pData, &oc, sizeof(oc));
					a_context->Unmap(objectCb, 0);
					ID3D11Buffer* vbs[2] = { s.vb, s.shadowVb ? s.shadowVb : onesVb };
					const UINT    strides[2] = { sizeof(proto::RenVertex), sizeof(float) }, offsets[2] = { 0, 0 };
					a_context->IASetVertexBuffers(0, 2, vbs, strides, offsets);
					a_context->Draw(count, a_translucent ? s.opaque : 0);
				}
			};
			static int sectionLogs = 0;
			if (sectionLogs < 3 && !sections.empty()) {
				++sectionLogs;
				const auto& s0 = sections.begin()->second;
				const double gx = double(s0.sx) * 16.0 * proto::kUnitsPerBlock, gy = -double(s0.sz) * 16.0 * proto::kUnitsPerBlock, gz = double(s0.sy) * 16.0 * proto::kUnitsPerBlock;
				REX::INFO("blocks: drawing {} sections; e.g. section ({}, {}, {}) at Fallout ({:.0f}, {:.0f}, {:.0f}), camera ({:.0f}, {:.0f}, {:.0f}), {} + {} vertices",
					sections.size(), s0.sx, s0.sy, s0.sz, gx, gy, gz, cam.x, cam.y, cam.z, s0.opaque, s0.translucent);
			}
			drawPass(false);
			if (a_gb) {
				backup.Restore(a_context);
				return true;  // only the solid blocks go into the G-buffer; the rest at Present
			}
			drawPass(true);

			// Minecraft's entities and particles, and the player's own body in third person (F5).
			auto drawMesh = [&](const Mesh& a_mesh, const double a_mcOrigin[3], bool a_blended) {
				if (a_mesh.batches.empty() || !a_mesh.vb) {
					return;
				}
				ObjectConstants oc{};
				RelativeToCamera(a_mcOrigin[0], a_mcOrigin[1], a_mcOrigin[2], oc.offset);
				D3D11_MAPPED_SUBRESOURCE om{};
				if (FAILED(a_context->Map(objectCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &om))) {
					return;
				}
				std::memcpy(om.pData, &oc, sizeof(oc));
				a_context->Unmap(objectCb, 0);
				ID3D11Buffer* vbs[2] = { a_mesh.vb, onesVb };
				const UINT    strides[2] = { sizeof(proto::RenVertex), sizeof(float) }, offsets[2] = { 0, 0 };
				a_context->IASetVertexBuffers(0, 2, vbs, strides, offsets);
				a_context->OMSetBlendState(a_blended ? alphaBlend : opaqueBlend, factor, 0xFFFFFFFF);
				a_context->OMSetDepthStencilState(a_blended ? depthTest[reversed] : depthWrite[reversed], 0);
				a_context->PSSetShader(a_blended ? psTranslucent : psOpaque, nullptr, 0);
				for (const auto& b : a_mesh.batches) {
					if (((b.flags & 1) != 0) != a_blended) {
						continue;
					}
					ID3D11ShaderResourceView* srv = atlasSrv;
					if (b.texture != 0) {
						const auto it = entityTextures.find(b.texture);
						if (it == entityTextures.end() || !it->second.srv) {
							continue;
						}
						srv = it->second.srv;
					}
					a_context->PSSetShaderResources(0, 1, &srv);
					a_context->Draw(b.count, b.first);
				}
				a_context->PSSetShaderResources(0, 1, &atlasSrv);
			};
			const auto& st = State();
			const bool  body = st.feetValid && st.cameraMode != 0;
			const double feet[3] = { st.feetX, st.feetY, st.feetZ };
			for (int pass = 0; pass < 2; ++pass) {
				drawMesh(scene, scene.origin, pass == 1);
				if (body) {
					drawMesh(avatar, feet, pass == 1);
				}
			}

			// Dropped items, arrows, cracks and the outline, relative to the Minecraft block under the camera.
			if (haveEntities) {
				const double mcCam[3] = { cam.x / proto::kUnitsPerBlock + offX, cam.z / proto::kUnitsPerBlock, -cam.y / proto::kUnitsPerBlock + offZ };
				const double origin[3] = { std::floor(mcCam[0]), std::floor(mcCam[1]), std::floor(mcCam[2]) };
				UINT solidCount = 0, crackCount = 0, lineCount = 0;
				if (BuildDynamic(origin, solidCount, crackCount, lineCount) && UploadDynamic(a_context)) {
					ObjectConstants oc{};
					RelativeToCamera(origin[0], origin[1], origin[2], oc.offset);
					D3D11_MAPPED_SUBRESOURCE om{};
					if (SUCCEEDED(a_context->Map(objectCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &om))) {
						std::memcpy(om.pData, &oc, sizeof(oc));
						a_context->Unmap(objectCb, 0);
						ID3D11Buffer* vbs[2] = { dynVb, onesVb };
						const UINT    strides[2] = { sizeof(proto::RenVertex), sizeof(float) }, offsets[2] = { 0, 0 };
						a_context->IASetVertexBuffers(0, 2, vbs, strides, offsets);
						if (solidCount) {
							a_context->OMSetBlendState(opaqueBlend, factor, 0xFFFFFFFF);
							a_context->OMSetDepthStencilState(depthWrite[reversed], 0);
							a_context->PSSetShader(psOpaque, nullptr, 0);
							a_context->Draw(solidCount, 0);
						}
						if (crackCount) {
							a_context->OMSetBlendState(alphaBlend, factor, 0xFFFFFFFF);
							a_context->OMSetDepthStencilState(depthTest[reversed], 0);
							a_context->PSSetShader(psTranslucent, nullptr, 0);
							a_context->Draw(crackCount, solidCount);
						}
						if (lineCount) {
							a_context->OMSetBlendState(alphaBlend, factor, 0xFFFFFFFF);
							a_context->OMSetDepthStencilState(depthTest[reversed], 0);
							a_context->PSSetShader(psTranslucent, nullptr, 0);
							a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
							a_context->Draw(lineCount, solidCount + crackCount);
							a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
						}
					}
				}
			}

			backup.Restore(a_context);
			Release(rtv);
			return true;
		}
	}

	void Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, IDXGISwapChain* a_swapChain)
	{
		if (!link::IsOpen() || !Init(a_device)) {
			return;
		}
		{
			std::scoped_lock lock(ringMutex);
			if (resendNeeded.exchange(false)) {
				// Anything Minecraft sent before now was thrown away: have it send everything again.
				ClearSections();
				link::RequestResend();
				REX::INFO("blocks: asked Minecraft to resend its atlas and blocks");
			}
			DrainContext ctx{ a_context };
			link::DrainRender(&OnMessage, &ctx, 48ull << 20);
			lastDrainMs = ::GetTickCount64();
		}
		if (std::exchange(atlasMipsStale, false) && atlasSrv) {
			a_context->GenerateMips(atlasSrv);
		}

		const auto now = ::GetTickCount64();
		if (now - statsAt > 10000) {
			statsAt = now;
			if (receivedSections) {
				REX::INFO("blocks: {} section updates received, {} sections with blocks", receivedSections, sections.size());
				receivedSections = 0;
			}
		}

		Inject::Install(a_context);
		const bool injected = Inject::InjectedThisFrame();
		skipOpaqueSections = Inject::GBufferThisFrame();
		DXGI_SWAP_CHAIN_DESC scd{};
		a_swapChain->GetDesc(&scd);
		auto& st = State();
		if (link::MinecraftAlive() && st.mcInWorld && !st.falloutMenuOpen && !injected) {
			Render(a_context, a_swapChain);  // not drawn inside Fallout's frame: over it, as before
		}
		Inject::EndFrame(scd.BufferDesc.Width, scd.BufferDesc.Height);
	}

	bool DrawGBuffer(ID3D11DeviceContext* a_context, const GBufferTarget& a_target)
	{
		auto& st = State();
		if (!device || !link::IsOpen() || !link::MinecraftAlive() || !st.mcInWorld || st.falloutMenuOpen) {
			return false;
		}
		return Render(a_context, nullptr, nullptr, &a_target);
	}

	bool DrawInto(ID3D11DeviceContext* a_context, ID3D11Texture2D* a_target)
	{
		auto& st = State();
		if (!device || !link::IsOpen() || !link::MinecraftAlive() || !st.mcInWorld || st.falloutMenuOpen) {
			return false;
		}
		static bool loaded = false;
		if (!loaded) {
			loaded = true;
			wchar_t path[MAX_PATH]{};
			::GetModuleFileNameW(nullptr, path, MAX_PATH);
			if (auto* slash = std::wcsrchr(path, L'\\')) {
				*(slash + 1) = 0;
			}
			::wcscat_s(path, MAX_PATH, L"Data\\F4SE\\Plugins\\SkyCraft.ini");
			wchar_t buf[32]{};
			::GetPrivateProfileStringW(L"Render", L"fHDRExposure", L"1.0", buf, 32, path);
			hdrExposure = std::clamp(static_cast<float>(_wtof(buf)), 0.05f, 20.0f);
			REX::INFO("blocks: drawing inside Fallout's HDR frame (exposure {:.2f})", hdrExposure);
		}
		return Render(a_context, nullptr, a_target);
	}

	void DiscardIfStale()
	{
		const auto last = lastDrainMs.load();
		if (last != 0 && ::GetTickCount64() - last < 2000) {
			return;  // Present drains it
		}
		std::unique_lock lock(ringMutex, std::try_to_lock);
		if (!lock.owns_lock()) {
			return;
		}
		if (link::DrainRender(nullptr, nullptr, ~0ull) > 0) {
			resendNeeded = true;
		}
	}
}
