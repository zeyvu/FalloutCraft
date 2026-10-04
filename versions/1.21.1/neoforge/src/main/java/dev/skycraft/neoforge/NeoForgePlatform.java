package dev.skycraft.neoforge;

import dev.skycraft.platform.Platform;
import dev.skycraft.world.SkyDig;
import java.nio.file.Path;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.level.chunk.LevelChunk;
import net.neoforged.fml.loading.FMLPaths;
import org.jspecify.annotations.Nullable;

/** NeoForge 1.21.1's side of {@link Platform}. */
final class NeoForgePlatform extends Platform {
	@Override
	public String name() {
		return "NeoForge";
	}

	@Override
	public Path configDir() {
		return FMLPaths.CONFIGDIR.get();
	}

	@Override
	public boolean sendToPlayer(ServerPlayer player, CustomPacketPayload payload) {
		return NeoNet.sendToPlayer(player, payload);
	}

	@Override
	public SkyDig.@Nullable DugColumn getDug(LevelChunk chunk) {
		return NeoDugStore.get(chunk);
	}

	@Override
	public void setDug(LevelChunk chunk, SkyDig.DugColumn column) {
		NeoDugStore.set(chunk, column);
	}
}
