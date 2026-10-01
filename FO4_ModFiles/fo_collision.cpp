#include "fo_collision.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef ERROR  // wingdi.h; clashes with REX::ERROR

#include <algorithm>
#include <array>
#include <cfloat>
#include <climits>
#include <cstddef>
#include <cstring>
#include <format>
#include <string>

namespace skycraft
{
	namespace
	{
		using namespace std::chrono_literals;

		constexpr int   kRadius = 5;  // regions around the player horizontally
		constexpr int   kBelow = 3;   // regions below the player
		constexpr int   kAbove = 2;   // regions above the player
		constexpr int   kGrid = Collision::kRegionSize * 8;  // voxels per region edge (64)
		constexpr auto  kRefreshNear = 2000ms;  // re-send regions next to the player this often (doors etc.)
		constexpr auto  kFrameBudget = 1500us;  // main-thread time per frame (more shows up as stutter)
		constexpr int   kMaxHarvestsPerFrame = 2;
		constexpr float kSteepMin = 0.1f;    // |n.y| below this is a wall: keep it fine-grained
		constexpr float kSteepMax = 0.643f;  // |n.y| below this (steeper than ~50 deg) gets block-coarsened
		constexpr std::size_t kMaxCachedTris = 3'000'000;
		constexpr std::size_t kMaxTrisPerJob = 400'000;

		// Blocks per Havok unit (Havok unit = 69.99125 game units, block = 70 game units).
		constexpr float kBlocksPerHavok = RE::HK_TO_BS_SCALE / static_cast<float>(proto::kUnitsPerBlock);

		bool Finite(const float* a_v, int a_n)
		{
			for (int i = 0; i < a_n; ++i) {
				if (!std::isfinite(a_v[i]) || std::fabs(a_v[i]) > 1.0e7f) {
					return false;
				}
			}
			return true;
		}

		void XfPoint(const float* a_xf, const float* a_p, float* a_out)
		{
			for (int i = 0; i < 3; ++i) {
				a_out[i] = a_xf[i] * a_p[0] + a_xf[4 + i] * a_p[1] + a_xf[8 + i] * a_p[2] + a_xf[12 + i];
			}
		}

		bool Overlaps(const float* a_lo, const float* a_hi, const float* b_lo, const float* b_hi)
		{
			return a_lo[0] <= b_hi[0] && a_hi[0] >= b_lo[0] && a_lo[1] <= b_hi[1] && a_hi[1] >= b_lo[1] && a_lo[2] <= b_hi[2] && a_hi[2] >= b_lo[2];
		}

		// Collision layers the player stands on or walks into.
		bool Included(int a_layer, bool a_static)
		{
			using L = RE::COL_LAYER;
			switch (static_cast<L>(a_layer)) {
			case L::kStairHelper:
			case L::kStatic:
			case L::kAnimStatic:
			case L::kTransparent:
			case L::kTransparentSmall:
			case L::kTransparentSmallAnim:
			case L::kTrees:
			case L::kProps:
			case L::kTerrain:
			case L::kGround:
			case L::kInvisibleWall:
				return true;
			case L::kClutter:
			case L::kClutterLarge:
			case L::kUnidentified:
			case L::kDebrisLarge:
				return a_static;  // wrecked cars, rubble piles, planks and the like that never move
			default:
				return false;
			}
		}

		// ---- triangle / box overlap (Akenine-Moller SAT), voxel units -------------------------
		inline void  Sub(const float* a, const float* b, float* o) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
		inline void  Cross(const float* a, const float* b, float* o)
		{
			o[0] = a[1] * b[2] - a[2] * b[1];
			o[1] = a[2] * b[0] - a[0] * b[2];
			o[2] = a[0] * b[1] - a[1] * b[0];
		}
		inline float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

		bool AxisTest(const float* v0, const float* v1, const float* v2, const float* axis, float h)
		{
			const float p0 = Dot(v0, axis), p1 = Dot(v1, axis), p2 = Dot(v2, axis);
			const float mn = std::min({ p0, p1, p2 }), mx = std::max({ p0, p1, p2 });
			const float r = h * (std::fabs(axis[0]) + std::fabs(axis[1]) + std::fabs(axis[2]));
			return !(mn > r || mx < -r);
		}

