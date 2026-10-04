package dev.skycraft.client.render;

import com.mojang.blaze3d.platform.NativeImage;
import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.blaze3d.vertex.VertexConsumer;
import dev.skycraft.SkyCraft;
import dev.skycraft.client.mixin.CompositeRenderTypeAccessor;
import dev.skycraft.client.mixin.CompositeStateAccessor;
import dev.skycraft.client.mixin.ParticleEngineAccessor;
import dev.skycraft.client.mixin.RenderStateShardAccessor;
import dev.skycraft.client.mixin.TextureStateShardInvoker;
import dev.skycraft.client.mixin.WalkAnimationStateAccessor;
import dev.skycraft.combat.SkyrimActorEntity;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;
import net.minecraft.client.particle.Particle;
import net.minecraft.client.particle.ParticleRenderType;
import net.minecraft.client.player.AbstractClientPlayer;
import net.minecraft.client.renderer.MultiBufferSource;
import net.minecraft.client.renderer.RenderType;
import net.minecraft.client.renderer.texture.AbstractTexture;
import net.minecraft.client.renderer.texture.DynamicTexture;
import net.minecraft.client.renderer.texture.OverlayTexture;
import net.minecraft.client.renderer.texture.TextureAtlas;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.util.Mth;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.item.ItemEntity;
import net.minecraft.world.entity.projectile.AbstractArrow;
import net.minecraft.world.entity.projectile.ItemSupplier;
import net.minecraft.world.phys.Vec3;
import org.jspecify.annotations.Nullable;

/**
 * Minecraft 1.21.1: Minecraft's own entity and particle rendering, captured as triangles for Fallout.
 * The renderers draw into this buffer source instead of a GPU buffer, and the posed, animated
 * geometry goes over with its textures. Same messages as the 26.x version (which captures 26.x's
 * submit nodes instead).
 *
 * <p>The player's body in third person (F5), relative to its feet; everything else (lit TNT,
 * falling blocks, minecarts, boats, mobs, block entities and particles) relative to a block near
 * the camera; and once a second the body standing still for the death ragdoll. Arrows, dropped
 * items and thrown items have their own lighter path (WorldExporter). Render thread only.
 */
final class AvatarExporter implements MultiBufferSource {
	// Vertex flags: cutout, full-detail texture, lit by its own faces / without a normal / blended.
	private static final int SOLID = 1 | 8 | (7 << 4);
	private static final int PARTICLE = 1 | 8;
	private static final int PARTICLE_BLENDED = 2 | 8;
	// How a batch's raw UVs map into the combined atlas (texture 0).
	private static final int UV_RAW = 0, UV_BLOCK_ATLAS = 1;
	private static final double SCENE_RANGE = 64.0;
	private static final int SCENE_MAX_ENTITIES = 48;
	private static final double BLOCK_ENTITY_RANGE = 48.0;
	private static final int SCENE_MAX_BLOCK_ENTITIES = 256;
	private static final int MAX_PARTICLES = 4000;
	private static boolean warnedBlockEntity;

	// Textures Fallout holds, shared by the captures.
	private static final Map<ResourceLocation, Integer> TEXTURE_IDS = new HashMap<>();
	private static final Set<ResourceLocation> UNUSABLE = new HashSet<>();
	private static int nextTextureId = 1;
	private static boolean warnedEntity;
	private static boolean loggedAvatar;

	private static final AvatarExporter AVATAR = new AvatarExporter();
	private static final AvatarExporter SCENE = new AvatarExporter();
	private static final AvatarExporter RAGDOLL = new AvatarExporter();
	private static long nextRagdollNanos;

	private final Map<Long, Batch> batches = new HashMap<>();
	private boolean shown;
	private SkyAtlas atlas;
	// The ragdoll leaves out what's held (items drop when a player dies): block-atlas geometry.
	private boolean skipBlockAtlas;
	// Added to every position (particles come relative to the camera).
	private float offX, offY, offZ;

	private AvatarExporter() {
	}

	/** Fallout dropped everything (new link, new world, new atlas): textures go again. */
	static void reset() {
		TEXTURE_IDS.clear();
		UNUSABLE.clear();
		nextTextureId = 1;
		AVATAR.shown = false;
		SCENE.shown = false;
		RAGDOLL.shown = false;
		nextRagdollNanos = 0;
	}

