package dev.skycraft.client.mixin;

import dev.skycraft.client.SkyClient;
import net.minecraft.client.gui.Gui;
import net.minecraft.client.gui.GuiGraphics;
import net.minecraft.world.entity.Entity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Minecraft 1.21.1: no vignette while linked. It's drawn over the whole screen (with Fancy
 * graphics, the default) with its own alpha written into the target, so the overlay Fallout gets
 * would be opaque black everywhere instead of see-through. (26.x has a vignette option, which
 * SkyClient turns off.)
 */
@Mixin(Gui.class)
public abstract class GuiVignetteMixin {
	@Inject(method = "renderVignette", at = @At("HEAD"), cancellable = true)
	private void skycraft$noVignette(GuiGraphics guiGraphics, Entity entity, CallbackInfo ci) {
		if (SkyClient.linked()) {
			ci.cancel();
		}
	}
}
