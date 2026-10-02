// FalloutCraft F4SE plugin entry point.
//
// Phase 0 (done, see the 2026-10-01 log): shared memory link, Minecraft walks on a synthetic floor
// from our input ring, and Actor::SetPosition moves Fallout's player.
// Phase 1: Minecraft drives Fallout's player every frame (fo_game.cpp) on Fallout's
// real Havok collision (fo_collision.cpp), with the keyboard and mouse routed to Minecraft
// (fo_input.cpp).
// Phase 2a (this build): Minecraft's hand, hotbar, HUD and screens drawn over Fallout (fo_overlay.cpp),
// Fallout's first-person arms hidden, keys read as Fallout's virtual-key codes.

#include "fo_common.h"
#include "fo_blocks.h"

#include <chrono>
#include <thread>

namespace
{
	// Keeps our heartbeat fresh while Fallout's frames stop (Pip-Boy, pause menu, loading), so
	// Minecraft doesn't take a paused Fallout for a crashed one.
	void HeartbeatLoop()
	{
		using namespace std::chrono_literals;
		for (;;) {
			std::this_thread::sleep_for(20ms);
			skycraft::link::Heartbeat();
			// Present draws Minecraft's blocks from the render ring; while it doesn't run, throw the
			// messages away so Minecraft never waits for room (that stalls its frames).
			skycraft::Blocks::DiscardIfStale();
		}
	}

	void OnF4SEMessage(F4SE::MessagingInterface::Message* a_message)
	{
		switch (a_message->type) {
		case F4SE::MessagingInterface::kGameDataReady:
			skycraft::Overlay::Install();
			break;
		case F4SE::MessagingInterface::kPostLoadGame:
		case F4SE::MessagingInterface::kNewGame:
			REX::INFO("SkyCraft: a game was loaded; Minecraft will be moved to the player");
			skycraft::Game::OnGameLoaded();
			break;
		default:
			break;
		}
	}
}

F4SE_PLUGIN_PRELOAD(const F4SE::PreLoadInterface* a_f4se)
{
	F4SE::Init(a_f4se);
	return true;
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	F4SE::InitInfo info{};
	info.trampoline = true;  // the third-person camera redirects calls (fo_camera.cpp)
	info.trampolineSize = 1024;
	F4SE::Init(a_f4se, info);

	REX::INFO("SkyCraft (FalloutCraft) plugin build {} {}", __DATE__, __TIME__);
	skycraft::Crash::Install();

	if (!skycraft::link::Open()) {
		REX::WARN("SkyCraft: could not create the shared memory mapping");
		return false;
	}
	REX::INFO("SkyCraft: shared memory 'Local\\SkyCraft_v1' is open");

	if (auto* messaging = F4SE::GetMessagingInterface()) {
		messaging->RegisterListener(OnF4SEMessage);
	}

	skycraft::Game::Install();
	skycraft::Input::Install();
	skycraft::Camera::Install();
	skycraft::Combat::Install();
	skycraft::Scavenge::Install();

	std::thread(HeartbeatLoop).detach();
	return true;
}
