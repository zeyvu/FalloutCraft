// FalloutCraft Phase 1: Minecraft drives Fallout 4's player.
//
// Every frame (PlayerCharacter::Update):
//  - read Minecraft's state, keep the teleport handshake (Fallout moved the player -> Minecraft follows),
//  - while Minecraft drives: put Fallout's player at Minecraft's (interpolated) feet and look,
//  - publish Fallout's state to Minecraft, stream Fallout's collision around the player.
// Ported from the Skyrim plugin (skse/src/Game.cpp), without the rendering/combat parts.

#include "fo_collision.h"
#include "fo_common.h"

#include <chrono>
#include <limits>
#include <cstring>
#include <format>
#include <string>
#include <vector>
#include <unordered_set>
#include <array>
#include <deque>

namespace skycraft
{
	Runtime& State()
	{
		static Runtime runtime;
		return runtime;
	}

	namespace
	{
		constexpr float kRadToDeg = 57.2957795f;
		constexpr float kDegToRad = 0.0174532925f;
		constexpr float kTeleportThreshold = 300.0f;  // units; bigger jumps are Fallout moving the player
		constexpr float kSettleSeconds = 1.5f;        // Havok bodies stream in over a few frames after loads

		// Main thread only (PlayerCharacter::Update).
		proto::McState mc{};
		bool           mcWasAlive = false;
		std::uint32_t  lastMcPid = 0;
		// Starts somewhere new each run, so a Minecraft still acknowledging the last run's teleport
		// can't be taken for having arrived at this run's.
		std::uint32_t teleportSeq = [] {
			std::int64_t t = 0;
			REX::W32::QueryPerformanceCounter(&t);
			return static_cast<std::uint32_t>(t) | 1u;
		}();
		bool          teleportPending = true;
		float         zoom = 0.0f;  // the third-person camera distance we use (blocks)
		std::uint32_t zoomMode = 0;
		RE::NiPoint3  lastGround{};
		double        lastGroundMcY = 0.0;
		bool          haveGround = false;
		constexpr double kRescueDrop = 12.0;  // blocks below the last ground before rescuing (fallback)
		RE::NiPoint3  prevFeet{};             // Minecraft's feet last frame (game units)
		bool          havePrevFeet = false;
		int           rescues = 0;
		float         lastDepth = 0.0f;
		int           sinking = 0;
		// Fallout's surface where Minecraft last stood level with it (game z), and since when.
		float         trustedZ = 0.0f;
		float         trustedAge = 99.0f;
		float         dipLogTimer = 0.0f;
		float         groundRef = 0.0f;     // Fallout's ground under Minecraft's feet last frame (MC y)
		float         groundRefAge = 99.0f;       // frames in a row the feet went deeper under Fallout's surface
		RE::NiPoint3  lastRescueAt{};
		float         sinceRescue = 1e9f;  // how far below Fallout's surface the feet were last frame
		// Where Minecraft last stood on its own ground: its collision there is known to work, so a
		// rescue that Minecraft can't settle at falls back to it.
		RE::NiPoint3  safeGround{};
		bool          haveSafeGround = false;
		// While Minecraft settles after a rescue, Fallout's player is held at the rescue point.
		RE::NiPoint3  pinAt{};
		bool          pinned = false;
		float         settleWait = 0.0f;  // seconds Minecraft has been holding after a teleport
		float         holdMismatch = 0.0f;
		std::uint32_t worldId = 0;
		std::uint32_t epoch = 0;
		RE::NiPoint3  lastSetPos{};
		bool          haveLastSet = false;
		float         settleTimer = 2.0f;
		float         logTimer = 0.0f;
		const char*   takeover = nullptr;
		std::string   lastMenuReason;

		// A Fallout menu that pauses the game or takes the mouse (Pip-Boy, pause menu, console,
		// terminals, containers, barter, ...). Always-open menus (HUD, cursor) don't count.
		bool AnyBlockingMenuOpen(RE::UI* a_ui, std::string* a_which = nullptr)
		{
			if (!a_ui) {
				return false;
			}
			for (const auto& menu : a_ui->menuStack) {
				if (!menu || menu->menuFlags.all(RE::UI_MENU_FLAGS::kAlwaysOpen)) {
					continue;
				}
				if (menu->menuFlags.any(RE::UI_MENU_FLAGS::kPausesGame, RE::UI_MENU_FLAGS::kUsesCursor)) {
					if (a_which) {
						*a_which = menu->menuName.c_str();
					}
					return true;
				}
			}
			return false;
		}

		// Fallout keeps the player while its own camera states run: power armor and other
		// furniture, dialogue, VATS, scripted/animated cameras. Minecraft picks up afterwards.
		// Tab pressed while Minecraft drives: Fallout gets the player for a moment, so its own
		// controls raise the arm and open the Pip-Boy with its animation (opening it while
		// Minecraft held the player did nothing; only the pause menu's Pip-Boy button worked).
		std::atomic<std::int64_t> pipboyUntilMs{ 0 };

		std::int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		const char* FalloutTakeover()
		{
			if (auto* ui = RE::UI::GetSingleton(); ui && ui->GetMenuOpen("PipboyMenu")) {
				return "Pip-Boy";
			}
			if (NowMs() < pipboyUntilMs.load()) {
				return "opening the Pip-Boy";
			}
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera || !camera->currentState) {
				return nullptr;
			}
			switch (camera->currentState->id.get()) {
			case RE::CameraState::kFurniture:
				return "furniture / power armor";
			case RE::CameraState::kDialogue:
				return "dialogue";
			case RE::CameraState::kVATS:
				return "VATS";
			case RE::CameraState::kAnimated:
				return "animated camera";
			case RE::CameraState::kMount:
				return "mounted";
			case RE::CameraState::kBleedout:
				return "bleedout";
			default:
				return nullptr;
			}
		}

