package dev.skycraft.platform;

import dev.skycraft.world.SkyDig;
import java.nio.file.Path;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.level.chunk.LevelChunk;
import org.jspecify.annotations.Nullable;

/**
 * What the mod needs from its mod loader, so the same code runs on Fabric and Forge.
 *
 * <p>Everything else (events, registration) is wired by each loader's entry point: Fabric's in
 * {@code dev.skycraft.fabric}, Forge's in the {@code forge/} project. They call the loader-neutral
 * methods of {@code SkyCraft}, {@code SkyCombat}, {@code SkyNet}, {@code FalloutNight}, ...
 */
public abstract class Platform {
	private static @Nullable Platform instance;

	/** The loader's platform; set by its entry point before anything else runs. */
	public static Platform get() {
		Platform platform = instance;
		if (platform == null) {
			throw new IllegalStateException("SkyCraft: no mod loader platform set");
		}
		return platform;
	}

	public static void set(Platform platform) {
		instance = platform;
	}

	/** "Fabric" or "Forge" (for the log). */
	public abstract String name();

	/** The game's config folder (config/). */
	public abstract Path configDir();

	/**
	 * Server to one player. False if their client can't take it (no FalloutCraft there), in which
	 * case nothing is sent.
	 */
	public abstract boolean sendToPlayer(ServerPlayer player, CustomPacketPayload payload);

	/**
	 * A chunk's dug cells (SkyDig), or null for none. Saved with the world and kept in sync on every
	 * client that has the chunk. Any thread.
	 */
	public abstract SkyDig.@Nullable DugColumn getDug(LevelChunk chunk);

	/** Server only: the chunk's dug cells changed (saved, and sent to the players who have it). */
	public abstract void setDug(LevelChunk chunk, SkyDig.DugColumn column);
}
