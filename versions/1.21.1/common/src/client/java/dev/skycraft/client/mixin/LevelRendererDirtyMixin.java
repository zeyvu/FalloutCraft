package dev.skycraft.client.mixin;

import dev.skycraft.client.render.WorldExporter;
import net.minecraft.client.renderer.LevelRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Minecraft 1.21.1: every block change (and chunk load, light change) marks its 16^3 section for
 * re-meshing into Fallout. (26.x: LevelExtractorMixin.)
 */
@Mixin(LevelRenderer.class)
public abstract class LevelRendererDirtyMixin {
	@Inject(method = "setSectionDirty(IIIZ)V", at = @At("HEAD"))
	private void skycraft$sectionDirty(int sectionX, int sectionY, int sectionZ, boolean playerChanged, CallbackInfo ci) {
		if (playerChanged) {
			WorldExporter.markDirtyNow(sectionX, sectionY, sectionZ);
		} else {
			WorldExporter.markDirty(sectionX, sectionY, sectionZ);
		}
	}
}
