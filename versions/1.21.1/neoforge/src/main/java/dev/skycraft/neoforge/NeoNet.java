package dev.skycraft.neoforge;

import dev.skycraft.SkyCraft;
import dev.skycraft.client.SkyCraftClient;
import dev.skycraft.net.SkyNet;
import dev.skycraft.world.SkyDig;
import net.minecraft.network.RegistryFriendlyByteBuf;
import net.minecraft.network.codec.StreamCodec;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.level.chunk.LevelChunk;
import net.neoforged.neoforge.network.PacketDistributor;
import net.neoforged.neoforge.network.event.RegisterPayloadHandlersEvent;
import net.neoforged.neoforge.network.registration.PayloadRegistrar;

/**
 * FalloutCraft's packets on NeoForge 1.21.1: SkyNet's (guests' hits, digging, "you died") and
 * DugSync (a chunk's dug cells, server to client; see NeoDugStore).
 */
public final class NeoNet {
	private NeoNet() {
	}

	/** Server to client: a chunk's dug cells (all of them, replacing what the client had). */
	public record DugSync(long chunk, SkyDig.DugColumn column) implements CustomPacketPayload {
		public static final Type<DugSync> TYPE = new Type<>(ResourceLocation.fromNamespaceAndPath(SkyCraft.MOD_ID, "dug_sync"));
		public static final StreamCodec<RegistryFriendlyByteBuf, DugSync> CODEC = StreamCodec.of(
			(buf, sync) -> {
				buf.writeLong(sync.chunk());
				SkyDig.DugColumn.STREAM_CODEC.encode(buf, sync.column());
			},
			buf -> new DugSync(buf.readLong(), SkyDig.DugColumn.STREAM_CODEC.decode(buf))
		);

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	/** Mod bus: RegisterPayloadHandlersEvent. Handlers run on the main thread (NeoForge's default). */
	static void register(RegisterPayloadHandlersEvent event) {
		PayloadRegistrar registrar = event.registrar("1").optional();
		registrar.playToServer(SkyNet.Hurt.TYPE, SkyNet.Hurt.CODEC, (payload, context) -> {
			if (context.player() instanceof ServerPlayer player) {
				SkyNet.onHurt(player, payload);
			}
		});
		registrar.playToServer(SkyNet.DigOpen.TYPE, SkyNet.DigOpen.CODEC, (payload, context) -> {
			if (context.player() instanceof ServerPlayer player) {
				SkyNet.onDigOpen(player, payload);
			}
		});
		registrar.playToServer(SkyNet.DigReveal.TYPE, SkyNet.DigReveal.CODEC, (payload, context) -> {
			if (context.player() instanceof ServerPlayer player) {
				SkyNet.onDigReveal(player, payload);
			}
		});
		registrar.playToClient(SkyNet.Died.TYPE, SkyNet.Died.CODEC, (payload, context) -> SkyCraftClient.onDied(payload));
		registrar.playToClient(DugSync.TYPE, DugSync.CODEC, (payload, context) -> NeoDugStore.Client.receive(payload.chunk(), payload.column()));
	}

	/** Server to one player, if their client has FalloutCraft. */
	static boolean sendToPlayer(ServerPlayer player, CustomPacketPayload payload) {
		if (!player.connection.hasChannel(payload.type().id())) {
			return false;
		}
		PacketDistributor.sendToPlayer(player, payload);
		return true;
	}

	/** A chunk's dug cells, to every player who has that chunk. */
	static void sendDug(ServerLevel level, LevelChunk chunk, SkyDig.DugColumn column) {
		PacketDistributor.sendToPlayersTrackingChunk(level, chunk.getPos(), new DugSync(chunk.getPos().toLong(), column));
	}

	/** A chunk's dug cells, to one player (they've just been sent the chunk). */
	static void sendDug(ServerPlayer player, long chunk, SkyDig.DugColumn column) {
		PacketDistributor.sendToPlayer(player, new DugSync(chunk, column));
	}
}
