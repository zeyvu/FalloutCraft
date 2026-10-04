package dev.skycraft.mixin;

//#if MC_1_21_1
//$$ import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
//$$ import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
//$$ import dev.skycraft.link.Proto;
//$$ import dev.skycraft.link.SkyLink;
//$$ import dev.skycraft.world.SkyDigBlast;
//$$ import java.util.Optional;
//$$ import net.minecraft.core.BlockPos;
//$$ import net.minecraft.world.level.BlockGetter;
//$$ import net.minecraft.world.level.Explosion;
//$$ import net.minecraft.world.level.ExplosionDamageCalculator;
//$$ import net.minecraft.world.level.Level;
//$$ import net.minecraft.world.level.block.state.BlockState;
//$$ import net.minecraft.world.level.material.FluidState;
//$$ import org.spongepowered.asm.mixin.Final;
//$$ import org.spongepowered.asm.mixin.Mixin;
//$$ import org.spongepowered.asm.mixin.Shadow;
//$$ import org.spongepowered.asm.mixin.Unique;
//$$ import org.spongepowered.asm.mixin.injection.At;
//$$ import org.spongepowered.asm.mixin.injection.Inject;
//$$ import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
//$$
//$$ /**
//$$  * Minecraft explosions in Skyrim's world: they blow Skyrim's ground and rock apart like blocks
//$$  * (SkyDigBlast), and Skyrim feels them (loose objects are thrown and people knocked away).
//$$  * <p>
//$$  * Minecraft 1.21.1 has no ServerExplosion: Explosion.explode() works out what breaks (into
//$$  * getToBlow()) and hurts entities, on the server only; finalizeExplosion(boolean) then breaks the
//$$  * blocks, on the server and again on clients replaying the explosion packet (which never ran
//$$  * explode() on that instance, so they're skipped here).
//$$  */
//$$ @Mixin(Explosion.class)
//$$ public abstract class ServerExplosionMixin {
//$$ 	@Shadow @Final private Level level;
//$$ 	@Shadow @Final private double x;
//$$ 	@Shadow @Final private double y;
//$$ 	@Shadow @Final private double z;
//$$ 	@Shadow @Final private float radius;
//$$
//$$ 	@Unique
//$$ 	private SkyDigBlast skycraft$blast;
//$$ 	@Unique
//$$ 	private boolean skycraft$detonated;
//$$
//$$ 	@Inject(method = "explode", at = @At("HEAD"))
//$$ 	private void skycraft$begin(CallbackInfo ci) {
//$$ 		if (this.level.isClientSide) {
//$$ 			return;
//$$ 		}
//$$ 		this.skycraft$detonated = true;
//$$ 		this.skycraft$blast = SkyDigBlast.begin((net.minecraft.server.level.ServerLevel) this.level, new net.minecraft.world.phys.Vec3(this.x, this.y, this.z), this.radius);
//$$ 	}
//$$
//$$ 	@WrapOperation(
//$$ 		method = "explode",
//$$ 		at = @At(
//$$ 			value = "INVOKE",
//$$ 			target = "Lnet/minecraft/world/level/ExplosionDamageCalculator;getBlockExplosionResistance(Lnet/minecraft/world/level/Explosion;Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/material/FluidState;)Ljava/util/Optional;"
//$$ 		)
//$$ 	)
//$$ 	private Optional<Float> skycraft$skyrimResists(
//$$ 		ExplosionDamageCalculator calculator, Explosion explosion, BlockGetter level, BlockPos pos, BlockState block, FluidState fluid, Operation<Optional<Float>> original
//$$ 	) {
//$$ 		Optional<Float> vanilla = original.call(calculator, explosion, level, pos, block, fluid);
//$$ 		return this.skycraft$blast != null ? this.skycraft$blast.resistance(pos, vanilla) : vanilla;
//$$ 	}
//$$
//$$ 	/** Before the blocks are broken (26.x: where explode() stores its list of them). */
//$$ 	@Inject(method = "finalizeExplosion", at = @At("HEAD"))
//$$ 	private void skycraft$skyrimBreaks(boolean spawnParticles, CallbackInfo ci) {
//$$ 		if (this.skycraft$blast != null) {
//$$ 			Explosion self = (Explosion) (Object) this;
//$$ 			this.skycraft$blast.materialize(self.getToBlow(), self.getBlockInteraction() != Explosion.BlockInteraction.KEEP
//$$ 				&& self.getBlockInteraction() != Explosion.BlockInteraction.TRIGGER_BLOCK);
//$$ 		}
//$$ 	}
//$$
//$$ 	@Inject(method = "finalizeExplosion", at = @At("RETURN"))
//$$ 	private void skycraft$tellSkyrim(boolean spawnParticles, CallbackInfo ci) {
//$$ 		if (!this.skycraft$detonated) {
//$$ 			return;
//$$ 		}
//$$ 		this.skycraft$detonated = false;
//$$ 		if (this.skycraft$blast != null) {
//$$ 			this.skycraft$blast.finish();
//$$ 			this.skycraft$blast = null;
//$$ 		}
//$$ 		if (!SkyLink.active()) {
//$$ 			return;
//$$ 		}
//$$ 		SkyLink.pushEvent(Proto.EV_EXPLOSION, 0, (float) this.x, (float) this.y, (float) this.z, this.radius, 0);
//$$ 	}
//$$ }
//#else
import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import dev.skycraft.world.SkyDigBlast;
import java.util.List;
import java.util.Optional;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.Explosion;
import net.minecraft.world.level.ExplosionDamageCalculator;
import net.minecraft.world.level.ServerExplosion;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FluidState;
import org.jspecify.annotations.Nullable;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.ModifyVariable;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Minecraft explosions in Skyrim's world: they blow Skyrim's ground and rock apart like blocks
 * (SkyDigBlast), and Skyrim feels them (loose objects are thrown and people knocked away).
 */
