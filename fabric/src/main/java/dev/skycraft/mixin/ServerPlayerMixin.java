package dev.skycraft.mixin;

import dev.skycraft.SkyCraft;
import dev.skycraft.combat.SkyCombat;
import dev.skycraft.combat.SkyrimActorEntity;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.server.level.ServerPlayer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(ServerPlayer.class)
public abstract class ServerPlayerMixin {
	/** Critical hits on a Skyrim actor are flagged so Skyrim can play them up. */
	@Inject(method = "crit", at = @At("HEAD"))
	private void skycraft$critSkyrim(Entity entity, CallbackInfo ci) {
		if (entity instanceof SkyrimActorEntity proxy) {
			proxy.markCritical();
		}
	}

	/** Dying in Minecraft is dying in Skyrim: the host's through the link, a guest's through theirs. */
	@Inject(method = "die", at = @At("HEAD"))
	private void skycraft$diesInSkyrim(DamageSource source, CallbackInfo ci) {
		ServerPlayer self = (ServerPlayer) (Object) this;
		int attacker = SkyCombat.attackerFormId(source);
		if (!dev.skycraft.net.SkyNet.isHost(self)) {
			dev.skycraft.platform.Platform.get().sendToPlayer(self, new dev.skycraft.net.SkyNet.Died(attacker));
//#if MC_1_21_1
//$$ 			SkyCraft.LOG.info("SkyCraft: guest {} died ({}); telling their Skyrim", self.getName().getString(), source.getMsgId());
//#else
			SkyCraft.LOG.info("SkyCraft: guest {} died ({}); telling their Skyrim", self.getPlainTextName(), source.getMsgId());
//#endif
			return;
		}
		if (SkyLink.active()) {
			SkyLink.pushEvent(Proto.EV_PLAYER_DIED, attacker, 0, 0, 0, 0, 0);
			SkyCraft.LOG.info("SkyCraft: player died ({}); telling Skyrim", source.getMsgId());
		}
	}
}
