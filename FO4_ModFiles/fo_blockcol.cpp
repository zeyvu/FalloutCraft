// FalloutCraft: Minecraft blocks as Fallout collision.
//
// Minecraft already tells us, per 16x16x16 section, which of its blocks are solid (kRenSolids, sent
// with each section's mesh). Here those become static Havok bodies in Fallout's physics world, so
// NPCs, creatures, ragdolls and thrown things bump into what the player builds instead of walking
// through it. Each section's solid blocks are merged into as few boxes as possible (greedy), one
// box shape and one body per box.
//
// Havok has no "remove body" we can call, so a section that changes has its old bodies switched to
// "don't collide" and new ones made. Only sections near the player are built; the rest wait.
// Fallout's ground picks and our own collision export to Minecraft skip these bodies (Owns()):
// Minecraft has the blocks already.

#include "fo_collision.h"
#include "fo_common.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef ERROR

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace skycraft::BlockCollision
{
	namespace
	{
		constexpr int         kNearBlocks = 80;      // build sections whose centre is this close (blocks)
		constexpr int         kSectionsPerFrame = 3;  // main-thread budget
		constexpr std::size_t kMaxBodies = 30000;

		using Bits = std::array<std::uint64_t, 64>;  // bit x + 16z + 256y

		struct Section
		{
			std::int32_t sx, sy, sz;
			Bits         bits{};
			bool         dirty{ true };
			struct Made
			{
				std::uint32_t          id;
				const RE::hknpShape*   shape;
			};
			std::vector<Made> bodies;  // in builtWorld
		};

		std::mutex                                   pendingLock;
		std::unordered_map<std::uint64_t, std::pair<bool, Bits>> pending;  // key -> (has blocks, bits)
		bool                                         pendingClear = false;

		// Main thread only from here.
		std::unordered_map<std::uint64_t, Section> sections;
		std::unordered_set<const RE::hknpShape*>   ownShapes;
		std::mutex                                 ownLock;  // Owns() is called from pick/collision code
		RE::hknpWorld*                             builtWorld = nullptr;
		float                                      builtOffset[3]{ 1e30f, 1e30f, 1e30f };
		double                                     builtWorldOffset[2]{ 1e30, 1e30 };
		std::size_t                                liveBodies = 0;
		std::uint64_t                              statMade = 0, statBoxes = 0;
		bool                                       loggedFail = false;

		std::uint64_t Key(std::int32_t x, std::int32_t y, std::int32_t z)
		{
			return (std::uint64_t(std::uint32_t(x) & 0x1FFFFF) << 42) | (std::uint64_t(std::uint32_t(y) & 0x1FFFFF) << 21) | std::uint64_t(std::uint32_t(z) & 0x1FFFFF);
		}

		bool Get(const Bits& b, int x, int y, int z) { const int i = x + 16 * z + 256 * y; return (b[i >> 6] >> (i & 63)) & 1; }
		void Clear(Bits& b, int x, int y, int z) { const int i = x + 16 * z + 256 * y; b[i >> 6] &= ~(1ull << (i & 63)); }

		struct Box
		{
			int x0, y0, z0, x1, y1, z1;  // inclusive-exclusive, section-local blocks
		};

		// Greedy merge: grow along x, then z, then y.
		std::vector<Box> Boxes(Bits b)
		{
			std::vector<Box> out;
			for (int y = 0; y < 16; ++y) {
				for (int z = 0; z < 16; ++z) {
					for (int x = 0; x < 16; ++x) {
						if (!Get(b, x, y, z)) {
							continue;
						}
						int x1 = x + 1;
						while (x1 < 16 && Get(b, x1, y, z)) {
							++x1;
						}
						auto rowFull = [&](int yy, int zz) {
							for (int xx = x; xx < x1; ++xx) {
								if (!Get(b, xx, yy, zz)) {
									return false;
								}
							}
							return true;
						};
						int z1 = z + 1;
						while (z1 < 16 && rowFull(y, z1)) {
							++z1;
						}
						int y1 = y + 1;
						for (bool ok = true; y1 < 16 && ok;) {
							for (int zz = z; zz < z1 && ok; ++zz) {
								ok = rowFull(y1, zz);
							}
							if (ok) {
								++y1;
							}
						}
						for (int yy = y; yy < y1; ++yy) {
							for (int zz = z; zz < z1; ++zz) {
								for (int xx = x; xx < x1; ++xx) {
									Clear(b, xx, yy, zz);
								}
							}
						}
						out.push_back({ x, y, z, x1, y1, z1 });
					}
				}
			}
			return out;
		}

		bool StillOurs(RE::hknpWorld* a_world, const Section::Made& a_made)
		{
			auto& bm = a_world->m_bodyManager;
			if (a_made.id >= static_cast<std::uint32_t>(bm.m_bodies.size())) {
				return false;
			}
			const RE::hknpBody& body = bm.m_bodies.data()[a_made.id];
			std::uint32_t       id = 0;
			std::memcpy(&id, &body.m_id, sizeof(id));
			return id == a_made.id && body.m_shape == a_made.shape;
		}

		// Old bodies stop colliding (there's no removing them). Only if they're still ours in this world.
		void Retire(RE::hknpWorld* a_world, Section& a_section)
		{
			for (const auto& m : a_section.bodies) {
				if (a_world && a_world == builtWorld && StillOurs(a_world, m)) {
					RE::hknpBodyId id{};
					id.m_value = m.id;
					RE::hknpCollisionFlags flags{};
					flags.set(RE::hknpCollisionFlagsEnum::kDontCollide);
					a_world->EnableBodyFlags(id, flags);
				}
				liveBodies -= liveBodies > 0 ? 1 : 0;
			}
			a_section.bodies.clear();
		}

		// Havok calls that must not take the game down (SEH). Plain function pointers and plain
		// arguments only: MSVC refuses __try in a function that has objects to unwind.
		constexpr unsigned long kExecuteHandler = 1;
		using CreateFn = void* (*)(RE::hknpWorld*, RE::hknpBodyId*, const RE::hknpBodyCinfo*, std::int32_t, std::uint8_t);
		using BoxFn = RE::hknpConvexShape* (*)(const RE::hkVector4f*, float, const RE::hknpConvexShape::BuildConfig*);

		// a_code: the exception code if Havok threw (0 if not).
		bool GuardedCreate(CreateFn a_fn, RE::hknpWorld* a_world, RE::hknpBodyId* a_id, const RE::hknpBodyCinfo* a_info, std::int32_t a_mode, unsigned long* a_code)
		{
			__try {
				a_fn(a_world, a_id, a_info, a_mode, 0 /* no flags */);
				return true;
			} __except (*a_code = GetExceptionCode(), kExecuteHandler) {
				return false;
			}
		}

		RE::hknpConvexShape* GuardedBox(BoxFn a_fn, const RE::hkVector4f* a_half, const RE::hknpConvexShape::BuildConfig* a_config, unsigned long* a_code)
		{
			__try {
				return a_fn(a_half, 0.0f, a_config);
			} __except (*a_code = GetExceptionCode(), kExecuteHandler) {
				return nullptr;
			}
		}

		int failLogs = 0;

		// What a real Fallout static body uses (motion, quality, material, collision filter): ours copy
		// it. The defaults hknpBodyCinfo's constructor leaves (0x7FFFFFFF motion) crash createBody.
		struct Template
		{
			bool          ok{ false };
			std::uint32_t motion{ 0 };
			std::uint16_t material{ 0 };
			std::uint8_t  quality{ 0 };
			std::uint32_t filter{ 0 };
			std::uint32_t flags{ 0 };
		};
		Template       statics;
		RE::hknpWorld* templateWorld = nullptr;

		void FindTemplate(RE::hknpWorld* a_world)
		{
			statics = {};
			templateWorld = a_world;
			auto&     bm = a_world->m_bodyManager;
			const int count = std::min<int>(bm.m_bodies.size(), static_cast<int>(bm.m_peakBodyIndex) + 1);
			for (int i = 0; i < count; ++i) {
				const RE::hknpBody& body = bm.m_bodies.data()[i];
				std::uint32_t       id = 0, flags = 0, motion = 0;
				std::memcpy(&id, &body.m_id, sizeof(id));
				std::memcpy(&flags, &body.m_flags, sizeof(flags));
				std::memcpy(&motion, &body.m_motionId, sizeof(motion));
				if (id != static_cast<std::uint32_t>(i) || !body.m_shape || (flags & 0x1) == 0 || (flags & 0x100) != 0 || motion == 0x7FFFFFFF) {
					continue;  // not a live, colliding static body
				}
				if ((body.m_collisionFilterInfo & 0x7F) != static_cast<std::uint32_t>(RE::COL_LAYER::kStatic) || Owns(body.m_shape)) {
					continue;
				}
				statics.ok = true;
				statics.motion = motion;
				std::memcpy(&statics.material, &body.m_materialId, sizeof(statics.material));
				std::memcpy(&statics.quality, &body.m_qualityId, sizeof(statics.quality));
				statics.filter = body.m_collisionFilterInfo;
				statics.flags = flags & 0x00FFFF00u & ~0x100u;  // user/material flags, not the internal ones
				REX::INFO("block collision: copying static body {} (motion {}, quality {}, material {}, filter {:08X})", i, statics.motion, statics.quality, statics.material, statics.filter);
				return;
			}
			REX::WARN("block collision: no static body in this world to copy; not making any");
		}

		bool MakeBody(RE::hknpWorld* a_world, const float a_center[3], const float a_half[3], Section& a_section)
		{
			RE::hknpConvexShape::BuildConfig config{};
			const RE::hkVector4f            half{ a_half[0], a_half[1], a_half[2], 0.0f };
			// References are pointers in the x64 ABI, so these signatures match the game's functions.
			static const auto boxFn = reinterpret_cast<BoxFn>(REL::Relocation<std::uintptr_t>{ RE::ID::hknpConvexShape::CreateFromHalfExtents }.address());
			static const auto createFn = reinterpret_cast<CreateFn>(REL::Relocation<std::uintptr_t>{ RE::ID::hknpWorld::CreateBody }.address());
			unsigned long                   code = 0;
			auto*                           shape = GuardedBox(boxFn, &half, &config, &code);
			if (!shape) {
				if (failLogs++ < 3) {
					REX::WARN("block collision: making the box shape failed (exception {:08X}, half {:.2f} {:.2f} {:.2f})", code, a_half[0], a_half[1], a_half[2]);
				}
				return false;
			}
			RE::hknpBodyCinfo info;
			info.m_shape = shape;
			info.m_collisionFilterInfo = statics.filter;
			std::memcpy(&info.m_motionId, &statics.motion, sizeof(statics.motion));
			std::memcpy(&info.m_materialId, &statics.material, sizeof(statics.material));
			std::memcpy(&info.m_qualityId, &statics.quality, sizeof(statics.quality));
			std::memcpy(&info.m_flags, &statics.flags, sizeof(statics.flags));
			info.m_position = RE::hkVector4f{ a_center[0], a_center[1], a_center[2], 0.0f };

			// hknpWorld::CreateBody(out id, cinfo, mode, flags) called directly: the RE wrapper
			// returns a reference to its own local. Added now; if Havok refuses mid-step, next step.
			RE::hknpBodyId id{};
			id.m_value = 0x7FFFFFFF;
			bool made = GuardedCreate(createFn, a_world, &id, &info, 1 /* kAddBodyNow */, &code);
			const unsigned long firstCode = code;
			if (!made || id.m_value == 0x7FFFFFFF) {
				code = 0;
				id.m_value = 0x7FFFFFFF;
				made = GuardedCreate(createFn, a_world, &id, &info, 2 /* kAddBodyInNextStep */, &code);
			}
			if (!made || id.m_value == 0x7FFFFFFF) {
				if (failLogs++ < 3) {
					REX::WARN("block collision: Havok's createBody failed (add now: exception {:08X}; next step: exception {:08X}; id {:X}; shape type {})",
						firstCode, code, id.m_value, static_cast<int>(shape->GetType()));
				}
				return false;
			}
			{
				std::scoped_lock lock(ownLock);
				ownShapes.insert(shape);
			}
			a_section.bodies.push_back({ id.m_value, shape });
			++liveBodies;
			++statMade;
			return true;
		}

		void Build(RE::hknpWorld* a_world, const float a_hkOffset[3], Section& a_section)
		{
			Retire(a_world, a_section);
			a_section.dirty = false;
			const double ox = g_worldOffset.x.load(std::memory_order_relaxed), oz = g_worldOffset.z.load(std::memory_order_relaxed);
			const float  k = RE::HK_TO_BS_SCALE / static_cast<float>(proto::kUnitsPerBlock);  // blocks per Havok unit
			for (const auto& b : Boxes(a_section.bits)) {
				if (liveBodies >= kMaxBodies) {
					return;
				}
				// The box in Minecraft blocks...
				const double mx0 = a_section.sx * 16.0 + b.x0, mx1 = a_section.sx * 16.0 + b.x1;
				const double my0 = a_section.sy * 16.0 + b.y0, my1 = a_section.sy * 16.0 + b.y1;
				const double mz0 = a_section.sz * 16.0 + b.z0, mz1 = a_section.sz * 16.0 + b.z1;
				// ...in Havok's world (inverse of Collision::HkPointToMc): x = mcX, y = -mcZ, z = mcY.
				const float center[3] = {
					float(((mx0 + mx1) * 0.5 - ox) / k) - a_hkOffset[0],
					float(-((mz0 + mz1) * 0.5 - oz) / k) - a_hkOffset[1],
					float(((my0 + my1) * 0.5) / k) - a_hkOffset[2],
				};
				const float half[3] = { float((mx1 - mx0) * 0.5 / k), float((mz1 - mz0) * 0.5 / k), float((my1 - my0) * 0.5 / k) };
				++statBoxes;
				if (!statics.ok || !MakeBody(a_world, center, half, a_section)) {
					statics.ok = false;  // don't keep poking Havok after a failure (until the next world)
					if (!loggedFail) {
						loggedFail = true;
						REX::WARN("block collision: Havok wouldn't make a body for a block box; NPCs won't collide with builds");
					}
					return;
				}
			}
		}
	}

	void OnSolids(const std::uint8_t* a_data, std::uint32_t a_bytes)
	{
		if (a_bytes < sizeof(proto::RenSolids)) {
			return;
		}
		proto::RenSolids head{};
		std::memcpy(&head, a_data, sizeof(head));
		std::pair<bool, Bits> entry{ false, {} };
		if (head.count > 0 && a_bytes >= sizeof(head) + 512) {
			std::memcpy(entry.second.data(), a_data + sizeof(head), 512);
			entry.first = true;
		}
		std::scoped_lock lock(pendingLock);
		pending[Key(head.sx, head.sy, head.sz)] = entry;
	}

	void ClearAll()
	{
		std::scoped_lock lock(pendingLock);
		pending.clear();
		pendingClear = true;
	}

	bool Owns(const RE::hknpShape* a_shape)
	{
		std::scoped_lock lock(ownLock);
		return !ownShapes.empty() && ownShapes.contains(a_shape);
	}

	void Update(RE::TESObjectCELL* a_cell, const McVec& a_player)
	{
		auto* bhk = a_cell ? a_cell->GetbhkWorld() : nullptr;
		auto* world = bhk ? bhk->m_worldNP.get() : nullptr;
		float hkOffset[3];
		if (!world || !Collision::Get().HavokOffset(hkOffset)) {
			return;
		}

		// A different physics world (or the same one read differently): what we built is gone or
		// misplaced. Bodies in a world we left are left alone (it may not exist any more).
		const double wox = g_worldOffset.x.load(std::memory_order_relaxed), woz = g_worldOffset.z.load(std::memory_order_relaxed);
		const bool   moved = std::memcmp(hkOffset, builtOffset, sizeof(hkOffset)) != 0 || wox != builtWorldOffset[0] || woz != builtWorldOffset[1];
		if (world != builtWorld || moved) {
			RE::BSAutoWriteLock lock(static_cast<RE::hknpBSWorld*>(world)->m_worldLock);
			for (auto& [key, s] : sections) {
				if (world == builtWorld) {
					Retire(world, s);
				} else {
					liveBodies -= std::min(liveBodies, s.bodies.size());
					s.bodies.clear();
				}
				s.dirty = true;
			}
			builtWorld = world;
			std::memcpy(builtOffset, hkOffset, sizeof(hkOffset));
			builtWorldOffset[0] = wox;
			builtWorldOffset[1] = woz;
		}

		{
			std::unordered_map<std::uint64_t, std::pair<bool, Bits>> take;
			bool                                                      clear = false;
			{
				std::scoped_lock lock(pendingLock);
				take.swap(pending);
				clear = pendingClear;
				pendingClear = false;
			}
			if (clear || !take.empty()) {
				RE::BSAutoWriteLock lock(static_cast<RE::hknpBSWorld*>(world)->m_worldLock);
				if (clear) {
					for (auto& [key, s] : sections) {
						Retire(world, s);
					}
					sections.clear();
				}
				for (auto& [key, entry] : take) {
					auto it = sections.find(key);
					if (!entry.first) {
						if (it != sections.end()) {
							Retire(world, it->second);
							sections.erase(it);
						}
						continue;
					}
					if (it == sections.end()) {
						Section s{};
						s.sx = std::int32_t(std::int64_t(key >> 42) << 43 >> 43);
						s.sy = std::int32_t(std::int64_t(key >> 21 & 0x1FFFFF) << 43 >> 43);
						s.sz = std::int32_t(std::int64_t(key & 0x1FFFFF) << 43 >> 43);
						it = sections.emplace(key, s).first;
					}
					if (!it->second.bodies.empty() && it->second.bits == entry.second) {
						continue;  // Minecraft re-sent the section (lighting, a neighbour) but its solid blocks didn't change
					}
					it->second.bits = entry.second;
					it->second.dirty = true;
				}
			}
		}

		// Build the dirty sections nearest the player first, a few per frame.
		std::vector<std::pair<double, Section*>> todo;
		for (auto& [key, s] : sections) {
			if (!s.dirty) {
				continue;
			}
			const double dx = s.sx * 16.0 + 8.0 - a_player.x, dy = s.sy * 16.0 + 8.0 - a_player.y, dz = s.sz * 16.0 + 8.0 - a_player.z;
			const double d2 = dx * dx + dy * dy + dz * dz;
			if (d2 <= double(kNearBlocks) * kNearBlocks) {
				todo.emplace_back(d2, &s);
			}
		}
		if (todo.empty()) {
			return;
		}
		std::sort(todo.begin(), todo.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		RE::BSAutoWriteLock lock(static_cast<RE::hknpBSWorld*>(world)->m_worldLock);
		if (templateWorld != world) {
			FindTemplate(world);
		}
		for (std::size_t i = 0; i < todo.size() && i < kSectionsPerFrame; ++i) {
			Build(world, hkOffset, *todo[i].second);
		}
		static std::uint64_t loggedAt = 0;
		if (statMade - loggedAt >= 50 || (loggedAt == 0 && statMade > 0)) {
			loggedAt = statMade;
			REX::INFO("block collision: {} Havok bodies for Minecraft blocks ({} made so far, {} sections known)", liveBodies, statMade, sections.size());
		}
	}
}
