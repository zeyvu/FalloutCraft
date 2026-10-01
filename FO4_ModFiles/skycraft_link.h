#pragma once
// SkyCraft link, host side (F4SE plugin).
// Creates the shared mapping and publishes the player state to Minecraft.
// Needs skycraft_protocol.h (SkyCraft/protocol, kVersion 10) on the include path.
// Pure Win32 + C++23: no CommonLibF4 types here, so it can be tested on its own.

#include <cstdint>

#include "skycraft_protocol.h"

namespace skycraft::link
{
	// Player pose as the game reports it, in Bethesda units (Z up, +Y north) and radians.
	struct PlayerPose
	{
		double x, y, z;     // feet position
		float  rotX, rotZ;  // pitch, yaw (radians)
	};

	// The same pose in Minecraft space (blocks, Y up, -Z north; degrees).
	struct McPose
	{
		double x, y, z;
		float  yaw, pitch;
	};

	// Create (or open) "Local\SkyCraft_v1" and write the header. Call once at plugin load.
	bool Open();
	void Close();
	bool IsOpen();

	// Convert to Minecraft space and publish SkyState under its seqlock.
	// Returns the converted pose (also when the mapping is closed, nothing is published then).
	McPose PublishSkyState(const PlayerPose& a_pose, std::uint32_t a_worldId, bool a_inGame, bool a_menuOpen, bool a_loading);

	// Bump the host heartbeat (call every frame or at least every few hundred ms).
	void Heartbeat();

	// True if Minecraft has written a heartbeat in the last two seconds.
	bool MinecraftAlive();

	// Latest McState published by Minecraft (seqlock read). Coordinates are Minecraft space.
	struct McSnapshot
	{
		std::uint32_t flags;  // proto::McFlags
		double        x, y, z;
		float         yaw, pitch;
		std::uint32_t teleportAck;
		std::uint64_t frameCounter;

		// Bits of proto::McFlags.
		bool inWorld() const { return (flags & (1u << 0)) != 0; }
		bool screenOpen() const { return (flags & (1u << 1)) != 0; }
		bool onGround() const { return (flags & (1u << 2)) != 0; }
	};

	// False if the mapping is closed or Minecraft has never written a state yet.
	bool ReadMcState(McSnapshot& a_out);

	// EXPERIMENT (Phase 0, no longer called): give Minecraft's player something to stand on. Sends a clear (a_epoch), then a
	// flat floor of full blocks whose top surface is the block boundary at or just below
	// (a_x, a_y, a_z), about 40 x 40 blocks, as both exact triangles and 8x8x8 voxels.
	// Returns false if the collision ring had no room; call again later.
	bool SendSyntheticFloor(double a_x, double a_y, double a_z, std::uint32_t a_epoch);

	// Queue one input event for Minecraft (16-byte InputEvent in the input ring, same layout the
	// Java side's drainInput reads). type 1 = key (code = SDL scancode, a = 1 press / 0 release).
	// Returns false if the ring is full.
	bool PushInput(std::uint16_t a_type, std::uint16_t a_code, std::int32_t a_a = 0, std::int32_t a_b = 0, std::int32_t a_c = 0);

	// TEMPORARY, until the real renderer/event handlers exist: mark everything Minecraft has
	// queued as consumed so its writer never waits for space. Returns bytes (render ring) or
	// entries (event ring) discarded by this call.
	std::uint64_t DrainRenderRing();
	std::uint64_t DrainEventRing();

	// Nearby Fallout actors for Minecraft's hittable stand-ins (seqlock write; null/0 clears).
	void WriteActors(const proto::ActorRecord* a_records, std::uint32_t a_count);
	// Next Minecraft event (hits on actors, the player's death, explosions, ...). False if none.
	bool PopEvent(proto::McEvent& a_out);

	// ---- Phase 1 (full link) -------------------------------------------------------------------

	// Full McState (seqlock read). False if Minecraft never wrote one or no consistent copy was had.
	bool ReadMcStateFull(proto::McState& a_out);

	// Write the whole SkyState (seqlock); a_state.seq is ignored.
	void WriteSkyState(const proto::SkyState& a_state);

	// Collision ring producer (one thread only: the collision worker). False if the ring is full.
	bool WriteCollision(std::uint32_t a_type, const void* a_payload, std::uint32_t a_bytes);

	// Overlay triple buffer (consumer side): Minecraft's hand, hotbar, HUD and screens, drawn
	// over Fallout's frame. If a newer frame is available, swaps it into the front slot.
	bool                         AcquireOverlayFrame();
	// Minecraft (re)connected: its writer starts at slot 1, so restart the swap from scratch.
	void                         ResetOverlay();
	const std::uint8_t*          FrontPixels();
	const proto::OverlaySlotHdr* FrontHeader();

	// Render ring consumer (one thread at a time): calls a_fn(type, payload, bytes) for each pending
	// message, up to about a_maxBytes. The payload points into shared memory. Returns bytes consumed.
	std::uint64_t DrainRender(void (*a_fn)(void* a_ctx, std::uint32_t a_type, const std::uint8_t* a_data, std::uint32_t a_bytes), void* a_ctx, std::uint64_t a_maxBytes);

	// Makes Minecraft resend everything it draws in our world (texture atlas, every block section):
	// it does so whenever the host process id in the header changes, so flip it between two values.
	// Also restarts the overlay swap, which Minecraft restarts at the same moment.
	void RequestResend();

	// Minecraft's per-frame world things Fallout draws itself (dropped items, arrows, block cracks)
	// and the targeted block's outline (seqlock read). False if none was ever written.
	bool ReadWorldEntities(proto::WorldEntities& a_out);

	// Diagnostics for the link itself.
	std::uint32_t McPid();                // 0 until Minecraft attaches
	std::int64_t  McHeartbeatAgeMs();     // ms since Minecraft's last heartbeat, -1 if it never wrote one
	std::uint32_t McStateSeq();           // McState seqlock counter, 0 = Minecraft never wrote a state
}