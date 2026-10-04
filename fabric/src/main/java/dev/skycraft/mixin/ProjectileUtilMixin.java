package dev.skycraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.skycraft.world.SkyClip;
import net.minecraft.world.entity.projectile.ProjectileUtil;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.BlockHitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/** Thrown projectiles (snowballs, eggs, pearls, potions) and spear reach checks see Skyrim surfaces. */
@Mixin(ProjectileUtil.class)
public abstract class ProjectileUtilMixin {
	@WrapOperation(
//#if MC_1_21_1
//$$ 		// 1.21.1 has no spears (getHitEntitiesAlong), and getHitResult uses level.clip.
//$$ 		method = "getHitResult",
//$$ 		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;clip(Lnet/minecraft/world/level/ClipContext;)Lnet/minecraft/world/phys/BlockHitResult;")
//#else
		method = { "getHitResult", "getHitEntitiesAlong" },
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;clipIncludingBorder(Lnet/minecraft/world/level/ClipContext;)Lnet/minecraft/world/phys/BlockHitResult;")
//#endif
	)
	private static BlockHitResult skycraft$hitSkyrim(Level level, ClipContext context, Operation<BlockHitResult> original) {
		return SkyClip.refine(context.getFrom(), context.getTo(), original.call(level, context), SkyClip.Use.PROJECTILE);
	}
}
