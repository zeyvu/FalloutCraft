package dev.skycraft.client.mixin;

//#if MC_1_21_1
//$$ import net.minecraft.client.server.IntegratedPlayerList;
//$$ import org.spongepowered.asm.mixin.injection.ModifyArg;
//#else
import net.minecraft.client.server.IntegratedServer;
//#endif
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * A world opened to friends takes up to 100 players, not Minecraft's fixed 8 for LAN worlds (it's the
 * host's own PC doing the serving, over e4mc).
 */
//#if MC_1_21_1
//$$ // 1.21.1: the limit is PlayerList's final maxPlayers, which IntegratedPlayerList's constructor
//$$ // passes to super(...) as 8; both MinecraftServer.getMaxPlayers() and the login check read it.
//$$ @Mixin(IntegratedPlayerList.class)
//$$ public abstract class IntegratedServerMixin {
//$$ 	@ModifyArg(
//$$ 		method = "<init>",
//$$ 		at = @At(value = "INVOKE", target = "Lnet/minecraft/server/players/PlayerList;<init>(Lnet/minecraft/server/MinecraftServer;Lnet/minecraft/core/LayeredRegistryAccess;Lnet/minecraft/world/level/storage/PlayerDataStorage;I)V"),
//$$ 		index = 3
//$$ 	)
//$$ 	private static int skycraft$morePlayers(int maxPlayers) {
//$$ 		return 100;
//$$ 	}
//$$ }
//#else
@Mixin(IntegratedServer.class)
public abstract class IntegratedServerMixin {
	@Inject(method = "getMaxPlayers", at = @At("HEAD"), cancellable = true)
	private void skycraft$morePlayers(CallbackInfoReturnable<Integer> cir) {
		cir.setReturnValue(100);
	}
}
//#endif
