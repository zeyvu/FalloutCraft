#include "skycraft_link.h"

#include "skycraft_protocol.h"

#define NOMINMAX  // std::min / std::max below
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef ERROR

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

namespace skycraft::link
{
	namespace
	{
		HANDLE        g_map = nullptr;
		std::uint8_t* g_base = nullptr;

		template <class T>
		T* At(std::uint64_t a_offset)
		{
			return reinterpret_cast<T*>(g_base + a_offset);
		}

		// Wrap to [-180, 180).
		float WrapDeg(double a_deg)
		{
			a_deg = std::fmod(a_deg, 360.0);
			if (a_deg >= 180.0) {
				a_deg -= 360.0;
			} else if (a_deg < -180.0) {
				a_deg += 360.0;
			}
			return static_cast<float>(a_deg);
		}

		constexpr double kRadToDeg = 180.0 / std::numbers::pi;

		int FloorDiv(int a_value, int a_divisor)
		{
			int q = a_value / a_divisor;
			if ((a_value % a_divisor != 0) && ((a_value < 0) != (a_divisor < 0))) {
				--q;
			}
			return q;
		}

		// Producer side of the host->MC collision ring. Same layout as the render ring Minecraft
		// writes (see SkyCollision.drainOnce on the Java side): head/tail are monotonic byte
		// counters, a message is {u32 type, u32 payloadBytes, payload} padded to 8 bytes, and a
		// message that would cross the end of the ring is preceded by a kColPad that skips to the start.
		// Single producer: only the plugin's pump thread calls this.
		bool PushCollision(std::uint32_t a_type, const void* a_payload, std::uint32_t a_payloadBytes)
		{
			if (!g_base) {
				return false;
			}

			constexpr std::uint64_t kData = proto::kColRingDataBytes;
			std::uint8_t*           ring = g_base + proto::kOffCollisionRing;
			std::atomic_ref<std::uint64_t> headRef(*reinterpret_cast<std::uint64_t*>(ring + proto::kColRingHeadOff));
			std::atomic_ref<std::uint64_t> tailRef(*reinterpret_cast<std::uint64_t*>(ring + proto::kColRingTailOff));

			std::uint64_t       head = headRef.load(std::memory_order_relaxed);
			const std::uint64_t tail = tailRef.load(std::memory_order_acquire);
			const std::uint64_t msgBytes = (8ull + a_payloadBytes + 7ull) & ~7ull;
			std::uint64_t       pos = head % kData;
			const std::uint64_t pad = (pos + msgBytes > kData) ? (kData - pos) : 0;

			if (kData - (head - tail) < msgBytes + pad) {
				return false;  // the consumer is behind; try again later
			}

			std::uint8_t* data = ring + proto::kColRingDataOff;
			if (pad > 0) {
				const proto::ColMsgHeader padHeader{ proto::kColPad, 0 };
				std::memcpy(data + pos, &padHeader, sizeof(padHeader));
				head += pad;
				pos = 0;
			}

			const proto::ColMsgHeader header{ a_type, a_payloadBytes };
			std::memcpy(data + pos, &header, sizeof(header));
			if (a_payloadBytes > 0) {
				std::memcpy(data + pos + sizeof(header), a_payload, a_payloadBytes);
			}
			headRef.store(head + msgBytes, std::memory_order_release);
			return true;
		}

		proto::ColTri MakeTri(float a_ax, float a_ay, float a_az, float a_bx, float a_by, float a_bz, float a_cx, float a_cy, float a_cz)
		{
			proto::ColTri t{};
			t.v[0] = a_ax; t.v[1] = a_ay; t.v[2] = a_az;
			t.v[3] = a_bx; t.v[4] = a_by; t.v[5] = a_bz;
			t.v[6] = a_cx; t.v[7] = a_cy; t.v[8] = a_cz;
			t.flags = 0;
			return t;
		}

		// Consumer side of one MC->host ring: head and tail are monotonic u64 counters, so
		// "consume everything" is tail = head.
		std::uint64_t DrainRing(std::uint64_t a_ringOffset, std::uint64_t a_headOff, std::uint64_t a_tailOff)
		{
			if (!g_base) {
				return 0;
			}
			std::atomic_ref<std::uint64_t> head(*At<std::uint64_t>(a_ringOffset + a_headOff));
			std::atomic_ref<std::uint64_t> tail(*At<std::uint64_t>(a_ringOffset + a_tailOff));
			const std::uint64_t h = head.load(std::memory_order_acquire);
			const std::uint64_t t = tail.load(std::memory_order_relaxed);
			if (h == t) {
				return 0;
			}
			tail.store(h, std::memory_order_release);
			return h - t;
		}
	}