	static void frame(Minecraft minecraft, SkyAtlas atlas, float partialTick) {
		AVATAR.exportAvatar(minecraft, atlas, partialTick);
		SCENE.exportScene(minecraft, atlas, partialTick);
		long now = System.nanoTime();
		if (now >= nextRagdollNanos) {
			nextRagdollNanos = now + 1_000_000_000L;
			RAGDOLL.exportRagdoll(minecraft, atlas, partialTick);
		}
	}

	// ---- the captures ------------------------------------------------------------------------------

	private void exportAvatar(Minecraft minecraft, SkyAtlas atlas, float partialTick) {
		this.atlas = atlas;
		var player = minecraft.player;
		Camera camera = minecraft.gameRenderer.getMainCamera();
		if (player == null || !camera.isDetached()) {
			this.sendEmpty(Proto.REN_AVATAR, false);
			return;
		}
		this.begin();
		try {
			var dispatcher = minecraft.getEntityRenderDispatcher();
			dispatcher.prepare(minecraft.level, camera, minecraft.crosshairPickEntity);
			float yaw = Mth.lerp(partialTick, player.yRotO, player.getYRot());
			// At the origin: positions come out relative to the player's feet.
			dispatcher.render(player, 0.0, 0.0, 0.0, yaw, partialTick, new PoseStack(), this, dispatcher.getPackedLightCoords(player, partialTick));
		} catch (RuntimeException e) {
			SkyCraft.LOG.warn("SkyCraft: couldn't capture the player model", e);
			return;
		}
		if (!loggedAvatar) {
			loggedAvatar = true;
			int quads = 0;
			for (Batch b : this.batches.values()) {
				quads += b.count / 4;
			}
			SkyCraft.LOG.info("SkyCraft: third-person player model captured for Fallout ({} quads, {} textures)", quads, this.batches.size());
		}
		this.send(Proto.REN_AVATAR, null);
	}

	/**
	 * The player's body standing still, facing +Z, feet at the origin, split into Minecraft's six
	 * parts, for Fallout to hang on its ragdoll when the player dies. Kept up to date while alive
	 * (skin and armour change), so the last one before death is the one that falls. 1.21.1 renders
	 * straight from the entity, so its pose is set on the player for the capture and put back.
	 */
	private void exportRagdoll(Minecraft minecraft, SkyAtlas atlas, float partialTick) {
		this.atlas = atlas;
		var player = minecraft.player;
		if (player == null || player.isDeadOrDying() || player.isSpectator() || player.isCrouching() || player.isVisuallySwimming() || player.isFallFlying()) {
			return;
		}
		this.begin();
		Pose saved = Pose.of(player);
		try {
			Pose.standing(player);
			var dispatcher = minecraft.getEntityRenderDispatcher();
			dispatcher.prepare(minecraft.level, minecraft.gameRenderer.getMainCamera(), minecraft.crosshairPickEntity);
			this.skipBlockAtlas = true;
			dispatcher.render(player, 0.0, 0.0, 0.0, 0.0F, 1.0F, new PoseStack(), this, dispatcher.getPackedLightCoords(player, partialTick));
		} catch (RuntimeException e) {
			SkyCraft.LOG.warn("SkyCraft: couldn't capture the player's body for the ragdoll", e);
			return;
		} finally {
			this.skipBlockAtlas = false;
			saved.restore(player);
		}
		this.sendParts(Proto.REN_RAGDOLL);
	}

