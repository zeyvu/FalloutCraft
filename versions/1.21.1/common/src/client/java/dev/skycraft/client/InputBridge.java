package dev.skycraft.client;

import dev.skycraft.combat.SkyCombat;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.screens.PauseScreen;
import net.minecraft.server.level.ServerPlayer;

import dev.skycraft.SkyCraft;
import dev.skycraft.client.mixin.KeyboardHandlerInvoker;
import dev.skycraft.client.mixin.MouseHandlerInvoker;
import org.lwjgl.glfw.GLFW;






/**
 * Minecraft 1.21.1 (GLFW): replays Skyrim-captured input into Minecraft's own input handlers, as if
 * the (hidden) MC window had focus, calling them directly on the render thread like the 26.x
 * version (KeyboardHandler.keyPress, and MouseHandler.onPress/onScroll/onMove and
 * KeyboardHandler.charTyped through invoker mixins). Skyrim sends SDL (USB HID) scancodes and SDL
 * mouse buttons, turned into GLFW key codes / buttons here. Keeps a virtual keyboard by GLFW key
 * code so InputConstants.isKeyDown(window, key) still works.
 */
public final class InputBridge {
	private static final boolean[] KEYS = new boolean[512];
	private static final boolean[] BUTTONS = new boolean[8];
	private static double cursorX, cursorY;
	private static int modifiers;
	private static int clickLogs;

	private static final boolean[] GLFW_KEYS = new boolean[GLFW.GLFW_KEY_LAST + 1];
	private static int keyLogs;
	private static long lastStateLog;

	private InputBridge() {
	}


	/** 1.21.1: {@code key} is a GLFW key code (InputConstants.isKeyDown(long window, int key)). */
	public static boolean isKeyDown(int key) {
		return key >= 0 && key < GLFW_KEYS.length && GLFW_KEYS[key];
	}
	
	private static void keyEvent(Minecraft minecraft, long handle, int sdl, int key, int action) {
		if (key == GLFW.GLFW_KEY_UNKNOWN) {
			if (keyLogs++ < 40) {
				SkyCraft.LOG.info("SkyCraft: key {} (SDL) has no GLFW key; ignored", sdl);
			}
			return;
		}
		minecraft.keyboardHandler.keyPress(handle, key, GLFW.glfwGetKeyScancode(key), action, glfwModifiers());
		if (keyLogs++ < 40) {
			var o = minecraft.options;
			SkyCraft.LOG.info("SkyCraft: key SDL {} -> GLFW {} action {} (screen {}; forward {} jump {} sprint {} sneak {})", sdl, key, action,
				minecraft.screen == null ? "none" : minecraft.screen.getClass().getSimpleName(),
				o.keyUp.isDown(), o.keyJump.isDown(), o.keySprint.isDown(), o.keyShift.isDown());
		}
	}

	private static void buttonEvent(Minecraft minecraft, long handle, int sdlButton, boolean down) {
		// SDL: 1 left, 2 middle, 3 right, 4 X1, 5 X2. GLFW: 0 left, 1 right, 2 middle, 3, 4.
		int button = switch (sdlButton) {
			case 1 -> GLFW.GLFW_MOUSE_BUTTON_LEFT;
			case 2 -> GLFW.GLFW_MOUSE_BUTTON_MIDDLE;
			case 3 -> GLFW.GLFW_MOUSE_BUTTON_RIGHT;
			default -> sdlButton - 1;
		};
		if (button >= 0 && button <= GLFW.GLFW_MOUSE_BUTTON_LAST) {
			((MouseHandlerInvoker) minecraft.mouseHandler).skycraft$onPress(handle, button, down ? GLFW.GLFW_PRESS : GLFW.GLFW_RELEASE, glfwModifiers());
		}
	}

	/** Every few seconds while keys are held: what Minecraft makes of them (for the log). */
	private static void logState(Minecraft minecraft) {
		long now = System.currentTimeMillis();
		if (now - lastStateLog < 5000 || minecraft.player == null) {
			return;
		}
		boolean any = false;
		for (boolean k : GLFW_KEYS) {
			any |= k;
		}
		if (!any) {
			return;
		}
		lastStateLog = now;
		var input = minecraft.player.input;
		SkyCraft.LOG.info("SkyCraft: input state: forward {} left {} jumping {} sneak {}; grabbed {}, screen {}, window active {}",
			input.forwardImpulse, input.leftImpulse, input.jumping, input.shiftKeyDown, minecraft.mouseHandler.isMouseGrabbed(),
			minecraft.screen == null ? "none" : minecraft.screen.getClass().getSimpleName(), minecraft.isWindowActive());
	}

	private static int glfwModifiers() {
		int m = 0;
		if (KEYS[225] || KEYS[229]) m |= GLFW.GLFW_MOD_SHIFT;
		if (KEYS[224] || KEYS[228]) m |= GLFW.GLFW_MOD_CONTROL;
		if (KEYS[226] || KEYS[230]) m |= GLFW.GLFW_MOD_ALT;
		if (KEYS[227] || KEYS[231]) m |= GLFW.GLFW_MOD_SUPER;
		return m;
	}
	
