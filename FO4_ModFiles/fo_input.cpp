// FalloutCraft Phase 1: while Minecraft drives the player, keyboard and mouse go to Minecraft.
//
// Fallout hands every frame's input queue to its input receivers. Two are hooked:
//  - PlayerControls (moving, looking, shooting, jumping, sneaking...): while Minecraft owns the
//    player it gets nothing, and the events are forwarded to Minecraft instead. G becomes
//    Fallout's Activate (doors, NPCs, containers, terminals); E stays Minecraft's inventory.
//  - MenuControls (Pip-Boy, pause menu, console, quick save/load, screenshots...): only Esc
//    (pause menu), Tab (Pip-Boy), ~ (console) and F9 (quickload) reach it.
// Ported from the Skyrim plugin (skse/src/Input.cpp).

#include "fo_common.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef ERROR  // wingdi.h; clashes with REX::ERROR

#include <array>
#include <vector>

namespace skycraft
{
	namespace
	{
		// DirectInput scan code (what Fallout reports) -> SDL scancode / USB HID usage (what Minecraft uses).
		constexpr auto kDikToSdl = [] {
			std::array<std::uint16_t, 256> t{};
			t[0x01] = 41;  // Esc
			for (int i = 0; i < 9; ++i) t[0x02 + i] = static_cast<std::uint16_t>(30 + i);  // 1-9
			t[0x0B] = 39;  // 0
			t[0x0C] = 45, t[0x0D] = 46, t[0x0E] = 42, t[0x0F] = 43;  // - = Backspace Tab
			t[0x10] = 20, t[0x11] = 26, t[0x12] = 8, t[0x13] = 21, t[0x14] = 23;  // Q W E R T
			t[0x15] = 28, t[0x16] = 24, t[0x17] = 12, t[0x18] = 18, t[0x19] = 19;  // Y U I O P
			t[0x1A] = 47, t[0x1B] = 48, t[0x1C] = 40, t[0x1D] = 224;               // [ ] Enter LCtrl
			t[0x1E] = 4, t[0x1F] = 22, t[0x20] = 7, t[0x21] = 9, t[0x22] = 10;     // A S D F G
			t[0x23] = 11, t[0x24] = 13, t[0x25] = 14, t[0x26] = 15;                // H J K L
			t[0x27] = 51, t[0x28] = 52, t[0x29] = 53, t[0x2A] = 225, t[0x2B] = 49;  // ; ' ` LShift backslash
			t[0x2C] = 29, t[0x2D] = 27, t[0x2E] = 6, t[0x2F] = 25, t[0x30] = 5;    // Z X C V B
			t[0x31] = 17, t[0x32] = 16, t[0x33] = 54, t[0x34] = 55, t[0x35] = 56;  // N M , . /
			t[0x36] = 229, t[0x37] = 85, t[0x38] = 226, t[0x39] = 44, t[0x3A] = 57;  // RShift KP* LAlt Space Caps
			for (int i = 0; i < 10; ++i) t[0x3B + i] = static_cast<std::uint16_t>(58 + i);  // F1-F10
			t[0x45] = 83, t[0x46] = 71;                                             // NumLock ScrollLock
			t[0x47] = 95, t[0x48] = 96, t[0x49] = 97, t[0x4A] = 86;                 // KP7 KP8 KP9 KP-
			t[0x4B] = 92, t[0x4C] = 93, t[0x4D] = 94, t[0x4E] = 87;                 // KP4 KP5 KP6 KP+
			t[0x4F] = 89, t[0x50] = 90, t[0x51] = 91, t[0x52] = 98, t[0x53] = 99;   // KP1 KP2 KP3 KP0 KP.
			t[0x56] = 100, t[0x57] = 68, t[0x58] = 69;                              // OEM102 F11 F12
			t[0x9C] = 88, t[0x9D] = 228, t[0xB5] = 84, t[0xB7] = 70, t[0xB8] = 230;  // KPEnter RCtrl KP/ PrtSc RAlt
			t[0xC5] = 72, t[0xC7] = 74, t[0xC8] = 82, t[0xC9] = 75, t[0xCB] = 80;  // Pause Home Up PgUp Left
			t[0xCD] = 79, t[0xCF] = 77, t[0xD0] = 81, t[0xD1] = 78, t[0xD2] = 73;  // Right End Down PgDn Insert
			t[0xD3] = 76, t[0xDB] = 227, t[0xDC] = 231, t[0xDD] = 101;             // Delete LWin RWin Menu
			return t;
		}();

