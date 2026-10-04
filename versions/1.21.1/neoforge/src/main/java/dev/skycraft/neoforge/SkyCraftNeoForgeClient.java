package dev.skycraft.neoforge;

import com.mojang.brigadier.arguments.StringArgumentType;
import dev.skycraft.client.DestructionToggle;
import dev.skycraft.client.SkyClient;
import dev.skycraft.client.SkyCraftClient;
import dev.skycraft.client.platform.ClientPlatform;
import dev.skycraft.combat.SkyCombat;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.entity.NoopRenderer;
import net.minecraft.commands.Commands;
import net.minecraft.network.chat.Component;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.neoforged.bus.api.IEventBus;
import net.neoforged.neoforge.client.event.ClientPlayerNetworkEvent;
import net.neoforged.neoforge.client.event.ClientTickEvent;
import net.neoforged.neoforge.client.event.EntityRenderersEvent;
import net.neoforged.neoforge.client.event.RegisterClientCommandsEvent;
import net.neoforged.neoforge.client.event.ScreenEvent;
import net.neoforged.neoforge.common.NeoForge;
import net.neoforged.neoforge.network.PacketDistributor;

/** NeoForge 1.21.1's client side: wires the client's common code to NeoForge's client events. */
final class SkyCraftNeoForgeClient {
	private SkyCraftNeoForgeClient() {
	}

	static void init(IEventBus modBus) {
		ClientPlatform.set(new ClientPlatform() {
			@Override
			public boolean sendToServer(CustomPacketPayload payload) {
				if (Minecraft.getInstance().getConnection() == null) {
					return false;
				}
				PacketDistributor.sendToServer(payload);
				return true;
			}
		});
		SkyCraftClient.init();

		NeoForge.EVENT_BUS.addListener(ClientTickEvent.Post.class, event -> SkyClient.clientTick(Minecraft.getInstance()));

		// The pause menu's "Skyrim destruction" button.
		NeoForge.EVENT_BUS.addListener(ScreenEvent.Init.Post.class, event -> {
			var button = DestructionToggle.buttonFor(Minecraft.getInstance(), event.getScreen());
			if (button != null) {
				event.addListener(button);
			}
		});

		// /join <link> and /leave (a friend's world over e4mc).
		NeoForge.EVENT_BUS.addListener(RegisterClientCommandsEvent.class, event -> {
			event.getDispatcher().register(Commands.literal("join")
				.then(Commands.argument("link", StringArgumentType.greedyString())
					.executes(c -> {
						String link = StringArgumentType.getString(c, "link");
						Minecraft.getInstance().gui.getChat().addMessage(Component.literal("Joining " + link.trim() + "..."));
						SkyCraftClient.join(link);
						return 1;
					})));
			event.getDispatcher().register(Commands.literal("leave").executes(c -> {
				SkyCraftClient.leave();
				return 1;
			}));
		});

		NeoForge.EVENT_BUS.addListener(ClientPlayerNetworkEvent.LoggingIn.class, event -> SkyCraftClient.onJoinedWorld(Minecraft.getInstance()));
		NeoForge.EVENT_BUS.addListener(ClientPlayerNetworkEvent.LoggingOut.class, event -> NeoDugStore.Client.clear());

		// Skyrim draws the real NPC; its Minecraft stand-in is only a hitbox.
		modBus.addListener(EntityRenderersEvent.RegisterRenderers.class, event -> event.registerEntityRenderer(SkyCombat.SKYRIM_ACTOR, NoopRenderer::new));
	}
}
