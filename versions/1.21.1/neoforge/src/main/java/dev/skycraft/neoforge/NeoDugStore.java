package dev.skycraft.neoforge;

import dev.skycraft.SkyCraft;
import dev.skycraft.world.SkyDig;
import java.util.Collections;
import java.util.Map;
import java.util.WeakHashMap;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.Supplier;
import net.minecraft.client.Minecraft;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.chunk.LevelChunk;
import net.neoforged.neoforge.attachment.AttachmentType;
import net.neoforged.neoforge.event.level.ChunkWatchEvent;
import net.neoforged.neoforge.registries.DeferredRegister;
import net.neoforged.neoforge.registries.NeoForgeRegistries;
import org.jspecify.annotations.Nullable;

/**
 * Which cells were dug out of Skyrim's geometry (SkyDig), per chunk, on NeoForge 1.21.1: saved
 * with the chunk as a NeoForge data attachment (skycraft:dug), and sent to the players who have the
 * chunk with NeoNet.DugSync (when it changes, and when a player is sent the chunk); clients keep
 * what they were sent.
 */
public final class NeoDugStore {
	private NeoDugStore() {
	}

	static final DeferredRegister<AttachmentType<?>> ATTACHMENTS = DeferredRegister.create(NeoForgeRegistries.Keys.ATTACHMENT_TYPES, SkyCraft.MOD_ID);
	static final Supplier<AttachmentType<SkyDig.DugColumn>> DUG = ATTACHMENTS.register("dug",
		() -> AttachmentType.builder(() -> SkyDig.DugColumn.EMPTY)
			.serialize(SkyDig.DugColumn.CODEC, column -> !column.sections().isEmpty())
			.build());

	/** Platform.getDug: any thread, either side. */
	static SkyDig.@Nullable DugColumn get(LevelChunk chunk) {
		Level level = chunk.getLevel();
		if (level instanceof ServerLevel) {
			return chunk.getExistingDataOrNull(DUG.get());
		}
		return Client.get(level, chunk.getPos().toLong());
	}

	/** Platform.setDug: server only. Saved with the chunk, and sent to the players who have it. */
	static void set(LevelChunk chunk, SkyDig.DugColumn column) {
		if (!(chunk.getLevel() instanceof ServerLevel server)) {
			return;
		}
		chunk.setData(DUG.get(), column);
		NeoNet.sendDug(server, chunk, column);
	}

	/** A player was just sent a chunk: its dug cells follow. */
	static void onSent(ChunkWatchEvent.Sent event) {
		SkyDig.DugColumn column = event.getChunk().getExistingDataOrNull(DUG.get());
		if (column != null && !column.sections().isEmpty()) {
			NeoNet.sendDug(event.getPlayer(), event.getPos().toLong(), column);
		}
	}

	/** The client's copy of the dug cells of the chunks it has (per client level). */
	public static final class Client {
		private static final Map<Level, Map<Long, SkyDig.DugColumn>> LEVELS = Collections.synchronizedMap(new WeakHashMap<>());

		private Client() {
		}

		static SkyDig.@Nullable DugColumn get(Level level, long chunk) {
			Map<Long, SkyDig.DugColumn> chunks = LEVELS.get(level);
			return chunks == null ? null : chunks.get(chunk);
		}

		/** DugSync arrived (main thread). */
		static void receive(long chunk, SkyDig.DugColumn column) {
			Level level = Minecraft.getInstance().level;
			if (level != null) {
				LEVELS.computeIfAbsent(level, l -> new ConcurrentHashMap<>()).put(chunk, column);
			}
		}

		/** Left the world. */
		public static void clear() {
			LEVELS.clear();
		}
	}
}