		// Fallout 4 reports keys as Windows virtual-key codes (VK_*), not DirectInput scan codes like
		// Skyrim. Minecraft wants the physical key, so map VK -> scan code with the current keyboard
		// layout (MAPVK_VK_TO_VSC_EX, extended keys as 0x80 | code, DirectInput style).
		std::uint32_t DikFromVk(std::uint32_t a_vk)
		{
			static std::array<std::uint16_t, 256> cache{};
			static std::array<bool, 256>          known{};
			if (a_vk >= 256) {
				return 0;
			}
			if (!known[a_vk]) {
				known[a_vk] = true;
				const UINT sc = ::MapVirtualKeyW(a_vk, MAPVK_VK_TO_VSC_EX);
				std::uint16_t dik = static_cast<std::uint16_t>(sc & 0x7F);
				if ((sc & 0xFF00) == 0xE000 || (sc & 0xFF00) == 0xE100) {
					dik |= 0x80;
				}
				// Keys Windows reports without the extended flag although DirectInput has it.
				switch (a_vk) {
				case VK_LEFT: dik = 0xCB; break;
				case VK_RIGHT: dik = 0xCD; break;
				case VK_UP: dik = 0xC8; break;
				case VK_DOWN: dik = 0xD0; break;
				case VK_PRIOR: dik = 0xC9; break;
				case VK_NEXT: dik = 0xD1; break;
				case VK_HOME: dik = 0xC7; break;
				case VK_END: dik = 0xCF; break;
				case VK_INSERT: dik = 0xD2; break;
				case VK_DELETE: dik = 0xD3; break;
				case VK_RCONTROL: dik = 0x9D; break;
				case VK_RMENU: dik = 0xB8; break;
				case VK_DIVIDE: dik = 0xB5; break;
				case VK_LWIN: dik = 0xDB; break;
				case VK_RWIN: dik = 0xDC; break;
				case VK_APPS: dik = 0xDD; break;
				default: break;
				}
				cache[a_vk] = dik;
			}
			return cache[a_vk];
		}

		// Keys Fallout keeps while Minecraft drives the player. Everything else is Minecraft's.
		constexpr std::uint32_t kDikEscape = 0x01;   // Fallout pause menu (or closes a Minecraft screen)
		constexpr std::uint32_t kDikTab = 0x0F;      // Pip-Boy (and its light, held)
		constexpr std::uint32_t kDikConsole = 0x29;  // Fallout console
		constexpr std::uint32_t kDikF9 = 0x43;       // Fallout quickload
		constexpr std::uint32_t kDikE = 0x12;        // Fallout Activate, as in Fallout (doors, NPCs, loot, terminals, power armor)
		constexpr std::uint32_t kDikO = 0x18;        // Minecraft pause / options menu (Esc is Fallout's)
		// Keys moved for Minecraft, so Fallout's own layout keeps working (only while no Minecraft
		// screen is open; in screens every key is Minecraft's as usual):
		constexpr std::uint32_t kDikI = 0x17;        // I -> Minecraft's inventory (its E)
		constexpr std::uint32_t kDikLShift = 0x2A;   // Shift -> Minecraft sprint (its Ctrl), held like Fallout's sprint
		constexpr std::uint32_t kDikLCtrl = 0x1D;    // Ctrl  -> Minecraft sneak (its Shift), like Fallout's sneak
		constexpr std::uint16_t kSdlE = 8, kSdlLCtrl = 224, kSdlLShift = 225;

		bool IsFalloutMenuKey(std::uint32_t a_code)
		{
			return a_code == kDikEscape || a_code == kDikTab || a_code == kDikConsole || a_code == kDikF9;
		}

		float lookDx = 0.0f;
		float lookDy = 0.0f;
		// The Minecraft key each physical key's press was sent as, so its release matches even if a
		// screen opened in between.
		std::array<std::uint16_t, 256> sentAs{};

		bool IsDown(const RE::ButtonEvent* a_button) { return a_button->QJustPressed(); }
		bool IsUp(const RE::ButtonEvent* a_button) { return a_button->QReleased(); }

		std::uint16_t MinecraftKeyFor(std::uint32_t a_dik, bool a_screenOpen)
		{
			if (!a_screenOpen) {
				switch (a_dik) {
				case kDikI:
					return kSdlE;
				case kDikLShift:
					return kSdlLCtrl;
				case kDikLCtrl:
					return kSdlLShift;
				default:
					break;
				}
			}
			return kDikToSdl[a_dik & 0xFF];
		}

