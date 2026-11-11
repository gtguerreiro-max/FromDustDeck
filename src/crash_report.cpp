// Writes the host call stack (module + offset per frame) to crash.txt beside the executable
// and to stderr when the process aborts (abort(), std::terminate) or faults. On Windows,
// abort() otherwise ends as 0xC0000409 with nothing in the log. Resolve the offsets with the
// link maps (/MAP) or llvm-symbolizer.

#include "crash_report.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <initializer_list>

namespace {

void WriteStack(const char* why) {
  wchar_t exe[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  while (n && exe[n - 1] != L'\\') {
    --n;
  }
  wcscpy_s(exe + n, MAX_PATH - n, L"crash.txt");
  FILE* f = _wfopen(exe, L"a");
  void* frames[62];
  USHORT count = RtlCaptureStackBackTrace(1, 62, frames, nullptr);
  for (FILE* out : {f, stderr}) {
    if (!out) {
      continue;
    }
    fprintf(out, "fromdust: %s (thread %lu)\n", why, GetCurrentThreadId());
    for (USHORT i = 0; i < count; ++i) {
      HMODULE mod = nullptr;
      char name[MAX_PATH] = "?";
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             static_cast<LPCSTR>(frames[i]), &mod)) {
        GetModuleFileNameA(mod, name, MAX_PATH);
      }
      const char* base = strrchr(name, '\\');
      fprintf(out, "  #%02u %s+0x%llx\n", i, base ? base + 1 : name,
              static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) -
                                              reinterpret_cast<uintptr_t>(mod)));
    }
    fflush(out);
  }
  if (f) {
    fclose(f);
  }
}

void OnAbort(int) {
  WriteStack("abort");
}

void OnTerminate() {
  WriteStack("std::terminate");
  std::abort();
}

void WriteFault(EXCEPTION_POINTERS* info, const char* why) {
  char text[160];
  HMODULE mod = nullptr;
  char name[MAX_PATH] = "?";
  void* at = info->ExceptionRecord->ExceptionAddress;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(at), &mod)) {
    GetModuleFileNameA(mod, name, MAX_PATH);
  }
  const char* file = strrchr(name, '\\');
  snprintf(text, sizeof(text), "%s %08lX at %s+0x%llx", why, info->ExceptionRecord->ExceptionCode,
           file ? file + 1 : name,
           static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(at) -
                                           reinterpret_cast<uintptr_t>(mod)));
  WriteStack(text);
}

// Last in the vectored chain, so the runtime's own handlers (guest MMIO, write watches) have
// already passed on the fault: what reaches here ends the process. Only the first is written.
LONG CALLBACK OnLastChanceFault(EXCEPTION_POINTERS* info) {
  static LONG written = 0;
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
       code == EXCEPTION_STACK_OVERFLOW) &&
      InterlockedExchange(&written, 1) == 0) {
    WriteFault(info, "fault");
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallCrashReport() {
  AddVectoredExceptionHandler(0, OnLastChanceFault);
  signal(SIGABRT, OnAbort);
  std::set_terminate(OnTerminate);
  // Route the CRT's invalid-parameter and abort paths through SIGABRT, not straight to
  // __fastfail, so the handler above runs.
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}
#else
void InstallCrashReport() {}
#endif
