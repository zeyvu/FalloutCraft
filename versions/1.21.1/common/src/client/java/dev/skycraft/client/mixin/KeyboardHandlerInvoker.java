package dev.skycraft.client.mixin;

import net.minecraft.client.KeyboardHandler;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Invoker;

/** Minecraft 1.21.1: KeyboardHandler.charTyped is private; InputBridge calls it directly. */
@Mixin(KeyboardHandler.class)
public interface KeyboardHandlerInvoker {
	@Invoker("charTyped")
	void skycraft$charTyped(long window, int codePoint, int modifiers);
}
