#pragma once
// Minecraft's blocks drawn in Fallout's frame. Include after the Direct3D headers.

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11Texture2D;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;

namespace skycraft::Blocks
{
	// Present (render thread), before Minecraft's overlay: takes Minecraft's new block meshes and
	// texture atlas from the render ring and draws every block, hidden behind Fallout's geometry.
	void Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, IDXGISwapChain* a_swapChain);

	// Heartbeat thread: while Fallout isn't presenting frames (or before the renderer is up), throw
	// away what Minecraft queues so it never waits for room; Minecraft is then asked to resend
	// everything once drawing resumes.
	void DiscardIfStale();

	// FalloutCraft (fo_inject.cpp): draw the blocks into Fallout's HDR scene target, mid-frame,
	// before its post-processing. False if nothing was drawn (Present then draws them as before).
	bool DrawInto(ID3D11DeviceContext* a_context, ID3D11Texture2D* a_target);

	// FalloutCraft (fo_inject.cpp): the solid blocks as Fallout geometry, written into Fallout's
	// G-buffer (its targets and depth) at the end of its geometry pass, so its own lighting and
	// shadows shade them. Translucent blocks and entities are still drawn at Present.
	struct GBufferTarget
	{
		ID3D11RenderTargetView* rtvs[8]{};
		unsigned                count{ 0 };
		ID3D11DepthStencilView* dsv{ nullptr };
		unsigned                stencil{ 0 };      // the stencil value Fallout's ground has
		int                     encoding{ 0 };     // how Fallout encodes normals (see PSGBuffer)
		float                   albedoAlpha{ 1 };  // what Fallout writes in the albedo target's alpha
		float                   templ[4][4]{};     // RT2..RT5 as a real Fallout surface wrote them
	};
	bool DrawGBuffer(ID3D11DeviceContext* a_context, const GBufferTarget& a_target);
}
