package dev.skycraft.client.platform;

import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import org.jspecify.annotations.Nullable;

/** The client half of {@link dev.skycraft.platform.Platform}: set by the loader's client entry point. */
public abstract class ClientPlatform {
	private static @Nullable ClientPlatform instance;

	public static ClientPlatform get() {
		ClientPlatform platform = instance;
		if (platform == null) {
			throw new IllegalStateException("SkyCraft: no client platform set");
		}
		return platform;
	}

	public static void set(ClientPlatform platform) {
		instance = platform;
	}

	/** Client to the server it's connected to. False (nothing sent) if that server can't take it. */
	public abstract boolean sendToServer(CustomPacketPayload payload);
}
