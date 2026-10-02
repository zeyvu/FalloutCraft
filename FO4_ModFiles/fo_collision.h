#pragma once

#include "fo_common.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace skycraft
{
	// Streams Fallout 4's Havok (hknp) collision around the player to Minecraft as exact triangles
	// (for Minecraft's smooth collider) and 1/8-block voxels, in 8x8x8-block regions.
	//
	// Skyrim's plugin decoded every Havok 2010 (hkp) shape type by hand. Fallout 4 runs Havok 2014
	// (hknp), whose shapes can all triangulate themselves (hknpShape::BuildSurfaceGeometry), so
	// here each shape is triangulated once, cached in its own space, and placed per body.
	//
	// Main thread: picks regions to (re)send, walks the world's bodies under its read lock and
	// copies out the triangles touching each region. Worker thread: voxelizes and writes the ring.
	class Collision
	{
	public:
		static Collision& Get();

		void Start();
		// Drops everything (world change, Minecraft reconnect); Minecraft clears its store on the new epoch.
		void Reset(std::uint32_t a_epoch);
		// Main thread, once per frame: the player's cell and position (Minecraft coords).
		void Update(RE::TESObjectCELL* a_cell, const McVec& a_playerMc);
		// Main thread: harvest everything around the player again (after a fall through missing ground).
		// Only the regions right around the player: clearing everything queued ~700 regions at once
		// and Minecraft fell behind on the ground under the player.
		void Refresh();
		// Main thread, diagnostics: triangles last sent for the region holding this point (-1: never sent).
		long long TrianglesAt(const McVec& a_p) const;
		// Main thread, diagnostics: what our copy knows about the body Fallout's pick hit at a_point.
		std::string Describe(const RE::hknpBody* a_body, const McVec& a_point);

		static constexpr int kRegionSize = 8;  // blocks per region edge (must match the Java side)

		// FalloutCraft: the Havok origin reading (see GatherBodies), once settled. Bodies we create
		// for Minecraft blocks are placed with it.
		bool HavokOffset(float a_out[3]) const
		{
			a_out[0] = offset_[0], a_out[1] = offset_[1], a_out[2] = offset_[2];
			return offsetChosen_;
		}

	private:
		using Clock = std::chrono::steady_clock;

		struct Tri
		{
			float         v[9];  // three vertices, MC space
			std::uint32_t flags{ 0 };  // proto::ColTriFlags (diggable, terrain, material)
		};

		struct Job
		{
			int              rx{ 0 }, ry{ 0 }, rz{ 0 };
			std::uint32_t    epoch{ 0 };
			bool             clear{ false };
			std::vector<Tri> tris;
			std::vector<Tri> helperTris;  // stair helpers: sent as triangles only, never voxelized
			std::vector<Tri> ghostTris;   // diggable surfaces as they were before cells were dug out (not collision)
		};

		// A shape's surface, triangulated once, in the shape's own space (Havok units).
		struct Geom
		{
			std::vector<float>         verts;  // xyz per vertex
			std::vector<std::uint32_t> idx;    // 3 per triangle
			bool                       ok{ false };
			Clock::time_point          used{};
		};

		struct Body
		{
			const RE::hknpShape* shape;
			alignas(16) float    xf[16];  // hkTransform: rotation columns [0..2] [4..6] [8..10], translation [12..14]
			float                lo[3], hi[3];  // world AABB, MC space
			bool                 helper;        // stair helper (triangles only)
			std::uint32_t        triFlags{ 0 };  // FalloutCraft: diggable ground / trees / rock (proto::ColTriFlags)
			std::uint32_t        id{ 0 };        // Havok body id
			std::uint64_t        userData{ 0 };  // Bethesda's: the bhkNPCollisionObject that owns it
		};

		void        GatherBodies(RE::bhkWorld* a_bhk, RE::hknpBSWorld* a_world, const McVec& a_player);
		// False when the result was held back (the region lost most of its triangles: Fallout is
		// probably streaming the bodies there, so Minecraft keeps the previous copy for a moment).
		bool        Harvest(int a_rx, int a_ry, int a_rz, std::vector<Tri>* a_sheet = nullptr);
		// Fallout's own picks down a 1-block grid over the region: a sheet over the ground it finds,
		// bridging the cracks and potholes Fallout's player walks over (Minecraft's feet are narrower).
		void        BuildSheet(RE::TESObjectCELL* a_cell, int a_rx, int a_ry, int a_rz, std::vector<Tri>& a_out);
		const Geom* GetGeom(const RE::hknpShape* a_shape);
		void        EvictGeoms(bool a_all);

		void WorkerLoop();
		void Voxelize(const Job& a_job);
		void SendTriangles(const Job& a_job);
		void Send(const std::vector<std::uint8_t>& a_payload, proto::ColType a_type);

		// Havok world -> MC: mc = HkToMc(hk + offset_) * k.
		void HkPointToMc(const float* a_hk, float* a_out) const;

		std::mutex              mutex_;
		std::condition_variable cv_;
		std::deque<Job>         queue_;
		std::thread             worker_;

		std::atomic<std::uint32_t>                                     epoch_{ 0 };
		std::unordered_map<std::uint64_t, Clock::time_point>           harvested_;
		std::unordered_set<std::uint64_t>                              emptyRegions_;  // harvested with no triangles
		struct RegionRecord
		{
			std::size_t       tris{ 0 };       // triangles last sent
			Clock::time_point shrinkSince{};   // when it first came back with far fewer
		};
		std::unordered_map<std::uint64_t, RegionRecord>                regions_;
		std::unordered_set<std::uint64_t>                              doubtful_;      // held back; look again soon
		std::uint64_t                                                  statHeld_{ 0 };
		std::uint64_t                                                  statSheets_{ 0 }, statSheetTris_{ 0 };
		std::uint64_t                                                  statQueuePeak_{ 0 }, statReplaced_{ 0 };
		std::atomic<int>                                               playerRegion_[3]{ 0, 0, 0 };
		std::vector<Body>                                              bodies_;
		std::vector<std::array<int, 3>>                                offsets_;
		std::unordered_map<const RE::hknpShape*, Geom>                 geoms_;
		std::size_t                                                    geomTris_{ 0 };
		Clock::time_point                                              lastEvict_{};

		// World origin handling: Havok positions may be relative to a shifting origin.
		float offset_[3]{ 0, 0, 0 };
		float originSeen_[3]{ 1e30f, 1e30f, 1e30f };
		bool  offsetChosen_{ false };
		// FalloutCraft: Fallout's own ground (a pick) under the player while the origin reading is
		// being chosen: the reading whose triangles put the floor there is the right one.
		bool  groundRefOk_{ false };
		float groundRef_[3]{};
		int   offsetTries_{ 0 };
		bool  offsetGuessed_{ false };
		float GroundErrorFor(const float* a_offset);
		std::vector<const RE::hknpBody*> rawForOrigin_;
		// FalloutCraft: the Fallout world being exported (SkyState::worldId) and whether it's an
		// exterior (its ground sheets are diggable land), for cutting out dug cells (fo_dig.cpp).
		std::uint32_t worldId_{ 0 };
		bool          exterior_{ false };
		void          CutDugCells(Job& a_job);
		// Trees and loose rocks a cell was dug out of are taken out of Fallout's world (Disable).
		void          RemoveDugObjects(RE::bhkWorld* a_bhk, RE::hknpBSWorld* a_world);

		// Diagnostics.
		std::unordered_set<int> loggedTypes_;
		std::unordered_set<int> failedTypes_;
		bool                    loggedGather_{ false };
		Clock::time_point       statsAt_{};
		std::uint64_t           statRegions_{ 0 }, statTris_{ 0 }, statBlocks_{ 0 };
		std::atomic<std::uint64_t> statSentBlocks_{ 0 }, statSentTris_{ 0 }, statDropped_{ 0 };
	};
}