	/** What the ragdoll capture changes on the player, to put it back afterwards. */
	private record Pose(float bodyRot, float bodyRotO, float headRot, float headRotO, float xRot, float xRotO, float yRot, float yRotO, int hurtTime,
		int deathTime, float attackAnim, float oAttackAnim, boolean swinging, float walkSpeedOld, float walkSpeed, float walkPosition, int tickCount) {
		static Pose of(AbstractClientPlayer p) {
			WalkAnimationStateAccessor walk = (WalkAnimationStateAccessor) p.walkAnimation;
			return new Pose(p.yBodyRot, p.yBodyRotO, p.yHeadRot, p.yHeadRotO, p.getXRot(), p.xRotO, p.getYRot(), p.yRotO, p.hurtTime, p.deathTime, p.attackAnim,
				p.oAttackAnim, p.swinging, walk.skycraft$speedOld(), walk.skycraft$speed(), walk.skycraft$position(), p.tickCount);
		}

		static void standing(AbstractClientPlayer p) {
			p.yBodyRot = p.yBodyRotO = p.yHeadRot = p.yHeadRotO = 0.0F;
			p.setXRot(0.0F);
			p.xRotO = 0.0F;
			p.setYRot(0.0F);
			p.yRotO = 0.0F;
			p.hurtTime = 0;
			p.deathTime = 0;
			p.attackAnim = p.oAttackAnim = 0.0F;
			p.swinging = false;
			p.tickCount = 0;
			WalkAnimationStateAccessor walk = (WalkAnimationStateAccessor) p.walkAnimation;
			walk.skycraft$setSpeedOld(0.0F);
			walk.skycraft$setSpeed(0.0F);
			walk.skycraft$setPosition(0.0F);
		}

		void restore(AbstractClientPlayer p) {
			p.yBodyRot = this.bodyRot;
			p.yBodyRotO = this.bodyRotO;
			p.yHeadRot = this.headRot;
			p.yHeadRotO = this.headRotO;
			p.setXRot(this.xRot);
			p.xRotO = this.xRotO;
			p.setYRot(this.yRot);
			p.yRotO = this.yRotO;
			p.hurtTime = this.hurtTime;
			p.deathTime = this.deathTime;
			p.attackAnim = this.attackAnim;
			p.oAttackAnim = this.oAttackAnim;
			p.swinging = this.swinging;
			p.tickCount = this.tickCount;
			WalkAnimationStateAccessor walk = (WalkAnimationStateAccessor) p.walkAnimation;
			walk.skycraft$setSpeedOld(this.walkSpeedOld);
			walk.skycraft$setSpeed(this.walkSpeed);
			walk.skycraft$setPosition(this.walkPosition);
		}
	}

	/** Which of Minecraft's six parts a point of the standing, +Z-facing body belongs to. */
	private static int partAt(float x, float y) {
		if (y >= 1.5F) {
			return Proto.PART_HEAD;
		}
		if (y >= 0.75F) {
			// The arms hang outside the body's 8-pixel width; its right side is -X facing +Z.
			return x < -0.255F ? Proto.PART_RIGHT_ARM : x > 0.255F ? Proto.PART_LEFT_ARM : Proto.PART_BODY;
		}
		return x < 0.0F ? Proto.PART_RIGHT_LEG : Proto.PART_LEFT_LEG;
	}

	/** Like send, with each batch split by the part its quads belong to (RenBatch flags bits 8-11). */
	private void sendParts(int message) {
		this.flushAll();
		record Group(Batch batch, int part, int[] quads, int count) {
		}
		List<Group> groups = new ArrayList<>();
		int vertices = 0;
		for (Batch b : this.batches.values()) {
			int quadCount = b.count / 4;
			if (quadCount == 0) {
				continue;
			}
			int[][] byPart = new int[7][quadCount];
			int[] counts = new int[7];
			for (int q = 0; q < quadCount; q++) {
				float cx = 0.0F, cy = 0.0F;
				for (int k = 0; k < 4; k++) {
					int o = (q * 4 + k) * 8;
					cx += Float.intBitsToFloat(b.data[o]);
					cy += Float.intBitsToFloat(b.data[o + 1]);
				}
				int part = partAt(cx * 0.25F, cy * 0.25F);
				byPart[part][counts[part]++] = q;
			}
			for (int part = 1; part < 7; part++) {
				if (counts[part] > 0) {
					groups.add(new Group(b, part, byPart[part], counts[part]));
					vertices += counts[part] * 6;
				}
			}
		}
		if (groups.isEmpty()) {
			return;
		}
		ByteBuffer header = ByteBuffer.allocate(8 + groups.size() * 16).order(ByteOrder.LITTLE_ENDIAN);
		header.putInt(groups.size()).putInt(vertices);
		ByteBuffer body = ByteBuffer.allocateDirect(vertices * Proto.REN_VERTEX_BYTES).order(ByteOrder.LITTLE_ENDIAN);
		int first = 0;
		for (Group g : groups) {
			int count = g.count() * 6;
			header.putInt(g.batch().texture).putInt(first).putInt(count).putInt((g.batch().translucent ? 1 : 0) | g.part() << 8);
			for (int i = 0; i < g.count(); i++) {
				g.batch().writeQuad(body, g.quads()[i]);
			}
			first += count;
		}
		header.flip();
		body.flip();
		if (SkyLink.tryWriteRender(message, header, body)) {
			this.shown = true;
		}
	}

