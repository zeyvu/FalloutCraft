package dev.skycraft.client.mixin;

import net.minecraft.client.MouseHandler;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Invoker;

/** Minecraft 1.21.1: MouseHandler's GLFW event handlers are private; InputBridge calls them directly. */
@Mixin(MouseHandler.class)
public interface MouseHandlerInvoker {
	@Invoker("onPress")
	void skycraft$onPress(long window, int button, int action, int modifiers);

	@Invoker("onScroll")
	void skycraft$onScroll(long window, double xOffset, double yOffset);

	@Invoker("onMove")
	void skycraft$onMove(long window, double x, double y);
}
