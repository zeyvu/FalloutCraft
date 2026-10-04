package dev.skycraft.client;

import com.mojang.blaze3d.pipeline.RenderTarget;
import dev.skycraft.SkyCraft;
import dev.skycraft.link.Proto;
import dev.skycraft.link.Shm;
import dev.skycraft.link.SkyLink;
import java.nio.ByteBuffer;
import net.minecraft.client.Minecraft;
import org.lwjgl.opengl.GL11;
import org.lwjgl.opengl.GL15;
import org.lwjgl.opengl.GL21;
import org.lwjgl.opengl.GL30;
import org.lwjgl.opengl.GL32;

/**
 * Minecraft 1.21.1 (OpenGL): copies Minecraft's main render target (hand + HUD + screens on a
 * transparent background) back from the GPU and publishes it to Skyrim through the overlay triple
 * buffer. Same job as the 26.x version (which uses 26.x's GPU device API).
 *
 * The copy is asynchronous: glReadPixels into one of a few pixel-pack buffers, shipped once its
 * fence says the GPU finished, typically a frame later. Bottom-up rows, RGBA8, like the 26.x copy.
 */
public final class FrameExporter {
	private static final int STAGING = 3;

	private static final Staging[] staging = new Staging[STAGING];
	private static long nextFrameId = 1;
	private static boolean loggedFormat;

	private static final class Staging {
		int buffer;
		int width;
		int height;
		long fence; // 0: free
		long frameId;
	}

	private static int shipped;

	private FrameExporter() {
	}

	/**
	 * Just before GameRenderer.render, while linked and in a world: the main target starts fully
	 * transparent, so everything Fallout doesn't get from Minecraft (the world) stays see-through.
	 */
	public static void beforeRender(Minecraft minecraft) {
		if (!SkyClient.linked() || minecraft.level == null) {
			return;
		}
		RenderTarget target = minecraft.getMainRenderTarget();
		target.setClearColor(0.0F, 0.0F, 0.0F, 0.0F);
		target.clear(Minecraft.ON_OSX);
		target.bindWrite(true);
	}

	/** For the log: how much of a shipped frame is see-through (alpha 0), solid, or in between. */
	private static void logAlpha(ByteBuffer data, int width, int height) {
		int clear = 0, solid = 0, partial = 0, samples = 0;
		int pixels = width * height;
		for (int i = 0; i < pixels; i += 97) {
			int a = data.get(i * 4 + 3) & 0xFF;
			samples++;
			if (a == 0) {
				clear++;
			} else if (a == 255) {
				solid++;
			} else {
				partial++;
			}
		}
		int c = ((height / 2) * width + width / 2) * 4;
		SkyCraft.LOG.info("SkyCraft: overlay frame {}x{}: {}% see-through, {}% solid, {}% partly; centre pixel RGBA {} {} {} {}",
			width, height, clear * 100 / Math.max(1, samples), solid * 100 / Math.max(1, samples), partial * 100 / Math.max(1, samples),
			data.get(c) & 0xFF, data.get(c + 1) & 0xFF, data.get(c + 2) & 0xFF, data.get(c + 3) & 0xFF);
	}

	public static void capture(Minecraft minecraft) {
		shipReadyFrames();

		RenderTarget target = minecraft.getMainRenderTarget();
		int width = target.width;
		int height = target.height;
		if (width <= 0 || height <= 0 || width > Proto.MAX_OVERLAY_W || height > Proto.MAX_OVERLAY_H) {
			return;
		}
		if (!loggedFormat) {
			loggedFormat = true;
			SkyCraft.LOG.info("SkyCraft: overlay capture {}x{} (OpenGL readback)", width, height);
		}

		Staging slot = null;
		for (int i = 0; i < STAGING; i++) {
			if (staging[i] == null) {
				staging[i] = new Staging();
			}
			if (staging[i].fence == 0) {
				slot = staging[i];
				break;
			}
		}
		if (slot == null) {
			return; // all pixel buffers still in flight; skip this frame
		}

		int previousRead = GL11.glGetInteger(GL30.GL_READ_FRAMEBUFFER_BINDING);
		int previousPack = GL11.glGetInteger(GL21.GL_PIXEL_PACK_BUFFER_BINDING);
		long bytes = (long) width * height * 4L;
		if (slot.buffer == 0) {
			slot.buffer = GL15.glGenBuffers();
		}
		GL15.glBindBuffer(GL21.GL_PIXEL_PACK_BUFFER, slot.buffer);
		if (slot.width != width || slot.height != height) {
			GL15.glBufferData(GL21.GL_PIXEL_PACK_BUFFER, bytes, GL15.GL_STREAM_READ);
			slot.width = width;
			slot.height = height;
		}
		GL30.glBindFramebuffer(GL30.GL_READ_FRAMEBUFFER, target.frameBufferId);
		GL11.glPixelStorei(GL11.GL_PACK_ALIGNMENT, 4);
		GL11.glReadPixels(0, 0, width, height, GL11.GL_RGBA, GL11.GL_UNSIGNED_BYTE, 0L);
		slot.fence = GL32.glFenceSync(GL32.GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		slot.frameId = nextFrameId++;
		GL15.glBindBuffer(GL21.GL_PIXEL_PACK_BUFFER, previousPack);
		GL30.glBindFramebuffer(GL30.GL_READ_FRAMEBUFFER, previousRead);
	}

	/** Maps the newest finished readback and copies it into shared memory. */
	private static void shipReadyFrames() {
		Staging newest = null;
		for (Staging s : staging) {
			if (s == null || s.fence == 0) {
				continue;
			}
			int status = GL32.glClientWaitSync(s.fence, 0, 0L);
			if (status == GL32.GL_ALREADY_SIGNALED || status == GL32.GL_CONDITION_SATISFIED) {
				if (newest == null || s.frameId > newest.frameId) {
					newest = s;
				}
			}
		}
		if (newest == null) {
			return;
		}
		Shm shm = SkyLink.segment();
		if (shm != null) {
			long bytes = (long) newest.width * newest.height * 4L;
			int previousPack = GL11.glGetInteger(GL21.GL_PIXEL_PACK_BUFFER_BINDING);
			GL15.glBindBuffer(GL21.GL_PIXEL_PACK_BUFFER, newest.buffer);
			ByteBuffer data = GL30.glMapBufferRange(GL21.GL_PIXEL_PACK_BUFFER, 0L, bytes, GL30.GL_MAP_READ_BIT);
			if (data != null) {
				if (shipped < 3 || shipped % 1800 == 0) {
					logAlpha(data, newest.width, newest.height);
				}
				shipped++;
				shm.copyFrom(data, SkyLink.overlayBackSlotOffset(), bytes);
				GL15.glUnmapBuffer(GL21.GL_PIXEL_PACK_BUFFER);
			}
			GL15.glBindBuffer(GL21.GL_PIXEL_PACK_BUFFER, previousPack);
			if (data != null) {
				SkyLink.publishOverlay(newest.width, newest.height, true, newest.frameId);
			}
		}
		// Anything as old as what we just shipped is useless now.
		for (Staging s : staging) {
			if (s != null && s.fence != 0 && s.frameId <= newest.frameId) {
				int status = GL32.glClientWaitSync(s.fence, 0, 0L);
				if (status == GL32.GL_ALREADY_SIGNALED || status == GL32.GL_CONDITION_SATISFIED) {
					GL32.glDeleteSync(s.fence);
					s.fence = 0;
				}
			}
		}
	}
}