	private void exportScene(Minecraft minecraft, SkyAtlas atlas, float partialTick) {
		this.atlas = atlas;
		var level = minecraft.level;
		var player = minecraft.player;
		if (level == null || player == null) {
			return;
		}
		Camera camera = minecraft.gameRenderer.getMainCamera();
		Vec3 cam = camera.getPosition();
		double[] origin = { Math.floor(cam.x), Math.floor(cam.y), Math.floor(cam.z) };
		this.begin();
		var dispatcher = minecraft.getEntityRenderDispatcher();
		dispatcher.prepare(level, camera, minecraft.crosshairPickEntity);
		PoseStack pose = new PoseStack();
		int entities = 0;
		for (Entity e : level.entitiesForRendering()) {
			if (e == player || e instanceof ItemEntity || e instanceof AbstractArrow || e instanceof ItemSupplier || e instanceof SkyrimActorEntity
				|| e.distanceToSqr(cam) > SCENE_RANGE * SCENE_RANGE || entities >= SCENE_MAX_ENTITIES) {
				continue;
			}
			entities++;
			try {
				double x = Mth.lerp(partialTick, e.xOld, e.getX()) - origin[0];
				double y = Mth.lerp(partialTick, e.yOld, e.getY()) - origin[1];
				double z = Mth.lerp(partialTick, e.zOld, e.getZ()) - origin[2];
				float yaw = Mth.lerp(partialTick, e.yRotO, e.getYRot());
				dispatcher.render(e, x, y, z, yaw, partialTick, pose, this, dispatcher.getPackedLightCoords(e, partialTick));
			} catch (RuntimeException ex) {
				if (!warnedEntity) {
					warnedEntity = true;
					SkyCraft.LOG.warn("SkyCraft: couldn't capture {} for Fallout", e, ex);
				}
			}
		}
		submitBlockEntities(minecraft, level, camera, cam, origin, partialTick, pose);
		// Particles are drawn relative to Minecraft's camera.
		this.flushAll();
		this.offX = (float) (cam.x - origin[0]);
		this.offY = (float) (cam.y - origin[1]);
		this.offZ = (float) (cam.z - origin[2]);
		try {
			this.submitParticles(minecraft, camera, partialTick);
		} catch (RuntimeException ex) {
			SkyCraft.LOG.warn("SkyCraft: couldn't capture particles for Fallout", ex);
		}
		this.flushAll();
		this.offX = this.offY = this.offZ = 0.0F;
		this.send(Proto.REN_SCENE, origin);
	}

	/**
	 * Blocks Minecraft draws with their own renderer rather than as block models, so they aren't in
	 * the section meshes: chests, beds, signs, banners, shulker boxes, heads, bells, lecterns, pots,
	 * campfire items, spawners, and blocks being pushed by a piston. Minecraft's world isn't drawn
	 * while linked, so they're taken straight from the loaded chunks around the camera.
	 */
	private void submitBlockEntities(Minecraft minecraft, net.minecraft.client.multiplayer.ClientLevel level, Camera camera, Vec3 cam, double[] origin,
		float partialTick, PoseStack pose) {
		var dispatcher = minecraft.getBlockEntityRenderDispatcher();
		dispatcher.prepare(level, camera, minecraft.hitResult);
		double range2 = BLOCK_ENTITY_RANGE * BLOCK_ENTITY_RANGE;
		int count = 0;
		int cx0 = (int) Math.floor((cam.x - BLOCK_ENTITY_RANGE) / 16.0), cx1 = (int) Math.floor((cam.x + BLOCK_ENTITY_RANGE) / 16.0);
		int cz0 = (int) Math.floor((cam.z - BLOCK_ENTITY_RANGE) / 16.0), cz1 = (int) Math.floor((cam.z + BLOCK_ENTITY_RANGE) / 16.0);
		for (int cx = cx0; cx <= cx1; cx++) {
			for (int cz = cz0; cz <= cz1; cz++) {
				var chunk = level.getChunkSource().getChunk(cx, cz, false);
				if (chunk == null) {
					continue;
				}
				for (var blockEntity : chunk.getBlockEntities().values()) {
					var pos = blockEntity.getBlockPos();
					if (blockEntity.isRemoved() || pos.distToCenterSqr(cam) > range2 || count >= SCENE_MAX_BLOCK_ENTITIES
						|| dispatcher.getRenderer(blockEntity) == null) {
						continue;
					}
					try {
						count++;
						pose.pushPose();
						pose.translate(pos.getX() - origin[0], pos.getY() - origin[1], pos.getZ() - origin[2]);
						dispatcher.render(blockEntity, partialTick, pose, this);
						pose.popPose();
					} catch (RuntimeException ex) {
						pose.setIdentity();
						if (!warnedBlockEntity) {
							warnedBlockEntity = true;
							SkyCraft.LOG.warn("SkyCraft: couldn't capture {} at {} for Fallout", blockEntity.getType(), pos, ex);
						}
					}
				}
			}
		}
	}

