package dev.skycraft.combat;

import dev.skycraft.SkyCraft;
import dev.skycraft.link.SkyLink;
import java.util.Arrays;
import net.minecraft.core.Holder;
import net.minecraft.resources.Identifier;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.ai.attributes.Attribute;
import net.minecraft.world.entity.ai.attributes.AttributeInstance;
import net.minecraft.world.entity.ai.attributes.AttributeModifier;
import net.minecraft.world.entity.ai.attributes.Attributes;

/**
 * FalloutCraft: the Fallout player's S.P.E.C.I.A.L. shapes Minecraft's player. Each stat counts
 * from 5 (an average Vault Dweller): above 5 is a bonus, below 5 a penalty.
 * <ul>
 * <li>Strength: melee damage and knockback resistance.</li>
 * <li>Perception: reach (blocks and creatures).</li>
 * <li>Endurance: maximum health, breath under water, armour toughness.</li>
 * <li>Charisma, Intelligence: nothing in Minecraft's body (Fallout keeps using them).</li>
 * <li>Agility: walking/running speed, attack speed, falls that don't hurt.</li>
 * <li>Luck: Minecraft's luck (better loot from chests and fishing).</li>
 * </ul>
 */
public final class FalloutSpecial {
	private static final String[] NAMES = { "S", "P", "E", "C", "I", "A", "L" };
	private static int[] logged;

	private FalloutSpecial() {
	}

	/** Server thread: brings the player's attribute bonuses in line with Fallout's S.P.E.C.I.A.L. */
	public static void apply(ServerPlayer player) {
		int[] s = SkyLink.special;
		if (s == null || s.length < 7) {
			return;
		}
		int str = s[0] - 5, per = s[1] - 5, end = s[2] - 5, agi = s[5] - 5, lck = s[6] - 5;
		set(player, Attributes.ATTACK_DAMAGE, "special_strength_damage", 0.3 * str, AttributeModifier.Operation.ADD_VALUE);
		set(player, Attributes.KNOCKBACK_RESISTANCE, "special_strength_knockback", Math.max(0, str) * 0.04, AttributeModifier.Operation.ADD_VALUE);
		set(player, Attributes.ENTITY_INTERACTION_RANGE, "special_perception_reach", 0.15 * per, AttributeModifier.Operation.ADD_VALUE);
		set(player, Attributes.BLOCK_INTERACTION_RANGE, "special_perception_block_reach", 0.15 * per, AttributeModifier.Operation.ADD_VALUE);
		set(player, Attributes.MAX_HEALTH, "special_endurance_health", 2.0 * Math.max(end, -4), AttributeModifier.Operation.ADD_VALUE);
		set(player, Attributes.OXYGEN_BONUS, "special_endurance_breath", Math.max(0, end) * 0.5, AttributeModifier.Operation.ADD_VALUE);
		set(player, Attributes.ARMOR_TOUGHNESS, "special_endurance_toughness", Math.max(0, end) * 0.5, AttributeModifier.Operation.ADD_VALUE);
		set(player, Attributes.MOVEMENT_SPEED, "special_agility_speed", 0.02 * agi, AttributeModifier.Operation.ADD_MULTIPLIED_BASE);
		set(player, Attributes.ATTACK_SPEED, "special_agility_attack_speed", 0.04 * agi, AttributeModifier.Operation.ADD_MULTIPLIED_BASE);
		set(player, Attributes.SAFE_FALL_DISTANCE, "special_agility_fall", Math.max(0, agi) * 0.4, AttributeModifier.Operation.ADD_VALUE);
		set(player, Attributes.LUCK, "special_luck", lck, AttributeModifier.Operation.ADD_VALUE);
		if (player.getHealth() > player.getMaxHealth()) {
			player.setHealth(player.getMaxHealth());
		}
		if (logged == null || !Arrays.equals(logged, s)) {
			logged = s.clone();
			StringBuilder b = new StringBuilder();
			for (int i = 0; i < 7; i++) {
				b.append(NAMES[i]).append(s[i]).append(' ');
			}
			SkyCraft.LOG.info("SkyCraft: Fallout S.P.E.C.I.A.L. {}-> max health {}, speed x{}", b, player.getMaxHealth(),
				String.format("%.2f", 1.0 + 0.02 * agi));
		}
	}

	private static void set(ServerPlayer player, Holder<Attribute> attribute, String name, double amount, AttributeModifier.Operation operation) {
		AttributeInstance instance = player.getAttribute(attribute);
		if (instance == null) {
			return;
		}
		Identifier id = Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, name);
		if (amount == 0.0) {
			instance.removeModifier(id);
			return;
		}
		instance.addOrUpdateTransientModifier(new AttributeModifier(id, amount, operation));
	}
}
