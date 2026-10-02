// FalloutCraft: scavenging Minecraft things in Fallout.
//
// The first time the player opens a container (a desk, a toolbox, a fridge...) or searches a corpse,
// Minecraft gets a little ordinary loot too (sticks, coal, iron nuggets, bread, bones, string...).
// The loot itself is picked on the Minecraft side (InputBridge / FalloutScavenge). Each container and
// corpse gives it once; the ones already searched are remembered in
// Documents\My Games\Fallout4\F4SE\FalloutCraft_scavenged.txt.

#include "fo_common.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef ERROR

#include <cstdio>
#include <unordered_set>

namespace skycraft::Scavenge
{
	namespace
	{
		std::unordered_set<std::uint32_t> searched;
		wchar_t                           filePath[MAX_PATH]{};

		void Load()
		{
			wchar_t profile[MAX_PATH]{};
			if (!::GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) {
				return;
			}
			std::swprintf(filePath, MAX_PATH, L"%s\\Documents\\My Games\\Fallout4\\F4SE\\FalloutCraft_scavenged.txt", profile);
			FILE* f = nullptr;
			if (_wfopen_s(&f, filePath, L"r") != 0 || !f) {
				return;
			}
			unsigned id = 0;
			while (std::fscanf(f, "%x", &id) == 1) {
				searched.insert(id);
			}
			std::fclose(f);
		}

		void Remember(std::uint32_t a_id)
		{
			searched.insert(a_id);
			if ((a_id >> 24) == 0xFF) {
				return;  // a spawned ref: its FormID gets reused by later ones, so only this session
			}
			FILE* f = nullptr;
			if (filePath[0] && _wfopen_s(&f, filePath, L"a") == 0 && f) {
				std::fprintf(f, "%08X\n", a_id);
				std::fclose(f);
			}
		}

		class ActivateSink final : public RE::BSTEventSink<RE::TESActivateEvent>
		{
		public:
			static ActivateSink* Get()
			{
				static ActivateSink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent& a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
			{
				auto* player = RE::PlayerCharacter::GetSingleton();
				auto* ref = a_event.objectActivated.get();
				if (!player || !ref || a_event.actionRef.get() != player) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const std::uint32_t id = ref->GetFormID();
				if (searched.contains(id)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				std::uint16_t kind = 0xFFFF;
				std::int32_t  level = 0;
				if (auto* actor = ref->As<RE::Actor>()) {
					if (actor->IsDead(false)) {
						kind = 1;
						level = actor->GetLevel();
					}
				} else if (auto* base = ref->GetObjectReference(); base && base->GetFormType() == RE::ENUM_FORM_ID::kCONT) {
					kind = 0;
				}
				if (kind == 0xFFFF) {
					return RE::BSEventNotifyControl::kContinue;
				}
				Remember(id);
				link::PushInput(proto::kInScavenge, kind, static_cast<std::int32_t>(id), level);
				REX::INFO("scavenge: {} {:08X} searched for the first time; Minecraft adds some loot", kind == 1 ? "corpse" : "container", id);
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		Load();
		if (auto* source = RE::TESActivateEvent::GetEventSource()) {
			source->RegisterSink(ActivateSink::Get());
			REX::INFO("scavenge: listening to activations ({} containers and corpses already searched)", searched.size());
		} else {
			REX::WARN("scavenge: no activate event source; no Minecraft loot from containers");
		}
	}
}