		void ForceFirstPerson()
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera || !camera->currentState) {
				return;
			}
			if (camera->currentState->id.get() == RE::CameraState::k3rdPerson) {
				if (auto& first = camera->cameraStates[RE::CameraState::kFirstPerson]) {
					camera->SetState(first.get());
				}
			}
		}

		// Fallout's first-person arms and weapon: hidden while Minecraft draws the hand. Only
		// meshes are hidden, never nodes (the camera and animations use those); exactly the meshes
		// hidden here are shown again. Fallout un-hides some itself (weapon hands), so this runs
		// every frame and again just before Present; anything it keeps un-hiding is logged once.
		std::vector<RE::NiPointer<RE::BSGeometry>> hiddenFirstPerson;
		std::unordered_set<RE::BSGeometry*>        hiddenSet;
		std::unordered_set<RE::BSGeometry*>        reportedFighting;
		bool                                       firstPersonHidden = false;
		bool                                       loggedFirstPerson = false;
		RE::NiPointer<RE::NiAVObject>              culledRoot;
		int                                        lastMeshCount = -1;

		void ShowFirstPerson(RE::PlayerCharacter* a_player)
		{
			for (auto& mesh : hiddenFirstPerson) {
				if (mesh && mesh->GetAppCulled()) {
					mesh->SetAppCulled(false);
				}
			}
			hiddenFirstPerson.clear();
			hiddenSet.clear();
			if (culledRoot && culledRoot->GetAppCulled()) {
				culledRoot->SetAppCulled(false);
			}
			culledRoot.reset();
			firstPersonHidden = false;
		}

		void HideFirstPersonNow(RE::PlayerCharacter* a_player)
		{
			auto* root = a_player->Get3D(true);
			if (!root) {
				root = a_player->firstPerson3D.get();
			}
			if (!root) {
				return;
			}
			firstPersonHidden = true;
			// Hiding the meshes alone wasn't enough: with fists or guns Fallout still drew the hands
			// (its first-person pass doesn't check every mesh). So the whole first-person model is
			// culled at its root as well; its nodes keep updating (culling only stops drawing).
			if (culledRoot.get() != root) {
				if (culledRoot && culledRoot->GetAppCulled()) {
					culledRoot->SetAppCulled(false);
				}
				culledRoot.reset(root);
			}
			if (!root->GetAppCulled()) {
				root->SetAppCulled(true);
			}
			int total = 0, culled = 0;
			RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_mesh) {
				++total;
				if (!a_mesh->GetAppCulled()) {
					a_mesh->SetAppCulled(true);
					++culled;
					if (hiddenSet.insert(a_mesh).second) {
						hiddenFirstPerson.emplace_back(a_mesh);
					} else if (reportedFighting.insert(a_mesh).second) {
						REX::INFO("first person: Fallout keeps showing mesh '{}' again", a_mesh->name.c_str());
					}
				}
				return RE::BSVisitControl::kContinue;
			});
			if (total != lastMeshCount) {
				lastMeshCount = total;
				std::string names;
				RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_mesh) {
					if (names.size() < 600) {
						names += std::string(" '") + a_mesh->name.c_str() + "'";
					}
					return RE::BSVisitControl::kContinue;
				});
				REX::INFO("first person: hiding Fallout's model '{}' ({} meshes:{})", root->name.c_str(), total, names);
			}
		}

		void HideFirstPerson(RE::PlayerCharacter* a_player, bool a_hide)
		{
			if (a_hide) {
				HideFirstPersonNow(a_player);
			} else if (firstPersonHidden || !hiddenFirstPerson.empty()) {
				ShowFirstPerson(a_player);
			}
		}

		RE::bhkCharacterController* CharController(RE::PlayerCharacter* a_player)
		{
			auto* process = a_player->currentProcess;
			auto* middle = process ? process->middleHigh : nullptr;
			return middle ? middle->charController.get() : nullptr;
		}

		// Anything Fallout's player could stand on: not actors, triggers, water or query-only layers.
		bool GroundLayer(int a_layer)
		{
			using L = RE::COL_LAYER;
			switch (static_cast<L>(a_layer)) {
			case L::kWeapon:
			case L::kProjectile:
			case L::kSpell:
			case L::kBiped:
			case L::kWater:
			case L::kTrigger:
			case L::kNonCollidable:
			case L::kDebrisSmall:
			case L::kAcousticSpace:
			case L::kActorZone:
			case L::kProjectileZone:
			case L::kShellCasing:
			case L::kCharController:
			case L::kDeadBip:
			case L::kBipedNoCC:
			case L::kAvoidBox:
			case L::kCameraSphere:
			case L::kDoorDetection:
			case L::kConeProjectile:
			case L::kCamera:
			case L::kItemPicker:
			case L::kLOS:
			case L::kPathingPick:
			case L::kSpellExplosion:
			case L::kDroppingPick:
				return false;
			default:
				return a_layer >= 0;
			}
		}

		// Fallout's own collision along a segment (game units): where it first meets a static
		// surface (ground, floors, rocks). Actors, the player's capsule and loose objects don't count.
		bool PickGround(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, RE::NiPoint3& a_hit, int& a_layer, std::uint32_t* a_flags = nullptr,
			const RE::hknpBody** a_body = nullptr)
		{
			if (!a_cell) {
				return false;
			}
			RE::bhkPickData pd;
			pd.SetStartEnd(a_from, a_to);
			pd.castQuery.m_filterData.m_collisionFilterInfo = static_cast<std::uint32_t>(RE::COL_LAYER::kPathingPick);
			(void)a_cell->Pick(pd);
			if (!pd.HasHit()) {
				return false;
			}
			a_layer = -1;
			bool mcBlock = false;
			if (const auto* body = pd.GetBody()) {
				if (a_body) {
					*a_body = body;
				}
				a_layer = static_cast<int>(body->m_collisionFilterInfo & 0x7F);
				mcBlock = BlockCollision::Owns(body->m_shape);  // Minecraft has it already: look past it
				if (a_flags) {
					std::memcpy(a_flags, &body->m_flags, sizeof(std::uint32_t));
				}
			}
			const float f = std::clamp(pd.GetHitFraction(), 0.0f, 1.0f);
			a_hit = a_from + (a_to - a_from) * f;  // set even when it isn't ground (callers may look past it)
			if (a_layer < 0) {
				a_layer = 0x7F;  // a hit without a body: not ground, but something to look past
			}
			return !mcBlock && GroundLayer(a_layer);
		}

		std::uint32_t WorldIdOf(RE::TESObjectCELL* a_cell)
		{
			if (!a_cell) {
				return 0;
			}
			if (a_cell->IsInterior() || !a_cell->worldSpace) {
				return a_cell->GetFormID();
			}
			return a_cell->worldSpace->GetFormID();
		}

		// ---- smooth motion ------------------------------------------------------------------------
		// Minecraft ticks exactly every tickMs, but stamps a tick only after that frame's work and we
		// see it on our next frame, so rendering "now" sometimes runs past the latest tick and the
		// player stops for a frame, then jumps (the lag you see). As in the Skyrim plugin: keep a short
		// history of ticks, lock their times to the exact rhythm, and render a little in the past -
		// as far as ticks have really been arriving late over the last 2 s - so the next tick is
		// always there: pure interpolation, never a stop-and-jump.
		struct Tick
		{
			double       prev[3], cur[3];
			std::int64_t stamp;  // Minecraft's stamp
			std::int64_t at;     // on the locked rhythm
			int          slots;  // ticks since the one before (2+: we missed one)
		};
		std::deque<Tick>       tickHistory;
		std::int64_t           lastFrameQpc = 0;
		int                    stampOutliers = 0;
		double                 renderDelayMs = 10.0;
		std::array<double, 40> tickDue{};
		std::size_t            tickDueNext = 0;
		bool                   tickDueInit = false;
		int                    lateFrames = 0, frames = 0;

		void Interpolate(double& a_x, double& a_y, double& a_z)
		{
			static const std::int64_t qpcFreq = [] { std::int64_t f = 1; REX::W32::QueryPerformanceFrequency(&f); return f; }();
			const double       qpcPerMs = double(qpcFreq) / 1000.0;
			const std::int64_t period = std::max<std::int64_t>(1, std::llround(double(mc.tickMs) * qpcPerMs));
			std::int64_t       now = 0;
			REX::W32::QueryPerformanceCounter(&now);

			if (tickHistory.empty() || tickHistory.back().stamp != mc.tickQpc) {
				if (!tickHistory.empty() && mc.tickQpc < tickHistory.back().stamp) {
					tickHistory.clear();  // Minecraft restarted
				}
				Tick tick{ { mc.prevX, mc.prevY, mc.prevZ }, { mc.curX, mc.curY, mc.curZ }, mc.tickQpc, mc.tickQpc, 1 };
				if (!tickHistory.empty()) {
					auto&              last = tickHistory.back();
					const std::int64_t n = std::llround(double(mc.tickQpc - last.at) / double(period));
					const std::int64_t err = mc.tickQpc - (last.at + n * period);
					if (n == 0 && last.slots >= 2) {
						// Two ticks in one Minecraft frame and we saw both: the first carries the second's stamp.
						last.at -= period;
						last.slots -= 1;
						tick.at = last.at + period;
					} else if (n >= 1 && n <= 10 && std::abs(err) < period * 3 / 10) {
						tick.at = last.at + n * period + err / 16;  // the rhythm is exact; the stamps are noisy
						tick.slots = static_cast<int>(n);
						stampOutliers = 0;
					} else if (n <= 10 && ++stampOutliers < 3) {
						tick.slots = static_cast<int>(std::max<std::int64_t>(n, 1));
						tick.at = last.at + tick.slots * period;  // one odd stamp (a hitch): keep the rhythm
					} else {
						stampOutliers = 0;  // lost the rhythm (a pause, a new tick rate): start from this stamp
					}
				}
				if (lastFrameQpc != 0) {
					if (!tickDueInit) {
						tickDue.fill(renderDelayMs - 1.0);
						tickDueInit = true;
					}
					const double dueMs = double(lastFrameQpc - tick.at) / qpcPerMs;
					if (dueMs < 30.0) {
						tickDue[tickDueNext++ % tickDue.size()] = dueMs;
					}
				}
				tickHistory.push_back(tick);
				if (tickHistory.size() > 8) {
					tickHistory.pop_front();
				}
			}

			// The render delay follows how late ticks have been over the last 2 s.
			const double frameMs = lastFrameQpc != 0 ? double(now - lastFrameQpc) / qpcPerMs : 0.0;
			lastFrameQpc = now;
			if (tickDueInit) {
				const double target = std::clamp(*std::ranges::max_element(tickDue) + 1.0, 4.0, 30.0);
				const double dt = std::min(frameMs, 100.0) / 1000.0;
				renderDelayMs = target > renderDelayMs ? std::min(target, renderDelayMs + 20.0 * dt) : std::max(target, renderDelayMs - 2.0 * dt);
			}
			const std::int64_t renderQpc = now - std::llround(renderDelayMs * qpcPerMs);

			std::size_t i = 0;
			for (std::size_t k = tickHistory.size(); k-- > 0;) {
				if (tickHistory[k].at <= renderQpc) {
					i = k;
					break;
				}
			}
			const Tick&  tick = tickHistory[i];
			const Tick*  next = i + 1 < tickHistory.size() ? &tickHistory[i + 1] : nullptr;
			const double ticks = double(renderQpc - tick.at) / double(period);
			const double t = std::clamp(ticks, 0.0, 1.0);
			double       p[3];
			for (int k = 0; k < 3; ++k) {
				p[k] = tick.prev[k] + (tick.cur[k] - tick.prev[k]) * t;
			}
			if (ticks > 1.0 && next) {
				// Past this tick's end and the next one we have starts later: Minecraft ran one we
				// never saw. Carry on from this tick's end to the next one's start.
				const double gap = double(next->at - (tick.at + period));
				const double u = gap > 0.0 ? std::clamp(double(renderQpc - (tick.at + period)) / gap, 0.0, 1.0) : 1.0;
				for (int k = 0; k < 3; ++k) {
					p[k] = tick.cur[k] + (next->prev[k] - tick.cur[k]) * u;
				}
			} else if (ticks > 1.0) {
				++lateFrames;
			}
			++frames;
			a_x = p[0];
			a_y = p[1];
			a_z = p[2];
		}

		void PerFrame(RE::PlayerCharacter* a_player, float a_delta)
		{
			auto& st = State();
			auto* ui = RE::UI::GetSingleton();
			link::Heartbeat();

			const bool mcAlive = link::MinecraftAlive();
			const bool haveMc = mcAlive && link::ReadMcStateFull(mc);
			st.mcHealth = haveMc && (mc.flags & proto::kMcHealthValid) && !(mc.flags & proto::kMcDead) ? std::clamp(mc.health, 0.0f, 1.0f) : -1.0f;
			const auto mcPid = link::McPid();
			const bool newMcProcess = mcAlive && mcPid != 0 && mcPid != lastMcPid;
			if (mcAlive) {
				lastMcPid = mcPid;
			}
			if (mcAlive && (!mcWasAlive || newMcProcess)) {
				REX::INFO("Minecraft connected (pid {})", mcPid);
				link::ResetOverlay();
				settleTimer = kSettleSeconds;
				++epoch;
				Collision::Get().Reset(epoch);
				teleportPending = true;
			} else if (!mcAlive && mcWasAlive) {
				REX::INFO("Minecraft disconnected; Fallout drives the player again");
			}
			mcWasAlive = mcAlive;

			const bool inWorld = haveMc && (mc.flags & proto::kMcInWorld);
			st.mcInWorld = inWorld;
			st.mcGuiScale = haveMc ? static_cast<int>(mc.guiScale) : 0;
			Overlay::Install();
			const bool screenOpen = haveMc && (mc.flags & proto::kMcScreenOpen);
			if (screenOpen && !st.mcScreenOpen) {
				st.cursorX = st.viewportW / 2;
				st.cursorY = st.viewportH / 2;
			}
			st.mcScreenOpen = screenOpen;
			if (haveMc && mc.sensitivity > 0.0f) {
				st.sensitivity = mc.sensitivity;
			}

			auto*       cell = a_player->GetParentCell();
			const bool  loading = !cell || !a_player->Get3D() || (ui && ui->GetMenuOpen("LoadingMenu"));
			std::string which;
			bool        menu = AnyBlockingMenuOpen(ui, &which);
			// The Pip-Boy (and its radio) is Fallout's: its menu, its arm and its screen.
			if (!menu && ui && ui->GetMenuOpen("PipboyMenu")) {
				menu = true;
				which = "PipboyMenu";
			}
			if ((menu || loading) && !st.falloutMenuOpen) {
				Input::ReleaseAll();
			}
			if (menu && which != lastMenuReason) {
				REX::INFO("Fallout menu open: {} (input goes to Fallout)", which);
			}
			lastMenuReason = menu ? which : std::string{};
			st.falloutMenuOpen = menu || loading;

			// World identity: exterior worldspace or interior cell. A change wipes Minecraft's collision.
			if (cell) {
				const auto id = WorldIdOf(cell);
				if (id != worldId) {
					REX::INFO("world changed {:08X} -> {:08X}", worldId, id);
					worldId = id;
					Worlds::Select(cell);  // its own place in Minecraft's world
					++epoch;
					Collision::Get().Reset(epoch);
					teleportPending = true;
					settleTimer = kSettleSeconds;
					haveSafeGround = haveGround = pinned = false;
				}
			}

			// Fallout moved the player itself (load door, fast travel, script, loading a save).
			const RE::NiPoint3 current{ a_player->data.location.x, a_player->data.location.y, a_player->data.location.z };
			if (loading) {
				teleportPending = true;
				haveLastSet = false;
				settleTimer = kSettleSeconds;
			} else if (haveLastSet && current.GetDistance(lastSetPos) > kTeleportThreshold) {
				REX::INFO("Fallout moved the player ({:.0f} units); resyncing Minecraft", current.GetDistance(lastSetPos));
				teleportPending = true;
				haveLastSet = false;
			}

			const char* takeoverNow = a_player->IsDead(false) ? nullptr : FalloutTakeover();
			if ((takeoverNow != nullptr) != (takeover != nullptr)) {
				if (takeoverNow) {
					REX::INFO("Fallout takes the player ({})", takeoverNow);
				} else {
					// Minecraft picks up wherever Fallout left the player - unless Fallout didn't move
					// them (the Pip-Boy): then Minecraft carries on from where it is, instead of being
					// put back to the slightly older spot Fallout's player was shown at.
					const bool moved = !haveLastSet || current.GetDistance(lastSetPos) > 100.0f;
					REX::INFO("Fallout hands the player back{}", moved ? "; Minecraft follows" : "");
					if (moved) {
						teleportPending = true;
					}
				}
			}
			takeover = takeoverNow;

			if (teleportPending && !loading && !takeover) {
				++teleportSeq;
				teleportPending = false;
				st.yaw = HeadingToMcYaw(a_player->data.angle.z);
				st.pitch = a_player->data.angle.x * kRadToDeg;
				st.lookInitialized = true;
			}

			// Mouse look (Minecraft's formula), integrated here so the camera has no added latency.
			float dx = 0.0f, dy = 0.0f;
			Input::ConsumeLook(dx, dy);
			if (!st.lookInitialized) {
				st.yaw = HeadingToMcYaw(a_player->data.angle.z);
				st.pitch = a_player->data.angle.x * kRadToDeg;
				st.lookInitialized = true;
			}
			if (!st.mcScreenOpen && !st.falloutMenuOpen) {
				const float s = st.sensitivity * 0.6f + 0.2f;
				const float factor = s * s * s * 8.0f * 0.15f;
				st.yaw = std::fmod(st.yaw + dx * factor, 360.0f);
				st.pitch = std::clamp(st.pitch + dy * factor, -90.0f, 90.0f);
			}

			// Minecraft holds its player after a teleport until the ground around them arrives. If it
			// holds somewhere Fallout's player isn't, that ground never comes: send it again.
			const bool arriving = haveMc && inWorld && !loading && mc.teleportAck != teleportSeq && !takeover;
			// Minecraft got the teleport but holds the player until it has ground there. If that
			// doesn't come (its copy of the collision there is broken), go back to where it last stood.
			if (arriving && mc.teleportAck + 1 == teleportSeq) {
				settleWait += a_delta;
				if (pinned) {
					a_player->SetPosition(pinAt, true);
					if (auto* controller = CharController(a_player)) {
						controller->SetLinearVelocityImpl(RE::hkVector4f(0.0f, 0.0f, 0.0f, 0.0f));
						controller->fallStartHeight = pinAt.z;
						controller->fallTime = 0.0f;
					}
				}
				if (settleWait > 1.5f && haveSafeGround && pinned) {
					REX::WARN("Minecraft has held the player {:.0f} s without finding ground at ({:.0f}, {:.0f}, {:.0f}); moving both players back to ({:.0f}, {:.0f}, {:.0f})",
						settleWait, current.x, current.y, current.z, safeGround.x, safeGround.y, safeGround.z);
					a_player->SetPosition(safeGround, true);
					pinAt = safeGround;
					pinned = true;
					haveSafeGround = false;  // if that fails too, let Minecraft's own timeout decide
					Collision::Get().Refresh();
					teleportPending = true;
					settleWait = 0.0f;
				}
			} else {
				settleWait = 0.0f;
				if (!arriving) {
					pinned = false;
				}
			}
			if (arriving) {
				const auto   here = GameToMc(current);
				const double gap = std::sqrt((here.x - mc.x) * (here.x - mc.x) + (here.y - mc.y) * (here.y - mc.y) + (here.z - mc.z) * (here.z - mc.z));
				holdMismatch = gap > 8.0 ? holdMismatch + a_delta : 0.0f;
				if (holdMismatch > 1.0f) {
					REX::INFO("Minecraft is waiting {:.0f} blocks from Fallout's player; teleporting it again", gap);
					teleportPending = true;
					holdMismatch = 0.0f;
				}
			} else {
				holdMismatch = 0.0f;
			}

			const bool dead = a_player->IsDead(false);
			bool       puppet = haveMc && inWorld && mc.teleportAck == teleportSeq && !loading && !dead && !takeover;
			st.minecraftOwnsPlayer = puppet || (arriving && !dead);
			if (puppet != st.puppeting) {
				REX::INFO("puppet {}", puppet ? "on (Minecraft drives the player)" : "off");
			}
			st.puppeting = puppet;
			st.mcCrosshair = puppet && mc.cameraMode == 0 && !st.mcScreenOpen && !st.falloutMenuOpen;
			HideFirstPerson(a_player, puppet && !st.falloutMenuOpen);  // the Pip-Boy needs Fallout's arm
			Hud::Update(puppet);

			// Minecraft's 20 Hz physics ticks, interpolated on our own clock exactly like Minecraft's
			// renderer does with partial ticks (sampling its per-frame position judders instead).
			double feetX = mc.x, feetY = mc.y, feetZ = mc.z;
			if (mc.tickQpc != 0 && mc.tickMs > 0.0f) {
				Interpolate(feetX, feetY, feetZ);
			}

			// Fall rescue: if Minecraft's player drops through a gap in the collision (ground that
			// hadn't arrived yet), put both players back where they last stood instead of letting
			// them fall under Fallout's world (fall damage, and Fallout crashed down there).
			if (puppet && (mc.flags & proto::kMcOnGround) && !(mc.flags & (proto::kMcFlying | proto::kMcSwimming))) {
				lastGround = current;
				lastGroundMcY = mc.y;
				haveGround = true;
				safeGround = current;
				haveSafeGround = true;
			}
			sinceRescue += a_delta;
			const bool airborne = !(mc.flags & (proto::kMcOnGround | proto::kMcFlying | proto::kMcSwimming));
			const auto feetNow = McToGame(feetX, feetY, feetZ);
			bool       rescue = false;
			RE::NiPoint3 rescueAt{};
			// Main check: the feet's path since last frame, against Fallout's own collision. Minecraft
			// can't legitimately move the feet down through a Fallout floor (its collision is a copy of
			// it), so if they did, its copy had a hole there: stop them on Fallout's surface right away,
			// before any fall builds up.
			if (puppet && airborne && havePrevFeet && feetNow.z < prevFeet.z - 0.5f) {
				const float mx = feetNow.x - prevFeet.x, my = feetNow.y - prevFeet.y;
				if (mx * mx + my * my < 300.0f * 300.0f) {
					// From just above last frame's feet (feet resting slightly inside a floor aren't
					// "through" it) - or, once below the ground they jumped from, from that height, so a
					// slow sink through a floor over several frames is caught too.
					float fromZ = prevFeet.z + 20.0f;
					if (haveGround && feetNow.z < lastGround.z) {
						fromZ = std::max(fromZ, std::min(lastGround.z + 40.0f, feetNow.z + 420.0f));
					}
					const RE::NiPoint3 from{ feetNow.x, feetNow.y, fromZ };
					RE::NiPoint3       hit{};
					int                layer = -1;
					std::uint32_t      bodyFlags = 0;
					const RE::hknpBody* hitBody = nullptr;
					const bool  below = PickGround(cell, from, feetNow, hit, layer, &bodyFlags, &hitBody);
					const float depth = below ? hit.z - feetNow.z : 0.0f;
					// Steep ground is block-coarsened for Minecraft, so feet a little under Fallout's
					// surface happen while running downhill: through means deep, or getting deeper.
					// Minecraft stands up to about a block under Fallout's surface on steep ground (its
					// voxel copy is coarser there): that is not falling through. Through means well
					// over a block deep, or sinking deeper frame after frame.
					sinking = (depth > lastDepth + 4.0f && depth > 30.0f) ? sinking + 1 : 0;
					const bool through = depth > 160.0f || (depth > 110.0f && sinking >= 4);
					lastDepth = depth;
					if (through) {
						rescue = true;
						rescueAt = { hit.x, hit.y, hit.z + 8.0f };
						REX::WARN("Minecraft's feet went {:.0f} units through Fallout's ground (layer {}, body flags {:X}) at ({:.0f}, {:.0f}, {:.0f}); standing the player back on it",
							hit.z - feetNow.z, layer, bodyFlags, hit.x, hit.y, hit.z);
						const auto hitMc = GameToMc({ hit.x, hit.y, hit.z - 10.0f });
						REX::WARN("  Minecraft was sent {} triangles for the region there ({}, {}, {})", Collision::Get().TrianglesAt(hitMc),
							static_cast<int>(std::floor(hitMc.x / 8)), static_cast<int>(std::floor(hitMc.y / 8)), static_cast<int>(std::floor(hitMc.z / 8)));
						REX::WARN("  {}", Collision::Get().Describe(hitBody, GameToMc(hit)));
					}
				}
			}
			// Fallback: a long drop below where the player last stood.
			if (!rescue && puppet && haveGround && airborne && mc.y < lastGroundMcY - kRescueDrop) {
				rescue = true;
				rescueAt = lastGround;
				RE::NiPoint3 hit{};
				int          layer = -1;
				// Prefer the ground straight above where they are now, if Fallout has one.
				if (PickGround(cell, { feetNow.x, feetNow.y, lastGround.z + 200.0f }, { feetNow.x, feetNow.y, feetNow.z }, hit, layer)) {
					rescueAt = { hit.x, hit.y, hit.z + 8.0f };
				}
				REX::WARN("Minecraft's player fell {:.0f} blocks through missing ground at ({:.0f}, {:.0f}, {:.0f}); putting both players back at ({:.0f}, {:.0f}, {:.0f})",
					lastGroundMcY - mc.y, feetNow.x, feetNow.y, feetNow.z, rescueAt.x, rescueAt.y, rescueAt.z);
			}
			if (rescue) {
				// The same hole again: Minecraft has no ground there, so go back to where it last stood.
				const float again = (rescueAt - lastRescueAt).Length();
				if (rescues > 0 && sinceRescue < 10.0f && again < 300.0f && haveSafeGround) {
					REX::WARN("same hole again; putting the player back where Minecraft last stood ({:.0f}, {:.0f}, {:.0f})", safeGround.x, safeGround.y, safeGround.z);
					rescueAt = safeGround;
				}
				lastRescueAt = rescueAt;
				sinceRescue = 0.0f;
				++rescues;
				a_player->SetPosition(rescueAt, true);
				pinAt = rescueAt;
				pinned = true;
				Collision::Get().Refresh();  // harvest the ground around there again
				teleportPending = true;      // Minecraft follows (its fall distance is reset on teleport)
				haveLastSet = false;
				haveGround = false;
				havePrevFeet = false;
				puppet = false;
				st.puppeting = false;
			}
			if (puppet) {
				prevFeet = feetNow;
				havePrevFeet = true;
				if (!airborne) {
					lastDepth = 0.0f;
					sinking = 0;
				}
			} else {
				havePrevFeet = false;
				lastDepth = 0.0f;
				sinking = 0;
			}

			if (!puppet) {
				st.feetValid = false;
				st.cameraMode = 0;
				Camera::Set(false, 0, RE::NiPoint3{}, 0.0f, 0.0f, 0.0f);
			}

			// Minecraft's feet sometimes dip up to a block or so under the road for a moment (its
			// exact-triangle collider and Fallout's surface disagree for a tick or two) and come back
			// up by themselves. Fallout's player and the camera stay on Fallout's surface meanwhile,
			// instead of sinking into the road and popping back up.
			float lift = 0.0f;
			trustedAge += a_delta;
			dipLogTimer -= a_delta;
			if (puppet) {
				RE::NiPoint3 ground{};
				if (PickGroundAt(cell, { feetNow.x, feetNow.y, feetNow.z + 120.0f }, { feetNow.x, feetNow.y, feetNow.z - 30.0f }, ground)) {
					const float d = ground.z - feetNow.z;
					if (!airborne && std::fabs(d) < 10.0f) {
						trustedZ = ground.z;
						trustedAge = 0.0f;
					} else if (d > 3.0f && d < 120.0f && trustedAge < 3.0f && std::fabs(ground.z - trustedZ) < 40.0f) {
						lift = d;
						if (d > 40.0f && dipLogTimer <= 0.0f) {
							dipLogTimer = 2.0f;
							REX::INFO("Minecraft's feet dipped {:.0f} units under Fallout's surface (Minecraft y {:.3f}, ticks {:.3f} -> {:.3f}, {}); showing the player on the surface",
								d, mc.y, mc.prevY, mc.curY, airborne ? "airborne" : "on ground");
						}
					}
				}
			}

			if (puppet) {
				const auto pos = McToGame(feetX, feetY + lift / proto::kUnitsPerBlock, feetZ);
				a_player->SetPosition(pos, true);
				if (auto* controller = CharController(a_player)) {
					// Minecraft moves the player; Fallout keeps no momentum or fall damage of its own.
					controller->SetLinearVelocityImpl(RE::hkVector4f(0.0f, 0.0f, 0.0f, 0.0f));
					controller->fallStartHeight = pos.z;
					controller->fallTime = 0.0f;
				}
				lastSetPos = pos;
				haveLastSet = true;

				// Minecraft's F5 view: behind the player, or in front looking back. Its zoom pulls in the
				// moment something is behind the player and eases back out (as Minecraft's does).
				st.feetX = feetX, st.feetY = feetY + lift / proto::kUnitsPerBlock, st.feetZ = feetZ;
				st.feetValid = true;
				st.cameraMode = static_cast<int>(mc.cameraMode);
				{
					const bool detached = mc.cameraMode != 0 && mc.cameraDistance > 0.0f && !st.falloutMenuOpen;
					if (!detached || zoomMode != mc.cameraMode || mc.cameraDistance < zoom) {
						zoom = detached ? mc.cameraDistance : 0.0f;
					} else {
						zoom += (mc.cameraDistance - zoom) * (1.0f - std::exp(-std::max(a_delta, 0.0f) / 0.2f));
					}
					zoomMode = mc.cameraMode;
					const float eyeHeight = mc.eyeHeight > 0.1f ? mc.eyeHeight : 1.62f;
					const auto  eye = McToGame(feetX, feetY + lift / proto::kUnitsPerBlock + eyeHeight, feetZ);
					Camera::Set(detached, static_cast<int>(mc.cameraMode), eye, McYawToHeading(st.yaw), st.pitch * kDegToRad,
						zoom * static_cast<float>(proto::kUnitsPerBlock));
				}

				if (!st.falloutMenuOpen) {
					a_player->data.angle.z = McYawToHeading(st.yaw);
					a_player->data.angle.x = st.pitch * kDegToRad;
					ForceFirstPerson();
				}
			}

			// Tell Minecraft where Fallout's player is and where they're looking.
			proto::SkyState sky{};
			sky.flags = (cell ? proto::kSkyInGame : 0u) | (menu ? proto::kSkyMenuOpen : 0u) | (loading ? proto::kSkyLoading : 0u);
			const auto playerMc = GameToMc(current);
			sky.worldId = worldId;
			sky.collisionEpoch = epoch;
			sky.posX = playerMc.x;
			sky.posY = playerMc.y;
			sky.posZ = playerMc.z;
			sky.yaw = st.yaw;
			sky.pitch = st.pitch;
			sky.teleportSeq = teleportSeq;
			sky.viewportW = static_cast<std::uint32_t>(st.viewportW.load());
			sky.viewportH = static_cast<std::uint32_t>(st.viewportH.load());
			if (auto* calendar = RE::Calendar::GetSingleton(); calendar && calendar->gameHour) {
				sky.gameHour = calendar->gameHour->GetValue();
			}
			// Fallout's own ground around Minecraft's player, for Minecraft to land on when its copy of
			// the triangles misses (sprint-jump landings, seams in the road).
			if (puppet && cell) {
				constexpr float kStep = 0.5f;
				const int       n = static_cast<int>(proto::kGroundGrid);
				const double    x0 = std::floor(mc.x / kStep) * kStep - kStep * (n / 2);
				const double    z0 = std::floor(mc.z / kStep) * kStep - kStep * (n / 2);
				// From a step above the feet (or above where the ground just was, if the feet are
				// already under it) down to 3 blocks below.
				const double from = std::max(mc.y, groundRefAge < 1.0f ? double(groundRef) : mc.y) + 0.6;
				const double to = mc.y - 3.0;
				for (int j = 0; j < n; ++j) {
					for (int i = 0; i < n; ++i) {
						const double x = x0 + i * kStep, z = z0 + j * kStep;
						RE::NiPoint3 hit{};
						float        y = std::numeric_limits<float>::quiet_NaN();
						if (PickGroundAt(cell, McToGame(x, from, z), McToGame(x, to, z), hit)) {
							y = float(double(hit.z) / proto::kUnitsPerBlock);
						}
						sky.groundY[i + j * n] = y;
					}
				}
				sky.groundX0 = float(x0);
				sky.groundZ0 = float(z0);
				sky.groundStep = kStep;
				sky.groundN = proto::kGroundGrid;
				const float center = sky.groundY[(n / 2) + (n / 2) * n];
				if (center == center) {
					groundRef = center;
					groundRefAge = 0.0f;
				}
			}
			groundRefAge += a_delta;
			// Fallout's S.P.E.C.I.A.L. for Minecraft's player (strength, speed, health, ...).
			if (auto* avs = RE::ActorValue::GetSingleton()) {
				RE::ActorValueInfo* special[7] = { avs->strength, avs->perception, avs->endurance, avs->charisma, avs->intelligence, avs->agility, avs->luck };
				bool ok = true;
				for (int k = 0; k < 7; ++k) {
					if (!special[k]) {
						ok = false;
						break;
					}
					sky.special[k] = static_cast<std::uint8_t>(std::clamp(a_player->GetActorValue(*special[k]), 0.0f, 20.0f));
				}
				sky.specialValid = ok ? 1 : 0;
			}
			link::WriteSkyState(sky);

			Combat::PerFrame(a_player, puppet && !st.falloutMenuOpen, a_delta);

			settleTimer -= a_delta;
			if (haveMc && !loading && cell && settleTimer <= 0.0f) {
				Collision::Get().Update(cell, puppet ? McVec{ mc.x, mc.y, mc.z } : playerMc);
				BlockCollision::Update(cell, puppet ? McVec{ mc.x, mc.y, mc.z } : playerMc);
			}

			logTimer -= a_delta;
			if (logTimer <= 0.0f) {
				logTimer = 5.0f;
				if (frames > 0) {
					REX::INFO("motion: {} of {} frames waited for a late Minecraft tick, render delay {:.1f} ms", lateFrames, frames, renderDelayMs);
				}
				lateFrames = frames = 0;
				REX::INFO("frame: minecraft={} inWorld={} puppet={} ack={}/{} game=({:.0f}, {:.0f}, {:.0f}) mc=({:.2f}, {:.2f}, {:.2f}) onGround={} yaw={:.1f} pitch={:.1f} world={:08X} epoch={}",
					mcAlive ? "connected" : "no", inWorld, puppet, haveMc ? mc.teleportAck : 0, teleportSeq, current.x, current.y, current.z,
					mc.x, mc.y, mc.z, (mc.flags & proto::kMcOnGround) != 0, st.yaw, st.pitch, worldId, epoch);
			}
		}

		struct PlayerUpdateHook
		{
			static void thunk(RE::PlayerCharacter* a_this, float a_delta)
			{
				func(a_this, a_delta);
				try {
					PerFrame(a_this, a_delta);
				} catch (const std::exception& e) {
					REX::ERROR("per-frame update: {}", e.what());
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	bool PickGroundAt(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, RE::NiPoint3& a_hit)
	{
		// Bodies, the player's capsule and other non-ground things in the way: look past them.
		RE::NiPoint3 from = a_from;
		for (int attempt = 0; attempt < 3; ++attempt) {
			int layer = -1;
			if (PickGround(a_cell, from, a_to, a_hit, layer)) {
				return true;
			}
			if (layer < 0) {
				return false;  // nothing hit at all
			}
			// Something that isn't ground was hit first: start again just past it.
			const RE::NiPoint3 dir = a_to - from;
			const float        len = dir.Length();
			if (len < 1.0f || (a_to - a_hit).Length() < 4.0f) {
				return false;
			}
			from = a_hit + dir * (2.0f / len);
		}
		return false;
	}

	namespace Game
	{
		void Install()
		{
			// Actor::Update(float) is vfunc 0xCF (RE/A/Actor.h); PlayerCharacter's primary vtable.
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE::PlayerCharacter[0] };
			PlayerUpdateHook::func = vtbl.write_vfunc(0xCF, PlayerUpdateHook::thunk);
			Collision::Get().Start();
			REX::INFO("game hooks installed (PlayerCharacter::Update)");
		}

		void KeepFirstPersonHidden()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}
			// Live check: menus like the Pip-Boy may pause the game, and then the per-frame update
			// that normally notices them doesn't run.
			if (FalloutMenuOpen()) {
				State().falloutMenuOpen = true;
				if (firstPersonHidden || !hiddenFirstPerson.empty()) {
					ShowFirstPerson(player);  // the Pip-Boy needs Fallout's arm
				}
				return;
			}
			if (State().puppeting) {
				HideFirstPersonNow(player);
			}
		}

		void NotePipboyKey()
		{
			pipboyUntilMs = NowMs() + 2500;
		}

		bool FalloutMenuOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return AnyBlockingMenuOpen(ui) || (ui && ui->GetMenuOpen("PipboyMenu"));
		}

		void OnGameLoaded()
		{
			teleportPending = true;
			haveLastSet = false;
			haveSafeGround = haveGround = pinned = false;
			State().lookInitialized = false;
			Hud::Reset();
		}
	}
}