@Mixin(ServerExplosion.class)
public abstract class ServerExplosionMixin {
	@Unique
	private @Nullable SkyDigBlast skycraft$blast;

	@Inject(method = "explode", at = @At("HEAD"))
	private void skycraft$begin(CallbackInfoReturnable<Integer> cir) {
		this.skycraft$blast = SkyDigBlast.begin((ServerExplosion) (Object) this);
	}

	@WrapOperation(
		method = "calculateExplodedPositions",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/world/level/ExplosionDamageCalculator;getBlockExplosionResistance(Lnet/minecraft/world/level/Explosion;Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/material/FluidState;)Ljava/util/Optional;"
		)
	)
	private Optional<Float> skycraft$skyrimResists(
		ExplosionDamageCalculator calculator, Explosion explosion, BlockGetter level, BlockPos pos, BlockState block, FluidState fluid, Operation<Optional<Float>> original
	) {
		Optional<Float> vanilla = original.call(calculator, explosion, level, pos, block, fluid);
		return this.skycraft$blast != null ? this.skycraft$blast.resistance(pos, vanilla) : vanilla;
	}

	@ModifyVariable(method = "explode", at = @At("STORE"), ordinal = 0)
	private List<BlockPos> skycraft$skyrimBreaks(List<BlockPos> targets) {
		if (this.skycraft$blast != null) {
			ServerExplosion self = (ServerExplosion) (Object) this;
			this.skycraft$blast.materialize(targets, self.getBlockInteraction() != Explosion.BlockInteraction.KEEP
				&& self.getBlockInteraction() != Explosion.BlockInteraction.TRIGGER_BLOCK);
		}
		return targets;
	}

	@Inject(method = "explode", at = @At("RETURN"))
	private void skycraft$tellSkyrim(CallbackInfoReturnable<Integer> cir) {
		if (this.skycraft$blast != null) {
			this.skycraft$blast.finish();
			this.skycraft$blast = null;
		}
		if (!SkyLink.active()) {
			return;
		}
		ServerExplosion self = (ServerExplosion) (Object) this;
		var center = self.center();
		SkyLink.pushEvent(Proto.EV_EXPLOSION, 0, (float) center.x, (float) center.y, (float) center.z, self.radius(), 0);
	}
}
//#endif
