package dev.skycraft.client.mixin;

import dev.skycraft.client.SkyClient;
//#if MC_1_21_1
//$$ import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
//$$ import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
//$$ import net.minecraft.client.multiplayer.LevelLoadStatusManager;
//$$ import net.minecraft.client.renderer.LevelRenderer;
//$$ import net.minecraft.core.BlockPos;
//#endif
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * The "Loading terrain" screen waits for the player's chunk section to be compiled for
 * rendering. We never render Minecraft's level while linked, so don't wait for it.
 */
//#if MC_1_21_1
//$$ // 1.21.1: LevelLoadStatusManager.tick(), in its WAITING_FOR_PLAYER_CHUNK state, waits for
//$$ // levelRenderer.isSectionCompiled(player's block pos) before the level counts as ready.
//$$ @Mixin(LevelLoadStatusManager.class)
//$$ public abstract class WaitingForPlayerChunkMixin {
//$$ 	@WrapOperation(
//$$ 		method = "tick",
//$$ 		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/LevelRenderer;isSectionCompiled(Lnet/minecraft/core/BlockPos;)Z")
//$$ 	)
//$$ 	private boolean skycraft$ready(LevelRenderer renderer, BlockPos pos, Operation<Boolean> original) {
//$$ 		return SkyClient.linked() || original.call(renderer, pos);
//$$ 	}
//$$ }
//#else
@Mixin(targets = "net.minecraft.client.multiplayer.LevelLoadTracker$WaitingForPlayerChunk")
public abstract class WaitingForPlayerChunkMixin {
	@Inject(method = "isReady", at = @At("HEAD"), cancellable = true)
	private void skycraft$ready(CallbackInfoReturnable<Boolean> cir) {
		if (SkyClient.linked()) {
			cir.setReturnValue(true);
		}
	}
}
//#endif
