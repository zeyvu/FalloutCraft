package dev.skycraft.client.mixin;

import dev.skycraft.client.DiscordPresence;
import net.minecraft.client.gui.components.ChatComponent;
import net.minecraft.network.chat.Component;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.ModifyVariable;

/** Every chat line passes by Discord Rich Presence, which picks e4mc's link out of it (unchanged). */
@Mixin(ChatComponent.class)
public abstract class ChatComponentMixin {
	//#if MC_1_21_1
	//$$ // addMessage(Component) forwards to this overload, which every chat line reaches exactly once.
	//$$ @ModifyVariable(method = "addMessage(Lnet/minecraft/network/chat/Component;Lnet/minecraft/network/chat/MessageSignature;Lnet/minecraft/client/GuiMessageTag;)V", at = @At("HEAD"), argsOnly = true, ordinal = 0)
	//#else
	@ModifyVariable(method = "addMessage", at = @At("HEAD"), argsOnly = true, ordinal = 0)
	//#endif
	private Component skycraft$watchForLink(Component contents) {
		DiscordPresence.onChat(contents);
		return contents;
	}
}