		enum class Route
		{
			kMinecraft,  // forwarded to Minecraft (or dropped)
			kActivate,   // E: to Fallout's PlayerControls as "Activate"
			kFallout,    // to Fallout's PlayerControls as it is (Tab: the Pip-Boy)
		};

		// Forwards one input event to Minecraft, or says where in Fallout it goes.
		Route Forward(const RE::InputEvent* a_event)
		{
			auto& st = State();
			switch (a_event->eventType.get()) {
			case RE::INPUT_EVENT_TYPE::kMouseMove:
				{
					const auto* mm = static_cast<const RE::MouseMoveEvent*>(a_event);
					if (st.mcScreenOpen) {
						const int x = std::clamp(st.cursorX.load() + mm->mouseInputX, 0, st.viewportW.load() - 1);
						const int y = std::clamp(st.cursorY.load() + mm->mouseInputY, 0, st.viewportH.load() - 1);
						st.cursorX = x;
						st.cursorY = y;
						link::PushInput(proto::kInCursor, 0, x, y);
					} else {
						lookDx += static_cast<float>(mm->mouseInputX);
						lookDy += static_cast<float>(mm->mouseInputY);
					}
					return Route::kMinecraft;
				}
			case RE::INPUT_EVENT_TYPE::kButton:
				{
					const auto* button = static_cast<const RE::ButtonEvent*>(a_event);
					const bool  down = IsDown(button);
					const bool  up = IsUp(button);
					const auto  rawCode = button->QIDCode();
					const auto  device = button->device.get();
					if (device == RE::INPUT_DEVICE::kKeyboard) {
						const auto code = DikFromVk(rawCode);
						// E is Fallout's Activate in every state (pressed, held for power armor or
						// "hold to transfer", released), except in Minecraft's screens where it closes them.
						if (code == kDikE && !st.mcScreenOpen && sentAs[kDikE] == 0) {
							return Route::kActivate;
						}
						if (code == kDikTab && !st.mcScreenOpen) {
							if (down) {
								Game::NotePipboyKey();
							}
							return Route::kFallout;
						}
						if (!down && !up) {
							return Route::kMinecraft;  // held: Minecraft tracks held keys itself
						}
						// With a Minecraft screen up (chat, inventory, options) every key is Minecraft's,
						// so typing works and Esc closes the screen.
						if (!st.mcScreenOpen && down) {
							if (IsFalloutMenuKey(code)) {
								return Route::kMinecraft;  // MenuControls opens Fallout's menu (see below)
							}
							if (code == kDikO) {
								Input::ReleaseAll();
								link::PushInput(proto::kInOpenMenu, 0);
								return Route::kMinecraft;
							}
						}
						if (down) {
							if (const auto sdl = MinecraftKeyFor(code, st.mcScreenOpen)) {
								sentAs[code & 0xFF] = sdl;
								link::PushInput(proto::kInKey, sdl, 1);
							}
						} else if (const auto sdl = sentAs[code & 0xFF]) {
							sentAs[code & 0xFF] = 0;
							link::PushInput(proto::kInKey, sdl, 0);
						}
						return Route::kMinecraft;
					}
					if (!down && !up) {
						return Route::kMinecraft;
					}
					if (device == RE::INPUT_DEVICE::kMouse) {
						const auto code = rawCode;
						if (code == 0x800 || code == 0x900) {  // wheel up / down
							if (down) {
								link::PushInput(proto::kInScroll, 0, code == 0x800 ? 120 : -120);
							}
							return Route::kMinecraft;
						}
						static constexpr std::uint16_t kButtons[8] = { 1, 3, 2, 4, 5, 0, 0, 0 };  // L R M X1 X2 -> SDL
						const std::uint16_t            sdlButton = code < 8 ? kButtons[code] : std::uint16_t(0);
						if (sdlButton) {
							link::PushInput(proto::kInMouseButton, sdlButton, down ? 1 : 0);
						}
					}
					return Route::kMinecraft;
				}
			case RE::INPUT_EVENT_TYPE::kChar:
				if (st.mcScreenOpen) {
					link::PushInput(proto::kInText, 0, static_cast<std::int32_t>(static_cast<const RE::CharacterEvent*>(a_event)->charCode));
				}
				return Route::kMinecraft;
			default:
				return Route::kMinecraft;
			}
		}

