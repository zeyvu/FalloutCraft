package dev.skycraft.client.mixin;

import net.minecraft.world.entity.WalkAnimationState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

/** Minecraft 1.21.1: stilling the legs for the ragdoll's standing capture (AvatarExporter). */
@Mixin(WalkAnimationState.class)
public interface WalkAnimationStateAccessor {
	@Accessor("speedOld")
	float skycraft$speedOld();

	@Accessor("speedOld")
	void skycraft$setSpeedOld(float value);

	@Accessor("speed")
	float skycraft$speed();

	@Accessor("speed")
	void skycraft$setSpeed(float value);

	@Accessor("position")
	float skycraft$position();

	@Accessor("position")
	void skycraft$setPosition(float value);
}