	bool Open()
	{
		if (g_base) {
			return true;
		}

		const std::uint64_t size = proto::kMappingBytes;  // ~190 MB of address space, committed lazily
		g_map = CreateFileMappingW(
			INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
			static_cast<DWORD>(size >> 32), static_cast<DWORD>(size & 0xFFFFFFFFu),
			proto::kMappingName);
		if (!g_map) {
			return false;
		}

		g_base = static_cast<std::uint8_t*>(MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
		if (!g_base) {
			CloseHandle(g_map);
			g_map = nullptr;
			return false;
		}

		// The OS hands out zeroed pages, so every seqlock and ring starts empty.
		// Fill the header last-field-first: Minecraft trusts the header once it sees the magic.
		auto* h = At<proto::Header>(proto::kOffHeader);
		h->version = proto::kVersion;
		h->skyrimPid = GetCurrentProcessId();  // field name kept from v10; rename to hostPid in v11
		h->skyrimHeartbeatMs = GetTickCount64();
		std::atomic_thread_fence(std::memory_order_release);
		h->magic = proto::kMagic;
		return true;
	}

	void Close()
	{
		if (g_base) {
			UnmapViewOfFile(g_base);
			g_base = nullptr;
		}
		if (g_map) {
			CloseHandle(g_map);
			g_map = nullptr;
		}
	}

	bool IsOpen()
	{
		return g_base != nullptr;
	}

	McPose PublishSkyState(const PlayerPose& a_pose, std::uint32_t a_worldId, bool a_inGame, bool a_menuOpen, bool a_loading)
	{
		// Bethesda (Z up, +Y north) -> Minecraft (Y up, -Z north), 70 units per block.
		// HYPOTHESIS for the angles, to be confirmed by the Phase 0 test: yaw 0 = north in the game
		// and 180 in Minecraft, both clockwise seen from above; pitch positive = looking down in both.
		const McPose mc{
			a_pose.x / proto::kUnitsPerBlock,
			a_pose.z / proto::kUnitsPerBlock,
			-a_pose.y / proto::kUnitsPerBlock,
			WrapDeg(a_pose.rotZ * kRadToDeg + 180.0),
			static_cast<float>(a_pose.rotX * kRadToDeg)
		};

		if (!g_base) {
			return mc;
		}

		auto* s = At<proto::SkyState>(proto::kOffSkyState);
		std::atomic_ref<std::uint32_t> seq(s->seq);

		// Seqlock write: odd while writing, even when consistent.
		const std::uint32_t start = seq.load(std::memory_order_relaxed);
		seq.store(start + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);

		std::uint32_t flags = 0;
		if (a_inGame) flags |= proto::kSkyInGame;
		if (a_menuOpen) flags |= proto::kSkyMenuOpen;
		if (a_loading) flags |= proto::kSkyLoading;
		s->flags = flags;
		s->worldId = a_worldId;
		s->posX = mc.x;
		s->posY = mc.y;
		s->posZ = mc.z;
		s->yaw = mc.yaw;
		s->pitch = mc.pitch;

		// Not touched yet: collisionEpoch, teleportSeq, viewportW/H, gameHour.

		seq.store(start + 2, std::memory_order_release);
		return mc;
	}

	void Heartbeat()
	{
		if (g_base) {
			At<proto::Header>(proto::kOffHeader)->skyrimHeartbeatMs = GetTickCount64();
		}
	}

	bool MinecraftAlive()
	{
		if (!g_base) {
			return false;
		}
		const auto* h = At<proto::Header>(proto::kOffHeader);
		const std::uint64_t beat = h->mcHeartbeatMs;
		return h->mcPid != 0 && beat != 0 && (GetTickCount64() - beat) < 2000;
	}

	bool ReadMcState(McSnapshot& a_out)
	{
		if (!g_base) {
			return false;
		}

		auto*                          s = At<proto::McState>(proto::kOffMcState);
		std::atomic_ref<std::uint32_t> seq(s->seq);

		// Seqlock read: retry while the writer is mid-update (odd) or changed it under us.
		for (int attempt = 0; attempt < 8; ++attempt) {
			const std::uint32_t before = seq.load(std::memory_order_acquire);
			if (before == 0) {
				return false;  // never written
			}
			if (before & 1u) {
				continue;
			}

			proto::McState copy;
			std::memcpy(&copy, s, sizeof(copy));
			std::atomic_thread_fence(std::memory_order_acquire);

			if (seq.load(std::memory_order_relaxed) == before) {
				a_out.flags = copy.flags;
				a_out.x = copy.x;
				a_out.y = copy.y;
				a_out.z = copy.z;
				a_out.yaw = copy.yaw;
				a_out.pitch = copy.pitch;
				a_out.teleportAck = copy.teleportAck;
				a_out.frameCounter = copy.frameCounter;
				return true;
			}
		}
		return false;
	}

	std::uint32_t McPid()
	{
		return g_base ? At<proto::Header>(proto::kOffHeader)->mcPid : 0;
	}

	std::int64_t McHeartbeatAgeMs()
	{
		if (!g_base) {
			return -1;
		}
		const std::uint64_t beat = At<proto::Header>(proto::kOffHeader)->mcHeartbeatMs;
		if (beat == 0) {
			return -1;
		}
		return static_cast<std::int64_t>(GetTickCount64() - beat);
	}

	std::uint32_t McStateSeq()
	{
		if (!g_base) {
			return 0;
		}
		auto* s = At<proto::McState>(proto::kOffMcState);
		return std::atomic_ref<std::uint32_t>(s->seq).load(std::memory_order_acquire);
	}

	std::uint64_t DrainRenderRing()
	{
		return DrainRing(proto::kOffRenderRing, proto::kRenRingHeadOff, proto::kRenRingTailOff);
	}

	std::uint64_t DrainEventRing()
	{
		return DrainRing(proto::kOffEventRing, proto::kEventRingHeadOff, proto::kEventRingTailOff);
	}

	void WriteActors(const proto::ActorRecord* a_records, std::uint32_t a_count)
	{
		if (!g_base) {
			return;
		}
		auto*                          table = At<proto::ActorTable>(proto::kOffActorTable);
		std::atomic_ref<std::uint32_t> seq(table->seq);
		const std::uint32_t            s = seq.load(std::memory_order_relaxed);
		seq.store(s + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		const std::uint32_t count = a_records ? (std::min)(a_count, proto::kMaxActors) : 0u;
		table->count = count;
		if (count) {
			std::memcpy(table->actors, a_records, sizeof(proto::ActorRecord) * count);
		}
		seq.store(s + 2, std::memory_order_release);
	}

	bool PopEvent(proto::McEvent& a_out)
	{
		if (!g_base) {
			return false;
		}
		std::atomic_ref<std::uint64_t> headRef(*At<std::uint64_t>(proto::kOffEventRing + proto::kEventRingHeadOff));
		std::atomic_ref<std::uint64_t> tailRef(*At<std::uint64_t>(proto::kOffEventRing + proto::kEventRingTailOff));
		const std::uint64_t            head = headRef.load(std::memory_order_acquire);
		std::uint64_t                  tail = tailRef.load(std::memory_order_relaxed);
		if (tail >= head) {
			return false;
		}
		if (head - tail > proto::kEventRingEntries) {
			tail = head - proto::kEventRingEntries;  // overrun: keep the newest
		}
		a_out = At<proto::McEvent>(proto::kOffEventRing + proto::kEventRingDataOff)[tail & (proto::kEventRingEntries - 1)];
		tailRef.store(tail + 1, std::memory_order_release);
		return true;
	}

	bool SendSyntheticFloor(double a_x, double a_y, double a_z, std::uint32_t a_epoch)
	{
		if (!g_base) {
			return false;
		}

		constexpr int kRegion = 8;  // SkyCollision.REGION_SIZE on the Java side
		const int     bx = static_cast<int>(std::floor(a_x));
		const int     by = static_cast<int>(std::floor(a_y - 0.001));  // block boundary at or below the feet
		const int     bz = static_cast<int>(std::floor(a_z));
		const int     floorY = by - 1;  // full blocks here; their top surface is the plane y = by

		const int rx = FloorDiv(bx, kRegion);
		const int ry = FloorDiv(by, kRegion);
		const int rz = FloorDiv(bz, kRegion);

		// 1) Start a fresh collision epoch (Minecraft drops whatever it had).
		if (!PushCollision(proto::kColClear, &a_epoch, sizeof(a_epoch))) {
			return false;
		}

		// 2) Exact triangles, one message per 8x8 patch of the floor plane. Both windings are sent
		//    because I could not see how SkyTri decides which side is solid.
		for (int px = rx - 2; px <= rx + 2; ++px) {
			for (int pz = rz - 2; pz <= rz + 2; ++pz) {
				const float x0 = static_cast<float>(px * kRegion), x1 = x0 + kRegion;
				const float z0 = static_cast<float>(pz * kRegion), z1 = z0 + kRegion;
				const float y = static_cast<float>(by);

				struct
				{
					proto::ColRegion head;
					proto::ColTri    tris[4];
				} msg{};
				msg.head = { px * kRegion, ry * kRegion, pz * kRegion,
					px * kRegion + kRegion - 1, ry * kRegion + kRegion - 1, pz * kRegion + kRegion - 1,
					a_epoch, 4 };
				msg.tris[0] = MakeTri(x0, y, z0, x1, y, z0, x1, y, z1);
				msg.tris[1] = MakeTri(x0, y, z0, x1, y, z1, x0, y, z1);
				msg.tris[2] = MakeTri(x0, y, z0, x1, y, z1, x1, y, z0);
				msg.tris[3] = MakeTri(x0, y, z0, x0, y, z1, x1, y, z1);
				if (!PushCollision(proto::kColTris, &msg, sizeof(msg))) {
					return false;
				}
			}
		}

		// 3) The 8x8x8 voxels, and with them the "this region is known" marker. The box is aligned to
		//    whole regions and contains the floor layer.
		const int minX = (rx - 2) * kRegion, maxX = (rx + 3) * kRegion - 1;
		const int minZ = (rz - 2) * kRegion, maxZ = (rz + 3) * kRegion - 1;
		const int minY = (ry - 1) * kRegion, maxY = (ry + 2) * kRegion - 1;

		std::vector<proto::ColBlock> blocks;
		blocks.reserve(static_cast<std::size_t>(maxX - minX + 1) * static_cast<std::size_t>(maxZ - minZ + 1));
		for (int x = minX; x <= maxX; ++x) {
			for (int z = minZ; z <= maxZ; ++z) {
				proto::ColBlock block{};
				block.x = x;
				block.y = floorY;
				block.z = z;
				for (auto& layer : block.bits) {
					layer = ~0ull;
				}
				blocks.push_back(block);
			}
		}

		const proto::ColRegion region{ minX, minY, minZ, maxX, maxY, maxZ, a_epoch, static_cast<std::uint32_t>(blocks.size()) };
		std::vector<std::uint8_t> payload(sizeof(region) + blocks.size() * sizeof(proto::ColBlock));
		std::memcpy(payload.data(), &region, sizeof(region));
		std::memcpy(payload.data() + sizeof(region), blocks.data(), blocks.size() * sizeof(proto::ColBlock));
		return PushCollision(proto::kColRegion, payload.data(), static_cast<std::uint32_t>(payload.size()));
	}

	bool ReadMcStateFull(proto::McState& a_out)
	{
		if (!g_base) {
			return false;
		}
		auto*                          s = At<proto::McState>(proto::kOffMcState);
		std::atomic_ref<std::uint32_t> seq(s->seq);
		for (int attempt = 0; attempt < 8; ++attempt) {
			const std::uint32_t before = seq.load(std::memory_order_acquire);
			if (before == 0) {
				return false;
			}
			if (before & 1u) {
				continue;
			}
			std::memcpy(&a_out, s, sizeof(a_out));
			std::atomic_thread_fence(std::memory_order_acquire);
			if (seq.load(std::memory_order_relaxed) == before) {
				return true;
			}
		}
		return false;
	}

	void WriteSkyState(const proto::SkyState& a_state)
	{
		if (!g_base) {
			return;
		}
		auto*                          s = At<proto::SkyState>(proto::kOffSkyState);
		std::atomic_ref<std::uint32_t> seq(s->seq);
		const std::uint32_t            start = seq.load(std::memory_order_relaxed);
		seq.store(start + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		// Everything after the seq field.
		std::memcpy(reinterpret_cast<std::uint8_t*>(s) + sizeof(std::uint32_t),
			reinterpret_cast<const std::uint8_t*>(&a_state) + sizeof(std::uint32_t),
			sizeof(proto::SkyState) - sizeof(std::uint32_t));
		seq.store(start + 2, std::memory_order_release);
	}

	bool WriteCollision(std::uint32_t a_type, const void* a_payload, std::uint32_t a_bytes)
	{
		return PushCollision(a_type, a_payload, a_bytes);
	}

	namespace
	{
		std::uint32_t g_overlayFront = 2;
	}

	bool AcquireOverlayFrame()
	{
		if (!g_base) {
			return false;
		}
		std::atomic_ref<std::uint32_t> state(At<proto::OverlayCtl>(proto::kOffOverlayCtl)->state);
		if (!(state.load(std::memory_order_acquire) & proto::kOverlayDirty)) {
			return false;
		}
		const auto old = state.exchange(g_overlayFront, std::memory_order_acq_rel);
		g_overlayFront = old & 3;
		return true;
	}

	void ResetOverlay()
	{
		if (!g_base) {
			return;
		}
		std::atomic_ref<std::uint32_t>(At<proto::OverlayCtl>(proto::kOffOverlayCtl)->state).store(0, std::memory_order_release);
		g_overlayFront = 2;
	}

	const std::uint8_t* FrontPixels()
	{
		return g_base + proto::kOffOverlayPixels + proto::kOverlaySlotBytes * g_overlayFront;
	}

	const proto::OverlaySlotHdr* FrontHeader()
	{
		return At<proto::OverlaySlotHdr>(proto::kOffOverlaySlotHdr + sizeof(proto::OverlaySlotHdr) * g_overlayFront);
	}

	std::uint64_t DrainRender(void (*a_fn)(void*, std::uint32_t, const std::uint8_t*, std::uint32_t), void* a_ctx, std::uint64_t a_maxBytes)
	{
		if (!g_base) {
			return 0;
		}
		std::uint8_t*                  ring = g_base + proto::kOffRenderRing;
		std::atomic_ref<std::uint64_t> headRef(*reinterpret_cast<std::uint64_t*>(ring + proto::kRenRingHeadOff));
		std::atomic_ref<std::uint64_t> tailRef(*reinterpret_cast<std::uint64_t*>(ring + proto::kRenRingTailOff));
		const std::uint64_t            head = headRef.load(std::memory_order_acquire);
		std::uint64_t                  tail = tailRef.load(std::memory_order_relaxed);
		const std::uint8_t*            data = ring + proto::kRenRingDataOff;
		constexpr auto                 size = proto::kRenRingDataBytes;
		std::uint64_t                  done = 0;
		while (tail < head && done < a_maxBytes) {
			const auto pos = tail % size;
			const auto* hdr = reinterpret_cast<const proto::ColMsgHeader*>(data + pos);
			if (hdr->type == proto::kRenPad) {
				tail += size - pos;
				continue;
			}
			if (a_fn) {
				a_fn(a_ctx, hdr->type, data + pos + sizeof(proto::ColMsgHeader), hdr->payloadBytes);
			}
			const auto msgBytes = (sizeof(proto::ColMsgHeader) + hdr->payloadBytes + 7) & ~7ull;
			tail += msgBytes;
			done += msgBytes;
		}
		tailRef.store(tail, std::memory_order_release);
		return done;
	}

	bool ReadWorldEntities(proto::WorldEntities& a_out)
	{
		if (!g_base) {
			return false;
		}
		auto*                          s = At<proto::WorldEntities>(proto::kOffWorldEntities);
		std::atomic_ref<std::uint32_t> seq(s->seq);
		for (int attempt = 0; attempt < 8; ++attempt) {
			const std::uint32_t before = seq.load(std::memory_order_acquire);
			if (before == 0) {
				return false;
			}
			if (before & 1u) {
				continue;
			}
			std::memcpy(&a_out, s, sizeof(a_out));
			std::atomic_thread_fence(std::memory_order_acquire);
			if (seq.load(std::memory_order_relaxed) == before) {
				a_out.count = std::min<std::uint32_t>(a_out.count, proto::kMaxWorldEntities);
				return true;
			}
		}
		return false;
	}

	void RequestResend()
	{
		if (!g_base) {
			return;
		}
		auto* h = At<proto::Header>(proto::kOffHeader);
		const std::uint32_t pid = GetCurrentProcessId();
		h->skyrimPid = (h->skyrimPid == pid) ? (pid ^ 0x40000000u) : pid;
		ResetOverlay();
	}

	bool PushInput(std::uint16_t a_type, std::uint16_t a_code, std::int32_t a_a, std::int32_t a_b, std::int32_t a_c)
	{
		if (!g_base) {
			return false;
		}

		std::uint8_t*                  ring = g_base + proto::kOffInputRing;
		std::atomic_ref<std::uint64_t> headRef(*reinterpret_cast<std::uint64_t*>(ring + proto::kInputRingHeadOff));
		std::atomic_ref<std::uint64_t> tailRef(*reinterpret_cast<std::uint64_t*>(ring + proto::kInputRingTailOff));

		const std::uint64_t head = headRef.load(std::memory_order_relaxed);
		const std::uint64_t tail = tailRef.load(std::memory_order_acquire);
		if (head - tail >= proto::kInputRingEntries) {
			return false;
		}

		const proto::InputEvent event{ a_type, a_code, a_a, a_b, a_c };
		std::memcpy(ring + proto::kInputRingDataOff + (head & (proto::kInputRingEntries - 1)) * sizeof(event), &event, sizeof(event));
		headRef.store(head + 1, std::memory_order_release);
		return true;
	}
}