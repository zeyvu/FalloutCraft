// FalloutCraft: digging into Fallout's world (Minecraft's SkyDig, Fallout side).
//
// Minecraft decides which cells were dug out of Fallout's ground, trees and rocks and sends them per
// section and per Fallout world (kRenDug). Here they're kept so the collision export
// (fo_collision.cpp) can cut those cells out of the diggable triangles it sends: Minecraft's player
// then falls into the hole, and Minecraft's own blocks/walls take over around it.
//
// Fallout still draws its ground over a hole (its terrain can't be changed from here); from inside,
// the hole shows Minecraft's walls.

#include "fo_common.h"

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace skycraft::Dig
{
	namespace
	{
		using Bits = std::array<std::uint64_t, 64>;  // bit x + 16z + 256y

		std::mutex                                                               lock;
		std::unordered_map<std::uint32_t, std::unordered_map<std::uint64_t, Bits>> dug;  // world -> section -> bits
		std::atomic<bool>                                                        changed{ false };
		std::vector<Cell>                                                        fresh;  // newly dug, for the main thread
		std::atomic<std::uint64_t>                                               version{ 0 };
		std::atomic<std::uint32_t>                                               currentWorld{ 0 };

		std::uint64_t SectionKey(std::int32_t sx, std::int32_t sy, std::int32_t sz)
		{
			return (std::uint64_t(std::uint32_t(sx) & 0x1FFFFF) << 42) | (std::uint64_t(std::uint32_t(sy) & 0x1FFFFF) << 21) | std::uint64_t(std::uint32_t(sz) & 0x1FFFFF);
		}

		std::int32_t FloorDiv16(std::int32_t v) { return v >= 0 ? v / 16 : -((-v + 15) / 16); }
	}

	void OnDug(const std::uint8_t* a_data, std::uint32_t a_bytes)
	{
		if (a_bytes < sizeof(proto::RenDug)) {
			return;
		}
		proto::RenDug head{};
		std::memcpy(&head, a_data, sizeof(head));
		const auto       key = SectionKey(head.sx, head.sy, head.sz);
		std::scoped_lock guard(lock);
		auto&            world = dug[head.worldId];
		if (head.count > 0 && a_bytes >= sizeof(head) + 512) {
			Bits bits{};
			std::memcpy(bits.data(), a_data + sizeof(head), 512);
			const auto old = world.find(key);
			if (old != world.end() && old->second == bits) {
				return;  // Minecraft re-sent the section (it does, often): nothing new was dug
			}
			for (int i = 0; i < 4096 && fresh.size() < 4096; ++i) {
				const bool now = (bits[i >> 6] >> (i & 63)) & 1;
				const bool before = old != world.end() && ((old->second[i >> 6] >> (i & 63)) & 1);
				if (now && !before) {
					fresh.push_back({ head.worldId, head.sx * 16 + (i & 15), head.sy * 16 + (i >> 8), head.sz * 16 + ((i >> 4) & 15) });
				}
			}
			world[key] = bits;
		} else {
			if (world.erase(key) == 0) {
				return;  // nothing was dug there before either
			}
		}
		changed = true;
		++version;
	}

	bool TakeChanged()
	{
		return changed.exchange(false);
	}

	std::unique_lock<std::mutex> Lock()
	{
		return std::unique_lock<std::mutex>(lock);
	}

	bool AnyLocked(std::uint32_t a_world)
	{
		auto it = dug.find(a_world);
		return it != dug.end() && !it->second.empty();
	}

	bool IsDugLocked(std::uint32_t a_world, std::int32_t x, std::int32_t y, std::int32_t z)
	{
		const std::int32_t sx = FloorDiv16(x), sy = FloorDiv16(y), sz = FloorDiv16(z);
		auto               w = dug.find(a_world);
		if (w == dug.end()) {
			return false;
		}
		auto it = w->second.find(SectionKey(sx, sy, sz));
		if (it == w->second.end()) {
			return false;
		}
		const int lx = x - sx * 16, ly = y - sy * 16, lz = z - sz * 16;
		const int i = lx + 16 * lz + 256 * ly;
		return (it->second[i >> 6] >> (i & 63)) & 1;
	}

	bool IsDug(std::uint32_t a_world, std::int32_t a_x, std::int32_t a_y, std::int32_t a_z)
	{
		std::scoped_lock guard(lock);
		return AnyLocked(a_world) && IsDugLocked(a_world, a_x, a_y, a_z);
	}

	void TakeFresh(std::vector<Cell>& a_out)
	{
		std::scoped_lock guard(lock);
		a_out.swap(fresh);
		fresh.clear();
	}

	std::uint64_t Version() { return version.load(); }
	void          SetCurrentWorld(std::uint32_t a_world)
	{
		if (currentWorld.exchange(a_world) != a_world) {
			++version;
		}
	}
	std::uint32_t CurrentWorld() { return currentWorld.load(); }

	bool FillVolume(std::uint32_t a_world, std::int32_t a_x0, std::int32_t a_y0, std::int32_t a_z0, int a_w, int a_h, int a_d, std::uint8_t* a_out)
	{
		std::scoped_lock guard(lock);
		if (!AnyLocked(a_world)) {
			return false;
		}
		bool any = false;
		const auto& sections = dug.find(a_world)->second;
		// Each section the box touches, then its dug cells (layout for a Texture3D: x fastest, then
		// y rows, then z slices).
		for (std::int32_t sz = FloorDiv16(a_z0); sz * 16 < a_z0 + a_d; ++sz) {
			for (std::int32_t sy = FloorDiv16(a_y0); sy * 16 < a_y0 + a_h; ++sy) {
				for (std::int32_t sx = FloorDiv16(a_x0); sx * 16 < a_x0 + a_w; ++sx) {
					auto it = sections.find(SectionKey(sx, sy, sz));
					if (it == sections.end()) {
						continue;
					}
					for (int i = 0; i < 4096; ++i) {
						if (((it->second[i >> 6] >> (i & 63)) & 1) == 0) {
							continue;
						}
						const int x = sx * 16 + (i & 15) - a_x0, y = sy * 16 + (i >> 8) - a_y0, z = sz * 16 + ((i >> 4) & 15) - a_z0;
						if (x >= 0 && x < a_w && y >= 0 && y < a_h && z >= 0 && z < a_d) {
							a_out[(z * a_h + y) * a_w + x] = 1;
							any = true;
						}
					}
				}
			}
		}
		return any;
	}
}
