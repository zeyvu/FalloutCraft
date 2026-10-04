// FalloutCraft: Fallout's light on Minecraft's blocks.
//
// Minecraft's blocks are drawn into Fallout's frame by our own shader (fo_blocks.cpp), after
// Fallout has lit and shaded its own world, so on their own they'd keep Minecraft's flat lighting.
// Here, every frame, Fallout's current light is read from its sky (sun direction and colour, the
// directional ambient light and the fog), and the blocks are lit with it. And Fallout's shadows:
// for each face of each Minecraft block near the player, a ray goes towards the sun through
// Fallout's collision (buildings, trees, rocks, the land - and the other Minecraft blocks, which
// are in Fallout's physics too, fo_blockcol.cpp); a face whose ray is blocked is in shadow. A few
// hundred rays per frame, nearest blocks first, redone as the sun moves.
//
// What this can't do: Minecraft's blocks casting shadows onto Fallout's own world (that needs
// Fallout's shadow pass), and Fallout's post-processing (bloom, colour grading) on the blocks.

#include "fo_common.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef ERROR

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace skycraft::Shadows
{
	namespace
	{
		constexpr int    kRaysPerFrame = 300;
		constexpr double kNearBlocks = 72.0;     // only blocks this close get shadows (the rest: lit)
		constexpr float  kRayUnits = 70.0f * 96;  // how far towards the sun a shadow can come from
		constexpr float  kResunCos = 0.99966f;    // ~1.5 degrees of sun movement: work the shadows out again

		struct Entry
		{
			std::int32_t              sx{ 0 }, sy{ 0 }, sz{ 0 };
			std::vector<float>        points;  // per quad: a point just off its centre (MC blocks)
			std::vector<std::uint8_t> faces;   // per quad: Minecraft Direction ordinal + 1 (0 none)
			std::vector<std::uint8_t> lit;     // per quad: 255 sunlit, 0 in Fallout's shadow
			std::size_t               cursor{ 0 };
			float                     sun[3]{ 0, 0, 0 };  // the sun direction the results are for
			bool                      complete{ false };
			std::uint64_t             version{ 0 };       // bumped when `lit` changes
		};

		std::mutex                                lock;
		std::unordered_map<std::uint64_t, Entry> entries;
		Light                                     light{};
		std::uint64_t                             versions = 0;

		// Fallout (x east, y north, z up) direction for a Minecraft face (Direction ordinal + 1).
		void FaceNormal(int a_face, float a_out[3])
		{
			static constexpr float kNormals[7][3] = { { 0, 0, 1 }, { 0, 0, -1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, -1, 0 }, { -1, 0, 0 }, { 1, 0, 0 } };
			const auto& n = kNormals[(a_face >= 0 && a_face <= 6) ? a_face : 0];
			a_out[0] = n[0], a_out[1] = n[1], a_out[2] = n[2];
		}
	}

	void Submit(std::uint64_t a_key, std::int32_t a_sx, std::int32_t a_sy, std::int32_t a_sz, std::vector<float>&& a_points, std::vector<std::uint8_t>&& a_faces)
	{
		std::scoped_lock guard(lock);
		Entry& e = entries[a_key];
		e.sx = a_sx, e.sy = a_sy, e.sz = a_sz;
		e.points = std::move(a_points);
		e.faces = std::move(a_faces);
		e.lit.assign(e.faces.size(), 255);
		e.cursor = 0;
		e.complete = false;
		e.version = ++versions;
	}

	void Remove(std::uint64_t a_key)
	{
		std::scoped_lock guard(lock);
		entries.erase(a_key);
	}

	void Clear()
	{
		std::scoped_lock guard(lock);
		entries.clear();
	}

	bool Fetch(std::uint64_t a_key, std::uint64_t& a_version, std::vector<std::uint8_t>& a_out)
	{
		std::scoped_lock guard(lock);
		auto it = entries.find(a_key);
		if (it == entries.end() || it->second.version == a_version) {
			return false;
		}
		a_version = it->second.version;
		a_out = it->second.lit;
		return true;
	}

	Light Current()
	{
		std::scoped_lock guard(lock);
		return light;
	}

	void Update(RE::TESObjectCELL* a_cell, const McVec& a_player)
	{
		// FalloutCraft G-buffer probe (fo_inject.cpp): what is really at the centre of the screen -
		// Fallout's collision straight ahead, and its surface normal from three nearby rays.
		static ULONGLONG truthAt = 0;
		if (auto* cam = RE::Main::WorldRootCamera(); cam && a_cell && ::GetTickCount64() - truthAt > 250) {
			truthAt = ::GetTickCount64();
			const auto& m = cam->worldToCam;
			auto row = [&](int r, float o[3]) {
				const float l = std::sqrt(m[r][0] * m[r][0] + m[r][1] * m[r][1] + m[r][2] * m[r][2]);
				for (int k = 0; k < 3; ++k) {
					o[k] = l > 1e-6f ? m[r][k] / l : 0.0f;
				}
			};
			float fwd[3], right[3], up[3];
			row(3, fwd), row(0, right), row(1, up);
			const auto&  c = cam->world.translate;
			RE::NiPoint3 hits[3];
			bool         ok = true;
			const float  d[3][2] = { { 0, 0 }, { 0.01f, 0 }, { 0, 0.01f } };
			for (int i = 0; i < 3 && ok; ++i) {
				const RE::NiPoint3 dir{ fwd[0] + right[0] * d[i][0] + up[0] * d[i][1], fwd[1] + right[1] * d[i][0] + up[1] * d[i][1], fwd[2] + right[2] * d[i][0] + up[2] * d[i][1] };
				ok = PickGroundAt(a_cell, c, { c.x + dir.x * 20000.0f, c.y + dir.y * 20000.0f, c.z + dir.z * 20000.0f }, hits[i]);
			}
			if (ok) {
				const RE::NiPoint3 e1 = hits[1] - hits[0], e2 = hits[2] - hits[0];
				RE::NiPoint3       n{ e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
				const float        nl = n.Length();
				if (nl > 1e-6f) {
					n = n * (1.0f / nl);
					if (n.x * fwd[0] + n.y * fwd[1] + n.z * fwd[2] > 0) {
						n = n * -1.0f;  // facing the camera
					}
					const float dist = (hits[0] - c).Length();
					// The normal as the camera sees it (x right, y up, z forward).
					const float vx = n.x * right[0] + n.y * right[1] + n.z * right[2];
					const float vy = n.x * up[0] + n.y * up[1] + n.z * up[2];
					const float vz = n.x * fwd[0] + n.y * fwd[1] + n.z * fwd[2];
					const float nn[3] = { n.x, n.y, n.z };
					Inject::SetTruthNormal(true, dist, nn, right, up, fwd);
					(void)vx, (void)vy, (void)vz;
				}
			} else {
				Inject::SetTruthNormal(false, 0.0f, fwd, right, up, fwd);
			}
		}

		// ---- Fallout's light now -------------------------------------------------------------
		Light now{};
		auto* sky = RE::Sky::GetSingleton();
		auto* camera = RE::Main::WorldRootCamera();
		const bool outdoors = a_cell && !a_cell->IsInterior() && sky && sky->sun && camera;
		if (outdoors) {
			auto* sun = sky->sun;
			auto* base = sun->sunBaseNode.get();
			if (base) {
				// Towards the sun: from the camera to where the sky puts the sun's disc.
				const auto& c = camera->world.translate;
				const auto& s = base->world.translate;
				float       d[3] = { s.x - c.x, s.y - c.y, s.z - c.z };
				const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
				if (len > 1.0f) {
					for (int k = 0; k < 3; ++k) {
						now.toSun[k] = d[k] / len;
					}
				}
			}
			if (auto* sunLight = reinterpret_cast<const RE::NiLight*>(sun->light.get())) {
				const float dim = std::clamp(sunLight->dimmer, 0.0f, 4.0f);
				now.sunColor[0] = sunLight->diff.r * dim;
				now.sunColor[1] = sunLight->diff.g * dim;
				now.sunColor[2] = sunLight->diff.b * dim;
			}
			// The sun only lights from above the horizon (fading out as it sets).
			const float up = std::clamp(now.toSun[2] * 6.0f, 0.0f, 1.0f);
			for (float& c : now.sunColor) {
				c = std::clamp(c * up, 0.0f, 3.0f);
			}
			for (int axis = 0; axis < 3; ++axis) {
				for (int sign = 0; sign < 2; ++sign) {
					const auto& a = sky->directionalAmbientColorsA[axis][sign];
					now.ambient[axis * 2 + sign][0] = std::clamp(a.r, 0.0f, 3.0f);
					now.ambient[axis * 2 + sign][1] = std::clamp(a.g, 0.0f, 3.0f);
					now.ambient[axis * 2 + sign][2] = std::clamp(a.b, 0.0f, 3.0f);
				}
			}
			// Fog: the weather's current near/far distances, power and maximum, and its colours.
			const float nearD = sky->fogDistances[0], farD = sky->fogDistances[1];
			if (std::isfinite(nearD) && std::isfinite(farD) && farD > nearD + 100.0f && farD < 1.0e7f) {
				now.fog[0] = nearD;
				now.fog[1] = farD;
				now.fog[2] = (sky->fogPower > 0.05f && sky->fogPower < 10.0f) ? sky->fogPower : 1.0f;
				now.fog[3] = (sky->fogClamp > 0.0f && sky->fogClamp <= 1.0f) ? sky->fogClamp : 1.0f;
				const auto& nc = sky->skyColor[1];   // fog near
				const auto& fc = sky->skyColor[12];  // fog far
				now.fogNear[0] = nc.r, now.fogNear[1] = nc.g, now.fogNear[2] = nc.b;
				now.fogFar[0] = fc.r, now.fogFar[1] = fc.g, now.fogFar[2] = fc.b;
			}
			now.valid = true;
		}

		// ---- Fallout's shadows on the blocks --------------------------------------------------
		struct Job
		{
			std::uint64_t key;
			std::size_t   from, to;
			std::vector<float>        points;
			std::vector<std::uint8_t> faces;
			std::vector<std::uint8_t> lit;
		};
		std::vector<Job> jobs;
		{
			std::scoped_lock guard(lock);
			light = now;
			if (!now.valid) {
				return;
			}
			// Sections near the player, nearest first, whose shadows are missing or out of date.
			std::vector<std::pair<double, std::uint64_t>> order;
			for (auto& [key, e] : entries) {
				const double dx = e.sx * 16.0 + 8.0 - a_player.x, dy = e.sy * 16.0 + 8.0 - a_player.y, dz = e.sz * 16.0 + 8.0 - a_player.z;
				const double d2 = dx * dx + dy * dy + dz * dz;
				if (d2 > kNearBlocks * kNearBlocks || e.faces.empty()) {
					continue;
				}
				const float sunCos = e.sun[0] * now.toSun[0] + e.sun[1] * now.toSun[1] + e.sun[2] * now.toSun[2];
				if (e.complete && sunCos > kResunCos) {
					continue;
				}
				if (e.complete || e.cursor == 0) {
					std::memcpy(e.sun, now.toSun, sizeof(e.sun));  // start (again) for this sun
					e.cursor = 0;
					e.complete = false;
				}
				order.emplace_back(d2, key);
			}
			std::sort(order.begin(), order.end());
			int budget = kRaysPerFrame;
			for (const auto& [d2, key] : order) {
				if (budget <= 0) {
					break;
				}
				auto&             e = entries[key];
				const std::size_t n = std::min<std::size_t>(e.faces.size() - e.cursor, std::size_t(budget));
				Job               job{ key, e.cursor, e.cursor + n, {}, {}, {} };
				job.points.assign(e.points.begin() + e.cursor * 3, e.points.begin() + (e.cursor + n) * 3);
				job.faces.assign(e.faces.begin() + e.cursor, e.faces.begin() + e.cursor + n);
				budget -= int(n);
				jobs.push_back(std::move(job));
			}
		}

		// The rays, outside the lock (each pick takes Fallout's physics lock).
		for (auto& job : jobs) {
			job.lit.resize(job.faces.size(), 255);
			for (std::size_t i = 0; i < job.faces.size(); ++i) {
				float n[3];
				FaceNormal(job.faces[i], n);
				const bool faced = job.faces[i] == 0 || n[0] * now.toSun[0] + n[1] * now.toSun[1] + n[2] * now.toSun[2] > 0.0f;
				if (!faced) {
					job.lit[i] = 0;  // facing away from the sun: no direct light anyway
					continue;
				}
				const float* p = &job.points[i * 3];
				const auto   from = McToGame(p[0], p[1], p[2]);
				const RE::NiPoint3 to{ from.x + now.toSun[0] * kRayUnits, from.y + now.toSun[1] * kRayUnits, from.z + now.toSun[2] * kRayUnits };
				job.lit[i] = RayBlocked(a_cell, from, to) ? 0 : 255;
			}
		}

		std::scoped_lock guard(lock);
		for (auto& job : jobs) {
			auto it = entries.find(job.key);
			if (it == entries.end() || it->second.cursor != job.from || job.to > it->second.lit.size()) {
				continue;  // the section changed meanwhile
			}
			auto& e = it->second;
			bool  changed = false;
			for (std::size_t i = 0; i < job.lit.size(); ++i) {
				changed |= e.lit[job.from + i] != job.lit[i];
				e.lit[job.from + i] = job.lit[i];
			}
			e.cursor = job.to;
			if (e.cursor >= e.faces.size()) {
				e.cursor = 0;
				e.complete = true;
			}
			if (changed) {
				e.version = ++versions;
			}
		}
	}
}
