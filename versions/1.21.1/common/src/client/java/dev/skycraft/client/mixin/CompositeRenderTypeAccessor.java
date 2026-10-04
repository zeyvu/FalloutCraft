package dev.skycraft.client.mixin;

import net.minecraft.client.renderer.RenderType;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

/** Minecraft 1.21.1: the state (texture, shader, ...) of a render type. */
@Mixin(targets = "net.minecraft.client.renderer.RenderType$CompositeRenderType")
public interface CompositeRenderTypeAccessor {
	@Accessor("state")
	RenderType.CompositeState skycraft$state();
}
