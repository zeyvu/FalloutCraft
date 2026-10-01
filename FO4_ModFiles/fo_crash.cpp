// FalloutCraft: a small crash logger, so a crash says where it happened.
//
// Writes Documents\My Games\Fallout4\F4SE\SkyCraft_crash.log on access violations and similar
// faults: the exception, the registers that matter and the call stack as module+offset, with
// function names and lines for our own plugin when its .pdb is next to where it was built.
// First-chance faults are logged too (some are caught and harmless, e.g. our guarded Havok reads,
// and say so); the last entry before a crash is the one that matters.

#include "fo_common.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#undef ERROR

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace skycraft::Crash
{
	namespace
	{
		std::mutex         logLock;
		std::atomic<int>   logged{ 0 };
		bool               symbolsReady = false;
		wchar_t            logPath[MAX_PATH]{};

		std::string ModuleAndOffset(DWORD64 a_addr)
		{
			HMODULE module = nullptr;
			if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(a_addr), &module) || !module) {
				char buf[32];
				std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(a_addr));
				return buf;
			}
			char name[MAX_PATH]{};
			::GetModuleFileNameA(module, name, MAX_PATH);
			const char* base = std::strrchr(name, '\\');
			char        buf[MAX_PATH + 64];
			std::snprintf(buf, sizeof(buf), "%s+0x%llX", base ? base + 1 : name, static_cast<unsigned long long>(a_addr - reinterpret_cast<DWORD64>(module)));
			std::string out = buf;
			if (symbolsReady) {
				alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256]{};
				auto*                     sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
				sym->SizeOfStruct = sizeof(SYMBOL_INFO);
				sym->MaxNameLen = 255;
				DWORD64 disp = 0;
				if (::SymFromAddr(::GetCurrentProcess(), a_addr, &disp, sym)) {
					out += std::string("  ") + sym->Name;
					IMAGEHLP_LINE64 line{};
					line.SizeOfStruct = sizeof(line);
					DWORD lineDisp = 0;
					if (::SymGetLineFromAddr64(::GetCurrentProcess(), a_addr, &lineDisp, &line) && line.FileName) {
						const char* file = std::strrchr(line.FileName, '\\');
						char        lb[MAX_PATH];
						std::snprintf(lb, sizeof(lb), " (%s:%lu)", file ? file + 1 : line.FileName, line.LineNumber);
						out += lb;
					}
				}
			}
			return out;
		}

		bool IsFatalCode(DWORD a_code)
		{
			switch (a_code) {
			case EXCEPTION_ACCESS_VIOLATION:
			case EXCEPTION_ILLEGAL_INSTRUCTION:
			case EXCEPTION_PRIV_INSTRUCTION:
			case EXCEPTION_INT_DIVIDE_BY_ZERO:
			case EXCEPTION_STACK_OVERFLOW:
			case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
			case EXCEPTION_IN_PAGE_ERROR:
			case 0xC0000374:  // heap corruption
			case 0xC0000409:  // stack buffer overrun / fail fast
				return true;
			default:
				return false;
			}
		}

		LONG CALLBACK Handler(EXCEPTION_POINTERS* a_info)
		{
			const auto* rec = a_info->ExceptionRecord;
			if (!IsFatalCode(rec->ExceptionCode) || logged.load() >= 200) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			std::scoped_lock lock(logLock);
			// The same place faulting again (a guarded read that keeps failing) is logged once.
			static void* seen[64]{};
			for (auto* s : seen) {
				if (s == rec->ExceptionAddress) {
					return EXCEPTION_CONTINUE_SEARCH;
				}
			}
			seen[logged.load() % 64] = rec->ExceptionAddress;
			++logged;
			FILE* f = nullptr;
			if (_wfopen_s(&f, logPath, L"a") != 0 || !f) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			SYSTEMTIME t;
			::GetLocalTime(&t);
			const auto* ctx = a_info->ContextRecord;
			std::fprintf(f, "\n[%02d:%02d:%02d.%03d] exception 0x%08lX at %s (thread %lu)\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
				rec->ExceptionCode, ModuleAndOffset(reinterpret_cast<DWORD64>(rec->ExceptionAddress)).c_str(), ::GetCurrentThreadId());
			if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
				std::fprintf(f, "  %s address 0x%llX\n", rec->ExceptionInformation[0] == 0 ? "reading" : (rec->ExceptionInformation[0] == 1 ? "writing" : "executing"),
					static_cast<unsigned long long>(rec->ExceptionInformation[1]));
			}
			std::fprintf(f, "  rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX r8=%llX r9=%llX rsp=%llX\n",
				ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx, ctx->Rsi, ctx->Rdi, ctx->R8, ctx->R9, ctx->Rsp);

			// Walk the faulting thread's stack from the exception context.
			CONTEXT  c = *ctx;
			for (int frame = 0; frame < 40; ++frame) {
				const DWORD64 pc = c.Rip;
				if (!pc) {
					break;
				}
				std::fprintf(f, "  #%02d %s\n", frame, ModuleAndOffset(pc).c_str());
				DWORD64 imageBase = 0;
				auto*   fn = ::RtlLookupFunctionEntry(pc, &imageBase, nullptr);
				if (!fn) {
					// Leaf function: return address is at [rsp].
					if (!c.Rsp) {
						break;
					}
					c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
					c.Rsp += 8;
					continue;
				}
				void*   handlerData = nullptr;
				DWORD64 establisher = 0;
				::RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, pc, fn, &c, &handlerData, &establisher, nullptr);
			}
			std::fprintf(f, "  (first chance: if the game kept running after this, it was caught and harmless)\n");
			std::fclose(f);
			return EXCEPTION_CONTINUE_SEARCH;
		}
	}

	void Install()
	{
		wchar_t profile[MAX_PATH]{};
		if (!::GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) {
			return;
		}
		std::swprintf(logPath, MAX_PATH, L"%s\\Documents\\My Games\\Fallout4\\F4SE\\SkyCraft_crash.log", profile);
		if (FILE* f = nullptr; _wfopen_s(&f, logPath, L"w") == 0 && f) {
			std::fprintf(f, "SkyCraft crash log (plugin build %s %s). Empty below = no faults.\n", __DATE__, __TIME__);
			std::fclose(f);
		}
		::SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
		symbolsReady = ::SymInitialize(::GetCurrentProcess(), nullptr, TRUE) != FALSE;
		::AddVectoredExceptionHandler(1, Handler);
		REX::INFO("crash logger installed (SkyCraft_crash.log, symbols {})", symbolsReady ? "on" : "off");
	}
}
