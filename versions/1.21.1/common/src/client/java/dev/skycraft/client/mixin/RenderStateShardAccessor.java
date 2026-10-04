package dev.skycraft.client.mixin;

import net.minecraft.client.renderer.RenderStateShard;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

/** Minecraft 1.21.1: a render type's name ("entity_cutout", "entity_shadow", ...). */
@Mixin(RenderStateShard.class)
public interface RenderStateShardAccessor {
	@Accessor("name")
	String skycraft$name();
}
