package dev.skycraft.fabric;

import dev.skycraft.platform.Platform;
import dev.skycraft.world.SkyDig;
import java.nio.file.Path;
import net.fabricmc.fabric.api.attachment.v1.AttachmentRegistry;
import net.fabricmc.fabric.api.attachment.v1.AttachmentSyncPredicate;
import net.fabricmc.fabric.api.attachment.v1.AttachmentType;
import net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.level.chunk.LevelChunk;
import org.jspecify.annotations.Nullable;

/** Fabric's side of {@link Platform}: Fabric API networking and data attachments. */
public final class FabricPlatform extends Platform {
	/** A chunk's dug cells: saved with the chunk and synced to every client that has it. */
	public static final AttachmentType<SkyDig.DugColumn> DUG = AttachmentRegistry.<SkyDig.DugColumn>builder()
		.persistent(SkyDig.DugColumn.CODEC)
		.syncWith(SkyDig.DugColumn.STREAM_CODEC, AttachmentSyncPredicate.all())
		.buildAndRegister(SkyDig.DUG_ID);

	@Override
	public String name() {
		return "Fabric";
	}

	@Override
	public Path configDir() {
		return FabricLoader.getInstance().getConfigDir();
	}

	@Override
	public boolean sendToPlayer(ServerPlayer player, CustomPacketPayload payload) {
		if (!ServerPlayNetworking.canSend(player, payload.type())) {
			return false;
		}
		ServerPlayNetworking.send(player, payload);
		return true;
	}

	@Override
	public SkyDig.@Nullable DugColumn getDug(LevelChunk chunk) {
		return chunk.getAttached(DUG);
	}

	@Override
	public void setDug(LevelChunk chunk, SkyDig.DugColumn column) {
		chunk.setAttached(DUG, column);
	}
}
