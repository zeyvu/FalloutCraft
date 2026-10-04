package dev.skycraft.client.fabric;

import com.mojang.brigadier.arguments.StringArgumentType;
import dev.skycraft.client.DestructionToggle;
import dev.skycraft.client.SkyClient;
import dev.skycraft.client.SkyCraftClient;
import dev.skycraft.client.platform.ClientPlatform;
import dev.skycraft.combat.SkyCombat;
import dev.skycraft.net.SkyNet;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.command.v2.ClientCommandRegistrationCallback;
import net.fabricmc.fabric.api.client.command.v2.ClientCommands;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.fabricmc.fabric.api.client.networking.v1.ClientPlayConnectionEvents;
import net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking;
import net.fabricmc.fabric.api.client.rendering.v1.EntityRendererRegistry;
import net.fabricmc.fabric.api.client.screen.v1.ScreenEvents;
import net.fabricmc.fabric.api.client.screen.v1.Screens;
import net.minecraft.client.renderer.entity.NoopRenderer;
import net.minecraft.network.chat.Component;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;

/** Fabric's client entry point: wires the client's common code to Fabric API. */
public final class SkyCraftClientFabric implements ClientModInitializer {
	@Override
	public void onInitializeClient() {
		ClientPlatform.set(new ClientPlatform() {
			@Override
			public boolean sendToServer(CustomPacketPayload payload) {
				if (!ClientPlayNetworking.canSend(payload.type())) {
					return false;
				}
				ClientPlayNetworking.send(payload);
				return true;
			}
		});
		SkyCraftClient.init();

		// The pause menu's "Skyrim destruction" button.
		ScreenEvents.AFTER_INIT.register((minecraft, screen, width, height) -> {
			var button = DestructionToggle.buttonFor(minecraft, screen);
			if (button != null) {
				Screens.getWidgets(screen).add(button);
			}
		});
		// /join <link> and /leave (a friend's world over e4mc).
		ClientCommandRegistrationCallback.EVENT.register((dispatcher, context) -> {
			dispatcher.register(ClientCommands.literal("join")
				.then(ClientCommands.argument("link", StringArgumentType.greedyString())
					.executes(c -> {
						String link = StringArgumentType.getString(c, "link");
						c.getSource().sendFeedback(Component.literal("Joining " + link.trim() + "..."));
						SkyCraftClient.join(link);
						return 1;
					})));
			dispatcher.register(ClientCommands.literal("leave").executes(c -> {
				SkyCraftClient.leave();
				return 1;
			}));
		});
		ClientTickEvents.END_CLIENT_TICK.register(SkyClient::clientTick);
		ClientPlayConnectionEvents.JOIN.register((handler, sender, minecraft) -> SkyCraftClient.onJoinedWorld(minecraft));
		ClientPlayNetworking.registerGlobalReceiver(SkyNet.Died.TYPE, (payload, context) -> SkyCraftClient.onDied(payload));
		// Skyrim draws the real NPC; its Minecraft stand-in is only a hitbox.
		EntityRendererRegistry.register(SkyCombat.SKYRIM_ACTOR, NoopRenderer::new);
	}
}
