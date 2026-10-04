package dev.skycraft.client.mixin;

//#if MC_1_21_1
//$$ import net.minecraft.client.Minecraft;
//#else
import com.mojang.blaze3d.platform.FramerateLimitTracker;
//#endif
import dev.skycraft.client.SkyClient;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** SkyClient.paceFrame() locks us to Skyrim's frame rate; don't let MC throttle on its own. */
//#if MC_1_21_1
//$$ // 1.21.1 has no FramerateLimitTracker: Minecraft.runTick asks its own private getFramerateLimit()
//$$ // and calls RenderSystem.limitDisplayFPS only below 260.
//$$ @Mixin(Minecraft.class)
//#else
@Mixin(FramerateLimitTracker.class)
//#endif
public abstract class FramerateLimitTrackerMixin {
	@Inject(method = "getFramerateLimit", at = @At("HEAD"), cancellable = true)
	private void skycraft$unlimited(CallbackInfoReturnable<Integer> cir) {
		if (SkyClient.linked()) {
			cir.setReturnValue(260);
		}
	}
}