	/** SDL scancode (USB HID usage) -> GLFW key code, or GLFW_KEY_UNKNOWN. */
	private static int glfwKey(int sc) {
		if (sc >= 4 && sc <= 29) return GLFW.GLFW_KEY_A + (sc - 4);
		if (sc >= 30 && sc <= 38) return GLFW.GLFW_KEY_1 + (sc - 30);
		if (sc >= 58 && sc <= 69) return GLFW.GLFW_KEY_F1 + (sc - 58);
		if (sc >= 104 && sc <= 115) return GLFW.GLFW_KEY_F13 + (sc - 104);
		if (sc >= 89 && sc <= 97) return GLFW.GLFW_KEY_KP_1 + (sc - 89);
		return switch (sc) {
			case 39 -> GLFW.GLFW_KEY_0;
			case 40 -> GLFW.GLFW_KEY_ENTER;
			case 41 -> GLFW.GLFW_KEY_ESCAPE;
			case 42 -> GLFW.GLFW_KEY_BACKSPACE;
			case 43 -> GLFW.GLFW_KEY_TAB;
			case 44 -> GLFW.GLFW_KEY_SPACE;
			case 45 -> GLFW.GLFW_KEY_MINUS;
			case 46 -> GLFW.GLFW_KEY_EQUAL;
			case 47 -> GLFW.GLFW_KEY_LEFT_BRACKET;
			case 48 -> GLFW.GLFW_KEY_RIGHT_BRACKET;
			case 49, 50 -> GLFW.GLFW_KEY_BACKSLASH;
			case 51 -> GLFW.GLFW_KEY_SEMICOLON;
			case 52 -> GLFW.GLFW_KEY_APOSTROPHE;
			case 53 -> GLFW.GLFW_KEY_GRAVE_ACCENT;
			case 54 -> GLFW.GLFW_KEY_COMMA;
			case 55 -> GLFW.GLFW_KEY_PERIOD;
			case 56 -> GLFW.GLFW_KEY_SLASH;
			case 57 -> GLFW.GLFW_KEY_CAPS_LOCK;
			case 70 -> GLFW.GLFW_KEY_PRINT_SCREEN;
			case 71 -> GLFW.GLFW_KEY_SCROLL_LOCK;
			case 72 -> GLFW.GLFW_KEY_PAUSE;
			case 73 -> GLFW.GLFW_KEY_INSERT;
			case 74 -> GLFW.GLFW_KEY_HOME;
			case 75 -> GLFW.GLFW_KEY_PAGE_UP;
			case 76 -> GLFW.GLFW_KEY_DELETE;
			case 77 -> GLFW.GLFW_KEY_END;
			case 78 -> GLFW.GLFW_KEY_PAGE_DOWN;
			case 79 -> GLFW.GLFW_KEY_RIGHT;
			case 80 -> GLFW.GLFW_KEY_LEFT;
			case 81 -> GLFW.GLFW_KEY_DOWN;
			case 82 -> GLFW.GLFW_KEY_UP;
			case 83 -> GLFW.GLFW_KEY_NUM_LOCK;
			case 84 -> GLFW.GLFW_KEY_KP_DIVIDE;
			case 85 -> GLFW.GLFW_KEY_KP_MULTIPLY;
			case 86 -> GLFW.GLFW_KEY_KP_SUBTRACT;
			case 87 -> GLFW.GLFW_KEY_KP_ADD;
			case 88 -> GLFW.GLFW_KEY_KP_ENTER;
			case 98 -> GLFW.GLFW_KEY_KP_0;
			case 99 -> GLFW.GLFW_KEY_KP_DECIMAL;
			case 100 -> GLFW.GLFW_KEY_WORLD_2;
			case 101 -> GLFW.GLFW_KEY_MENU;
			case 103 -> GLFW.GLFW_KEY_KP_EQUAL;
			case 224 -> GLFW.GLFW_KEY_LEFT_CONTROL;
			case 225 -> GLFW.GLFW_KEY_LEFT_SHIFT;
			case 226 -> GLFW.GLFW_KEY_LEFT_ALT;
			case 227 -> GLFW.GLFW_KEY_LEFT_SUPER;
			case 228 -> GLFW.GLFW_KEY_RIGHT_CONTROL;
			case 229 -> GLFW.GLFW_KEY_RIGHT_SHIFT;
			case 230 -> GLFW.GLFW_KEY_RIGHT_ALT;
			case 231 -> GLFW.GLFW_KEY_RIGHT_SUPER;
			default -> GLFW.GLFW_KEY_UNKNOWN;
		};
	}






	public static void drain(Minecraft minecraft) {
		SkyLink.drainInput((type, code, a, b, c) -> dispatch(minecraft, type, code, a, b, c));
		logState(minecraft);
	}

