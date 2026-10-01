#pragma once
// FalloutCraft (Phase 1): state shared by the per-frame update, the input hooks and collision.
// Ported from the Skyrim plugin (skse/src/Game.h); only what Phase 1 needs.

#include "skycraft_link.h"

#include <atomic>
#include <cmath>

namespace skycraft
{
	// ---- coordinate conversion (Fallout units <-> Minecraft blocks) ---------------------------
	// Same engine conventions as Skyrim: Z up, +Y north, 70 units per block.
	struct McVec
	{
		double x, y, z;
	};

	// Where Fallout's current world sits in Minecraft's one world (blocks, x/z). The Commonwealth
	// is at 0; every interior and every other worldspace gets its own place far from it
	// (fo_worlds.cpp), so what is built in one never shows up in another.
	struct WorldOffset
	{
		std::atomic<double> x{ 0.0 };
		std::atomic<double> z{ 0.0 };
	};
	inline WorldOffset g_worldOffset;

	inline McVec GameToMc(const RE::NiPoint3& a_p)
	{
		return { a_p.x / proto::kUnitsPerBlock + g_worldOffset.x.load(std::memory_order_relaxed), a_p.z / proto::kUnitsPerBlock,
			-a_p.y / proto::kUnitsPerBlock + g_worldOffset.z.load(std::memory_order_relaxed) };
	}

	inline RE::NiPoint3 McToGame(double a_x, double a_y, double a_z)
	{
		const double ox = g_worldOffset.x.load(std::memory_order_relaxed), oz = g_worldOffset.z.load(std::memory_order_relaxed);
		return { float((a_x - ox) * proto::kUnitsPerBlock), float(-(a_z - oz) * proto::kUnitsPerBlock), float(a_y * proto::kUnitsPerBlock) };
	}

	// Heading h (radians, 0 = north, clockwise) <-> MC yaw (degrees, 0 = south). The Phase 0 log
	// confirmed this for Fallout: heading 0.564 rad -> yaw -147.7.
	inline float HeadingToMcYaw(float a_heading) { return a_heading * 57.2957795f + 180.0f; }
	inline float McYawToHeading(float a_yaw) { return (a_yaw - 180.0f) * 0.0174532925f; }

	struct Runtime
	{
		// Minecraft is connected, in its world, acknowledged our last teleport, nothing is loading:
		// Minecraft's player position drives Fallout's player.
		std::atomic<bool> puppeting{ false };
		// The same, or waiting for Minecraft to arrive after a teleport: Fallout's own controls
		// don't move its player either way.
		std::atomic<bool> minecraftOwnsPlayer{ false };
		// A Minecraft GUI screen (inventory, chat, ...) is open: the mouse moves MC's cursor.
		std::atomic<bool> mcScreenOpen{ false };
		// A Fallout menu (Pip-Boy, pause, console, loading, dialogue, ...) owns input.
		std::atomic<bool> falloutMenuOpen{ false };

		// Minecraft's player is in a world (its overlay is drawn), and what its crosshair needs.
		std::atomic<bool> mcInWorld{ false };
		std::atomic<bool> mcCrosshair{ false };
		std::atomic<int>  mcGuiScale{ 0 };
		// Minecraft's health / max health this frame (-1: not known). Main thread.
		float mcHealth{ -1.0f };

		// Minecraft's feet as drawn this frame (Minecraft coords; its third-person body goes here)
		// and its F5 view. Main thread (PlayerCharacter::Update and Present).
		double feetX{ 0.0 }, feetY{ 0.0 }, feetZ{ 0.0 };
		bool   feetValid{ false };
		int    cameraMode{ 0 };

		// Look direction in MC degrees, integrated from raw mouse input (main thread).
		float yaw{ 0.0f };
		float pitch{ 0.0f };
		bool  lookInitialized{ false };
		float sensitivity{ 0.5f };

		// Virtual MC cursor while an MC screen is open.
		std::atomic<int> cursorX{ 0 };
		std::atomic<int> cursorY{ 0 };
		std::atomic<int> viewportW{ 1920 };
		std::atomic<int> viewportH{ 1080 };
	};

	Runtime& State();

	namespace Game
	{
		void Install();
		// A Fallout menu that pauses the game or takes the mouse is open (checked live).
		bool FalloutMenuOpen();
		// Present (main thread): hide again the first-person meshes Fallout showed during the frame.
		void KeepFirstPersonHidden();
		// Tab was pressed while Minecraft drives: hand the player to Fallout to open the Pip-Boy.
		void NotePipboyKey();
		// After a save is loaded: re-sync Minecraft to wherever the save put the player.
		void OnGameLoaded();
	}

	// Fallout's own collision along a segment (game units): the first static surface it meets.
	// Main thread.
	bool PickGroundAt(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, RE::NiPoint3& a_hit);

	// Minecraft blocks as Fallout collision, so NPCs (and Fallout's physics) bump into builds.
	namespace BlockCollision
	{
		void OnSolids(const std::uint8_t* a_data, std::uint32_t a_bytes);  // render thread
		void ClearAll();                                                    // render thread
		void Update(RE::TESObjectCELL* a_cell, const McVec& a_player);      // main thread
		bool Owns(const RE::hknpShape* a_shape);
	}

	namespace Worlds
	{
		// Main thread, when the player's world changes: moves g_worldOffset to that world's place.
		void Select(RE::TESObjectCELL* a_cell);
	}

	namespace Combat
	{
		void Install();
		// Main thread, every frame: Minecraft's hits on Fallout's actors, Fallout's hits on the
		// player sent to Minecraft, the actor table Minecraft's stand-ins follow.
		void PerFrame(RE::PlayerCharacter* a_player, bool a_puppeting, float a_delta);
	}

	namespace Camera
	{
		// Hooks PlayerCamera::Update (needs the trampoline).
		void Install();
		// Main thread, every frame: Minecraft's F5 view (mode 0 first person, 1 behind, 2 in front),
		// its eye (Fallout coords), look and camera distance (units).
		void Set(bool a_active, int a_mode, const RE::NiPoint3& a_eye, float a_heading, float a_pitch, float a_distance);
	}

	namespace Crash
	{
		// Crash logger (fo_crash.cpp): SkyCraft_crash.log next to the F4SE logs.
		void Install();
	}

	namespace Overlay
	{
		// Hooks Fallout's swap chain Present (idempotent; needs the renderer to exist).
		void Install();
	}

	namespace Hud
	{
		// Main thread, every frame: hide Fallout's HP/AP/ammo/crosshair while Minecraft drives.
		void Update(bool a_hide);
		// A save was loaded: the HUD's clips are new objects; look them up again.
		void Reset();
	}

	namespace Input
	{
		void Install();
		// Mouse deltas accumulated since the last frame (main thread).
		void ConsumeLook(float& a_dx, float& a_dy);
		// Tells Minecraft to release every held key/button (input focus moved to Fallout).
		void ReleaseAll();
	}
}
