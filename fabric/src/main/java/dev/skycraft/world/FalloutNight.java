package dev.skycraft.world;

import dev.skycraft.SkyCraft;
import dev.skycraft.link.SkyLink;
import java.util.List;
import java.util.Locale;
import net.minecraft.commands.CommandSourceStack;
import net.minecraft.core.BlockPos;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.util.RandomSource;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.phys.AABB;

/**
 * FalloutCraft: Minecraft's night comes to the Commonwealth.
 *
 * Minecraft's clock follows Fallout's (so zombies and skeletons burn at Fallout's dawn), and at
 * night zombies, skeletons and creepers turn up around the player out in the Wasteland, standing on
 * Fallout's ground. Minecraft draws them inside Fallout like every other Minecraft entity.
 *
 * Vanilla spawning stays off (the mirror world has no Minecraft terrain to spawn on), so they're
 * summoned here, on Fallout ground near the player. Minecraft's pathfinding only knows Minecraft
 * blocks, so when it can't find a path a mob simply walks straight at its target.
 */
public final class FalloutNight {
	public static final String TAG = "falloutcraft_night";
	public static final String RANGED_TAG = "falloutcraft_ranged"; // skeletons: keep their distance

	private static final int MAX_NEARBY = 10;        // night mobs alive around the player
	private static final double SPAWN_MIN = 18.0;    // blocks from the player
	private static final double SPAWN_MAX = 34.0;
	private static final int SPAWN_EVERY = 40;       // ticks between spawn attempts
	private static final double INTERIOR_FROM = 15000.0; // fo_worlds.cpp: interiors sit 16k..98k blocks out
	private static final double INTERIOR_TO = 110000.0;  // other worldspaces (Far Harbor, ...) at 120k

	private static final SkyLink.SkyState SKY = new SkyLink.SkyState();
	private static int lastTimeSet = Integer.MIN_VALUE;
	private static long lastLog;

	private FalloutNight() {
	}

	/** Fallout's hour (0-24), or NaN without a link. */
	private static float falloutHour() {
		if (!SkyLink.active() || !SkyLink.readSkyState(SKY)) {
			return Float.NaN;
		}
		return SKY.gameHour;
	}

	private static boolean isNight(float hour) {
		return hour >= 20.0F || hour < 5.0F;
	}

	private static boolean outdoors(ServerPlayer player) {
		double far = Math.max(Math.abs(player.getX()), Math.abs(player.getZ()));
		return far < INTERIOR_FROM || far >= INTERIOR_TO;
	}

	/** End of every server tick (registered by the mod loader). */
	public static void tick(MinecraftServer server) {
		List<ServerPlayer> players = server.getPlayerList().getPlayers();
		if (players.isEmpty() || server.getTickCount() % 5 != 0) {
			return;
		}
		float hour = falloutHour();
		if (Float.isNaN(hour)) {
			return;
		}
		if (server.getTickCount() % 200 == 0) {
			syncClock(server, hour);
		}
		ServerPlayer player = players.getFirst();
		//#if MC_1_21_1
		//$$ ServerLevel level = player.serverLevel();
		//#else
		ServerLevel level = player.level();
		//#endif
		AABB around = player.getBoundingBox().inflate(96.0);
		//#if MC_1_21_1
		//$$ List<Mob> mobs = level.getEntitiesOfClass(Mob.class, around, m -> m.isAlive() && m.getTags().contains(TAG));
		//#else
		List<Mob> mobs = level.getEntitiesOfClass(Mob.class, around, m -> m.isAlive() && m.entityTags().contains(TAG));
		//#endif

		for (Mob mob : mobs) {
			chase(mob);
			// Daytime or indoors: the ones the player isn't near fade away.
			if ((!isNight(hour) || !outdoors(player)) && mob.distanceToSqr(player) > 24.0 * 24.0) {
				mob.discard();
			}
		}

		if (server.getTickCount() % SPAWN_EVERY != 0 || !isNight(hour) || !outdoors(player) || player.isSpectator()) {
			return;
		}
		if (mobs.size() >= MAX_NEARBY || player.getRandom().nextFloat() > 0.5F) {
			return;
		}
		spawnNear(server, player);
	}

