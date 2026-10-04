package dev.skycraft.client;

import dev.skycraft.net.SkyNet;
import net.minecraft.client.Minecraft;

/**
 * The client's common start-up. Each mod loader's client entry point
 * (dev.skycraft.client.fabric.SkyCraftClientFabric, Forge's dev.skycraft.forge.SkyCraftForgeClient)
 * sets the ClientPlatform, registers the events (client tick, pause menu, /join and /leave,
 * joining a world, the "died" packet, the actor's renderer) and calls these.
 */
public final class SkyCraftClient {
	private SkyCraftClient() {
	}

	/** Once, at client start (after the loader's client platform is set). */
	public static void init() {
		dev.skycraft.link.SkyLink.announceRunning();
		DiscordPresence.start();
		DestructionToggle.load();
		// Players (client-side movement AND the integrated server's re-check of it) use the smooth
		// triangle collider, never Skyrim's voxels; otherwise the server sees the smooth position
		// dip into a voxel and teleports the player back every few ticks.
		dev.skycraft.world.SkyCollision.setSmoothCollider(e -> e instanceof net.minecraft.world.entity.player.Player && SkyClient.linked());
	}

	// Multiplayer without editing files: the host opens their world to LAN (O, Open to LAN) and
	// e4mc gives them a link; friends type /join <link> in chat, and /leave to come back.

	/** /join link: after the chat screen has closed (this leaves the current world). */
	public static void join(String link) {
		Minecraft minecraft = Minecraft.getInstance();
		minecraft.execute(() -> MirrorWorld.joinFriend(minecraft, link));
	}

	/** /leave: back to this player's own world. */
	public static void leave() {
		Minecraft minecraft = Minecraft.getInstance();
		minecraft.execute(() -> MirrorWorld.leaveFriend(minecraft));
	}

	/**
	 * Joined a world. Multiplayer testing on one PC: SKYCRAFT_LAN_PORT opens the world to LAN on
	 * that port as soon as it's loaded, and SKYCRAFT_LAN_OFFLINE lets offline (dev) clients join it.
	 */
	public static void onJoinedWorld(Minecraft minecraft) {
		String port = System.getenv("SKYCRAFT_LAN_PORT");
		var server = minecraft.getSingleplayerServer();
		if (port == null || port.isBlank() || server == null || server.isPublished()) {
			return;
		}
		minecraft.execute(() -> {
			if (System.getenv("SKYCRAFT_LAN_OFFLINE") != null) {
				server.setUsesAuthentication(false);
			}
			//#if MC_1_21_1
			//$$ boolean ok = server.publishServer(server.getDefaultGameType(), false, Integer.parseInt(port.trim()));
			//#else
			boolean ok = server.publishServer(net.minecraft.server.MinecraftServer.MultiplayerScope.LAN, false, Integer.parseInt(port.trim()));
			//#endif
			dev.skycraft.SkyCraft.LOG.info("SkyCraft: world opened to LAN on port {} ({}{})", port.trim(), ok ? "ok" : "FAILED",
				System.getenv("SKYCRAFT_LAN_OFFLINE") != null ? ", offline logins allowed" : "");
		});
	}

	/** A guest in a friend's world: dying there kills this player's own Skyrim character. */
	public static void onDied(SkyNet.Died payload) {
		if (dev.skycraft.link.SkyLink.active()) {
			dev.skycraft.link.SkyLink.pushEvent(dev.skycraft.link.Proto.EV_PLAYER_DIED, payload.attackerFormId(), 0, 0, 0, 0, 0);
		}
	}
}
