// FalloutCraft: where each Fallout world sits in Minecraft's single world.
//
// Fallout's interiors all use coordinates near the origin, and its other worldspaces (Far Harbor,
// Nuka-World, ...) overlap the Commonwealth's. Minecraft has one world, so without care a house
// built in one interior shows up in every other one at the same spot. Each world therefore gets
// its own place in Minecraft, and the same place every time (remembered in
// FalloutCraft_worlds.txt next to the F4SE logs):
//  - the Commonwealth at 0 (and worldspaces that share its map, like Sanctuary before the war),
//  - interiors in 2048-block slots on a grid between 16k and 98k blocks from the origin,
//  - other worldspaces at 120k blocks out.
// Everything stays within ~125k blocks so the floats the protocol carries keep ~1/64-block precision.

#include "fo_common.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef ERROR

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <utility>
#include <vector>

namespace skycraft::Worlds
{
	namespace
	{
		constexpr RE::TESFormID kCommonwealth = 0x0000003C;
		constexpr double     kInteriorSlot = 2048.0;  // blocks per interior
		constexpr int        kInnerRing = 8;          // slots around the origin left to the Commonwealth (16k blocks)
		constexpr int        kOuterRing = 48;         // slots out to 98k blocks

		struct Place
		{
			bool exterior;
			int  index;  // slot (interiors) or worldspace number (exteriors), from 0
		};

		std::unordered_map<RE::TESFormID, Place> places;
		int                                   nextInterior = 0;
		int                                   nextExterior = 0;
		bool                                  loaded = false;
		wchar_t                               filePath[MAX_PATH]{};

		// Interior slots ring by ring outward, skipping the middle the Commonwealth uses.
		std::pair<int, int> InteriorCell(int a_index)
		{
			static std::vector<std::pair<int, int>> order = [] {
				std::vector<std::pair<int, int>> cells;
				auto ring = [](const std::pair<int, int>& c) { return std::max(std::abs(2 * c.first + 1), std::abs(2 * c.second + 1)) / 2; };
				for (int i = -kOuterRing; i < kOuterRing; ++i) {
					for (int j = -kOuterRing; j < kOuterRing; ++j) {
						if (ring({ i, j }) >= kInnerRing) {
							cells.emplace_back(i, j);
						}
					}
				}
				std::stable_sort(cells.begin(), cells.end(), [&](const auto& a, const auto& b) { return ring(a) < ring(b); });
				return cells;
			}();
			return order[static_cast<std::size_t>(a_index) % order.size()];
		}

		void Offset(const Place& a_place, double& a_x, double& a_z)
		{
			if (a_place.exterior) {
				static const std::pair<int, int> dirs[] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 } };
				const auto d = dirs[a_place.index % 8];
				a_x = 120000.0 * d.first;
				a_z = 120000.0 * d.second;
				return;
			}
			const auto c = InteriorCell(a_place.index);
			a_x = c.first * kInteriorSlot;
			a_z = c.second * kInteriorSlot;
		}

		void Load()
		{
			loaded = true;
			wchar_t profile[MAX_PATH]{};
			if (!::GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) {
				return;
			}
			std::swprintf(filePath, MAX_PATH, L"%s\\Documents\\My Games\\Fallout4\\F4SE\\FalloutCraft_worlds.txt", profile);
			FILE* f = nullptr;
			if (_wfopen_s(&f, filePath, L"r") != 0 || !f) {
				return;
			}
			char line[128];
			while (std::fgets(line, sizeof(line), f)) {
				unsigned id = 0;
				char     kind = 0;
				int      index = 0;
				if (sscanf_s(line, "%x %c %d", &id, &kind, 1u, &index) == 3 && (kind == 'I' || kind == 'E')) {
					places[id] = { kind == 'E', index };
					if (kind == 'E') {
						nextExterior = std::max(nextExterior, index + 1);
					} else {
						nextInterior = std::max(nextInterior, index + 1);
					}
				}
			}
			std::fclose(f);
			REX::INFO("worlds: {} places remembered", places.size());
		}

		void Remember(RE::TESFormID a_id, const Place& a_place, const char* a_name)
		{
			if (!filePath[0]) {
				return;
			}
			FILE* f = nullptr;
			if (_wfopen_s(&f, filePath, L"a") == 0 && f) {
				std::fprintf(f, "%08X %c %d  %s\n", a_id, a_place.exterior ? 'E' : 'I', a_place.index, a_name ? a_name : "");
				std::fclose(f);
			}
		}

		// The worldspace whose map this one shares (child worldspaces use their parent's land).
		RE::TESWorldSpace* RootWorld(RE::TESWorldSpace* a_world)
		{
			for (int i = 0; i < 8 && a_world && a_world->parentWorld; ++i) {
				a_world = a_world->parentWorld;
			}
			return a_world;
		}
	}

	void Select(RE::TESObjectCELL* a_cell)
	{
		if (!loaded) {
			Load();
		}
		double x = 0.0, z = 0.0;
		RE::TESFormID  id = 0;
		const char* what = "the Commonwealth";
		const char* name = "";
		bool        exterior = false;
		if (a_cell && (a_cell->IsInterior() || !a_cell->worldSpace)) {
			id = a_cell->GetFormID();
			what = "interior";
			name = a_cell->GetFormEditorID();
		} else if (a_cell) {
			auto* root = RootWorld(a_cell->worldSpace);
			id = root ? root->GetFormID() : kCommonwealth;
			exterior = true;
			what = "worldspace";
			name = root ? root->GetFormEditorID() : "";
		}
		if (id != 0 && id != kCommonwealth) {
			auto it = places.find(id);
			if (it == places.end()) {
				const Place place{ exterior, exterior ? nextExterior++ : nextInterior++ };
				it = places.emplace(id, place).first;
				Remember(id, place, name);
			}
			Offset(it->second, x, z);
		}
		g_worldOffset.x.store(x, std::memory_order_relaxed);
		g_worldOffset.z.store(z, std::memory_order_relaxed);
		REX::INFO("worlds: {} {:08X} '{}' is at Minecraft ({:.0f}, {:.0f})", id == kCommonwealth || id == 0 ? "exterior" : what, id, name ? name : "", x, z);
	}
}
