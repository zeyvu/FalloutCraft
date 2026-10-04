package dev.skycraft.client;

import java.util.ArrayList;
import java.util.List;
import java.util.Random;
import net.minecraft.client.Minecraft;
import net.minecraft.ChatFormatting;
import net.minecraft.network.chat.Component;

/**
 * FalloutCraft: the first time the player opens a Fallout container or searches a corpse, Minecraft
 * finds a little ordinary loot in it too. Only everyday things, the kind a desk drawer, a toolbox or
 * a raider's pockets could hold: no diamonds or enchanted gear.
 */
public final class FalloutScavenge {
	private record Loot(String id, int min, int max, int weight) {
	}

	private static final List<Loot> CONTAINER = List.of(
		new Loot("stick", 1, 4, 20),
		new Loot("iron_nugget", 1, 5, 14),
		new Loot("coal", 1, 3, 12),
		new Loot("paper", 1, 3, 10),
		new Loot("string", 1, 3, 10),
		new Loot("torch", 1, 4, 8),
		new Loot("bread", 1, 2, 8),
		new Loot("glass_bottle", 1, 2, 6),
		new Loot("apple", 1, 2, 6),
		new Loot("flint", 1, 2, 5),
		new Loot("leather", 1, 1, 5),
		new Loot("book", 1, 1, 3),
		new Loot("gunpowder", 1, 2, 3),
		new Loot("iron_ingot", 1, 1, 3),
		new Loot("copper_ingot", 1, 2, 3),
		new Loot("bucket", 1, 1, 1),
		new Loot("oak_sapling", 1, 2, 4),
		new Loot("birch_sapling", 1, 2, 2),
		new Loot("spruce_sapling", 1, 2, 2),
		new Loot("wheat_seeds", 2, 5, 4)
	);

	private static final List<Loot> CORPSE = List.of(
		new Loot("bone", 1, 2, 20),
		new Loot("rotten_flesh", 1, 3, 18),
		new Loot("arrow", 2, 6, 12),
		new Loot("leather", 1, 2, 10),
		new Loot("string", 1, 2, 10),
		new Loot("iron_nugget", 1, 4, 10),
		new Loot("bread", 1, 1, 8),
		new Loot("coal", 1, 2, 5),
		new Loot("gunpowder", 1, 2, 5),
		new Loot("cooked_beef", 1, 1, 3)
	);

	private FalloutScavenge() {
	}

	/** Fallout said the player searched a container (kind 0) or a corpse (kind 1) for the first time. */
	public static void found(Minecraft minecraft, int kind, int refId, int level) {
		var server = minecraft.getSingleplayerServer();
		if (server == null || minecraft.player == null) {
			return; // a guest in someone else's world: their loot is the host's business
		}
		Random random = new Random(((long) refId << 1) ^ System.nanoTime());
		List<Loot> table = kind == 1 ? CORPSE : CONTAINER;
		int stacks = kind == 1 ? 1 + random.nextInt(2) : 1 + random.nextInt(3);
		if (kind == 1 && level >= 20 && random.nextInt(3) == 0) {
			stacks++; // tougher enemies carry a bit more
		}
		String name = minecraft.player.getName().getString();
		List<String> given = new ArrayList<>();
		List<String> commands = new ArrayList<>();
		for (int i = 0; i < stacks; i++) {
			Loot loot = pick(table, random);
			int count = loot.min + random.nextInt(loot.max - loot.min + 1);
			commands.add("give " + name + " minecraft:" + loot.id + " " + count);
			given.add(count + " " + loot.id.replace('_', ' '));
		}
		server.execute(() -> {
			var source = server.createCommandSourceStack().withSuppressedOutput();
			for (String command : commands) {
				server.getCommands().performPrefixedCommand(source, command);
			}
		});
		Component text = Component.literal("Found! ").withStyle(ChatFormatting.BOLD, ChatFormatting.YELLOW)
			.append(Component.literal(String.join(", ", given)).withStyle(ChatFormatting.WHITE));
		//#if MC_1_21_1
		//$$ minecraft.gui.getChat().addMessage(text);
		//#else
		minecraft.gui.hud.getChat().addClientSystemMessage(text);
		//#endif
		dev.skycraft.SkyCraft.LOG.info("FalloutCraft: {} {} -> {}", kind == 1 ? "corpse" : "container", Integer.toHexString(refId), String.join(", ", given));
	}

	private static Loot pick(List<Loot> table, Random random) {
		int total = 0;
		for (Loot loot : table) {
			total += loot.weight;
		}
		int roll = random.nextInt(total);
		for (Loot loot : table) {
			roll -= loot.weight;
			if (roll < 0) {
				return loot;
			}
		}
		return table.getFirst();
	}
}
