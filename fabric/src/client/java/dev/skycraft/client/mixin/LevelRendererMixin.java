package dev.skycraft.client.mixin;

import dev.skycraft.client.SkyClient;
import net.minecraft.client.renderer.LevelRenderer;
//#if MC_1_21_1
//$$ import com.mojang.blaze3d.systems.RenderSystem;
//$$ import net.minecraft.client.Minecraft;
//$$ import org.lwjgl.opengl.GL11;
//#endif
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Skyrim draws the world. While linked, Minecraft renders nothing of its own level (no sky,
 * clouds, fog or terrain) so the overlay is just hand + HUD on a transparent background.
 */
@Mixin(LevelRenderer.class)
public abstract class LevelRendererMixin {
	//#if MC_1_21_1
	//$$ @Inject(
	//$$ 	method = "renderLevel(Lnet/minecraft/client/DeltaTracker;ZLnet/minecraft/client/Camera;Lnet/minecraft/client/renderer/GameRenderer;Lnet/minecraft/client/renderer/LightTexture;Lorg/joml/Matrix4f;Lorg/joml/Matrix4f;)V",
	//$$ 	at = @At("HEAD"),
	//$$ 	cancellable = true
	//$$ )
	//$$ private void skycraft$skipLevel(net.minecraft.client.DeltaTracker deltaTracker, boolean renderBlockOutline, net.minecraft.client.Camera camera,
	//$$ 	net.minecraft.client.renderer.GameRenderer gameRenderer, net.minecraft.client.renderer.LightTexture lightTexture, org.joml.Matrix4f frustumMatrix,
	//$$ 	org.joml.Matrix4f projectionMatrix, CallbackInfo ci) {
	//$$ 	if (SkyClient.linked()) {
	//$$ 		// renderLevel is where the entity and block entity renderers learn the camera; entities
	//$$ 		// drawn in the GUI (the inventory's player, mods' model previews) need it too.
	//$$ 		Minecraft minecraft = Minecraft.getInstance();
	//$$ 		if (minecraft.level != null) {
	//$$ 			minecraft.getEntityRenderDispatcher().prepare(minecraft.level, camera, minecraft.crosshairPickEntity);
	//$$ 			minecraft.getBlockEntityRenderDispatcher().prepare(minecraft.level, camera, minecraft.hitResult);
	//$$ 		}
	//$$ 		// renderLevel is also what sets the fog clear colour and clears the bound main target;
	//$$ 		// clear it to fully transparent black ourselves.
	//$$ 		RenderSystem.clearColor(0.0F, 0.0F, 0.0F, 0.0F);
	//$$ 		RenderSystem.clear(GL11.GL_COLOR_BUFFER_BIT | GL11.GL_DEPTH_BUFFER_BIT, Minecraft.ON_OSX);
	//$$ 		ci.cancel();
	//$$ 	}
	//$$ }
	//#else
	@Inject(
		method = "render(Lcom/mojang/blaze3d/resource/GraphicsResourceAllocator;ZLnet/minecraft/client/renderer/state/level/CameraRenderState;Lcom/mojang/renderpearl/api/buffers/GpuBufferSlice;Lorg/joml/Vector4f;ZZ)V",
		at = @At("HEAD"),
		cancellable = true
	)
	private void skycraft$skipLevel(CallbackInfo ci) {
		if (SkyClient.linked()) {
			ci.cancel();
		}
	}
	//#endif
}
