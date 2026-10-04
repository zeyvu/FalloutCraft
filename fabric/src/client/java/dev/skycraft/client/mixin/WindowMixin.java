package dev.skycraft.client.mixin;

//#if MC_1_21_1
//$$ import net.minecraft.client.Minecraft;
//#else
import com.mojang.blaze3d.platform.Window;
//#endif
import dev.skycraft.client.SkyClient;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** The MC window is hidden while linked; Skyrim has the real focus, so pretend we do too. */
//#if MC_1_21_1
//$$ // 1.21.1: Window keeps no focus state; GLFW's focus callback sets Minecraft.windowActive, which
//$$ // isWindowActive() reports (MouseHandler.grabMouse and the pause-on-lost-focus check read it).
//$$ @Mixin(Minecraft.class)
//#else
@Mixin(Window.class)
//#endif
public abstract class WindowMixin {
	//#if MC_1_21_1
	//$$ @Inject(method = "isWindowActive", at = @At("HEAD"), cancellable = true)
	//#else
	@Inject(method = "isFocused", at = @At("HEAD"), cancellable = true)
	//#endif
	private void skycraft$focused(CallbackInfoReturnable<Boolean> cir) {
		if (SkyClient.tookOver()) {
			// Focused while Skyrim is connected; if Skyrim goes away, act unfocused so MC
			// never tries to grab the (hidden) mouse.
			cir.setReturnValue(SkyClient.linked());
		}
	}

	//#if MC_1_21_1
	//$$ // (1.21.1 neither tracks nor reacts to the window being iconified: nothing to override.)
	//#else
	@Inject(method = "isIconified", at = @At("HEAD"), cancellable = true)
	private void skycraft$notIconified(CallbackInfoReturnable<Boolean> cir) {
		if (SkyClient.linked()) {
			cir.setReturnValue(false);
		}
	}
	//#endif
}
