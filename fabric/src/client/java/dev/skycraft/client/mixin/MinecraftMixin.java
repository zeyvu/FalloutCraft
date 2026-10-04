package dev.skycraft.client.mixin;

import dev.skycraft.client.SkyClient;
import net.minecraft.client.Minecraft;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Minecraft.class)
public abstract class MinecraftMixin {
	@Inject(method = "runTick", at = @At("HEAD"))
	private void skycraft$beginFrame(boolean advanceGameTime, CallbackInfo ci) {
		SkyClient.beginFrame();
	}

	//#if MC_1_21_1
	//$$ // 1.21.1: start every frame from a fully transparent main target while linked, whatever
	//$$ // Minecraft would have drawn under the HUD (FrameExporter ships it to Fallout as an overlay).
	//$$ @Inject(
	//$$ 	method = "runTick",
	//$$ 	at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/GameRenderer;render(Lnet/minecraft/client/DeltaTracker;Z)V")
	//$$ )
	//$$ private void skycraft$beforeRender(boolean advanceGameTime, CallbackInfo ci) {
	//$$ 	dev.skycraft.client.FrameExporter.beforeRender((Minecraft) (Object) this);
	//$$ }
	//$$
	//$$ // 1.21.1 has no renderFrame: runTick renders, blits and swaps the frame itself.
	//$$ @Inject(
	//$$ 	method = "runTick",
	//$$ 	at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/GameRenderer;render(Lnet/minecraft/client/DeltaTracker;Z)V", shift = At.Shift.AFTER)
	//$$ )
	//#else
	@Inject(
		method = "renderFrame",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/GameRenderer;render()V", shift = At.Shift.AFTER)
	)
	//#endif
	private void skycraft$afterRender(boolean advanceGameTime, CallbackInfo ci) {
		SkyClient.afterRender();
	}

	//#if MC_1_21_1
	//$$ @Inject(method = "runTick", at = @At("TAIL"))
	//#else
	@Inject(method = "renderFrame", at = @At("TAIL"))
	//#endif
	private void skycraft$pace(boolean advanceGameTime, CallbackInfo ci) {
		SkyClient.paceFrame();
	}
}
