package dev.skycraft.client.mixin;

import com.mojang.blaze3d.platform.InputConstants;
import com.mojang.blaze3d.platform.Window;
import dev.skycraft.client.InputBridge;
import dev.skycraft.client.SkyClient;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Keyboard state and mouse capture come from Skyrim while linked, not from SDL. */
@Mixin(InputConstants.class)
public abstract class InputConstantsMixin {
	//#if MC_1_21_1
	//$$ @Inject(method = "isKeyDown", at = @At("HEAD"), cancellable = true)
	//$$ private static void skycraft$isKeyDown(long window, int key, CallbackInfoReturnable<Boolean> cir) {
	//$$ 	if (SkyClient.tookOver()) {
	//$$ 		cir.setReturnValue(InputBridge.isKeyDown(key));
	//$$ 	}
	//$$ }
	//$$
	//$$ // 1.21.1 (GLFW): one method both grabs and releases the mouse.
	//$$ @Inject(method = "grabOrReleaseMouse", at = @At("HEAD"), cancellable = true)
	//$$ private static void skycraft$grabOrReleaseMouse(long window, int cursorMode, double xpos, double ypos, CallbackInfo ci) {
	//$$ 	if (SkyClient.tookOver()) {
	//$$ 		ci.cancel();
	//$$ 	}
	//$$ }
	//#else
	@Inject(method = "isKeyDown", at = @At("HEAD"), cancellable = true)
	private static void skycraft$isKeyDown(int key, CallbackInfoReturnable<Boolean> cir) {
		if (SkyClient.tookOver()) {
			cir.setReturnValue(InputBridge.isKeyDown(key));
		}
	}

	@Inject(method = "grabMouse", at = @At("HEAD"), cancellable = true)
	private static void skycraft$grabMouse(Window window, double xpos, double ypos, CallbackInfo ci) {
		if (SkyClient.tookOver()) {
			ci.cancel();
		}
	}

	@Inject(method = "releaseMouse", at = @At("HEAD"), cancellable = true)
	private static void skycraft$releaseMouse(Window window, double xpos, double ypos, CallbackInfo ci) {
		if (SkyClient.tookOver()) {
			ci.cancel();
		}
	}
	//#endif
}
