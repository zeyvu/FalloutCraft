package dev.skycraft.neoforge;

import dev.skycraft.SkyCraft;
import dev.skycraft.combat.SkyCombat;
import dev.skycraft.platform.Platform;
import dev.skycraft.world.FalloutNight;
import net.minecraft.core.registries.Registries;
import net.minecraft.server.level.ServerPlayer;
import net.neoforged.api.distmarker.Dist;
import net.neoforged.bus.api.IEventBus;
import net.neoforged.fml.ModContainer;
import net.neoforged.fml.common.Mod;
import net.neoforged.fml.loading.FMLEnvironment;
import net.neoforged.neoforge.common.NeoForge;
import net.neoforged.neoforge.event.entity.EntityAttributeCreationEvent;
import net.neoforged.neoforge.event.entity.player.PlayerEvent;
import net.neoforged.neoforge.event.level.ChunkWatchEvent;
import net.neoforged.neoforge.event.server.ServerStartedEvent;
import net.neoforged.neoforge.event.tick.ServerTickEvent;
import net.neoforged.neoforge.network.event.RegisterPayloadHandlersEvent;
import net.neoforged.neoforge.registries.RegisterEvent;

/** NeoForge 1.21.1's entry point: wires the mod's common code (shared with every build) to NeoForge. */
@Mod(SkyCraft.MOD_ID)
public final class SkyCraftNeoForge {
	public SkyCraftNeoForge(IEventBus modBus, ModContainer container) {
		Platform.set(new NeoForgePlatform());

		// The invisible stand-in for Skyrim's actors (SkyCombat).
		modBus.addListener(RegisterEvent.class, event ->
			event.register(Registries.ENTITY_TYPE, SkyCombat.SKYRIM_ACTOR_ID, () -> {
				SkyCombat.SKYRIM_ACTOR = SkyCombat.createActorType();
				return SkyCombat.SKYRIM_ACTOR;
			}));
		modBus.addListener(EntityAttributeCreationEvent.class, event -> event.put(SkyCombat.SKYRIM_ACTOR, SkyCombat.actorAttributes().build()));

		// Dug cells saved with chunks; packets (SkyNet + the dug cells' sync).
		NeoDugStore.ATTACHMENTS.register(modBus);
		modBus.addListener(RegisterPayloadHandlersEvent.class, NeoNet::register);

		SkyCraft.init();
		NeoForge.EVENT_BUS.addListener(ServerTickEvent.Post.class, event -> {
			SkyCombat.serverTick(event.getServer());
			FalloutNight.tick(event.getServer()); // Minecraft mobs at night in the Wasteland
		});
		NeoForge.EVENT_BUS.addListener(ServerStartedEvent.class, event -> SkyCraft.configureServer(event.getServer()));
		NeoForge.EVENT_BUS.addListener(PlayerEvent.PlayerLoggedInEvent.class, event -> {
			if (event.getEntity() instanceof ServerPlayer player) {
				SkyCraft.onPlayerJoin(player);
			}
		});
		NeoForge.EVENT_BUS.addListener(ChunkWatchEvent.Sent.class, NeoDugStore::onSent);

		if (FMLEnvironment.dist == Dist.CLIENT) {
			SkyCraftNeoForgeClient.init(modBus);
		}
	}
}
