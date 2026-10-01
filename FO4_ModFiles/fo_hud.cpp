// FalloutCraft: Fallout's HUD while Minecraft drives the player.
//
// Minecraft's own HUD (hearts, hunger, hotbar, crosshair, XP) is drawn over the frame, so the
// Fallout parts that show the same things go: the HP bar, AP bar, ammo counter and crosshair.
// Kept: the compass, the activation prompt ("E) Open"), quest/XP notifications, subtitles, the
// enemy health bar.
//
// Fallout's HUD is ActionScript 3; the clips are looked up by instance name, and every name is
// logged once so the list can be adjusted if a name turns out different.

#include "fo_common.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace skycraft::Hud
{
	namespace
	{
		// Paths from the HUD menu's root (instance names in HUDMenu.swf).
		constexpr const char* kHidden[] = {
			"LeftMeters_mc.HPMeter_mc",
			"LeftMeters_mc.RadsMeter_mc",
			"RightMeters_mc.ActionPointMeter_mc",
			"RightMeters_mc.AmmoCount_mc",
			"RightMeters_mc.ExplosiveAmmoCount_mc",
			"CenterGroup_mc.HUDCrosshair_mc",
		};
		// Searched for by name anywhere in the first levels if the paths above don't resolve.
		constexpr const char* kHiddenNames[] = {
			"HPMeter_mc", "ActionPointMeter_mc", "AmmoCount_mc", "ExplosiveAmmoCount_mc", "HUDCrosshair_mc", "RadsMeter_mc",
		};

		struct Clip
		{
			std::string          path;
			Scaleform::GFx::Value value;
		};

		const RE::IMenu*   scannedFor = nullptr;
		// Never destroyed: releasing Scaleform values after Fallout shut Scaleform down crashed on
		// exit (2026-10-01 05:11).
		std::vector<Clip>& clips = *new std::vector<Clip>();
		bool              hidden = false;

		bool Resolve(const Scaleform::GFx::Value& a_root, const std::string& a_path, Scaleform::GFx::Value& a_out)
		{
			Scaleform::GFx::Value cur = a_root;
			std::size_t           start = 0;
			while (start <= a_path.size()) {
				const auto end = a_path.find('.', start);
				const auto name = a_path.substr(start, end == std::string::npos ? std::string::npos : end - start);
				Scaleform::GFx::Value next;
				if (!cur.IsObject() || !cur.GetMember(name, &next) || !next.IsObject()) {
					return false;
				}
				cur = next;
				if (end == std::string::npos) {
					break;
				}
				start = end + 1;
			}
			a_out = cur;
			return true;
		}

		void Search(const Scaleform::GFx::Value& a_node, const std::string& a_path, int a_depth, std::string& a_log)
		{
			if (a_depth > 3 || !a_node.IsObject()) {
				return;
			}
			a_node.VisitMembers([&](const char* a_name, const Scaleform::GFx::Value& a_val) {
				if (!a_name || !a_val.IsDisplayObject()) {
					return;
				}
				const std::string path = a_path.empty() ? a_name : a_path + "." + a_name;
				if (a_depth <= 1) {
					a_log += " " + path;
				}
				for (const auto* wanted : kHiddenNames) {
					if (std::strcmp(a_name, wanted) == 0) {
						bool known = false;
						for (const auto& c : clips) {
							known |= c.path == path;
						}
						if (!known) {
							clips.push_back({ path, a_val });
						}
					}
				}
				Search(a_val, path, a_depth + 1, a_log);
			});
		}

		void Scan(RE::IMenu* a_hud)
		{
			clips.clear();
			scannedFor = a_hud;
			auto& root = a_hud->menuObj;
			if (!root.IsObject()) {
				REX::WARN("HUD: the HUD menu has no root object yet");
				scannedFor = nullptr;
				return;
			}
			for (const auto* path : kHidden) {
				Scaleform::GFx::Value v;
				if (Resolve(root, path, v)) {
					clips.push_back({ path, v });
				}
			}
			std::string log;
			Search(root, "", 0, log);
			std::string found;
			for (const auto& c : clips) {
				found += " " + c.path;
			}
			REX::INFO("HUD: clips in Fallout's HUD:{}", log.empty() ? " (none listed)" : log);
			REX::INFO("HUD: hiding while Minecraft drives:{}", found.empty() ? " NOTHING FOUND" : found);
		}

		void SetVisible(bool a_visible)
		{
			const Scaleform::GFx::Value visible(a_visible);
			for (auto& c : clips) {
				if (c.value.IsObject()) {
					c.value.SetMember("visible", visible);
				}
			}
		}
	}

	// Main thread, every frame. Fallout's HUD (Scaleform) belongs to the UI thread, so all the
	// work is handed to it as a UI task, a few times a second (touching it from here raced the UI
	// thread).
	void Update(bool a_hide)
	{
		static std::atomic<bool> pending{ false };
		static bool              lastWanted = false;
		static std::uint64_t     lastQueued = 0;
		const std::uint64_t      now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		if (a_hide == lastWanted && (!a_hide || now - lastQueued < 250)) {
			return;
		}
		auto* tasks = F4SE::GetTaskInterface();
		if (!tasks || pending.exchange(true)) {
			return;
		}
		lastWanted = a_hide;
		lastQueued = now;
		tasks->AddUITask([a_hide]() {
			pending = false;
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return;
			}
			auto hud = ui->GetMenu("HUDMenu");
			if (!hud) {
				scannedFor = nullptr;
				clips.clear();
				return;
			}
			if (scannedFor != hud.get()) {
				if (!a_hide) {
					return;
				}
				Scan(hud.get());
			}
			if (a_hide) {
				SetVisible(false);  // again and again: Fallout's HUD shows its parts whenever it updates them
				hidden = true;
			} else if (hidden) {
				SetVisible(true);
				hidden = false;
			}
		});
	}
}