	/** Particles: smoke, explosions, block debris, crits, ... in their atlas (particles or blocks). */
	private void submitParticles(Minecraft minecraft, Camera camera, float partialTick) {
		Map<ParticleRenderType, java.util.Queue<Particle>> particles = ((ParticleEngineAccessor) minecraft.particleEngine).skycraft$particles();
		int count = 0;
		for (var entry : particles.entrySet()) {
			ParticleRenderType type = entry.getKey();
			Batch batch;
			if (type == ParticleRenderType.TERRAIN_SHEET) {
				batch = this.batch(0, UV_BLOCK_ATLAS, PARTICLE);
			} else if (type == ParticleRenderType.PARTICLE_SHEET_OPAQUE || type == ParticleRenderType.PARTICLE_SHEET_TRANSLUCENT
				|| type == ParticleRenderType.PARTICLE_SHEET_LIT) {
				int id = textureId(TextureAtlas.LOCATION_PARTICLES);
				if (id < 0) {
					continue;
				}
				batch = this.batch(id, UV_RAW, type.isTranslucent() ? PARTICLE_BLENDED : PARTICLE);
			} else {
				continue; // custom-drawn or invisible
			}
			for (Particle particle : entry.getValue()) {
				if (count++ >= MAX_PARTICLES) {
					break;
				}
				particle.render(batch.capture, camera, partialTick);
			}
			this.flushAll();
		}
	}

	private void begin() {
		for (Batch b : this.batches.values()) {
			b.clear();
		}
	}

	/** Ends every batch's last vertex (each batch has its own consumer: renderers may write to several in turn). */
	private void flushAll() {
		for (Batch b : this.batches.values()) {
			b.capture.flush();
		}
	}

	/** Header: [origin (3 doubles), scene only] batchCount, vertexCount, then batches; body: triangles. */
	private void send(int message, double @Nullable [] origin) {
		this.flushAll();
		List<Batch> used = new ArrayList<>();
		int vertices = 0;
		for (Batch b : this.batches.values()) {
			if (b.count >= 4) {
				used.add(b);
				vertices += b.count / 4 * 6;
			}
		}
		if (used.isEmpty()) {
			this.sendEmpty(message, origin != null);
			return;
		}
		ByteBuffer header = ByteBuffer.allocate((origin != null ? 24 : 0) + 8 + used.size() * 16).order(ByteOrder.LITTLE_ENDIAN);
		if (origin != null) {
			header.putDouble(origin[0]).putDouble(origin[1]).putDouble(origin[2]);
		}
		header.putInt(used.size()).putInt(vertices);
		ByteBuffer body = ByteBuffer.allocateDirect(vertices * Proto.REN_VERTEX_BYTES).order(ByteOrder.LITTLE_ENDIAN);
		int first = 0;
		for (Batch b : used) {
			int count = b.count / 4 * 6;
			header.putInt(b.texture).putInt(first).putInt(count).putInt(b.translucent ? 1 : 0);
			b.writeTriangles(body);
			first += count;
		}
		header.flip();
		body.flip();
		if (SkyLink.tryWriteRender(message, header, body)) {
			this.shown = true;
		}
	}

