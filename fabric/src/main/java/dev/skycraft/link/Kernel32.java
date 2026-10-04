package dev.skycraft.link;

import static java.lang.foreign.ValueLayout.*;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.StructLayout;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.VarHandle;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import org.jspecify.annotations.Nullable;

/**
 * The few Windows calls the link needs, through Java's foreign function API (Java 22+: Minecraft
 * 26.x). The 1.21.1 builds (Java 21) use their own Kernel32 with the same methods, through JNA
 * (versions/1.21.1/common).
 */
public final class Kernel32 {
	private static final int FILE_MAP_ALL_ACCESS = 0xF001F;

	private static final MethodHandle OPEN_FILE_MAPPING;
	// OpenFileMappingW's GetLastError, captured right after the call (the JVM may change it later).
	private static final StructLayout CALL_STATE = Linker.Option.captureStateLayout();
	private static final VarHandle LAST_ERROR = CALL_STATE.varHandle(java.lang.foreign.MemoryLayout.PathElement.groupElement("GetLastError"));
	private static final MemorySegment OPEN_STATE = Arena.global().allocate(CALL_STATE);
	private static final MethodHandle MAP_VIEW_OF_FILE;
	private static final MethodHandle GET_TICK_COUNT64;
	private static final MethodHandle GET_CURRENT_PROCESS_ID;
	private static final MethodHandle QUERY_PERFORMANCE_COUNTER;
	private static final MethodHandle QUERY_PERFORMANCE_FREQUENCY;
	private static final MethodHandle CREATE_MUTEX;
	private static final MemorySegment QPC_OUT = Arena.global().allocate(JAVA_LONG);

	static {
		Linker linker = Linker.nativeLinker();
		SymbolLookup k32 = SymbolLookup.libraryLookup("kernel32", Arena.global());
		OPEN_FILE_MAPPING = linker.downcallHandle(
			k32.find("OpenFileMappingW").orElseThrow(), FunctionDescriptor.of(ADDRESS, JAVA_INT, JAVA_INT, ADDRESS), Linker.Option.captureCallState("GetLastError")
		);
		MAP_VIEW_OF_FILE = linker.downcallHandle(
			k32.find("MapViewOfFile").orElseThrow(), FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, JAVA_INT, JAVA_LONG)
		);
		GET_TICK_COUNT64 = linker.downcallHandle(k32.find("GetTickCount64").orElseThrow(), FunctionDescriptor.of(JAVA_LONG));
		GET_CURRENT_PROCESS_ID = linker.downcallHandle(k32.find("GetCurrentProcessId").orElseThrow(), FunctionDescriptor.of(JAVA_INT));
		QUERY_PERFORMANCE_COUNTER = linker.downcallHandle(k32.find("QueryPerformanceCounter").orElseThrow(), FunctionDescriptor.of(JAVA_INT, ADDRESS));
		QUERY_PERFORMANCE_FREQUENCY = linker.downcallHandle(k32.find("QueryPerformanceFrequency").orElseThrow(), FunctionDescriptor.of(JAVA_INT, ADDRESS));
		CREATE_MUTEX = linker.downcallHandle(k32.find("CreateMutexW").orElseThrow(), FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, ADDRESS));
	}

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
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment wide = arena.allocateFrom(name, StandardCharsets.UTF_16LE);
			MemorySegment handle = (MemorySegment) OPEN_FILE_MAPPING.invokeExact(OPEN_STATE, FILE_MAP_ALL_ACCESS, 0, wide);
			if (handle.address() == 0) {
				throw new OpenFailed((int) LAST_ERROR.get(OPEN_STATE, 0L));
			}
			MemorySegment view = (MemorySegment) MAP_VIEW_OF_FILE.invokeExact(handle, FILE_MAP_ALL_ACCESS, 0, 0, 0L);
			if (view.address() == 0) {
				return null;
			}
			return view.reinterpret(bytes).asByteBuffer();
		} catch (OpenFailed e) {
			throw e;
		} catch (Throwable t) {
			throw new RuntimeException(t);
		}
	}

	/** CreateMutexW with this name (held until the process exits). False if it failed. */
	public static boolean createMutex(String name) {
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment wide = arena.allocateFrom(name, StandardCharsets.UTF_16LE);
			MemorySegment mutex = (MemorySegment) CREATE_MUTEX.invokeExact(MemorySegment.NULL, 0, wide);
			return mutex.address() != 0;
		} catch (Throwable t) {
			throw new RuntimeException(t);
		}
	}

	public static int currentProcessId() {
		try {
			return (int) GET_CURRENT_PROCESS_ID.invokeExact();
		} catch (Throwable t) {
			throw new RuntimeException(t);
		}
	}

	public static long tickCount64() {
		try {
			return (long) GET_TICK_COUNT64.invokeExact();
		} catch (Throwable t) {
			throw new RuntimeException(t);
		}
	}

	public static synchronized long queryPerformanceCounter() {
		try {
			int ok = (int) QUERY_PERFORMANCE_COUNTER.invokeExact(QPC_OUT);
			return QPC_OUT.get(JAVA_LONG, 0);
		} catch (Throwable t) {
			throw new RuntimeException(t);
		}
	}

	public static synchronized long queryPerformanceFrequency() {
		try {
			int ok = (int) QUERY_PERFORMANCE_FREQUENCY.invokeExact(QPC_OUT);
			return QPC_OUT.get(JAVA_LONG, 0);
		} catch (Throwable t) {
			throw new RuntimeException(t);
		}
	}
}
