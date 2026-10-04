package dev.skycraft.fabric;

import dev.skycraft.SkyCraft;
import dev.skycraft.combat.SkyCombat;
import dev.skycraft.net.SkyNet;
import dev.skycraft.platform.Platform;
import dev.skycraft.world.FalloutNight;
import net.fabricmc.api.ModInitializer;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.fabricmc.fabric.api.networking.v1.PayloadTypeRegistry;
import net.fabricmc.fabric.api.networking.v1.ServerPlayConnectionEvents;
import net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking;
import net.fabricmc.fabric.api.object.builder.v1.entity.FabricDefaultAttributeRegistry;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.level.ServerPlayer;

/** Fabric's entry point: wires the mod's common code to Fabric API. */
public final class SkyCraftFabric implements ModInitializer {
	@Override
	public void onInitialize() {
		Platform.set(new FabricPlatform());

		// The invisible stand-in for Skyrim's actors (SkyCombat).
		SkyCombat.SKYRIM_ACTOR = Registry.register(BuiltInRegistries.ENTITY_TYPE, SkyCombat.SKYRIM_ACTOR_KEY, SkyCombat.createActorType());
		FabricDefaultAttributeRegistry.register(SkyCombat.SKYRIM_ACTOR, SkyCombat.actorAttributes());

		// Packets (SkyNet): guests' hits, digging; and "you died" back to a guest.
		PayloadTypeRegistry.serverboundPlay().register(SkyNet.Hurt.TYPE, SkyNet.Hurt.CODEC);
		PayloadTypeRegistry.serverboundPlay().register(SkyNet.DigOpen.TYPE, SkyNet.DigOpen.CODEC);
		PayloadTypeRegistry.serverboundPlay().register(SkyNet.DigReveal.TYPE, SkyNet.DigReveal.CODEC);
		PayloadTypeRegistry.clientboundPlay().register(SkyNet.Died.TYPE, SkyNet.Died.CODEC);
		ServerPlayNetworking.registerGlobalReceiver(SkyNet.DigOpen.TYPE, (payload, context) -> {
			ServerPlayer player = context.player();
			context.server().execute(() -> SkyNet.onDigOpen(player, payload));
		});
		ServerPlayNetworking.registerGlobalReceiver(SkyNet.DigReveal.TYPE, (payload, context) -> {
			ServerPlayer player = context.player();
			context.server().execute(() -> SkyNet.onDigReveal(player, payload));
		});
		ServerPlayNetworking.registerGlobalReceiver(SkyNet.Hurt.TYPE, (payload, context) -> {
			ServerPlayer player = context.player();
			context.server().execute(() -> SkyNet.onHurt(player, payload));
		});

		SkyCraft.init();
		ServerTickEvents.END_SERVER_TICK.register(SkyCombat::serverTick);
		ServerTickEvents.END_SERVER_TICK.register(FalloutNight::tick); // Minecraft mobs at night in the Wasteland
		ServerLifecycleEvents.SERVER_STARTED.register(SkyCraft::configureServer);
		ServerPlayConnectionEvents.JOIN.register((handler, sender, server) -> SkyCraft.onPlayerJoin(handler.getPlayer()));
	}
}
