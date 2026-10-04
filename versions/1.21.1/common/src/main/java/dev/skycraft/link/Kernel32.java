package dev.skycraft.link;

import com.sun.jna.Library;
import com.sun.jna.Native;
import com.sun.jna.Pointer;
import com.sun.jna.WString;
import com.sun.jna.ptr.LongByReference;
import java.nio.ByteBuffer;
import org.jspecify.annotations.Nullable;

/**
 * Minecraft 1.21.1 (Java 21): the few Windows calls the link needs, through JNA (which Minecraft
 * ships). Same methods as the 26.x Kernel32, which uses Java 22's foreign function API instead.
 */
public final class Kernel32 {
	private static final int FILE_MAP_ALL_ACCESS = 0xF001F;

	private interface K32 extends Library {
		Pointer OpenFileMappingW(int desiredAccess, boolean inheritHandle, WString name);

		Pointer MapViewOfFile(Pointer mapping, int desiredAccess, int offsetHigh, int offsetLow, long bytes);

		Pointer CreateMutexW(Pointer attributes, boolean initialOwner, WString name);

		int GetCurrentProcessId();

		long GetTickCount64();

		boolean QueryPerformanceCounter(LongByReference count);

		boolean QueryPerformanceFrequency(LongByReference frequency);
	}

	private static final K32 LIB = Native.load("kernel32", K32.class);

	private Kernel32() {
	}

	/** Thrown for a failed OpenFileMappingW; {@link #error} is Windows' GetLastError. */
	public static final class OpenFailed extends Exception {
		public final int error;

		OpenFailed(int error) {
			super("OpenFileMappingW failed: " + error);
			this.error = error;
		}
	}

	/**
	 * Opens the named file mapping (made by the Fallout plugin) and maps {@code bytes} of it. Null
	 * if MapViewOfFile fails.
	 */
	public static @Nullable ByteBuffer openMapping(String name, long bytes) throws OpenFailed {
		Pointer handle = LIB.OpenFileMappingW(FILE_MAP_ALL_ACCESS, false, new WString(name));
		if (handle == null || Pointer.nativeValue(handle) == 0) {
			throw new OpenFailed(Native.getLastError());
		}
		Pointer view = LIB.MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, 0L);
		if (view == null || Pointer.nativeValue(view) == 0) {
			return null;
		}
		return view.getByteBuffer(0, bytes);
	}

	/** CreateMutexW with this name (held until the process exits). False if it failed. */
	public static boolean createMutex(String name) {
		Pointer mutex = LIB.CreateMutexW(null, false, new WString(name));
		return mutex != null && Pointer.nativeValue(mutex) != 0;
	}

	public static int currentProcessId() {
		return LIB.GetCurrentProcessId();
	}

	public static long tickCount64() {
		return LIB.GetTickCount64();
	}

	public static synchronized long queryPerformanceCounter() {
		LongByReference out = new LongByReference();
		LIB.QueryPerformanceCounter(out);
		return out.getValue();
	}

	public static synchronized long queryPerformanceFrequency() {
		LongByReference out = new LongByReference();
		LIB.QueryPerformanceFrequency(out);
		return out.getValue();
	}
}