	private static void dispatch(Minecraft minecraft, int type, int code, int a, int b, int c) {

		long handle = minecraft.getWindow().getWindow();



		switch (type) {
			case Proto.IN_KEY -> key(minecraft, handle, code, a != 0);
			case Proto.IN_MOUSE_BUTTON -> {
				if (code > 0 && code < BUTTONS.length) {
					BUTTONS[code] = a != 0;
				}
				if (a != 0 && clickLogs++ < 20) {
					var hit = minecraft.hitResult;
					dev.skycraft.SkyCraft.LOG.info("SkyCraft: click {} -> {} {} (grabbed {}, screen {})", code, hit == null ? "null" : hit.getType(),
						hit instanceof net.minecraft.world.phys.EntityHitResult eh ? eh.getEntity().getName().getString() : hit == null ? "" : hit.getLocation(),

						minecraft.mouseHandler.isMouseGrabbed(), minecraft.screen);



				}

				buttonEvent(minecraft, handle, code, a != 0);



			}

			case Proto.IN_SCROLL -> ((MouseHandlerInvoker) minecraft.mouseHandler).skycraft$onScroll(handle, 0.0, a / 120.0);



			case Proto.IN_CURSOR -> {
				double dx = a - cursorX;
				double dy = b - cursorY;
				cursorX = a;
				cursorY = b;

				// 1.21.1's MouseHandler.onMove works the deltas out itself from the last position.
				((MouseHandlerInvoker) minecraft.mouseHandler).skycraft$onMove(handle, a, b);



			}
			case Proto.IN_TEXT -> {

				if (minecraft.screen != null) {
					((KeyboardHandlerInvoker) minecraft.keyboardHandler).skycraft$charTyped(handle, a, glfwModifiers());
				}





			}
			case Proto.IN_RELEASE_ALL -> releaseAll();
			case Proto.IN_HURT -> hurt(minecraft, code, a / 100.0F, b, c);
			case Proto.IN_SCAVENGE -> FalloutScavenge.found(minecraft, code, a, b);
			case Proto.IN_OPEN_MENU -> {

				if (minecraft.screen == null && minecraft.player != null) {
					releaseAll();
					minecraft.setScreen(new PauseScreen(true));
				}






			}
			default -> {
			}
		}
	}

	/** Skyrim hit the player: apply it as Minecraft damage on the integrated server (or the host's). */
	private static void hurt(Minecraft minecraft, int kind, float skyrimDamage, int attacker, int flags) {
		var server = minecraft.getSingleplayerServer();
		if (minecraft.player == null) {
			return;
		}
		if (server == null) {
			// A guest in a friend's world: the host's server applies it.
			dev.skycraft.client.platform.ClientPlatform.get().sendToServer(new dev.skycraft.net.SkyNet.Hurt(kind, skyrimDamage, attacker, flags));
			return;
		}
		var uuid = minecraft.player.getUUID();
		server.execute(() -> {
			ServerPlayer player = server.getPlayerList().getPlayer(uuid);
			if (player != null) {
				SkyCombat.hurtPlayer(player, kind, skyrimDamage, attacker, flags);
			}
		});
	}

	private static void key(Minecraft minecraft, long handle, int scancode, boolean down) {
		if (scancode <= 0 || scancode >= KEYS.length) {
			return;
		}
		boolean wasDown = KEYS[scancode];
		KEYS[scancode] = down;
		updateModifiers();

		int key = glfwKey(scancode);
		if (key >= 0 && key < GLFW_KEYS.length) {
			GLFW_KEYS[key] = down;
		}
		keyEvent(minecraft, handle, scancode, key, down ? (wasDown ? GLFW.GLFW_REPEAT : GLFW.GLFW_PRESS) : GLFW.GLFW_RELEASE);





	}

	private static void updateModifiers() {
		int m = 0;
		if (KEYS[225]) m |= 0x0001; // SDL_KMOD_LSHIFT
		if (KEYS[229]) m |= 0x0002; // SDL_KMOD_RSHIFT
		if (KEYS[224]) m |= 0x0040; // SDL_KMOD_LCTRL
		if (KEYS[228]) m |= 0x0080; // SDL_KMOD_RCTRL
		if (KEYS[226]) m |= 0x0100; // SDL_KMOD_LALT
		if (KEYS[230]) m |= 0x0200; // SDL_KMOD_RALT
		modifiers = m;
	}

	/** Lift every key and button we think is held (focus moved to Skyrim, link dropped, ...). */
	public static void releaseAll() {
		Minecraft minecraft = Minecraft.getInstance();

		long handle = minecraft.getWindow().getWindow();
		for (int sc = 0; sc < KEYS.length; sc++) {
			if (KEYS[sc]) {
				KEYS[sc] = false;
				updateModifiers();
				int key = glfwKey(sc);
				if (key >= 0 && key < GLFW_KEYS.length) {
					GLFW_KEYS[key] = false;
				}
				keyEvent(minecraft, handle, sc, key, GLFW.GLFW_RELEASE);
			}
		}
		for (int button = 1; button < BUTTONS.length; button++) {
			if (BUTTONS[button]) {
				BUTTONS[button] = false;
				buttonEvent(minecraft, handle, button, false);
			}
		}
















	}
}
