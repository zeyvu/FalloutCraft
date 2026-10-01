package dev.skycraft.client;

import dev.skycraft.world.SkyCollision;
import dev.skycraft.world.SkyTri;
import dev.skycraft.world.TriCollider;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

/** Feeds the local player's movement through {@link TriCollider} against nearby Skyrim triangles. */
public final class SkyCollider {
	private SkyCollider() {
	}

	public static Vec3 collide(LocalPlayer player, Vec3 move) {
		AABB box = player.getBoundingBox();
		double step = player.maxUpStep();
		List<SkyTri> tris = new ArrayList<>();
		SkyCollision.trianglesNear(box.expandTowards(move).inflate(1.0, 1.0 + step, 1.0), tris);
		if (tris.isEmpty()) {
			return onFalloutGround(player, box, step, move);
		}
		double[] r = TriCollider.resolve(
			tris, (box.minX + box.maxX) * 0.5, box.minY, (box.minZ + box.maxZ) * 0.5, box.getXsize() * 0.5, box.getYsize(), step, player.onGround(),
			move.x, move.y, move.z
		);
		Vec3 result = move;
		if (!(r[0] == move.x && r[1] == move.y && r[2] == move.z)) {
			// The triangle pass (snapping down a slope, pushing out of a wall) can move the player into a
			// Minecraft block placed on the terrain; collide that result with Minecraft blocks again.
			result = Entity.collideBoundingBox(player, new Vec3(r[0], r[1], r[2]), box, player.level(), List.of());
		}
		return onFalloutGround(player, box, step, result);
	}

	/**
	 * FalloutCraft: Fallout's own ground is the last word. If this move would put the feet under it
	 * (the triangles missed it: a seam, a sprint-jump landing), they land on it instead. Only ground
	 * at most a step above where the feet started counts, so walls and roofs never lift anyone.
	 */
	private static Vec3 onFalloutGround(LocalPlayer player, AABB box, double step, Vec3 move) {
		if (move.y > 0.0) {
			return move;
		}
		double x = (box.minX + box.maxX) * 0.5 + move.x, z = (box.minZ + box.maxZ) * 0.5 + move.z;
		double ground = SkyClient.falloutGroundAt(x, z);
		if (Double.isNaN(ground)) {
			return move;
		}
		double feet = box.minY + move.y;
		if (feet >= ground - 0.02 || ground - box.minY > step + 0.05 || ground - feet > 1.5) {
			return move;
		}
		long now = System.currentTimeMillis();
		if (now - lastCatchLog > 2000) {
			lastCatchLog = now;
			dev.skycraft.SkyCraft.LOG.info("SkyCraft: caught the player on Fallout's ground {} blocks under it (the triangles missed it)",
				String.format("%.2f", ground - feet));
		}
		return new Vec3(move.x, ground - box.minY, move.z);
	}

	private static long lastCatchLog;

	/** Highest Skyrim surface at or below {@code maxAbove} over the feet at (x, y, z), or NaN. */
	public static double groundAt(double x, double y, double z, double maxAbove) {
		List<SkyTri> tris = new ArrayList<>();
		SkyCollision.trianglesNear(new AABB(x - 1, y - 4, z - 1, x + 1, y + maxAbove + 1, z + 1), tris);
		return TriCollider.groundAt(tris, x, y, z, maxAbove);
	}
}
