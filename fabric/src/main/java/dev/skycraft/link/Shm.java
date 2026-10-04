package dev.skycraft.link;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * The shared memory with Fallout, as a direct ByteBuffer over the mapped view.
 *
 * <p>Plain ByteBuffer access works on every Java version the mod runs on (Java 25 for Minecraft
 * 26.x, Java 21 for 1.21.1); only mapping the view is native ({@link Kernel32}). Little-endian like
 * the C++ side. Offsets are those of the protocol (Proto), all naturally aligned, which the
 * acquire/release accessors need.
 */
public final class Shm {
	private static final VarHandle INT = MethodHandles.byteBufferViewVarHandle(int[].class, ByteOrder.LITTLE_ENDIAN);
	private static final VarHandle LONG = MethodHandles.byteBufferViewVarHandle(long[].class, ByteOrder.LITTLE_ENDIAN);

	private final ByteBuffer buf;

	public Shm(ByteBuffer buffer) {
		this.buf = buffer.duplicate().order(ByteOrder.LITTLE_ENDIAN);
	}

	public long size() {
		return this.buf.capacity();
	}

	private static int at(long offset) {
		return Math.toIntExact(offset);
	}

	public byte getByte(long off) {
		return this.buf.get(at(off));
	}

	public void setByte(long off, byte v) {
		this.buf.put(at(off), v);
	}

	public short getShort(long off) {
		return this.buf.getShort(at(off));
	}

	public int getInt(long off) {
		return this.buf.getInt(at(off));
	}

	public void setInt(long off, int v) {
		this.buf.putInt(at(off), v);
	}

	public long getLong(long off) {
		return this.buf.getLong(at(off));
	}

	public void setLong(long off, long v) {
		this.buf.putLong(at(off), v);
	}

	public float getFloat(long off) {
		return this.buf.getFloat(at(off));
	}

	public void setFloat(long off, float v) {
		this.buf.putFloat(at(off), v);
	}

	public double getDouble(long off) {
		return this.buf.getDouble(at(off));
	}

	public void setDouble(long off, double v) {
		this.buf.putDouble(at(off), v);
	}

	public int getIntAcquire(long off) {
		return (int) INT.getAcquire(this.buf, at(off));
	}

	public void setIntRelease(long off, int v) {
		INT.setRelease(this.buf, at(off), v);
	}

	public int getAndSetInt(long off, int v) {
		return (int) INT.getAndSet(this.buf, at(off), v);
	}

	public long getLongAcquire(long off) {
		return (long) LONG.getAcquire(this.buf, at(off));
	}

	public void setLongRelease(long off, long v) {
		LONG.setRelease(this.buf, at(off), v);
	}

	public long getAndAddLong(long off, long delta) {
		return (long) LONG.getAndAdd(this.buf, at(off), delta);
	}

	/** Copies {@code src}'s remaining bytes (its position is left alone) to {@code off}. */
	public void copyFrom(ByteBuffer src, long off) {
		this.buf.put(at(off), src, src.position(), src.remaining());
	}

	/** Copies at most {@code bytes} of {@code src}'s remaining bytes to {@code off}. */
	public void copyFrom(ByteBuffer src, long off, long bytes) {
		int n = (int) Math.min(bytes, src.remaining());
		this.buf.put(at(off), src, src.position(), n);
	}
}
