package dev.skycraft.client.render;

import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.blaze3d.vertex.VertexConsumer;
import dev.skycraft.SkyCraft;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import dev.skycraft.world.SkyClip;
import it.unimi.dsi.fastutil.longs.LongLinkedOpenHashSet;
import it.unimi.dsi.fastutil.longs.LongOpenHashSet;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.client.renderer.ItemBlockRenderTypes;
import net.minecraft.client.renderer.RenderType;
import net.minecraft.client.renderer.block.BlockRenderDispatcher;
import net.minecraft.client.renderer.block.ModelBlockRenderer;
import net.minecraft.client.renderer.block.model.BakedQuad;
import net.minecraft.client.renderer.texture.TextureAtlasSprite;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.SectionPos;
import net.minecraft.server.level.BlockDestructionProgress;
import net.minecraft.util.Mth;
import net.minecraft.util.RandomSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.item.ItemEntity;
import net.minecraft.world.entity.projectile.AbstractArrow;
import net.minecraft.world.entity.projectile.Arrow;
import net.minecraft.world.entity.projectile.ItemSupplier;
import net.minecraft.world.entity.projectile.SpectralArrow;
import net.minecraft.world.entity.projectile.ThrownTrident;
import net.minecraft.world.item.BlockItem;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.RenderShape;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.LevelChunkSection;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;
import net.minecraft.world.phys.shapes.VoxelShape;
import net.neoforged.neoforge.client.model.data.ModelData;

/**
 * Minecraft 1.21.1: ships what Minecraft would draw in the world to Fallout, which draws it in its
 * own frame (locked to the world, hidden behind Fallout geometry). Same messages as the 26.x version:
 * block and fluid meshes built by Minecraft's own block renderer (models, tint, smooth lighting),
 * the texture atlas, arrows and dropped items, the targeted-block outline, plus the avatar and scene
 * captures (AvatarExporter). 1.21.1's block renderer writes into a VertexConsumer, so the mesh
 * builder is one. Render thread only.
 */
public final class WorldExporter {
	private static final int SECTIONS_PER_FRAME = 12;
	private static final double ENTITY_RANGE = 96.0;

	private static final LongLinkedOpenHashSet DIRTY = new LongLinkedOpenHashSet();
	private static final LongOpenHashSet SENT = new LongOpenHashSet(); // sections Fallout holds a mesh for
	private static final LongOpenHashSet LIT = new LongOpenHashSet(); // sections Fallout holds lights for
	private static final LongOpenHashSet SOLID = new LongOpenHashSet(); // sections Fallout holds NPC collision for
	private static final long[] SOLID_BITS = new long[64]; // 4096 blocks: bit x + 16z + 256y
	private static final LongOpenHashSet DUG = new LongOpenHashSet(); // sections Fallout holds dug cells for
	private static final ByteBuffer LIGHTS = ByteBuffer.allocate(16 * 16 * 16 * 8).order(ByteOrder.LITTLE_ENDIAN);
	private static int sentGeneration = Integer.MIN_VALUE;
	private static int meshesSent;
	private static ClientLevel sentLevel;
	private static SkyAtlas atlas;
	private static final MeshBuilder MESH = new MeshBuilder();
	private static final PoseStack POSE = new PoseStack();
	private static final List<SkyLink.WorldEntity> ENTITIES = new ArrayList<>();
	private static final java.util.Map<Item, float[]> ICONS = new java.util.HashMap<>();
	private static final java.util.Map<BlockState, float[]> CUBE_FACES = new java.util.HashMap<>();
	private static final RandomSource RANDOM = RandomSource.create();

	private WorldExporter() {
	}

	public static void markDirty(int sx, int sy, int sz) {
		synchronized (DIRTY) {
			DIRTY.add(SectionPos.asLong(sx, sy, sz));
		}
	}

	/** A block the player just placed or broke: re-mesh ahead of everything else. */
	public static void markDirtyNow(int sx, int sy, int sz) {
		synchronized (DIRTY) {
			DIRTY.addAndMoveToFirst(SectionPos.asLong(sx, sy, sz));
		}
	}

