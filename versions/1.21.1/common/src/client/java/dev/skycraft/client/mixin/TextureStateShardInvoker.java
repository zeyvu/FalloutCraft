package dev.skycraft.client.mixin;

import java.util.Optional;
import net.minecraft.client.renderer.RenderStateShard;
import net.minecraft.resources.ResourceLocation;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Invoker;

/** Minecraft 1.21.1: the texture of a render type's texture state (empty for none). */
@Mixin(RenderStateShard.EmptyTextureStateShard.class)
public interface TextureStateShardInvoker {
	@Invoker("cutoutTexture")
	Optional<ResourceLocation> skycraft$cutoutTexture();
}