		// PlayerControls turns input into the Fallout player's own actions. While Minecraft drives
		// the player it gets nothing but E, as Fallout's "Activate".
		struct PlayerControlsHook
		{
			static void thunk(RE::PlayerControls* a_this, const RE::InputEvent* a_queueHead)
			{
				auto& st = State();
				if (!a_queueHead || !st.minecraftOwnsPlayer || st.falloutMenuOpen || Game::FalloutMenuOpen()) {
					return func(a_this, a_queueHead);
				}
				static const RE::BSFixedString activate{ "Activate" };
				for (auto* e = a_queueHead; e; e = e->next) {
					const auto route = Forward(e);
					if (route == Route::kMinecraft) {
						continue;
					}
					// E (as Activate) or Tab: hand just this event to Fallout.
					auto*      button = const_cast<RE::ButtonEvent*>(static_cast<const RE::ButtonEvent*>(e));
					const auto savedName = button->strUserEvent;
					auto*      savedNext = button->next;
					if (route == Route::kActivate) {
						button->strUserEvent = activate;
					}
					button->next = nullptr;
					func(a_this, button);
					button->strUserEvent = savedName;
					button->next = savedNext;
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// MenuControls opens Fallout's menus. While Minecraft drives the player only Esc, Tab, ~ and
		// F9 reach it; while a Minecraft screen is open nothing does.
		struct MenuControlsHook
		{
			static void thunk(RE::MenuControls* a_this, const RE::InputEvent* a_queueHead)
			{
				auto& st = State();
				if (!a_queueHead || !st.minecraftOwnsPlayer || st.falloutMenuOpen || Game::FalloutMenuOpen()) {
					return func(a_this, a_queueHead);  // a Fallout menu is up: all its input
				}
				std::vector<RE::InputEvent*> keep;
				std::vector<RE::InputEvent*> all;
				for (auto* e = const_cast<RE::InputEvent*>(a_queueHead); e; e = e->next) {
					all.push_back(e);
					if (st.mcScreenOpen) {
						continue;
					}
					if (e->eventType.get() == RE::INPUT_EVENT_TYPE::kButton && e->device.get() == RE::INPUT_DEVICE::kKeyboard) {
						if (IsFalloutMenuKey(DikFromVk(static_cast<RE::ButtonEvent*>(e)->QIDCode()))) {
							keep.push_back(e);
						}
					}
				}
				if (keep.empty()) {
					return;
				}
				std::vector<RE::InputEvent*> savedNext;
				savedNext.reserve(all.size());
				for (auto* e : all) {
					savedNext.push_back(e->next);
				}
				for (std::size_t i = 0; i < keep.size(); ++i) {
					keep[i]->next = i + 1 < keep.size() ? keep[i + 1] : nullptr;
				}
				for (auto* e : keep) {
					const auto* b = static_cast<RE::ButtonEvent*>(e);
					if (b->QJustPressed() && DikFromVk(b->QIDCode()) == kDikTab) {
						Game::NotePipboyKey();
					}
					if (b->QJustPressed()) {
						REX::INFO("Fallout key: {:#x} passed to Fallout's menu controls", DikFromVk(b->QIDCode()));
					}
				}
				func(a_this, keep.front());
				for (std::size_t i = 0; i < all.size(); ++i) {
					all[i]->next = savedNext[i];
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	namespace Input
	{
		void Install()
		{
			// BSInputEventReceiver::PerformInputProcessing is vfunc 0 of both primary vtables.
			REL::Relocation<std::uintptr_t> playerVtbl{ RE::VTABLE::PlayerControls[0] };
			PlayerControlsHook::func = playerVtbl.write_vfunc(0x0, PlayerControlsHook::thunk);
			REL::Relocation<std::uintptr_t> menuVtbl{ RE::VTABLE::MenuControls[0] };
			MenuControlsHook::func = menuVtbl.write_vfunc(0x0, MenuControlsHook::thunk);
			REX::INFO("input hooks installed (PlayerControls, MenuControls)");
		}

		void ConsumeLook(float& a_dx, float& a_dy)
		{
			a_dx = lookDx;
			a_dy = lookDy;
			lookDx = lookDy = 0.0f;
		}

		void ReleaseAll()
		{
			sentAs.fill(0);
			link::PushInput(proto::kInReleaseAll, 0);
		}
	}
}