	private void sendEmpty(int message, boolean withOrigin) {
		if (!this.shown) {
			return;
		}
		ByteBuffer header = ByteBuffer.allocate((withOrigin ? 24 : 0) + 8).order(ByteOrder.LITTLE_ENDIAN);
		if (withOrigin) {
			header.putDouble(0).putDouble(0).putDouble(0);
		}
		header.putInt(0).putInt(0).flip();
		this.shown = !SkyLink.writeRender(message, header, null);
	}

	// ---- batches ---------------------------------------------------------------------------------

	private Batch batch(int texture, int uvMode, int flags) {
		long key = ((long) texture << 16) | ((long) uvMode << 8) | flags;
		return this.batches.computeIfAbsent(key, k -> new Batch(texture, uvMode, flags));
	}

	/** Triangles for one texture and one kind of surface. Vertices arrive as quads. */
	private final class Batch {
		final int texture;
		final int uvMode;
		final int flags;
		final boolean translucent;
		int[] data = new int[8 * 256];
		int count;
		final Capture capture = new Capture(this);

		Batch(int texture, int uvMode, int flags) {
			this.texture = texture;
			this.uvMode = uvMode;
			this.flags = flags;
			this.translucent = (flags & 2) != 0;
		}

		void clear() {
			this.count = 0;
			this.capture.pending = false;
		}

		void add(float x, float y, float z, float u, float v, int argb, int light, int overlay) {
			if ((this.count + 1) * 8 > this.data.length) {
				this.data = java.util.Arrays.copyOf(this.data, this.data.length * 2);
			}
			if (this.uvMode == UV_BLOCK_ATLAS) {
				u = AvatarExporter.this.atlas.blockU(u);
				v = AvatarExporter.this.atlas.blockV(v);
			}
			// Minecraft's red "hurt" flash is an overlay texture: tint instead. Its white flash
			// (lit TNT about to go) glows instead.
			if (((overlay >>> 16) & 0xFFFF) < 8) {
				int r = (argb >> 16) & 0xFF, g = (int) (((argb >> 8) & 0xFF) * 0.55F), b = (int) ((argb & 0xFF) * 0.55F);
				argb = (argb & 0xFF000000) | (r << 16) | (g << 8) | b;
			}
			if ((overlay & 0xFFFF) >= 8) {
				light = 0xF000F0;
			}
			int o = this.count * 8;
			this.data[o] = Float.floatToRawIntBits(x + AvatarExporter.this.offX);
			this.data[o + 1] = Float.floatToRawIntBits(y + AvatarExporter.this.offY);
			this.data[o + 2] = Float.floatToRawIntBits(z + AvatarExporter.this.offZ);
			this.data[o + 3] = Float.floatToRawIntBits(u);
			this.data[o + 4] = Float.floatToRawIntBits(v);
			this.data[o + 5] = argb;
			this.data[o + 6] = ((light >> 4) & 0xF) | (((light >> 20) & 0xF) << 8);
			this.data[o + 7] = this.flags;
			this.count++;
		}

		void writeTriangles(ByteBuffer out) {
			for (int q = 0; q + 4 <= this.count; q += 4) {
				this.writeQuad(out, q / 4);
			}
		}

		void writeQuad(ByteBuffer out, int quad) {
			for (int k : new int[] { 0, 1, 2, 0, 2, 3 }) {
				int o = (quad * 4 + k) * 8;
				out.putInt(this.data[o]).putInt(this.data[o + 1]).putInt(this.data[o + 2]).putInt(this.data[o + 3]).putInt(this.data[o + 4]);
				int argb = this.data[o + 5];
				out.put((byte) (argb >> 16)).put((byte) (argb >> 8)).put((byte) argb).put((byte) (argb >>> 24));
				out.putInt(this.data[o + 6]).putInt(this.data[o + 7]);
			}
		}
	}

	/** A VertexConsumer that records into one batch (renderers call addVertex + setters). */
	private static final class Capture implements VertexConsumer {
		private final Batch batch;
		boolean pending;
		private float x, y, z, u, v;
		private int color, light, overlay;

		Capture(Batch batch) {
			this.batch = batch;
		}

		void flush() {
			if (this.pending) {
				this.batch.add(this.x, this.y, this.z, this.u, this.v, this.color, this.light, this.overlay);
			}
			this.pending = false;
		}

		@Override
		public VertexConsumer addVertex(float x, float y, float z) {
			this.flush();
			this.x = x;
			this.y = y;
			this.z = z;
			this.color = -1;
			this.light = 0xF000F0;
			this.overlay = OverlayTexture.NO_OVERLAY;
			this.pending = true;
			return this;
		}

