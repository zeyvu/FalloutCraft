#pragma once
// Minecraft's blocks drawn in Fallout's frame. Include after the Direct3D headers.

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;

namespace skycraft::Blocks
{
	// Present (render thread), before Minecraft's overlay: takes Minecraft's new block meshes and
	// texture atlas from the render ring and draws every block, hidden behind Fallout's geometry.
	void Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, IDXGISwapChain* a_swapChain);

	// Heartbeat thread: while Fallout isn't presenting frames (or before the renderer is up), throw
	// away what Minecraft queues so it never waits for room; Minecraft is then asked to resend
	// everything once drawing resumes.
	void DiscardIfStale();
}