	/** Minecraft's clock = Fallout's (Minecraft's day starts at 6:00). */
	private static void syncClock(MinecraftServer server, float hour) {
		int ticks = (int) (((hour - 6.0F + 24.0F) % 24.0F) * 1000.0F);
		if (lastTimeSet != Integer.MIN_VALUE && Math.abs(ticks - lastTimeSet) < 100) {
			return;
		}
		lastTimeSet = ticks;
		run(server, "time set " + ticks);
	}

	private static void spawnNear(MinecraftServer server, ServerPlayer player) {
		RandomSource random = player.getRandom();
		for (int attempt = 0; attempt < 8; attempt++) {
			double angle = random.nextDouble() * Math.PI * 2.0;
			double dist = SPAWN_MIN + random.nextDouble() * (SPAWN_MAX - SPAWN_MIN);
			int x = (int) Math.floor(player.getX() + Math.cos(angle) * dist);
			int z = (int) Math.floor(player.getZ() + Math.sin(angle) * dist);
			double y = groundAt(x, (int) Math.floor(player.getY()), z);
			if (Double.isNaN(y)) {
				continue;
			}
			float roll = random.nextFloat();
			String type = roll < 0.45F ? "zombie" : roll < 0.8F ? "skeleton" : "creeper";
			String tags = type.equals("skeleton") ? "\"" + TAG + "\",\"" + RANGED_TAG + "\"" : "\"" + TAG + "\"";
			run(server, String.format(Locale.ROOT, "summon minecraft:%s %.2f %.2f %.2f {Tags:[%s]}", type, x + 0.5, y, z + 0.5, tags));
			long now = System.currentTimeMillis();
			if (now - lastLog > 10000) {
				lastLog = now;
				SkyCraft.LOG.info("FalloutCraft: night in the Wasteland: a {} turned up at {} {} {}", type, x, String.format(Locale.ROOT, "%.1f", y), z);
			}
			return;
		}
	}

	/** Top of Fallout's ground in the column (x, z) near height y, with two free blocks above it; NaN if none. */
	private static double groundAt(int x, int y, int z) {
		for (int yy = y + 12; yy >= y - 12; yy--) {
			BlockPos pos = new BlockPos(x, yy, z);
			if (!SkyCollision.supportsFromBelow(pos)) {
				continue;
			}
			if (SkyCollision.solidFraction(pos) > 0.15F || SkyCollision.solidFraction(pos.above()) > 0.15F || SkyCollision.solidFraction(pos.above(2)) > 0.15F) {
				continue;
			}
			return SkyCollision.hasGeometry(pos) ? yy + SkyCollision.groundTop(pos) : yy;
		}
		return Double.NaN;
	}

	/** Without a Minecraft path (Fallout's ground isn't Minecraft blocks), walk straight at the target. */
	private static void chase(Mob mob) {
		LivingEntity target = mob.getTarget();
		if (target == null || !target.isAlive() || !mob.getNavigation().isDone()) {
			return;
		}
		double d2 = mob.distanceToSqr(target);
		//#if MC_1_21_1
		//$$ double keep = mob.getTags().contains(RANGED_TAG) ? 8.0 * 8.0 : 1.2 * 1.2;
		//#else
		double keep = mob.entityTags().contains(RANGED_TAG) ? 8.0 * 8.0 : 1.2 * 1.2;
		//#endif
		if (d2 <= keep || d2 > 48.0 * 48.0) {
			return;
		}
		mob.getMoveControl().setWantedPosition(target.getX(), target.getY(), target.getZ(), 1.0);
		mob.getLookControl().setLookAt(target, 30.0F, 30.0F);
	}

	private static void run(MinecraftServer server, String command) {
		CommandSourceStack source = server.createCommandSourceStack().withSuppressedOutput();
		server.getCommands().performPrefixedCommand(source, command);
	}
}