		@Override
		public VertexConsumer setColor(int r, int g, int b, int a) {
			this.color = (a & 0xFF) << 24 | (r & 0xFF) << 16 | (g & 0xFF) << 8 | (b & 0xFF);
			return this;
		}

		@Override
		public VertexConsumer setUv(float u, float v) {
			this.u = u;
			this.v = v;
			return this;
		}

		@Override
		public VertexConsumer setUv1(int u, int v) {
			this.overlay = (u & 0xFFFF) | (v << 16);
			return this;
		}

		@Override
		public VertexConsumer setUv2(int u, int v) {
			this.light = (u & 0xFFFF) | (v << 16);
			return this;
		}

		@Override
		public VertexConsumer setNormal(float x, float y, float z) {
			return this;
		}
	}

	/** Swallows what Fallout doesn't show (outlines, shadows, glint, name tags, lines). */
	private static final VertexConsumer DISCARD = new VertexConsumer() {
		@Override
		public VertexConsumer addVertex(float x, float y, float z) {
			return this;
		}

		@Override
		public VertexConsumer setColor(int r, int g, int b, int a) {
			return this;
		}

		@Override
		public VertexConsumer setUv(float u, float v) {
			return this;
		}

		@Override
		public VertexConsumer setUv1(int u, int v) {
			return this;
		}

		@Override
		public VertexConsumer setUv2(int u, int v) {
			return this;
		}

		@Override
		public VertexConsumer setNormal(float x, float y, float z) {
			return this;
		}
	};

	// ---- MultiBufferSource: every render type's vertices go to the batch for its texture ---------

	@Override
	public VertexConsumer getBuffer(RenderType renderType) {
		Batch batch = this.batchFor(renderType);
		return batch == null ? DISCARD : batch.capture;
	}

	/** The batch for a render type's texture, sending the texture to Fallout the first time. Null: can't show it. */
	private @Nullable Batch batchFor(RenderType renderType) {
		String name = ((RenderStateShardAccessor) renderType).skycraft$name();
		if (name.contains("glint") || name.contains("outline") || name.contains("shadow") || name.contains("text") || name.contains("lines")
			|| name.contains("leash") || name.contains("lightning") || name.contains("debug")) {
			return null;
		}
		ResourceLocation texture = textureOf(renderType);
		if (texture == null) {
			return null;
		}
		if (texture.equals(TextureAtlas.LOCATION_BLOCKS)) {
			return this.skipBlockAtlas ? null : this.batch(0, UV_BLOCK_ATLAS, SOLID);
		}
		int id = textureId(texture);
		return id < 0 ? null : this.batch(id, UV_RAW, SOLID);
	}

	/** The texture a render type samples (RenderType.CompositeRenderType's texture state), or null. */
	private static @Nullable ResourceLocation textureOf(RenderType renderType) {
		if (!(renderType instanceof CompositeRenderTypeAccessor composite)) {
			return null;
		}
		Object textureState = ((CompositeStateAccessor) (Object) composite.skycraft$state()).skycraft$textureState();
		if (!(textureState instanceof TextureStateShardInvoker shard)) {
			return null;
		}
		Optional<ResourceLocation> texture = shard.skycraft$cutoutTexture();
		return texture.orElse(null);
	}

	// ---- textures --------------------------------------------------------------------------------

