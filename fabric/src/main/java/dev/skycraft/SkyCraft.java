package dev.skycraft;

import dev.skycraft.platform.Platform;
import net.minecraft.world.entity.EquipmentSlot;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
//#if MC_1_21_1
//$$ import net.minecraft.world.level.GameRules;
//#else
import net.minecraft.world.level.gamerules.GameRules;
//#endif
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * The mod's common start-up. Each mod loader's entry point (dev.skycraft.fabric.SkyCraftFabric,
 * Forge's dev.skycraft.forge.SkyCraftForge) sets the {@link Platform}, registers the actor entity,
 * the packets and the events, and calls these.
 */
public final class SkyCraft {
	public static final String MOD_ID = "skycraft";
	public static final String WORLD_NAME = "SkyCraft";
	public static final Logger LOG = LoggerFactory.getLogger(MOD_ID);
	private static final String KIT2_TAG = "skycraft_builder_kit";

	private SkyCraft() {
	}

	/** Once, at mod start (after the loader's platform is set). */
	public static void init() {
		LOG.info("FalloutCraft: starting on {}", Platform.get().name());
		dev.skycraft.world.SkyDig.init();
	}

	/** A player joined the server (the host, or a friend). */
	public static void onPlayerJoin(ServerPlayer player) {
		giveStarterKit(player);
		giveBuilderKit(player);
		dressTestGuest(player);
	}

	/** The mirror world is a void that only exists to host the player; Skyrim drives time and spawning. */
	public static void configureServer(MinecraftServer server) {
		GameRules rules = server.getGameRules();
		//#if MC_1_21_1
		//$$ rules.getRule(GameRules.RULE_DAYLIGHT).set(false, server);
		//$$ rules.getRule(GameRules.RULE_WEATHER_CYCLE).set(false, server);
		//$$ rules.getRule(GameRules.RULE_DOMOBSPAWNING).set(false, server); // 1.21.1 has no separate monster rule; doMobSpawning covers them
		//$$ rules.getRule(GameRules.RULE_DOINSOMNIA).set(false, server);
		//$$ rules.getRule(GameRules.RULE_DO_PATROL_SPAWNING).set(false, server);
		//$$ rules.getRule(GameRules.RULE_DO_TRADER_SPAWNING).set(false, server);
		//$$ // 1.21.1 has no disablePlayerMovementCheck (1.21.2+); the elytra check is the only one it can turn off
		//$$ rules.getRule(GameRules.RULE_DISABLE_ELYTRA_MOVEMENT_CHECK).set(true, server);
		//$$ rules.getRule(GameRules.RULE_KEEPINVENTORY).set(true, server);
		//$$ rules.getRule(GameRules.RULE_DO_IMMEDIATE_RESPAWN).set(true, server);
		//$$ rules.getRule(GameRules.RULE_ANNOUNCE_ADVANCEMENTS).set(false, server);
		//#else
		rules.set(GameRules.ADVANCE_TIME, false, server);
		rules.set(GameRules.ADVANCE_WEATHER, false, server);
		rules.set(GameRules.SPAWN_MOBS, false, server);
		rules.set(GameRules.SPAWN_MONSTERS, false, server);
		rules.set(GameRules.SPAWN_PHANTOMS, false, server);
		rules.set(GameRules.SPAWN_PATROLS, false, server);
		rules.set(GameRules.SPAWN_WANDERING_TRADERS, false, server);
		rules.set(GameRules.PLAYER_MOVEMENT_CHECK, false, server);
		rules.set(GameRules.KEEP_INVENTORY, true, server);
		rules.set(GameRules.IMMEDIATE_RESPAWN, true, server);
		rules.set(GameRules.SHOW_ADVANCEMENT_MESSAGES, false, server);
		//#endif
		server.getCommands().performPrefixedCommand(server.createCommandSourceStack().withSuppressedOutput(), "time set noon");
		LOG.info("SkyCraft: mirror world configured");
	}

	/**
	 * Local multiplayer test guests (tools/fake_guest.py; named Guest, Guest2, ...) wear a random
	 * mix of iron and diamond armour, so they're easy to tell apart.
	 */
	private static void dressTestGuest(ServerPlayer player) {
		if (!player.getName().getString().startsWith("Guest")) {
			return;
		}
		var random = player.getRandom();
		EquipmentSlot[] slots = { EquipmentSlot.HEAD, EquipmentSlot.CHEST, EquipmentSlot.LEGS, EquipmentSlot.FEET };
		net.minecraft.world.item.Item[][] pieces = {
			{ Items.IRON_HELMET, Items.DIAMOND_HELMET },
			{ Items.IRON_CHESTPLATE, Items.DIAMOND_CHESTPLATE },
			{ Items.IRON_LEGGINGS, Items.DIAMOND_LEGGINGS },
			{ Items.IRON_BOOTS, Items.DIAMOND_BOOTS },
		};
		for (int i = 0; i < slots.length; i++) {
			player.setItemSlot(slots[i], new ItemStack(pieces[i][random.nextBoolean() ? 1 : 0]));
		}
		LOG.info("SkyCraft: dressed test guest {} in iron and diamond", player.getName().getString());
	}

	private static void giveStarterKit(ServerPlayer player) {
		if (!player.getInventory().isEmpty()) {
			return;
		}
		player.getInventory().add(new ItemStack(Items.DIAMOND_SWORD));
		player.getInventory().add(new ItemStack(Items.DIAMOND_PICKAXE));
		player.getInventory().add(new ItemStack(Items.BOW));
		player.getInventory().add(new ItemStack(Items.COOKED_BEEF, 32));
		player.getInventory().add(new ItemStack(Items.OAK_PLANKS, 64));
		player.getInventory().add(new ItemStack(Items.TORCH, 32));
		player.getInventory().add(new ItemStack(Items.ARROW, 64));
		player.setItemSlot(net.minecraft.world.entity.EquipmentSlot.OFFHAND, new ItemStack(Items.SHIELD));
		LOG.info("SkyCraft: gave starter kit to {}", player.getName().getString());
	}

	/**
	 * Once per player: armor (Skyrim's enemies hit back now) and building materials, since there is
	 * no Minecraft terrain to mine in Skyrim.
	 */
	private static void giveBuilderKit(ServerPlayer player) {
		//#if MC_1_21_1
		//$$ if (player.getTags().contains(KIT2_TAG)) {
		//#else
		if (player.entityTags().contains(KIT2_TAG)) {
		//#endif
			return;
		}
		equipIfEmpty(player, EquipmentSlot.HEAD, Items.IRON_HELMET);
		equipIfEmpty(player, EquipmentSlot.CHEST, Items.IRON_CHESTPLATE);
		equipIfEmpty(player, EquipmentSlot.LEGS, Items.IRON_LEGGINGS);
		equipIfEmpty(player, EquipmentSlot.FEET, Items.IRON_BOOTS);
		var inventory = player.getInventory();
		inventory.add(new ItemStack(Items.COBBLESTONE, 64));
		inventory.add(new ItemStack(Items.STONE_BRICKS, 64));
		inventory.add(new ItemStack(Items.OAK_LOG, 64));
		inventory.add(new ItemStack(Items.GLASS, 64));
		inventory.add(new ItemStack(Items.OAK_STAIRS, 64));
		inventory.add(new ItemStack(Items.OAK_SLAB, 64));
		inventory.add(new ItemStack(Items.OAK_DOOR, 8));
		inventory.add(new ItemStack(Items.LADDER, 32));
		inventory.add(new ItemStack(Items.LANTERN, 16));
		inventory.add(new ItemStack(Items.CRAFTING_TABLE));
		inventory.add(new ItemStack(Items.WATER_BUCKET));
		inventory.add(new ItemStack(Items.ARROW, 64));
		inventory.add(new ItemStack(Items.GOLDEN_APPLE, 4));
		player.addTag(KIT2_TAG);
		LOG.info("SkyCraft: gave builder kit to {}", player.getName().getString());
	}

	private static void equipIfEmpty(ServerPlayer player, EquipmentSlot slot, net.minecraft.world.item.Item item) {
		if (player.getItemBySlot(slot).isEmpty()) {
			player.setItemSlot(slot, new ItemStack(item));
		} else {
			player.getInventory().add(new ItemStack(item));
		}
	}
}
