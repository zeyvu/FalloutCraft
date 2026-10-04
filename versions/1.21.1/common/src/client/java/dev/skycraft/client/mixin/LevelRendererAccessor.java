package dev.skycraft.client.mixin;

import it.unimi.dsi.fastutil.ints.Int2ObjectMap;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.server.level.BlockDestructionProgress;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

/** Minecraft 1.21.1: blocks being mined (the crack overlays) live in LevelRenderer. */
@Mixin(LevelRenderer.class)
public interface LevelRendererAccessor {
	@Accessor("destroyingBlocks")
	Int2ObjectMap<BlockDestructionProgress> skycraft$destroyingBlocks();
}