	private static int textureId(ResourceLocation texture) {
		Integer known = TEXTURE_IDS.get(texture);
		if (known != null) {
			return known;
		}
		if (UNUSABLE.contains(texture)) {
			return -1;
		}
		retryLater = false;
		NativeImage image = readTexture(texture);
		if (image == null) {
			if (retryLater) {
				return -1; // not loaded yet (a skin still downloading): try again later
			}
			UNUSABLE.add(texture);
			SkyCraft.LOG.info("SkyCraft: texture {} isn't available to Fallout; what uses it is left out", texture);
			return -1;
		}
		int id = nextTextureId++;
		try (image) {
			int w = image.getWidth(), h = image.getHeight();
			ByteBuffer pixels = ByteBuffer.allocateDirect(w * h * 4).order(ByteOrder.LITTLE_ENDIAN);
			for (int y = 0; y < h; y++) {
				for (int x = 0; x < w; x++) {
					int abgr = image.getPixelRGBA(x, y); // red in the low byte
					pixels.put((byte) abgr).put((byte) (abgr >> 8)).put((byte) (abgr >> 16)).put((byte) (abgr >>> 24));
				}
			}
			pixels.flip();
			ByteBuffer header = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN).putInt(id).putInt(w).putInt(h).putInt(0).flip();
			if (!SkyLink.writeRender(Proto.REN_TEXTURE, header, pixels)) {
				nextTextureId--;
				return -1;
			}
		}
		TEXTURE_IDS.put(texture, id);
		SkyCraft.LOG.info("SkyCraft: sent texture {} to Fallout (id {})", texture, id);
		return id;
	}

	/** A texture's pixels: resource packs, a runtime texture (downloaded skins), or an atlas. Caller closes it. */
	private static @Nullable NativeImage readTexture(ResourceLocation texture) {
		Minecraft minecraft = Minecraft.getInstance();
		var resource = minecraft.getResourceManager().getResource(texture);
		if (resource.isPresent()) {
			try (var in = resource.get().open()) {
				return NativeImage.read(in);
			} catch (java.io.IOException e) {
				SkyCraft.LOG.warn("SkyCraft: couldn't read {}", texture, e);
				return null;
			}
		}
		AbstractTexture registered = minecraft.getTextureManager().getTexture(texture, null);
		if (registered instanceof DynamicTexture dynamic && dynamic.getPixels() != null) {
			NativeImage copy = new NativeImage(dynamic.getPixels().getWidth(), dynamic.getPixels().getHeight(), false);
			copy.copyFrom(dynamic.getPixels());
			return copy;
		}
		if (registered instanceof TextureAtlas atlas) {
			return SkyAtlas.image(atlas);
		}
		if (registered != null) {
			// Downloaded player skins and other textures that keep no copy of their pixels: read
			// them back from the GPU.
			return readBack(registered.getId());
		}
		return null;
	}

	private static boolean retryLater;

	private static @Nullable NativeImage readBack(int textureId) {
		if (textureId <= 0) {
			return null;
		}
		int previous = org.lwjgl.opengl.GL11.glGetInteger(org.lwjgl.opengl.GL11.GL_TEXTURE_BINDING_2D);
		try {
			org.lwjgl.opengl.GL11.glBindTexture(org.lwjgl.opengl.GL11.GL_TEXTURE_2D, textureId);
			int w = org.lwjgl.opengl.GL11.glGetTexLevelParameteri(org.lwjgl.opengl.GL11.GL_TEXTURE_2D, 0, org.lwjgl.opengl.GL11.GL_TEXTURE_WIDTH);
			int h = org.lwjgl.opengl.GL11.glGetTexLevelParameteri(org.lwjgl.opengl.GL11.GL_TEXTURE_2D, 0, org.lwjgl.opengl.GL11.GL_TEXTURE_HEIGHT);
			if (w <= 1 || h <= 1 || w > 4096 || h > 4096) {
				retryLater = w <= 1 || h <= 1;
				return null;
			}
			ByteBuffer rgba = ByteBuffer.allocateDirect(w * h * 4).order(ByteOrder.LITTLE_ENDIAN);
			org.lwjgl.opengl.GL11.glPixelStorei(org.lwjgl.opengl.GL11.GL_PACK_ALIGNMENT, 4);
			org.lwjgl.opengl.GL11.glGetTexImage(org.lwjgl.opengl.GL11.GL_TEXTURE_2D, 0, org.lwjgl.opengl.GL11.GL_RGBA, org.lwjgl.opengl.GL11.GL_UNSIGNED_BYTE, rgba);
			NativeImage image = new NativeImage(w, h, false);
			for (int y = 0; y < h; y++) {
				for (int x = 0; x < w; x++) {
					image.setPixelRGBA(x, y, rgba.getInt((y * w + x) * 4)); // RGBA bytes = ABGR int
				}
			}
			return image;
		} catch (RuntimeException e) {
			SkyCraft.LOG.warn("SkyCraft: couldn't read texture {} back from the GPU", textureId, e);
			return null;
		} finally {
			org.lwjgl.opengl.GL11.glBindTexture(org.lwjgl.opengl.GL11.GL_TEXTURE_2D, previous);
		}
	}
}