	public static void frame(Minecraft minecraft, float partialTick) {
		ClientLevel level = minecraft.level;
		if (level == null || minecraft.player == null || !SkyLink.active()) {
			return;
		}
		if (sentGeneration != SkyLink.generation() || sentLevel != level || atlas == null || atlas.stale(minecraft)) {
			resendEverything(minecraft, level);
		}
		meshDirtySections(minecraft, level);
		// Animated textures (water, lava, fire, ...): the frame for this game tick.
		atlas.animate(level.getGameTime(), region -> {
			ByteBuffer header = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN).putInt(region.x()).putInt(region.y()).putInt(region.w()).putInt(region.h()).flip();
			return SkyLink.tryWriteRender(Proto.REN_ATLAS_REGION, header, region.pixels());
		});
		exportEntities(minecraft, level, partialTick);
		AvatarExporter.frame(minecraft, atlas, partialTick);
	}

	private static void resendEverything(Minecraft minecraft, ClientLevel level) {
		sentGeneration = SkyLink.generation();
		sentLevel = level;
		atlas = SkyAtlas.build(minecraft);
		SkyCraft.LOG.info("SkyCraft: {} animated textures (water, lava, fire, ...) will play in Fallout", atlas.animatedSprites());
		AvatarExporter.reset();
		ICONS.clear();
		CUBE_FACES.clear();
		SkyLink.writeRender(Proto.REN_CLEAR_ALL, ByteBuffer.allocate(0), null);
		boolean ok = SkyLink.writeAtlas(atlas.width, atlas.height, atlas.pixels);
		SkyCraft.LOG.info("SkyCraft: sent {}x{} texture atlas to Fallout ({})", atlas.width, atlas.height, ok ? "ok" : "FAILED");
		SENT.clear();
		LIT.clear();
		SOLID.clear();
		DUG.clear();
		dev.skycraft.client.SkyDigClient.resendAll();
		// Everything already loaded needs meshing again; later chunk loads mark themselves dirty.
		int radius = minecraft.options.getEffectiveRenderDistance() + 1;
		int pcx = SectionPos.blockToSectionCoord(minecraft.player.getBlockX()), pcz = SectionPos.blockToSectionCoord(minecraft.player.getBlockZ());
		for (int cx = pcx - radius; cx <= pcx + radius; cx++) {
			for (int cz = pcz - radius; cz <= pcz + radius; cz++) {
				LevelChunk chunk = level.getChunkSource().getChunk(cx, cz, ChunkStatus.FULL, false);
				if (chunk == null) {
					continue;
				}
				LevelChunkSection[] sections = chunk.getSections();
				for (int i = 0; i < sections.length; i++) {
					if (!sections[i].hasOnlyAir()) {
						markDirty(cx, chunk.getSectionYFromSectionIndex(i), cz);
					}
				}
			}
		}
	}

	private static void meshDirtySections(Minecraft minecraft, ClientLevel level) {
		// Chunk loads and light updates dirty thousands of all-air sections; those cost a lookup.
		// Real meshing is limited per frame.
		long deadline = System.nanoTime() + 3_000_000L;
		int meshed = 0;
		ModelBlockRenderer.enableCaching();
		try {
			while (meshed < SECTIONS_PER_FRAME && System.nanoTime() < deadline) {
				long key;
				synchronized (DIRTY) {
					if (DIRTY.isEmpty()) {
						return;
					}
					key = DIRTY.removeFirstLong();
				}
				if (meshSection(minecraft, level, key)) {
					meshed++;
				}
			}
		} finally {
			ModelBlockRenderer.clearCache();
		}
	}

	private static LevelChunkSection sectionAt(ClientLevel level, LevelChunk chunk, int sy) {
		int index = level.getSectionIndexFromSectionY(sy);
		return index >= 0 && index < chunk.getSections().length ? chunk.getSections()[index] : null;
	}

	/** Returns true if real meshing work was done. */
	private static boolean meshSection(Minecraft minecraft, ClientLevel level, long key) {
		int sx = SectionPos.x(key), sy = SectionPos.y(key), sz = SectionPos.z(key);
		LevelChunk chunk = level.getChunkSource().getChunk(sx, sz, ChunkStatus.FULL, false);
		if (chunk == null) {
			return false; // unloaded: Fallout keeps what it has
		}
		LevelChunkSection section = sectionAt(level, chunk, sy);
		boolean empty = section == null || section.hasOnlyAir();
		long[] dug = dev.skycraft.client.SkyDigClient.dugBits(chunk, sy);
		int dugCount = 0;
		if (dug != null) {
			for (long word : dug) {
				dugCount += Long.bitCount(word);
			}
		}
		if (empty && !SENT.contains(key) && !LIT.contains(key) && !SOLID.contains(key) && dugCount == 0 && !DUG.contains(key)) {
			return false; // nothing there and nothing to remove
		}
		MESH.reset();
		MESH.level = level;
		LIGHTS.clear();
		int lightCount = 0;
		java.util.Arrays.fill(SOLID_BITS, 0L);
		int solidCount = 0;
		if (!empty) {
			BlockRenderDispatcher blocks = minecraft.getBlockRenderer();
			BlockPos origin = SectionPos.of(sx, sy, sz).origin();
			BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
			for (int y = 0; y < 16; y++) {
				for (int z = 0; z < 16; z++) {
					for (int x = 0; x < 16; x++) {
						pos.set(origin.getX() + x, origin.getY() + y, origin.getZ() + z);
						BlockState state = chunk.getBlockState(pos);
						if (state.isAir()) {
							continue;
						}
						// Blocks Fallout's NPCs can't walk through (anything with a collision shape).
						if (!state.getCollisionShape(level, pos).isEmpty()) {
							int bit = x + 16 * z + 256 * y;
							SOLID_BITS[bit >> 6] |= 1L << (bit & 63);
							solidCount++;
						}
						// Light-emitting blocks (torches, lava, glowstone, ...) light Fallout's world too.
						int emission = state.getLightEmission(level, pos);
						if (emission > 0) {
							LIGHTS.put((byte) x).put((byte) y).put((byte) z).put((byte) emission).putInt(BlockLightColors.of(state));
							lightCount++;
						}
						FluidState fluid = state.getFluidState();
						if (!fluid.isEmpty()) {
							// Fallout ground in the cell: the fluid is drawn in the space above it.
							MESH.fluidGround = dev.skycraft.world.SkyCollision.groundTop(pos);
							MESH.fluidBaseY = y;
							RenderType layer = ItemBlockRenderTypes.getRenderLayer(fluid);
							MESH.beginFluid(layer == RenderType.translucent() || layer == RenderType.tripwire());
							blocks.renderLiquid(pos, level, MESH, state, fluid);
							MESH.endFluid();
							MESH.fluidGround = 0.0F;
						}
						if (state.getRenderShape() == RenderShape.MODEL) {
							BlockPos at = pos.immutable();
							var model = blocks.getBlockModel(state);
							ModelData data = model.getModelData(level, at, state, level.getModelData(at));
							RANDOM.setSeed(state.getSeed(at));
							for (RenderType layer : model.getRenderTypes(state, RANDOM, data)) {
								MESH.translucent = layer == RenderType.translucent() || layer == RenderType.tripwire();
								POSE.pushPose();
								POSE.translate(x, y, z);
								RANDOM.setSeed(state.getSeed(at));
								blocks.renderBatched(state, at, level, POSE, MESH, true, RANDOM, data, layer);
								POSE.popPose();
							}
						}
					}
				}
			}
		}
		// Holes dug into Fallout's ground: Minecraft walls where its surface still runs above them.
		var digLookup = dev.skycraft.world.SkyDig.clientDug;
		if (dugCount > 0 && digLookup != null) {
			DigWalls.add(level, sx, sy, sz, dug, digLookup, st -> cubeFaces(minecraft, st), MESH::wall);
		}
		MESH.level = null;
		if (MESH.vertexCount() == 0 && !SENT.contains(key) && lightCount == 0 && !LIT.contains(key) && solidCount == 0 && !SOLID.contains(key) && dugCount == 0
			&& !DUG.contains(key)) {
			return true;
		}
		ByteBuffer header = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN).putInt(sx).putInt(sy).putInt(sz).putInt(MESH.vertexCount()).flip();
		if (SkyLink.writeRender(Proto.REN_SECTION, header, MESH.bytes())) {
			if (MESH.vertexCount() > 0) {
				SENT.add(key);
			} else {
				SENT.remove(key);
			}
			if (lightCount > 0 || LIT.contains(key)) {
				ByteBuffer lightHeader = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN).putInt(sx).putInt(sy).putInt(sz).putInt(lightCount).flip();
				if (!SkyLink.writeRender(Proto.REN_LIGHTS, lightHeader, LIGHTS.flip())) {
					markDirty(sx, sy, sz); // ring full; send both again later
				} else if (lightCount > 0) {
					LIT.add(key);
				} else {
					LIT.remove(key);
				}
			}
			if (solidCount > 0 || SOLID.contains(key)) {
				ByteBuffer solidHeader = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN).putInt(sx).putInt(sy).putInt(sz).putInt(solidCount).flip();
				ByteBuffer bits = ByteBuffer.allocate(solidCount > 0 ? 512 : 0).order(ByteOrder.LITTLE_ENDIAN);
				if (solidCount > 0) {
					for (long word : SOLID_BITS) {
						bits.putLong(word);
					}
				}
				if (!SkyLink.writeRender(Proto.REN_SOLIDS, solidHeader, bits.flip())) {
					markDirty(sx, sy, sz);
				} else if (solidCount > 0) {
					SOLID.add(key);
				} else {
					SOLID.remove(key);
				}
			}
			// Cells dug out of Fallout's world: its geometry there goes.
			if (dugCount > 0 || DUG.contains(key)) {
				ByteBuffer dugHeader = ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN).putInt(sx).putInt(sy).putInt(sz).putInt(dugCount)
					.putInt(dev.skycraft.client.SkyDigClient.world()).putInt(0).flip();
				ByteBuffer bits = ByteBuffer.allocate(dugCount > 0 ? 512 : 0).order(ByteOrder.LITTLE_ENDIAN);
				if (dugCount > 0) {
					for (long word : dug) {
						bits.putLong(word);
					}
				}
				if (!SkyLink.writeRender(Proto.REN_DUG, dugHeader, bits.flip())) {
					markDirty(sx, sy, sz);
				} else if (dugCount > 0) {
					DUG.add(key);
				} else {
					DUG.remove(key);
				}
			}
			if (++meshesSent <= 10 || meshesSent % 200 == 0) {
				SkyCraft.LOG.info("SkyCraft: block mesh for section {} {} {}: {} vertices ({} sections in Fallout)", sx, sy, sz, MESH.vertexCount(), SENT.size());
			}
		} else {
			markDirty(sx, sy, sz); // ring full; try again later
		}
		return true;
	}

	private static void exportEntities(Minecraft minecraft, ClientLevel level, float partialTick) {
		ENTITIES.clear();
		Vec3 eye = minecraft.player.getEyePosition(partialTick);
		boolean detached = minecraft.gameRenderer.getMainCamera().isDetached();
		for (Entity e : level.entitiesForRendering()) {
			if (ENTITIES.size() >= Proto.MAX_WORLD_ENTITIES || e.distanceToSqr(eye) > ENTITY_RANGE * ENTITY_RANGE) {
				continue;
			}
			Vec3 p = e.getPosition(partialTick);
			if (e instanceof AbstractArrow arrow) {
				boolean trident = arrow instanceof ThrownTrident;
				int kind = trident ? Proto.WE_TRIDENT : Proto.WE_ARROW;
				float yaw = Mth.rotLerp(partialTick, arrow.yRotO, arrow.getYRot());
				float pitch = Mth.lerp(partialTick, arrow.xRotO, arrow.getXRot());
				// Arrows use Minecraft's arrow model and entity texture; tridents their item icon.
				float[] uv = trident ? ICONS.computeIfAbsent(Items.TRIDENT, i -> iconUv(minecraft, level, new ItemStack(i)))
					: atlas.arrowUv(arrow instanceof SpectralArrow ? 2 : arrow instanceof Arrow tippable && tippable.getColor() > 0 ? 1 : 0);
				if (uv != null) {
					ENTITIES.add(new SkyLink.WorldEntity(kind, e.getId(), (float) p.x, (float) p.y, (float) p.z, yaw, pitch, 1.0F, null, uv, 0));
				}
			} else if (e instanceof ItemEntity item) {
				float bob = Mth.sin((item.getAge() + partialTick) / 10.0F + item.bobOffs) * 0.1F + 0.1F;
				float spin = item.getSpin(partialTick) * Mth.RAD_TO_DEG;
				addItem(minecraft, level, e, item.getItem(), p.add(0, bob, 0), spin);
			} else if (e instanceof ItemSupplier supplier) {
				addItem(minecraft, level, e, supplier.getItem(), p.add(0, e.getBbHeight() * 0.5 - 0.25, 0), 0.0F);
			} else if (e instanceof net.minecraft.world.entity.LivingEntity && !(e instanceof dev.skycraft.combat.SkyrimActorEntity) && !e.isInvisible()
				&& (e != minecraft.player || detached)) {
				// Players and mobs: Fallout darkens the ground softly under their feet.
				ENTITIES.add(new SkyLink.WorldEntity(Proto.WE_SHADOW, e.getId(), (float) p.x, (float) p.y, (float) p.z, 0.0F, 0.0F, e.getBbWidth(), null, null, 0));
			}
		}
		addCracks(minecraft);
		SkyLink.writeWorldEntities(ENTITIES, selection(minecraft, level));
	}

	/**
	 * A dropped item the way Minecraft shows it: blocks as small spinning cubes with their own face
	 * textures, everything else as its icon. {@code p} is the item's resting point (bottom).
	 */
	private static void addItem(Minecraft minecraft, ClientLevel level, Entity e, ItemStack stack, Vec3 p, float yaw) {
		if (stack.getItem() instanceof BlockItem blockItem) {
			BlockState state = blockItem.getBlock().defaultBlockState();
			if (state.getRenderShape() == RenderShape.MODEL
				&& Block.isShapeFullBlock(state.getShape(net.minecraft.world.level.EmptyBlockGetter.INSTANCE, BlockPos.ZERO))) {
				float[] faces = cubeFaces(minecraft, state);
				if (faces != null) {
					float size = 0.25F;
					int tint = cubeTint(minecraft, state);
					ENTITIES.add(new SkyLink.WorldEntity(
						Proto.WE_BLOCK, e.getId(), (float) p.x, (float) (p.y + size * 0.5 + 0.02), (float) p.z, yaw, 0.0F, size, null, faces, tint
					));
					return;
				}
			}
		}
		float[] uv = iconUv(minecraft, level, stack);
		if (uv != null) {
			ENTITIES.add(new SkyLink.WorldEntity(Proto.WE_ITEM, e.getId(), (float) p.x, (float) p.y + 0.25F, (float) p.z, yaw, 0.0F, 0.5F, null, uv, 0));
		}
	}

	/** Side, top and bottom atlas rects of a full-cube block's model, cached per block state. */
	private static float[] cubeFaces(Minecraft minecraft, BlockState state) {
		return CUBE_FACES.computeIfAbsent(state, s -> {
			var model = minecraft.getBlockRenderer().getBlockModel(s);
			float[] out = new float[12];
			Direction[] dirs = { Direction.NORTH, Direction.UP, Direction.DOWN };
			for (int f = 0; f < 3; f++) {
				RANDOM.setSeed(42);
				List<BakedQuad> quads = model.getQuads(s, dirs[f], RANDOM, ModelData.EMPTY, null);
				if (quads.isEmpty()) {
					return null;
				}
				System.arraycopy(atlas.rect(quads.get(0).getSprite()), 0, out, f * 4, 4);
			}
			return out;
		});
	}

	/** Grass and leaves are grey in the atlas; Minecraft tints them. RGBA8, 0 for none. */
	private static int cubeTint(Minecraft minecraft, BlockState state) {
		int argb = minecraft.getBlockColors().getColor(state, null, null, 0);
		if (argb == -1) {
			return 0;
		}
		return 0xFF000000 | (argb & 0xFF) << 16 | (argb & 0xFF00) | (argb >> 16 & 0xFF);
	}

	/** Cracks over blocks being mined (ours and anyone else's). */
	private static void addCracks(Minecraft minecraft) {
		ClientLevel level = minecraft.level;
		for (BlockDestructionProgress progress : ((dev.skycraft.client.mixin.LevelRendererAccessor) minecraft.levelRenderer).skycraft$destroyingBlocks().values()) {
			int stage = progress.getProgress();
			if (stage < 0 || stage > 9 || ENTITIES.size() >= Proto.MAX_WORLD_ENTITIES) {
				continue;
			}
			BlockPos pos = progress.getPos();
			VoxelShape shape = level.getBlockState(pos).getShape(level, pos);
			if (shape.isEmpty()) {
				continue;
			}
			AABB box = shape.bounds().move(pos).inflate(0.004);
			ENTITIES.add(new SkyLink.WorldEntity(
				Proto.WE_CRACK, pos.hashCode(), (float) box.minX, (float) box.minY, (float) box.minZ, 0.0F, 0.0F, 1.0F,
				new float[] { (float) box.getXsize(), (float) box.getYsize(), (float) box.getZsize() }, atlas.crackUv(stage), 0
			));
		}
	}

	/** The item's icon in the combined atlas {u0, v0, u1, v1}, or null. */
	private static float[] iconUv(Minecraft minecraft, ClientLevel level, ItemStack stack) {
		var model = minecraft.getItemRenderer().getModel(stack, level, null, 0);
		TextureAtlasSprite sprite = model == null ? null : model.getParticleIcon(ModelData.EMPTY);
		return sprite == null ? null : atlas.rect(sprite);
	}

	/** Outline for Fallout to draw: the targeted block, or where a held block would go on Fallout ground. */
	private static float[] selection(Minecraft minecraft, ClientLevel level) {
		HitResult hit = minecraft.hitResult;
		if (!(hit instanceof BlockHitResult blockHit) || hit.getType() != HitResult.Type.BLOCK || minecraft.screen != null) {
			return null;
		}
		BlockPos pos = blockHit.getBlockPos();
		if (blockHit instanceof SkyClip.SkyrimHitResult) {
			if (!(minecraft.player.getMainHandItem().getItem() instanceof BlockItem)) {
				return null;
			}
			return new float[] { pos.getX(), pos.getY(), pos.getZ(), pos.getX() + 1, pos.getY() + 1, pos.getZ() + 1 };
		}
		VoxelShape shape = level.getBlockState(pos).getShape(level, pos);
		if (shape.isEmpty()) {
			return null;
		}
		AABB box = shape.bounds().move(pos);
		return new float[] { (float) box.minX, (float) box.minY, (float) box.minZ, (float) box.maxX, (float) box.maxY, (float) box.maxZ };
	}

	/**
	 * Collects Minecraft's block quads (and fluid vertices) as triangles in the RenVertex layout:
	 * section-relative position, combined-atlas UV, RGBA colour (tint and ambient occlusion),
	 * block/sky light. Block models arrive as whole quads (putBulkData), fluids vertex by vertex.
	 */
	private static final class MeshBuilder implements VertexConsumer {
		private ByteBuffer buf = ByteBuffer.allocateDirect(1 << 20).order(ByteOrder.LITTLE_ENDIAN);
		private int vertices;
		ClientLevel level;
		boolean translucent;

		// the block quad being written (putBulkData)
		private BakedQuad quad;
		private int quadFlags;
		private float quadShade;
		private final float[] bq = new float[4 * 8];
		private int bqCount;

		// fluid quad assembly
		private boolean fluid;
		private boolean fluidTranslucent;
		private final float[] fq = new float[4 * 8];
		private int fqCount;
		private boolean pending;
		private float px, py, pz, pu, pv;
		private int pColor = -1, pLight;

		void reset() {
			this.buf.clear();
			this.vertices = 0;
			this.fqCount = 0;
			this.bqCount = 0;
			this.pending = false;
			this.quad = null;
		}

		int vertexCount() {
			return this.vertices;
		}

		ByteBuffer bytes() {
			return this.buf.duplicate().flip();
		}

		private void ensure(int bytes) {
			if (this.buf.remaining() < bytes) {
				ByteBuffer bigger = ByteBuffer.allocateDirect(Math.max(this.buf.capacity() * 2, this.buf.position() + bytes)).order(ByteOrder.LITTLE_ENDIAN);
				this.buf.flip();
				bigger.put(this.buf);
				this.buf = bigger;
			}
		}

		private void vertex(float x, float y, float z, float u, float v, int argb, int light, int flags) {
			this.buf.putFloat(x).putFloat(y).putFloat(z).putFloat(u).putFloat(v);
			this.buf.put((byte) (argb >> 16)).put((byte) (argb >> 8)).put((byte) argb).put((byte) (argb >>> 24));
			int block = (light >> 4) & 0xF;
			int sky = (light >> 20) & 0xF;
			this.buf.putInt(block | (sky << 8));
			this.buf.putInt(flags);
			this.vertices++;
		}

		/**
		 * Vertex flags: cutout or translucent, plus the face normal (Direction ordinal + 1) that
		 * Fallout lights the face with. 0 leaves it without a normal (plants and other quads Minecraft
		 * doesn't shade by direction).
		 */
		private static int flags(boolean translucent, Direction normal) {
			return (translucent ? 2 : 1) | (normal == null ? 0 : (normal.ordinal() + 1) << 4);
		}

		/** Takes Minecraft's fixed face brightness back out of a colour, leaving tint and ambient occlusion. */
		private static int unshade(int argb, float shade) {
			if (shade >= 0.999F || shade <= 0.0F) {
				return argb;
			}
			int r = Math.min(255, Math.round(((argb >> 16) & 0xFF) / shade));
			int g = Math.min(255, Math.round(((argb >> 8) & 0xFF) / shade));
			int b = Math.min(255, Math.round((argb & 0xFF) / shade));
			return (argb & 0xFF000000) | (r << 16) | (g << 8) | b;
		}

		private float shade(Direction face) {
			return this.level == null ? 1.0F : this.level.getShade(face, true);
		}

		/** A dug hole's wall (DigWalls): an untinted opaque quad. */
		void wall(float[] xyz, float[] uv, int light, Direction normal) {
			this.ensure(6 * Proto.REN_VERTEX_BYTES);
			int flags = flags(false, normal);
			for (int k : new int[] { 0, 1, 2, 0, 2, 3 }) {
				this.vertex(xyz[k * 3], xyz[k * 3 + 1], xyz[k * 3 + 2], uv[k * 2], uv[k * 2 + 1], 0xFFFFFFFF, light, flags);
			}
		}

		// ---- block quads: Minecraft's model renderer hands over whole quads ----
		@Override
		public void putBulkData(PoseStack.Pose pose, BakedQuad quad, float[] brightness, float red, float green, float blue, float alpha, int[] lightmap,
			int overlay, boolean readExistingColor) {
			this.quad = quad;
			this.bqCount = 0;
			// Plants and the like aren't shaded by direction: no normal for them.
			Direction normal = quad.isShade() ? quad.getDirection() : null;
			this.quadShade = this.level == null ? 1.0F : this.level.getShade(quad.getDirection(), quad.isShade());
			this.quadFlags = flags(this.translucent, normal);
			VertexConsumer.super.putBulkData(pose, quad, brightness, red, green, blue, alpha, lightmap, overlay, readExistingColor);
			this.quad = null;
		}

		@Override
		public void addVertex(float x, float y, float z, int color, float u, float v, int overlay, int light, float nx, float ny, float nz) {
			if (this.quad == null) {
				// Not from a block model (shouldn't happen): take it vertex by vertex.
				this.addVertex(x, y, z);
				this.setColor(color);
				this.setUv(u, v);
				this.setUv2(light & 0xFFFF, light >> 16 & 0xFFFF);
				return;
			}
			int o = this.bqCount * 8;
			this.bq[o] = x;
			this.bq[o + 1] = y;
			this.bq[o + 2] = z;
			this.bq[o + 3] = atlas.blockU(u);
			this.bq[o + 4] = atlas.blockV(v);
			this.bq[o + 5] = Float.intBitsToFloat(unshade(color, this.quadShade));
			this.bq[o + 6] = Float.intBitsToFloat(light);
			if (++this.bqCount == 4) {
				this.bqCount = 0;
				this.ensure(6 * Proto.REN_VERTEX_BYTES);
				for (int k : new int[] { 0, 1, 2, 0, 2, 3 }) {
					int b = k * 8;
					this.vertex(this.bq[b], this.bq[b + 1], this.bq[b + 2], this.bq[b + 3], this.bq[b + 4], Float.floatToRawIntBits(this.bq[b + 5]),
						Float.floatToRawIntBits(this.bq[b + 6]), this.quadFlags);
				}
			}
		}

		// ---- fluids: vertex by vertex (addVertex, setColor, setUv, setLight, setNormal) ----
		// Fallout ground's height in the fluid's cell (0..1) and the cell's section-relative y: a
		// Minecraft fluid level counts from the cell's floor, so on Fallout ground partway up the cell
		// the fluid is squeezed into the space above it (thin edges stay visible on the ground).
		float fluidGround;
		int fluidBaseY;

		void beginFluid(boolean translucent) {
			this.fluid = true;
			this.fluidTranslucent = translucent;
			this.fqCount = 0;
			this.pending = false;
		}

		void endFluid() {
			this.flushFluidVertex();
			this.fluid = false;
			this.fqCount = 0;
		}

		private void flushFluidVertex() {
			if (!this.pending) {
				return;
			}
			this.pending = false;
			float y = this.py;
			if (this.fluidGround > 0.0F) {
				float t = Math.max(0.0F, Math.min(1.0F, y - this.fluidBaseY));
				y = this.fluidBaseY + this.fluidGround + t * (1.0F - this.fluidGround);
			}
			int o = this.fqCount * 8;
			this.fq[o] = this.px;
			this.fq[o + 1] = y;
			this.fq[o + 2] = this.pz;
			this.fq[o + 3] = atlas.blockU(this.pu);
			this.fq[o + 4] = atlas.blockV(this.pv);
			this.fq[o + 5] = Float.intBitsToFloat(this.pColor);
			this.fq[o + 6] = Float.intBitsToFloat(this.pLight);
			if (++this.fqCount == 4) {
				this.fqCount = 0;
				this.ensure(6 * Proto.REN_VERTEX_BYTES);
				// The face's normal from its corners (Minecraft's fluid vertices all say "up").
				float ax = this.fq[8] - this.fq[0], ay = this.fq[9] - this.fq[1], az = this.fq[10] - this.fq[2];
				float bx = this.fq[16] - this.fq[0], by = this.fq[17] - this.fq[1], bz = this.fq[18] - this.fq[2];
				float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
				Direction normal = nx == 0 && ny == 0 && nz == 0 ? null : Direction.getNearest(nx, ny, nz);
				// Fluid faces are shaded like block faces (up, down, or the side's brightness).
				float shade = normal == null ? 1.0F
					: normal.getAxis() == Direction.Axis.Y ? this.shade(normal) : this.shade(Direction.UP) * this.shade(normal);
				int flags = flags(this.fluidTranslucent, normal);
				for (int k : new int[] { 0, 1, 2, 0, 2, 3 }) {
					int b = k * 8;
					this.vertex(this.fq[b], this.fq[b + 1], this.fq[b + 2], this.fq[b + 3], this.fq[b + 4],
						unshade(Float.floatToRawIntBits(this.fq[b + 5]), shade), Float.floatToRawIntBits(this.fq[b + 6]), flags);
				}
			}
		}

		@Override
		public VertexConsumer addVertex(float x, float y, float z) {
			this.flushFluidVertex();
			this.px = x;
			this.py = y;
			this.pz = z;
			this.pu = this.pv = 0.0F;
			this.pColor = -1;
			this.pLight = 0xF000F0;
			this.pending = true;
			return this;
		}

		@Override
		public VertexConsumer setColor(int r, int g, int b, int a) {
			this.pColor = (a & 0xFF) << 24 | (r & 0xFF) << 16 | (g & 0xFF) << 8 | (b & 0xFF);
			return this;
		}

		@Override
		public VertexConsumer setUv(float u, float v) {
			this.pu = u;
			this.pv = v;
			return this;
		}

		@Override
		public VertexConsumer setUv1(int u, int v) {
			return this;
		}

		@Override
		public VertexConsumer setUv2(int u, int v) {
			this.pLight = (u & 0xFFFF) | (v << 16);
			return this;
		}

		@Override
		public VertexConsumer setNormal(float x, float y, float z) {
			return this;
		}
	}
}