		bool TriBoxOverlap(const float* c, float h, const float* ta, const float* tb, const float* tc, const float* n)
		{
			float v0[3], v1[3], v2[3];
			Sub(ta, c, v0);
			Sub(tb, c, v1);
			Sub(tc, c, v2);
			for (int i = 0; i < 3; ++i) {
				const float mn = std::min({ v0[i], v1[i], v2[i] }), mx = std::max({ v0[i], v1[i], v2[i] });
				if (mn > h || mx < -h) {
					return false;
				}
			}
			const float d = Dot(n, v0);
			const float r = h * (std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]));
			if (std::fabs(d) > r) {
				return false;
			}
			float e[3][3];
			Sub(v1, v0, e[0]);
			Sub(v2, v1, e[1]);
			Sub(v0, v2, e[2]);
			static constexpr float kAxes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
			for (auto& edge : e) {
				for (auto& unit : kAxes) {
					float axis[3];
					Cross(edge, unit, axis);
					if (!AxisTest(v0, v1, v2, axis, h)) {
						return false;
					}
				}
			}
			return true;
		}

		std::uint64_t RegionKey(int a_x, int a_y, int a_z)
		{
			return (std::uint64_t(std::uint32_t(a_x) & 0x1FFFFF) << 42) | (std::uint64_t(std::uint32_t(a_y) & 0x1FFFFF) << 21) | (std::uint32_t(a_z) & 0x1FFFFF);
		}

		// ---- Havok access, guarded: shape internals are only partly known; never let a bad read
		// take the game down. These functions hold no C++ objects with destructors (SEH rule). ----

		constexpr unsigned long kExecuteHandler = 1;  // EXCEPTION_EXECUTE_HANDLER

		// hkGeometry as Havok 2014 lays it out: hkReferencedObject, then the vertices and triangles.
		struct HkTriangle
		{
			std::int32_t a, b, c, material;
		};

		struct alignas(16) HkGeometryMirror
		{
			std::uintptr_t                   vtbl;
			std::uint32_t                    memSizeAndRefCount;
			std::uint32_t                    pad;
			RE::hkArrayBase<RE::hkVector4f>  vertices;
			RE::hkArrayBase<HkTriangle>      triangles;
		};
		static_assert(offsetof(HkGeometryMirror, vertices) == 0x10);
		static_assert(offsetof(HkGeometryMirror, triangles) == 0x20);

		bool GuardedBuildGeometry(const RE::hknpShape* a_shape, const void* a_config, HkGeometryMirror* a_geom, std::int32_t* a_result)
		{
			__try {
				const auto r = a_shape->BuildSurfaceGeometry(
					*static_cast<const RE::hknpShape::BuildSurfaceGeometryConfig*>(a_config),
					reinterpret_cast<RE::hkGeometry*>(a_geom));
				std::int32_t value = 0;
				std::memcpy(&value, &r, sizeof(value));
				*a_result = value;
				return true;
			} __except (kExecuteHandler) {
				return false;
			}
		}

		bool GuardedAabb(const RE::hknpShape* a_shape, const RE::hkTransformf* a_xf, RE::hkAabb* a_out)
		{
			__try {
				a_shape->CalcAabb(*a_xf, *a_out);
				return true;
			} __except (kExecuteHandler) {
				return false;
			}
		}

		// A body slot can still point at a shape Fallout already freed (a cell unloading): calling
		// into it ran garbage code and crashed the game (2026-10-01 04:42). Only touch shapes whose
		// vtable lies inside Fallout4.exe.
		std::pair<std::uintptr_t, std::uintptr_t> GameImageRange()
		{
			static const auto range = [] {
				auto*       base = reinterpret_cast<const std::uint8_t*>(::GetModuleHandleW(nullptr));
				const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
				const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
				return std::pair{ reinterpret_cast<std::uintptr_t>(base), reinterpret_cast<std::uintptr_t>(base) + nt->OptionalHeader.SizeOfImage };
			}();
			return range;
		}

		bool ReadPointer(const void* a_at, std::uintptr_t* a_out)
		{
			__try {
				*a_out = *static_cast<const std::uintptr_t*>(a_at);
				return true;
			} __except (kExecuteHandler) {
				return false;
			}
		}

		bool ShapeLooksValid(const RE::hknpShape* a_shape)
		{
			const auto [lo, hi] = GameImageRange();
			const auto p = reinterpret_cast<std::uintptr_t>(a_shape);
			if (p < 0x10000 || p >= 0x00007FFFFFFFFFFFull || (p & 0xF) != 0) {
				return false;
			}
			std::uintptr_t vtbl = 0;
			if (!ReadPointer(a_shape, &vtbl)) {
				return false;
			}
			return vtbl >= lo && vtbl < hi && (vtbl & 7) == 0;
		}

		void GuardedAddRef(const RE::hknpShape* a_shape)
		{
			__try {
				const_cast<RE::hknpShape*>(a_shape)->AddReference();
			} __except (kExecuteHandler) {
			}
		}

		void GuardedRelease(const RE::hknpShape* a_shape)
		{
			if (!ShapeLooksValid(a_shape)) {
				return;
			}
			__try {
				const_cast<RE::hknpShape*>(a_shape)->RemoveReference();
			} __except (kExecuteHandler) {
			}
		}

		int GuardedType(const RE::hknpShape* a_shape)
		{
			__try {
				return static_cast<int>(a_shape->GetType());
			} __except (kExecuteHandler) {
				return -1;
			}
		}

		template <class T>
		void FreeHkArray(RE::hkArrayBase<T>& a_array)
		{
			if (a_array.m_data && (a_array.m_capacityAndFlags & RE::hkArrayBase<T>::kDontDeallocateFlag) == 0) {
				if (auto* allocator = RE::hkContainerHeapAllocator::GetSingleton()) {
					allocator->BufFree(a_array.m_data, a_array.capacity() * static_cast<std::int32_t>(sizeof(T)));
				}
			}
			a_array.m_data = nullptr;
			a_array.m_size = 0;
			a_array.m_capacityAndFlags = RE::hkArrayBase<T>::kDontDeallocateFlag;
		}

		const char* ShapeTypeName(int a_type)
		{
			static constexpr const char* kNames[] = { "Convex", "ConvexPolytope", "Sphere", "Capsule", "Triangle", "CompressedMesh",
				"ExternMesh", "StaticCompound", "DynamicCompound", "HeightField", "CompressedHeightField", "ScaledConvex", "Masked",
				"MaskedCompound", "LOD", "Dummy", "User0", "User1", "User2", "User3" };
			return a_type >= 0 && a_type < static_cast<int>(std::size(kNames)) ? kNames[a_type] : "?";
		}
	}

	Collision& Collision::Get()
	{
		static Collision instance;
		return instance;
	}

	void Collision::Start()
	{
		for (int dx = -kRadius; dx <= kRadius; ++dx) {
			for (int dz = -kRadius; dz <= kRadius; ++dz) {
				for (int dy = -kBelow; dy <= kAbove; ++dy) {
					offsets_.push_back({ dx, dy, dz });
				}
			}
		}
		std::ranges::sort(offsets_, {}, [](const auto& o) { return o[0] * o[0] + o[2] * o[2] + o[1] * o[1] * 2; });
		worker_ = std::thread([this] { WorkerLoop(); });
		worker_.detach();
	}

	void Collision::Reset(std::uint32_t a_epoch)
	{
		epoch_ = a_epoch;
		harvested_.clear();
		emptyRegions_.clear();
		regions_.clear();
		doubtful_.clear();
		offsetChosen_ = false;
		loggedGather_ = false;
		EvictGeoms(true);  // a new world: its shapes are different ones
		std::scoped_lock lock(mutex_);
		queue_.clear();
		Job job{};
		job.clear = true;
		job.epoch = a_epoch;
		queue_.push_back(std::move(job));
		cv_.notify_one();
	}

	void Collision::Refresh()
	{
		const int px = playerRegion_[0].load(), py = playerRegion_[1].load(), pz = playerRegion_[2].load();
		for (int dx = -2; dx <= 2; ++dx) {
			for (int dy = -2; dy <= 1; ++dy) {
				for (int dz = -2; dz <= 2; ++dz) {
					harvested_.erase(RegionKey(px + dx, py + dy, pz + dz));
				}
			}
		}
	}

	long long Collision::TrianglesAt(const McVec& a_p) const
	{
		const int  rx = static_cast<int>(std::floor(a_p.x / kRegionSize));
		const int  ry = static_cast<int>(std::floor(a_p.y / kRegionSize));
		const int  rz = static_cast<int>(std::floor(a_p.z / kRegionSize));
		const auto it = regions_.find(RegionKey(rx, ry, rz));
		return it == regions_.end() ? -1 : static_cast<long long>(it->second.tris);
	}

	void Collision::HkPointToMc(const float* a_hk, float* a_out) const
	{
		const float x = a_hk[0] + offset_[0], y = a_hk[1] + offset_[1], z = a_hk[2] + offset_[2];
		a_out[0] = float(double(x * kBlocksPerHavok) + g_worldOffset.x.load(std::memory_order_relaxed));
		a_out[1] = z * kBlocksPerHavok;
		a_out[2] = float(double(-y * kBlocksPerHavok) + g_worldOffset.z.load(std::memory_order_relaxed));
	}

	void Collision::Update(RE::TESObjectCELL* a_cell, const McVec& a_playerMc)
	{
		auto* bhk = a_cell ? a_cell->GetbhkWorld() : nullptr;
		auto* world = bhk ? bhk->m_worldNP.get() : nullptr;
		if (!world) {
			return;
		}

		const int  prx = static_cast<int>(std::floor(a_playerMc.x / kRegionSize));
		const int  pry = static_cast<int>(std::floor(a_playerMc.y / kRegionSize));
		const int  prz = static_cast<int>(std::floor(a_playerMc.z / kRegionSize));
		const auto now = Clock::now();
		playerRegion_[0] = prx;
		playerRegion_[1] = pry;
		playerRegion_[2] = prz;

		int  done = 0;
		bool gathered = false;
		for (const auto& o : offsets_) {
			const int  rx = prx + o[0], ry = pry + o[1], rz = prz + o[2];
			const auto key = RegionKey(rx, ry, rz);
			const auto it = harvested_.find(key);
			const bool isNear = std::abs(o[0]) <= 1 && std::abs(o[2]) <= 1 && o[1] >= -1 && o[1] <= 0;
			// Ground that wasn't there yet (Fallout still streaming the cell in) is looked for again,
			// under and around the player, so running into new areas doesn't fall through.
			const bool emptyBelow = std::abs(o[0]) <= 2 && std::abs(o[2]) <= 2 && o[1] >= -2 && o[1] <= 0 && emptyRegions_.contains(key);
			const bool doubtful = it != harvested_.end() && doubtful_.contains(key) && now - it->second > 300ms;
			if (it != harvested_.end() && !(isNear && now - it->second > kRefreshNear) && !(emptyBelow && now - it->second > 2s) && !doubtful) {
				continue;
			}
			// Under and right around the player: also Fallout's own picks (before taking the world
			// lock; a pick takes it itself).
			std::vector<Tri> sheet;
			if (isNear) {
				BuildSheet(a_cell, rx, ry, rz, sheet);
			}
			RE::BSAutoLock<RE::BSReadWriteLock, RE::BSAutoLockReadLockPolicy> lock(world->m_worldLock);
			if (!gathered) {
				GatherBodies(bhk, world, a_playerMc);
				gathered = true;
			}
			Harvest(rx, ry, rz, isNear ? &sheet : nullptr);
			harvested_[key] = now;
			if (++done >= kMaxHarvestsPerFrame || Clock::now() - now > kFrameBudget) {
				break;
			}
		}

		// Bound memory: drop bookkeeping for far-away regions, and shapes not used for a while.
		if (harvested_.size() > offsets_.size() * 4) {
			harvested_.clear();
		}
		if (now - lastEvict_ > 10s) {
			lastEvict_ = now;
			EvictGeoms(false);
		}

		if (now - statsAt_ > 10s) {
			statsAt_ = now;
			if (statRegions_ > 0) {
				REX::INFO("collision: {} regions harvested ({} triangles, {} held back while their ground was missing, {} ground sheets with {} triangles; queue peak {}, {} replaced); sent {} blocks, {} triangles; {} messages dropped; {} shapes cached ({} triangles)",
					statRegions_, statTris_, statHeld_, statSheets_, statSheetTris_, statQueuePeak_, statReplaced_, statSentBlocks_.load(), statSentTris_.load(), statDropped_.load(), geoms_.size(), geomTris_);
			}
			statRegions_ = statTris_ = statHeld_ = statSheets_ = statSheetTris_ = 0;
			{
				std::scoped_lock lock(mutex_);
				statQueuePeak_ = queue_.size();
				statReplaced_ = 0;
			}
			statSentBlocks_ = 0;
			statSentTris_ = 0;
		}
	}

	void Collision::GatherBodies(RE::bhkWorld* a_bhk, RE::hknpBSWorld* a_world, const McVec& a_player)
	{
		bodies_.clear();

		// Havok's origin: positions may be stored relative to it. Which way round (if at all) is
		// found by checking which reading puts terrain under the player.
		const float origin[3] = { a_bhk->m_origin.x, a_bhk->m_origin.y, a_bhk->m_origin.z };
		if (origin[0] != originSeen_[0] || origin[1] != originSeen_[1] || origin[2] != originSeen_[2]) {
			std::memcpy(originSeen_, origin, sizeof(origin));
			offsetChosen_ = false;
		}

		auto&     bm = a_world->m_bodyManager;
		const int count = std::min<int>(bm.m_bodies.size(), static_cast<int>(bm.m_peakBodyIndex) + 1);

		struct Raw
		{
			const RE::hknpBody* body;
			bool                helper;
		};
		std::vector<Raw> raw;
		raw.reserve(1024);
		int layerCounts[64]{};
		int total = 0;
		for (int i = 0; i < count; ++i) {
			const RE::hknpBody& body = bm.m_bodies.data()[i];
			std::uint32_t       id = 0;
			std::memcpy(&id, &body.m_id, sizeof(id));
			if (id != static_cast<std::uint32_t>(i) || !body.m_shape) {
				continue;
			}
			if (!a_bhk->IsBodyAdded(body.m_id) || !ShapeLooksValid(body.m_shape)) {
				continue;  // a freed or not-yet-added body
			}
			++total;
			std::int32_t flags = 0;
			std::memcpy(&flags, &body.m_flags, sizeof(flags));
			const bool isStatic = (flags & 0x1) != 0;
			const bool isKeyframed = (flags & 0x4) != 0;
			const bool isDynamic = (flags & 0x2) != 0;
			if ((flags & 0x100) != 0 || (isDynamic && !isKeyframed)) {
				continue;  // doesn't collide, or a loose object that moves around
			}
			const int layer = static_cast<int>(body.m_collisionFilterInfo & 0x7F);
			++layerCounts[layer & 63];
			if (!Included(layer, isStatic)) {
				continue;
			}
			raw.push_back({ &body, layer == static_cast<int>(RE::COL_LAYER::kStairHelper) });
		}

		// Pick the origin reading: the candidate with the most bodies around the player's column.
		if (!offsetChosen_) {
			const float candidates[3][3] = { { 0, 0, 0 }, { origin[0], origin[1], origin[2] }, { -origin[0], -origin[1], -origin[2] } };
			int         best = 0, bestHits = -1;
			int         hitsPer[3]{};
			for (int c = 0; c < 3; ++c) {
				if (c > 0 && origin[0] == 0.0f && origin[1] == 0.0f && origin[2] == 0.0f) {
					break;
				}
				std::memcpy(offset_, candidates[c], sizeof(offset_));
				int hits = 0;
				for (const auto& r : raw) {
					RE::hkAabb box;
					if (!GuardedAabb(r.body->m_shape, &r.body->m_transform, &box)) {
						continue;
					}
					float lo[3], hi[3];
					const float mn[3] = { box.min.x, box.min.y, box.min.z }, mx[3] = { box.max.x, box.max.y, box.max.z };
					float a[3], b[3];
					HkPointToMc(mn, a);
					HkPointToMc(mx, b);
					for (int k = 0; k < 3; ++k) {
						lo[k] = std::min(a[k], b[k]);
						hi[k] = std::max(a[k], b[k]);
					}
					if (a_player.x >= lo[0] - 1 && a_player.x <= hi[0] + 1 && a_player.z >= lo[2] - 1 && a_player.z <= hi[2] + 1 &&
						a_player.y >= lo[1] - 64 && a_player.y <= hi[1] + 64) {
						++hits;
					}
				}
				hitsPer[c] = hits;
				if (hits > bestHits) {
					bestHits = hits;
					best = c;
				}
			}
			std::memcpy(offset_, candidates[best], sizeof(offset_));
			offsetChosen_ = true;
			REX::INFO("collision: Havok origin ({:.2f}, {:.2f}, {:.2f}); bodies around the player: as-is {}, +origin {}, -origin {} -> using {}",
				origin[0], origin[1], origin[2], hitsPer[0], hitsPer[1], hitsPer[2], best == 0 ? "as-is" : (best == 1 ? "+origin" : "-origin"));
		}

		for (const auto& r : raw) {
			RE::hkAabb box;
			if (!GuardedAabb(r.body->m_shape, &r.body->m_transform, &box)) {
				continue;
			}
			Body b{};
			b.shape = r.body->m_shape;
			std::memcpy(b.xf, &r.body->m_transform, sizeof(b.xf));
			b.helper = r.helper;
			const float mn[3] = { box.min.x, box.min.y, box.min.z }, mx[3] = { box.max.x, box.max.y, box.max.z };
			float       p[3], q[3];
			HkPointToMc(mn, p);
			HkPointToMc(mx, q);
			for (int k = 0; k < 3; ++k) {
				b.lo[k] = std::min(p[k], q[k]);
				b.hi[k] = std::max(p[k], q[k]);
			}
			if (Finite(b.xf, 16) && Finite(b.lo, 3) && Finite(b.hi, 3)) {
				bodies_.push_back(b);
			}
		}

		if (!loggedGather_) {
			loggedGather_ = true;
			std::string layers;
			for (int l = 0; l < 64; ++l) {
				if (layerCounts[l]) {
					layers += std::format(" {}:{}", l, layerCounts[l]);
				}
			}
			REX::INFO("collision: {} bodies in the world, {} used; by layer:{}", total, bodies_.size(), layers);
		}
	}

	const Collision::Geom* Collision::GetGeom(const RE::hknpShape* a_shape)
	{
		const auto now = Clock::now();
		if (auto it = geoms_.find(a_shape); it != geoms_.end()) {
			it->second.used = now;
			return it->second.ok ? &it->second : nullptr;
		}

		Geom geom{};
		geom.used = now;
		const int type = GuardedType(a_shape);

		static const std::uintptr_t geometryVtbl = REL::Relocation<std::uintptr_t>{ RE::VTABLE::hkGeometry[0] }.address();
		HkGeometryMirror            g{};
		g.vtbl = geometryVtbl;
		g.memSizeAndRefCount = 0xFFFF0001u;  // "not heap allocated", one reference
		alignas(16) std::uint8_t config[64]{};  // BuildSurfaceGeometryConfig: all defaults
		std::int32_t             result = 1;
		const bool               ran = type >= 0 && GuardedBuildGeometry(a_shape, config, &g, &result);

		const int numVerts = g.vertices.m_data ? g.vertices.m_size : 0;
		const int numTris = g.triangles.m_data ? g.triangles.m_size : 0;
		if (ran && numTris > 0 && numVerts > 0) {
			geom.verts.resize(static_cast<std::size_t>(numVerts) * 3);
			for (int v = 0; v < numVerts; ++v) {
				const auto& p = g.vertices.m_data[v];
				geom.verts[v * 3 + 0] = p.x;
				geom.verts[v * 3 + 1] = p.y;
				geom.verts[v * 3 + 2] = p.z;
			}
			geom.idx.reserve(static_cast<std::size_t>(numTris) * 3);
			for (int t = 0; t < numTris; ++t) {
				const auto& tri = g.triangles.m_data[t];
				if (tri.a < 0 || tri.b < 0 || tri.c < 0 || tri.a >= numVerts || tri.b >= numVerts || tri.c >= numVerts) {
					continue;
				}
				geom.idx.push_back(static_cast<std::uint32_t>(tri.a));
				geom.idx.push_back(static_cast<std::uint32_t>(tri.b));
				geom.idx.push_back(static_cast<std::uint32_t>(tri.c));
			}
			geom.ok = !geom.idx.empty() && Finite(geom.verts.data(), static_cast<int>(geom.verts.size()));
		}
		FreeHkArray(g.vertices);
		FreeHkArray(g.triangles);

		if (geom.ok) {
			GuardedAddRef(a_shape);  // keep it (and its address) alive while cached
			geomTris_ += geom.idx.size() / 3;
			if (loggedTypes_.insert(type).second) {
				REX::INFO("collision: shape type {} ({}) triangulated: {} triangles", type, ShapeTypeName(type), geom.idx.size() / 3);
			}
		} else if (failedTypes_.insert(type).second) {
			REX::WARN("collision: shape type {} ({}) could not be triangulated (ran={}, result={}, verts={}, tris={}); it is skipped",
				type, ShapeTypeName(type), ran, result, numVerts, numTris);
		}

		auto& slot = geoms_[a_shape];
		slot = std::move(geom);
		if (geomTris_ > kMaxCachedTris) {
			EvictGeoms(false);
		}
		return slot.ok ? &slot : nullptr;
	}

	void Collision::EvictGeoms(bool a_all)
	{
		const auto now = Clock::now();
		const auto maxAge = geomTris_ > kMaxCachedTris ? 5s : 60s;
		for (auto it = geoms_.begin(); it != geoms_.end();) {
			if (a_all || now - it->second.used > maxAge) {
				if (it->second.ok) {
					geomTris_ -= it->second.idx.size() / 3;
					GuardedRelease(it->first);
				}
				it = geoms_.erase(it);
			} else {
				++it;
			}
		}
	}

	void Collision::BuildSheet(RE::TESObjectCELL* a_cell, int a_rx, int a_ry, int a_rz, std::vector<Tri>& a_out)
	{
		constexpr int N = kRegionSize + 1;  // grid points per edge, 1 block apart
		float         h[N][N];
		bool          ok[N][N];
		const double  top = double((a_ry + 1) * kRegionSize) + 0.5, bottom = double(a_ry * kRegionSize) - 0.25;
		for (int i = 0; i < N; ++i) {
			for (int j = 0; j < N; ++j) {
				const double x = double(a_rx * kRegionSize + i), z = double(a_rz * kRegionSize + j);
				RE::NiPoint3 hit{};
				ok[i][j] = PickGroundAt(a_cell, McToGame(x, top, z), McToGame(x, bottom, z), hit);
				h[i][j] = ok[i][j] ? float(double(hit.z) / proto::kUnitsPerBlock) : 0.0f;
			}
		}
		++statSheets_;
		constexpr float kFlat = 0.35f;  // blocks: a cell whose corners differ more is a wall or an edge
		for (int i = 0; i + 1 < N; ++i) {
			for (int j = 0; j + 1 < N; ++j) {
				const int ci[4] = { i, i + 1, i + 1, i }, cj[4] = { j, j, j + 1, j + 1 };
				float     c[4];
				int       valid = 0;
				for (int k = 0; k < 4; ++k) {
					c[k] = h[ci[k]][cj[k]];
					valid += ok[ci[k]][cj[k]] ? 1 : 0;
				}
				if (valid < 3) {
					continue;
				}
				// One corner missing or down in a crack: bridge it at the others' height, as
				// Fallout's player (wider than a crack) would.
				float hi = -1e9f, lo = 1e9f;
				int   lowest = -1;
				for (int k = 0; k < 4; ++k) {
					if (!ok[ci[k]][cj[k]]) {
						lowest = k;
						continue;
					}
					if (c[k] < lo) {
						lo = c[k];
						if (valid == 4) {
							lowest = k;
						}
					}
					hi = std::max(hi, c[k]);
				}
				if (hi - lo > kFlat) {
					// Allow exactly one corner well below the rest (a crack, a pothole edge).
					float hi3 = -1e9f, lo3 = 1e9f;
					for (int k = 0; k < 4; ++k) {
						if (k != lowest && ok[ci[k]][cj[k]]) {
							hi3 = std::max(hi3, c[k]);
							lo3 = std::min(lo3, c[k]);
						}
					}
					if (hi3 - lo3 > kFlat || valid < 4) {
						continue;
					}
				}
				if (lowest >= 0 && (!ok[ci[lowest]][cj[lowest]] || hi - c[lowest] > kFlat)) {
					float sum = 0.0f;
					int   n = 0;
					for (int k = 0; k < 4; ++k) {
						if (k != lowest && ok[ci[k]][cj[k]]) {
							sum += c[k];
							++n;
						}
					}
					c[lowest] = n ? sum / float(n) : c[lowest];
				}
				float p[4][3];
				for (int k = 0; k < 4; ++k) {
					p[k][0] = float(a_rx * kRegionSize + ci[k]);
					p[k][1] = c[k];
					p[k][2] = float(a_rz * kRegionSize + cj[k]);
				}
				Tri t1{}, t2{};
				const int a1[3] = { 0, 1, 2 }, a2[3] = { 0, 2, 3 };
				for (int v = 0; v < 3; ++v) {
					std::memcpy(t1.v + v * 3, p[a1[v]], sizeof(float) * 3);
					std::memcpy(t2.v + v * 3, p[a2[v]], sizeof(float) * 3);
				}
				a_out.push_back(t1);
				a_out.push_back(t2);
				statSheetTris_ += 2;
			}
		}
	}

	std::string Collision::Describe(const RE::hknpBody* a_body, const McVec& a_point)
	{
		if (!a_body) {
			return "pick gave no body";
		}
		const RE::hknpShape* shape = a_body->m_shape;
		const Body*          mine = nullptr;
		for (const auto& b : bodies_) {
			if (b.shape == shape && std::fabs(b.xf[12] - a_body->m_transform.m_translation.x) < 0.01f) {
				mine = &b;
				break;
			}
		}
		std::uint32_t flags = 0;
		std::memcpy(&flags, &a_body->m_flags, sizeof(flags));
		const int layer = static_cast<int>(a_body->m_collisionFilterInfo & 0x7F);
		std::string out = std::format("hit body layer {} flags {:X} shape type {}: ", layer, flags, GuardedType(shape));
		if (!mine) {
			return out + "NOT in our body list (filtered out or not gathered) -> Minecraft never got it";
		}
		const Geom* geom = GetGeom(shape);
		if (!geom) {
			return out + "in our list but its shape did not triangulate";
		}
		// Triangles of that body over the point (vertical projection), and their heights.
		int   over = 0;
		float best = 1e9f, bestH = 0.0f;
		for (std::size_t t = 0; t + 2 < geom->idx.size(); t += 3) {
			float v[3][3];
			for (int k = 0; k < 3; ++k) {
				float w[3];
				XfPoint(mine->xf, &geom->verts[geom->idx[t + k] * 3], w);
				HkPointToMc(w, v[k]);
			}
			const float d1 = (a_point.x - v[1][0]) * (v[0][2] - v[1][2]) - (v[0][0] - v[1][0]) * (a_point.z - v[1][2]);
			const float d2 = (a_point.x - v[2][0]) * (v[1][2] - v[2][2]) - (v[1][0] - v[2][0]) * (a_point.z - v[2][2]);
			const float d3 = (a_point.x - v[0][0]) * (v[2][2] - v[0][2]) - (v[2][0] - v[0][0]) * (a_point.z - v[0][2]);
			const bool  neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
			if (neg && pos) {
				continue;
			}
			++over;
			const float hy = (v[0][1] + v[1][1] + v[2][1]) / 3.0f;
			if (std::fabs(hy - float(a_point.y)) < best) {
				best = std::fabs(hy - float(a_point.y));
				bestH = hy;
			}
		}
		return out + std::format("in our list, {} triangles, {} over the point (nearest at y {:.2f}, pick y {:.2f})", geom->idx.size() / 3, over, bestH, a_point.y);
	}

	bool Collision::Harvest(int a_rx, int a_ry, int a_rz, std::vector<Tri>* a_sheet)
	{
		Job job{};
		job.rx = a_rx;
		job.ry = a_ry;
		job.rz = a_rz;
		job.epoch = epoch_.load();
		const float margin = 0.25f;
		const float lo[3] = { float(a_rx * kRegionSize) - margin, float(a_ry * kRegionSize) - margin, float(a_rz * kRegionSize) - margin };
		const float hi[3] = { float((a_rx + 1) * kRegionSize) + margin, float((a_ry + 1) * kRegionSize) + margin, float((a_rz + 1) * kRegionSize) + margin };

		// The region's corners in Havok world space (inverse of HkPointToMc).
		const float k = kBlocksPerHavok;
		float       hkCorners[8][3];
		for (int c = 0; c < 8; ++c) {
			// Minus this world's place in Minecraft (fo_worlds.cpp).
			const float mx = float(double((c & 1) ? hi[0] : lo[0]) - g_worldOffset.x.load(std::memory_order_relaxed));
			const float my = (c & 2) ? hi[1] : lo[1];
			const float mz = float(double((c & 4) ? hi[2] : lo[2]) - g_worldOffset.z.load(std::memory_order_relaxed));
			hkCorners[c][0] = mx / k - offset_[0];
			hkCorners[c][1] = -mz / k - offset_[1];
			hkCorners[c][2] = my / k - offset_[2];
		}

		for (const auto& body : bodies_) {
			if (!Overlaps(body.lo, body.hi, lo, hi)) {
				continue;
			}
			const Geom* geom = GetGeom(body.shape);
			if (!geom) {
				continue;
			}
			// Region box in the shape's own space: R^T (p - t) of the 8 corners.
			float llo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, lhi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
			for (const auto& p : hkCorners) {
				const float d[3] = { p[0] - body.xf[12], p[1] - body.xf[13], p[2] - body.xf[14] };
				for (int i = 0; i < 3; ++i) {
					const float v = body.xf[i * 4] * d[0] + body.xf[i * 4 + 1] * d[1] + body.xf[i * 4 + 2] * d[2];
					llo[i] = std::min(llo[i], v);
					lhi[i] = std::max(lhi[i], v);
				}
			}
			auto&             out = body.helper ? job.helperTris : job.tris;
			const auto&       verts = geom->verts;
			const std::size_t n = geom->idx.size();
			for (std::size_t t = 0; t + 2 < n && out.size() < kMaxTrisPerJob; t += 3) {
				const float* a = &verts[geom->idx[t] * 3];
				const float* b = &verts[geom->idx[t + 1] * 3];
				const float* c = &verts[geom->idx[t + 2] * 3];
				if (std::max({ a[0], b[0], c[0] }) < llo[0] || std::min({ a[0], b[0], c[0] }) > lhi[0] ||
					std::max({ a[1], b[1], c[1] }) < llo[1] || std::min({ a[1], b[1], c[1] }) > lhi[1] ||
					std::max({ a[2], b[2], c[2] }) < llo[2] || std::min({ a[2], b[2], c[2] }) > lhi[2]) {
					continue;
				}
				Tri tri{};
				const float* src[3] = { a, b, c };
				for (int v = 0; v < 3; ++v) {
					float w[3];
					XfPoint(body.xf, src[v], w);
					HkPointToMc(w, tri.v + v * 3);
				}
				out.push_back(tri);
			}
		}

		if (a_sheet && !a_sheet->empty()) {
			job.tris.insert(job.tris.end(), a_sheet->begin(), a_sheet->end());
		}
		// A region that had ground and now comes back with (almost) none is far more likely Fallout
		// re-streaming its bodies this frame than the ground really vanishing; sending it would open
		// a hole under the player. Keep Minecraft's copy until the result has held for a while.
		const auto        key = RegionKey(a_rx, a_ry, a_rz);
		const std::size_t n = job.tris.size() + job.helperTris.size();
		const auto        now = Clock::now();
		auto&             rec = regions_[key];
		if (rec.tris >= 8 && n * 4 < rec.tris) {
			if (rec.shrinkSince == Clock::time_point{}) {
				rec.shrinkSince = now;
			}
			if (now - rec.shrinkSince < 2s) {
				doubtful_.insert(key);
				++statHeld_;
				return false;
			}
		}
		rec.tris = n;
		rec.shrinkSince = {};
		doubtful_.erase(key);
		if (regions_.size() > 50000) {
			regions_.clear();
			doubtful_.clear();
		}

		if (job.tris.empty() && job.helperTris.empty()) {
			emptyRegions_.insert(key);
		} else {
			emptyRegions_.erase(key);
		}
		if (emptyRegions_.size() > 20000) {
			emptyRegions_.clear();
		}
		++statRegions_;
		statTris_ += job.tris.size() + job.helperTris.size();
		std::scoped_lock lock(mutex_);
		// A newer copy of a region still waiting replaces the old one (the worker never sends stale
		// data, and the queue can't pile up behind Minecraft).
		auto same = std::find_if(queue_.begin(), queue_.end(), [&](const Job& a_job) {
			return !a_job.clear && a_job.rx == job.rx && a_job.ry == job.ry && a_job.rz == job.rz;
		});
		if (same != queue_.end()) {
			*same = std::move(job);
			++statReplaced_;
		} else {
			queue_.push_back(std::move(job));
		}
		statQueuePeak_ = std::max<std::uint64_t>(statQueuePeak_, queue_.size());
		cv_.notify_one();
		return true;
	}

	// ---- worker -----------------------------------------------------------------------------

	void Collision::WorkerLoop()
	{
		while (true) {
			Job job;
			{
				std::unique_lock lock(mutex_);
				cv_.wait(lock, [this] { return !queue_.empty(); });
				// The region nearest the player first (a clear always goes first, in order).
				auto best = queue_.begin();
				if (!best->clear) {
					const int px = playerRegion_[0].load(), py = playerRegion_[1].load(), pz = playerRegion_[2].load();
					int       bestD = INT_MAX;
					for (auto it = queue_.begin(); it != queue_.end(); ++it) {
						if (it->clear) {
							best = it;
							break;
						}
						const int d = std::abs(it->rx - px) + std::abs(it->ry - py) * 2 + std::abs(it->rz - pz);
						if (d < bestD) {
							bestD = d;
							best = it;
						}
					}
				}
				job = std::move(*best);
				queue_.erase(best);
			}
			try {
				if (job.clear) {
					std::vector<std::uint8_t> payload(4);
					std::memcpy(payload.data(), &job.epoch, 4);
					Send(payload, proto::kColClear);
				} else if (job.epoch == epoch_.load()) {
					SendTriangles(job);
					Voxelize(job);
				}
			} catch (const std::exception& e) {
				REX::ERROR("collision worker: {}", e.what());
			}
		}
	}

	void Collision::Send(const std::vector<std::uint8_t>& a_payload, proto::ColType a_type)
	{
		for (int attempt = 0; attempt < 2000; ++attempt) {
			if (link::WriteCollision(a_type, a_payload.data(), static_cast<std::uint32_t>(a_payload.size()))) {
				return;
			}
			std::this_thread::sleep_for(1ms);  // ring full: Minecraft is behind (or not running)
		}
		++statDropped_;
	}

	void Collision::SendTriangles(const Job& a_job)
	{
		const float lo[3] = { float(a_job.rx * kRegionSize) - 0.5f, float(a_job.ry * kRegionSize) - 0.5f, float(a_job.rz * kRegionSize) - 0.5f };
		const float hi[3] = { lo[0] + kRegionSize + 1.0f, lo[1] + kRegionSize + 1.0f, lo[2] + kRegionSize + 1.0f };
		std::vector<proto::ColTri> out;
		out.reserve(a_job.tris.size() + a_job.helperTris.size());
		auto add = [&](const Tri& a_tri, std::uint32_t a_flags) {
			float tlo[3], thi[3];
			for (int k = 0; k < 3; ++k) {
				tlo[k] = std::min({ a_tri.v[k], a_tri.v[3 + k], a_tri.v[6 + k] });
				thi[k] = std::max({ a_tri.v[k], a_tri.v[3 + k], a_tri.v[6 + k] });
			}
			if (Overlaps(tlo, thi, lo, hi) && Finite(a_tri.v, 9)) {
				proto::ColTri t{};
				std::memcpy(t.v, a_tri.v, sizeof(t.v));
				t.flags = a_flags;
				out.push_back(t);
			}
		};
		for (const auto& t : a_job.tris) {
			add(t, 0);
		}
		for (const auto& t : a_job.helperTris) {
			add(t, proto::kTriStairHelper);
		}
		proto::ColRegion header{};
		header.minX = a_job.rx * kRegionSize;
		header.minY = a_job.ry * kRegionSize;
		header.minZ = a_job.rz * kRegionSize;
		header.maxX = header.minX + kRegionSize - 1;
		header.maxY = header.minY + kRegionSize - 1;
		header.maxZ = header.minZ + kRegionSize - 1;
		header.epoch = a_job.epoch;
		header.count = static_cast<std::uint32_t>(out.size());
		std::vector<std::uint8_t> payload(sizeof(header) + out.size() * sizeof(proto::ColTri));
		std::memcpy(payload.data(), &header, sizeof(header));
		if (!out.empty()) {
			std::memcpy(payload.data() + sizeof(header), out.data(), out.size() * sizeof(proto::ColTri));
		}
		Send(payload, proto::kColTris);
		statSentTris_ += out.size();
	}

	void Collision::Voxelize(const Job& a_job)
	{
		constexpr int              G = kGrid;
		std::vector<std::uint64_t> solid(G * G, 0), steep(G * G, 0);
		auto set = [&](std::vector<std::uint64_t>& a_grid, int x, int y, int z) { a_grid[y * G + z] |= 1ull << x; };

		const float ox = float(a_job.rx * kRegionSize), oy = float(a_job.ry * kRegionSize), oz = float(a_job.rz * kRegionSize);
		auto toVoxel = [&](const float* a_mc, float* a_out) {
			a_out[0] = (a_mc[0] - ox) * 8.0f;
			a_out[1] = (a_mc[1] - oy) * 8.0f;
			a_out[2] = (a_mc[2] - oz) * 8.0f;
		};
		auto clampLo = [](float v) { return std::clamp(static_cast<int>(std::floor(v)), 0, G - 1); };
		auto clampHi = [](float v) { return std::clamp(static_cast<int>(std::ceil(v)) - 1, 0, G - 1); };

		// Triangles: plane-guided SAT test so big triangles cost O(area) instead of O(volume).
		for (const auto& tri : a_job.tris) {
			float a[3], b[3], c[3];
			toVoxel(tri.v, a);
			toVoxel(tri.v + 3, b);
			toVoxel(tri.v + 6, c);
			float lo[3], hi[3];
			for (int i = 0; i < 3; ++i) {
				lo[i] = std::min({ a[i], b[i], c[i] });
				hi[i] = std::max({ a[i], b[i], c[i] });
			}
			if (hi[0] < 0 || hi[1] < 0 || hi[2] < 0 || lo[0] > G || lo[1] > G || lo[2] > G) {
				continue;
			}
			float e1[3], e2[3], n[3];
			Sub(b, a, e1);
			Sub(c, a, e2);
			Cross(e1, e2, n);
			const float len = std::sqrt(Dot(n, n));
			if (len < 1e-9f) {
				continue;
			}
			n[0] /= len, n[1] /= len, n[2] /= len;
			const float ny = std::fabs(n[1]);
			auto&       grid = (ny >= kSteepMax || ny < kSteepMin) ? solid : steep;

			int dom = 0;
			if (std::fabs(n[1]) > std::fabs(n[dom])) dom = 1;
			if (std::fabs(n[2]) > std::fabs(n[dom])) dom = 2;
			const int   u = (dom + 1) % 3, v = (dom + 2) % 3;
			const float d = Dot(n, a);
			const float r = 0.5f * (std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]));
			const int   iu0 = clampLo(lo[u]), iu1 = clampHi(hi[u]), iv0 = clampLo(lo[v]), iv1 = clampHi(hi[v]);
			const int   id0 = clampLo(lo[dom]), id1 = clampHi(hi[dom]);
			for (int iu = iu0; iu <= iu1; ++iu) {
				for (int iv = iv0; iv <= iv1; ++iv) {
					const float cu = iu + 0.5f, cv = iv + 0.5f;
					const float s0 = (d - r - n[u] * cu - n[v] * cv) / n[dom];
					const float s1 = (d + r - n[u] * cu - n[v] * cv) / n[dom];
					const int   a0 = std::max(id0, static_cast<int>(std::floor(std::min(s0, s1) - 0.5f)));
					const int   a1 = std::min(id1, static_cast<int>(std::ceil(std::max(s0, s1) - 0.5f)));
					for (int id = a0; id <= a1; ++id) {
						float cen[3];
						cen[dom] = id + 0.5f;
						cen[u] = cu;
						cen[v] = cv;
						if (TriBoxOverlap(cen, 0.5f, a, b, c, n)) {
							int p[3];
							p[dom] = id, p[u] = iu, p[v] = iv;
							set(grid, p[0], p[1], p[2]);
						}
					}
				}
			}
		}

		// Steep (50-84 degree) surfaces: snap to whole-block footprints so the risers between
		// neighbouring columns exceed Minecraft's 0.6 step height. Its own step-up/jump rules then
		// decide what is climbable, like a cliff made of blocks.
		for (int by = 0; by < kRegionSize; ++by) {
			for (int bz = 0; bz < kRegionSize; ++bz) {
				for (int bx = 0; bx < kRegionSize; ++bx) {
					const std::uint64_t xmask = 0xFFull << (bx * 8);
					int                 minY = 99, maxY = -1;
					for (int y = by * 8; y < by * 8 + 8; ++y) {
						for (int z = bz * 8; z < bz * 8 + 8; ++z) {
							if (steep[y * G + z] & xmask) {
								minY = std::min(minY, y);
								maxY = std::max(maxY, y);
							}
						}
					}
					if (maxY < 0) {
						continue;
					}
					for (int y = minY; y <= maxY; ++y) {
						for (int z = bz * 8; z < bz * 8 + 8; ++z) {
							solid[y * G + z] |= xmask;
						}
					}
				}
			}
		}

		std::vector<proto::ColBlock> blocks;
		blocks.reserve(64);
		for (int by = 0; by < kRegionSize; ++by) {
			for (int bz = 0; bz < kRegionSize; ++bz) {
				for (int bx = 0; bx < kRegionSize; ++bx) {
					proto::ColBlock blk{};
					bool            any = false;
					for (int sy = 0; sy < 8; ++sy) {
						std::uint64_t layer = 0;
						for (int sz = 0; sz < 8; ++sz) {
							const auto row = (solid[(by * 8 + sy) * G + (bz * 8 + sz)] >> (bx * 8)) & 0xFF;
							layer |= row << (sz * 8);
						}
						blk.bits[sy] = layer;
						any |= layer != 0;
					}
					if (any) {
						blk.x = a_job.rx * kRegionSize + bx;
						blk.y = a_job.ry * kRegionSize + by;
						blk.z = a_job.rz * kRegionSize + bz;
						blocks.push_back(blk);
					}
				}
			}
		}

		proto::ColRegion header{};
		header.minX = a_job.rx * kRegionSize;
		header.minY = a_job.ry * kRegionSize;
		header.minZ = a_job.rz * kRegionSize;
		header.maxX = header.minX + kRegionSize - 1;
		header.maxY = header.minY + kRegionSize - 1;
		header.maxZ = header.minZ + kRegionSize - 1;
		header.epoch = a_job.epoch;
		header.count = static_cast<std::uint32_t>(blocks.size());
		std::vector<std::uint8_t> payload(sizeof(header) + blocks.size() * sizeof(proto::ColBlock));
		std::memcpy(payload.data(), &header, sizeof(header));
		if (!blocks.empty()) {
			std::memcpy(payload.data() + sizeof(header), blocks.data(), blocks.size() * sizeof(proto::ColBlock));
		}
		Send(payload, proto::kColRegion);
		statSentBlocks_ += blocks.size();
	}
}
