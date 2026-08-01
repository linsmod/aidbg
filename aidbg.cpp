// aidbg.cpp - AI-usable GDB-style native debugger built on TitanEngine.
//
// Build (x64, MSVC):
//   vcvars64.bat && cl /nologo /EHsc /std:c++17 /O2 /utf-8 aidbg.cpp ^
//       /link /LIBPATH:<TitanEngine>\build_x64\Release TitanEngine.lib
//   (copy TitanEngine.dll next to aidbg.exe at runtime)
//
// Usage:
//   aidbg.exe [--json] [--command "cmd"] [--commands file] [target.exe]
//
// GDB-compatible CLI (see the GDB manual):
//   aidbg.exe [--batch] [-ex "cmd" ...] [-x script] [--args prog arg ...]
//
// --json              machine-readable output: one JSON object per line
// --command           GDB: a file path runs as a command file; else a single
//                     command, then exit (AI-friendly)
// --commands          run a script file, then exit
// --batch / -batch    batch mode; exit nonzero if the inferior crashed or a
//                     command errored (in batch, "run" runs to exit/crash
//                     instead of stopping at the initial breakpoint)
// --batch-silent      like --batch but suppress all aidbg output
// -ex "cmd"           execute a command (repeatable; --eval-command=CMD too)
// -x file             execute commands from a file (--command-file=FILE too)
// -e file             set the target executable (--executable=FILE too)
// --args prog arg...  set the target and its inferior arguments (consumes the
//                     rest of the command line); a bare "run" uses those args
// -q / --quiet        suppress the startup banner
// <target>            convenience: same as "file <target>" then "run"
//
// GDB `start`: stops at the program's entry function (main/WinMain/...) using a
// temporary (one-shot) breakpoint, resolving the symbol from the PDB once the
// target is running. `run` keeps GDB batch semantics (continue to exit/crash).
// `break <symbol>` / `break <line>` / `break file.c:line` resolve from the PDB
// before `run` and are armed (pending) when the target starts.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>

#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cstdint>
#include <cctype>
#include <fstream>

// ------------------------------------------------------------- TitanEngine ---

#define TITCALL

extern "C" {
__declspec(dllimport) void*    TITCALL InitDebugW(wchar_t* f, wchar_t* cl, wchar_t* dir);
__declspec(dllimport) void*    TITCALL InitDLLDebugW(wchar_t* f, bool reserve, wchar_t* cl, wchar_t* dir, void* cb);
__declspec(dllimport) void     TITCALL DebugLoop();
__declspec(dllimport) void     TITCALL ForceClose();
__declspec(dllimport) bool     TITCALL StopDebug();
__declspec(dllimport) void     TITCALL SetCustomHandler(DWORD id, void* cb);
__declspec(dllimport) void     TITCALL SetNextDbgContinueStatus(DWORD status);
__declspec(dllimport) bool     TITCALL SetBPX(ULONG_PTR a, DWORD type, void* cb);
__declspec(dllimport) bool     TITCALL DeleteBPX(ULONG_PTR a);
__declspec(dllimport) bool     TITCALL EnableBPX(ULONG_PTR a);
__declspec(dllimport) bool     TITCALL DisableBPX(ULONG_PTR a);
__declspec(dllimport) bool     TITCALL SetAPIBreakPoint(const char* dll, const char* api, DWORD type, DWORD place, void* cb);
__declspec(dllimport) bool     TITCALL DeleteAPIBreakPoint(const char* dll, const char* api, DWORD place);
__declspec(dllimport) bool     TITCALL SetHardwareBreakPoint(ULONG_PTR a, DWORD reg, DWORD type, DWORD size, void* cb);
__declspec(dllimport) bool     TITCALL DeleteHardwareBreakPoint(DWORD reg);
__declspec(dllimport) bool     TITCALL GetUnusedHardwareBreakPointRegister(DWORD* reg);
__declspec(dllimport) bool     TITCALL SetMemoryBPXEx(ULONG_PTR a, SIZE_T size, DWORD type, bool restore, void* cb);
__declspec(dllimport) bool     TITCALL RemoveMemoryBPX(ULONG_PTR a, SIZE_T size);
__declspec(dllimport) void     TITCALL StepInto(void* cb);
__declspec(dllimport) void     TITCALL StepOver(void* cb);
__declspec(dllimport) void     TITCALL StepOut(void* cb, bool final);
__declspec(dllimport) ULONG_PTR TITCALL GetContextData(DWORD reg);
__declspec(dllimport) bool     TITCALL SetContextData(DWORD reg, ULONG_PTR val);
__declspec(dllimport) bool     TITCALL MemoryReadSafe(HANDLE h, void* a, void* buf, SIZE_T n, SIZE_T* r);
__declspec(dllimport) bool     TITCALL MemoryWriteSafe(HANDLE h, void* a, const void* buf, SIZE_T n, SIZE_T* w);
__declspec(dllimport) void*    TITCALL DisassembleEx(HANDLE h, void* a, bool type);
__declspec(dllimport) long     TITCALL LengthDisassembleEx(HANDLE h, void* a);
__declspec(dllimport) void*    TITCALL GetDebugData();
__declspec(dllimport) PROCESS_INFORMATION* TITCALL TitanGetProcessInformation();
__declspec(dllimport) ULONG_PTR TITCALL GetDebuggedFileBaseAddress();
__declspec(dllimport) long     TITCALL GetExitCode();
__declspec(dllimport) bool     TITCALL ThreaderPauseAllThreads(bool leave);
__declspec(dllimport) bool     TITCALL ThreaderResumeAllThreads(bool leave);
__declspec(dllimport) bool     TITCALL AttachDebugger(DWORD pid, bool kill, void* info, void* cb);
__declspec(dllimport) bool     TITCALL DetachDebuggerEx(DWORD pid);
__declspec(dllimport) void     TITCALL SetEngineVariable(DWORD var, bool set);
__declspec(dllimport) void     TITCALL LibrarianEnumLibraryInfoW(void* cb);
__declspec(dllimport) void     TITCALL ThreaderEnumThreadInfo(void* cb);
__declspec(dllimport) ULONG_PTR TITCALL FindEx(HANDLE h, void* start, DWORD size, void* pat, DWORD patsize, BYTE* wild);
__declspec(dllimport) bool     TITCALL GetRemoteString(HANDLE h, void* a, void* buf, int max);
__declspec(dllimport) void     TITCALL SetBPXOptions(long type);
}

// UE_* constants (TitanEngine/stdafx.h + definitions.h)
enum {
    UE_EAX=1, UE_EBX=2, UE_ECX=3, UE_EDX=4, UE_EDI=5, UE_ESI=6, UE_EBP=7, UE_ESP=8,
    UE_EIP=9, UE_EFLAGS=10, UE_DR0=11, UE_DR1=12, UE_DR2=13, UE_DR3=14, UE_DR6=15, UE_DR7=16,
    UE_RAX=17, UE_RBX=18, UE_RCX=19, UE_RDX=20, UE_RDI=21, UE_RSI=22, UE_RBP=23, UE_RSP=24,
    UE_RIP=25, UE_RFLAGS=26, UE_R8=27, UE_R9=28, UE_R10=29, UE_R11=30, UE_R12=31, UE_R13=32,
    UE_R14=33, UE_R15=34, UE_CIP=35, UE_CSP=36,
};

enum {
    UE_CH_BREAKPOINT=1, UE_CH_SINGLESTEP=2, UE_CH_ACCESSVIOLATION=3, UE_CH_ILLEGALINSTRUCTION=4,
    UE_CH_NONCONTINUABLEEXCEPTION=5, UE_CH_ARRAYBOUNDSEXCEPTION=6, UE_CH_FLOATDENORMALOPERAND=7,
    UE_CH_FLOATDEVIDEBYZERO=8, UE_CH_INTEGERDEVIDEBYZERO=9, UE_CH_INTEGEROVERFLOW=10,
    UE_CH_PRIVILEGEDINSTRUCTION=11, UE_CH_PAGEGUARD=12, UE_CH_EVERYTHINGELSE=13,
    UE_CH_CREATETHREAD=14, UE_CH_EXITTHREAD=15, UE_CH_CREATEPROCESS=16, UE_CH_EXITPROCESS=17,
    UE_CH_LOADDLL=18, UE_CH_UNLOADDLL=19, UE_CH_OUTPUTDEBUGSTRING=20, UE_CH_AFTEREXCEPTIONPROCESSING=21,
    UE_CH_SYSTEMBREAKPOINT=23, UE_CH_UNHANDLEDEXCEPTION=24, UE_CH_RIPEVENT=25, UE_CH_DEBUGEVENT=26,
};

enum {
    UE_BREAKPOINT=0, UE_SINGLESHOOT=1, UE_MEMORY=3, UE_MEMORY_READ=4, UE_MEMORY_WRITE=5,
    UE_MEMORY_EXECUTE=6, UE_BREAKPOINT_TYPE_INT3=0x10000000,
    UE_HARDWARE_EXECUTE=4, UE_HARDWARE_WRITE=5, UE_HARDWARE_READWRITE=6,
    UE_HARDWARE_SIZE_1=7, UE_HARDWARE_SIZE_2=8, UE_HARDWARE_SIZE_4=9, UE_HARDWARE_SIZE_8=10,
    UE_APISTART=0,
    UE_ENGINE_NO_CONSOLE_WINDOW=4, UE_ENGINE_PASS_ALL_EXCEPTIONS=3,
    UE_ENGINE_DISABLE_ASLR=12, UE_ENGINE_SAFE_STEP=13,
};

// LIBRARY_ITEM_DATAW (packed in TitanEngine, mirror it)
#pragma pack(push,1)
struct LIB_ITEM_W {
    HANDLE hFile;
    void*  BaseOfDll;
    HANDLE hFileMapping;
    void*  hFileMappingView;
    wchar_t szLibraryPath[260];
    wchar_t szLibraryName[260];
};
struct THREAD_ITEM_W {
    HANDLE hThread;
    DWORD  dwThreadId;
    void*  ThreadStartAddress;
    void*  ThreadLocalBase;
    void*  TebAddress;
    ULONG  WaitTime;
    LONG   Priority;
    LONG   BasePriority;
    ULONG  ContextSwitches;
    ULONG  ThreadState;
    ULONG  WaitReason;
};
#pragma pack(pop)

// ---------------------------------------------------------------- global state ---

static bool g_json = false;                 // JSON machine output
static bool g_batch = false;                // GDB --batch: run()-continues past the initial break
static bool g_quiet = false;                // -q: suppress startup banner
static bool g_silent = false;               // --batch-silent: suppress all aidbg output
static std::string g_target;                // target exe path
static std::wstring g_target_w;
static std::string g_default_args;          // inferior args from --args / set args (used by bare "run")

// stop/resume handshake between DebugLoop thread and REPL thread
static std::mutex g_mu;
static std::condition_variable g_cv;
static bool g_stopped = false;              // paused at a stop event
static bool g_waiting = false;              // a stop callback is blocked awaiting resume
static bool g_exited  = false;              // target process exited
static bool g_quit    = false;              // user quit
static std::string g_reason;                // reason of current stop
static bool g_running = false;              // debug loop is active
static std::thread g_loop_thread;

static DWORD g_exception_code = 0;          // last exception info
static ULONG_PTR g_exception_addr = 0;
static bool g_handle_exception_on_resume = false; // selected by the active stop callback

static std::vector<std::string> g_events;   // event log
static std::mutex g_ev_mu;

// register tables (x64 and x86)
struct RegInfo { const char* n64; const char* n32; DWORD idx64; DWORD idx32; };
static const RegInfo REGS[] = {
    {"rax","eax",UE_RAX,UE_EAX},{"rbx","ebx",UE_RBX,UE_EBX},{"rcx","ecx",UE_RCX,UE_ECX},
    {"rdx","edx",UE_RDX,UE_EDX},{"rdi","edi",UE_RDI,UE_EDI},{"rsi","esi",UE_RSI,UE_ESI},
    {"rbp","ebp",UE_RBP,UE_EBP},{"rsp","esp",UE_RSP,UE_ESP},
    {"r8","r8d",UE_R8,0},{"r9","r9d",UE_R9,0},{"r10","r10d",UE_R10,0},{"r11","r11d",UE_R11,0},
    {"r12","r12d",UE_R12,0},{"r13","r13d",UE_R13,0},{"r14","r14d",UE_R14,0},{"r15","r15d",UE_R15,0},
    {"rip","eip",UE_RIP,UE_EIP},{"rflags","eflags",UE_RFLAGS,UE_EFLAGS},
};
static const char* SEG_REGS[] = {"gs","fs","es","ds","cs","ss"};
static const DWORD SEG_IDX[] = {37,38,39,40,41,42};

// breakpoints we track (TitanEngine has no enumerator)
struct Bpx {
    int id;
    int kind;                 // 0=code,1=api,2=hardware,3=memory
    ULONG_PTR addr;
    std::string dll, api;
    DWORD hwreg;              // UE_DR0..3
    DWORD hwtype;
    SIZE_T memsize;
    bool enabled;
    std::string symbol;       // original location spec, for display (e.g. "boom")
    std::string file;         // source file for line breakpoints (file.c:NN)
    long line = 0;            // source line for line breakpoints
    ULONG_PTR hits = 0;       // how many times this breakpoint has fired
    int ignore = 0;           // ignore this many crossings before stopping
    std::string condition;    // stop only while this expression evaluates true
    bool oneshot = false;     // temporary breakpoint (tbreak / `start`): removed after first hit
    bool pending = false;     // symbol breakpoint set before run: applied on process start
};
static std::map<int,Bpx> g_bps;
static std::mutex g_bp_mu;               // guards g_bps (REPL vs DebugLoop thread)
static int g_bp_next_id = 1;
static int g_hit_bp_id = 0;              // breakpoint that triggered the current stop
static ULONG_PTR g_hit_bp_hits = 0;      // its hit count when it fired
static bool g_cond_failed = false;       // condition eval errored on the last hit
static bool g_hit_bp_oneshot = false;    // the hit came from a temporary (start) breakpoint

// current "display" thread: 0 = the debug-event thread (TitanEngine context)
static DWORD g_ctx_tid = 0;
static CONTEXT g_ctx;                    // cached GetThreadContext snapshot
static bool g_ctx_valid = false;

// modules cache (address -> module)
struct ModInfo { ULONG_PTR base; std::string name; std::string path; };
static std::vector<ModInfo> g_mods;

// PDB symbol resolution state (dbghelp)
static bool g_sym_active = false;
static HANDLE g_sym_proc = NULL;
static std::set<ULONG_PTR> g_sym_loaded;   // module bases already handed to dbghelp

// source-file / PDB checksum verification (optional, default off)
static bool g_source_checksum = false;
struct CsCache { std::string status; DWORD type; FILETIME mtime; ULONGLONG size; };
static std::map<std::string, CsCache> g_cs_cache;   // per-file-path cached result

static void sym_begin(PROCESS_INFORMATION* pi);
static void sym_end();
static void sym_sync();
static std::string resolve(ULONG_PTR addr);

struct CmdResult;
static CmdResult cmd_info_vars(bool args_only);

static bool mem_read(ULONG_PTR addr, void* buf, SIZE_T n, SIZE_T* nr);
static bool sym_lookup(const std::string& name, ULONG_PTR& out);
static bool eval_cond(const std::string& expr, bool& out);
static void apply_pending_bps();
static std::string resolve_gdb(ULONG_PTR addr);
static bool resolve_line(const std::string& file, long line, ULONG_PTR& out);
static bool require_running();

// ------------------------------------------------------------------- helpers ---

static bool target_is64()
{
    PROCESS_INFORMATION* pi = TitanGetProcessInformation();
    if (!pi || !pi->hProcess) return true;
    BOOL wow = FALSE;
    if (IsWow64Process(pi->hProcess, &wow) && wow) return false;
    return true;
}

static ULONG_PTR reg_get(DWORD idx)
{
    if (!idx) return 0;
    return GetContextData(idx);
}

static DWORD reg_index_for_name(const std::string& name, bool is64)
{
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    for (auto& r : REGS) {
        if (is64) {
            if (n == r.n64) return r.idx64;
            if (n == r.n32) return r.idx64;   // GDB: accept 32-bit alias, use the 64-bit reg
        } else {
            if (n == r.n32) return r.idx32;
            if (n == r.n64 && r.idx32) return r.idx32;  // accept 64-bit name on 32-bit target
        }
    }
    for (int i = 0; i < 6; i++) if (n == SEG_REGS[i]) return SEG_IDX[i];
    return 0;
}

static std::string reg_name(DWORD idx, bool is64)
{
    for (auto& r : REGS) {
        if (is64 && r.idx64 == idx) return r.n64;
        if (!is64 && r.idx32 == idx) return r.n32;
    }
    for (int i = 0; i < 6; i++) if (SEG_IDX[i] == idx) return SEG_REGS[i];
    return "?";
}

// JSON escaping
static std::string js_str(const std::string& s)
{
    std::ostringstream o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b,sizeof b,"\\u%04x",c); o << b; }
                else o << c;
        }
    }
    return o.str();
}

static std::string hex(ULONG_PTR v)
{
    std::ostringstream o;
    if (sizeof(void*) == 8) o << std::hex << std::setfill('0') << std::setw(16) << v;
    else o << std::hex << std::setfill('0') << std::setw(8) << v;
    return "0x" + o.str();
}

static std::string hex_brief(ULONG_PTR v)
{
    std::ostringstream o;
    o << "0x" << std::hex << std::setfill('0') << std::setw(8) << (v & 0xFFFFFFFF) << std::dec;
    return o.str();
}

static std::string utf8(const wchar_t* w)
{
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 1) return "";
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL);
    return s;
}

static std::wstring wide_target()
{
    return g_target_w;
}

static void set_target_path(const std::string& path)
{
    g_target = path;
    int n = MultiByteToWideChar(CP_UTF8, 0, g_target.c_str(), -1, NULL, 0);
    g_target_w.resize(n - 1);
    MultiByteToWideChar(CP_UTF8, 0, g_target.c_str(), -1, &g_target_w[0], n);
}

// ---------------------------------------------------------- stop/resume sync ---

// Runs on the DebugLoop thread. Pauses the target and blocks until the REPL
// thread issues continue/step.
static void pause_until_continue(const char* reason, bool handle_exception_on_resume = false)
{
    std::unique_lock<std::mutex> lk(g_mu);
    if (g_quit) return;
    g_reason = reason ? reason : "stop";
    g_handle_exception_on_resume = handle_exception_on_resume;
    g_stopped = true;
    g_waiting = true;
    ThreaderPauseAllThreads(false);
    g_cv.notify_all();
    g_cv.wait(lk, []{ return !g_waiting || g_quit; });
    ThreaderResumeAllThreads(false);
}

// Resume a callback blocked in pause_until_continue(). TitanEngine defaults
// debuggee-generated exceptions to DBG_EXCEPTION_NOT_HANDLED. The callback
// that classified the event may explicitly choose debugger handling instead.
static void resume_waiting_callback()
{
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_handle_exception_on_resume)
        SetNextDbgContinueStatus(DBG_CONTINUE);
    g_handle_exception_on_resume = false;
    g_waiting = false;
    g_cv.notify_all();
}

// exit-process handler: do NOT block (DebugLoop must be able to finish)
static void __cdecl cb_exit(void*)
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_reason = "exited";
    g_exited = true;
    g_stopped = true;
    g_waiting = false;
    g_cv.notify_all();
}

// stop handlers (TitanEngine calls them with one arg: &DBGEvent / exception record)
static void __cdecl cb_system_bp(void*) { pause_until_continue("initial-break"); }
static void __cdecl cb_single_step(void*) { pause_until_continue("single-step"); }
static void __cdecl cb_access_violation(void*)
{
    auto* de = (DEBUG_EVENT*)GetDebugData();
    if (de && de->dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
        g_exception_code = de->u.Exception.ExceptionRecord.ExceptionCode;
        g_exception_addr = (ULONG_PTR)de->u.Exception.ExceptionRecord.ExceptionAddress;
    }
    pause_until_continue("exception");
}
// catch-all for exception types we treat as stops (not UE_CH_EVERYTHINGELSE,
// which TitanEngine fires for every exception BEFORE the specific dispatch)
static void __cdecl cb_exception_stop(void*)
{
    auto* de = (DEBUG_EVENT*)GetDebugData();
    bool is_int3 = false;
    if (de && de->dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
        g_exception_code = de->u.Exception.ExceptionRecord.ExceptionCode;
        g_exception_addr = (ULONG_PTR)de->u.Exception.ExceptionRecord.ExceptionAddress;

        // Breakpoint disposition is debugger policy, not an engine heuristic.
        // Preserve TitanEngine's NOT_HANDLED default for RaiseException and
        // other exceptions, but make `continue` consume an executed short int3.
        if (g_exception_code == STATUS_BREAKPOINT) {
            g_hit_bp_id = 0;
            PROCESS_INFORMATION* pi = TitanGetProcessInformation();
            unsigned char opcode = 0;
            is_int3 = pi && MemoryReadSafe(pi->hProcess,
                                           (void*)g_exception_addr,
                                           &opcode, sizeof(opcode), nullptr) &&
                      opcode == 0xCC;
        }
    }
    pause_until_continue(is_int3 ? "breakpoint" : "exception", is_int3);
}

// our own breakpoint / step callbacks (TitanEngine calls no-arg ones)
//
// Decide whether to stop on a breakpoint hit. Finds the tracked Bpx that fired,
// bumps its hit counter and applies ignore-count / condition logic. Returns true
// when the debugger should pause; when false the callback simply returns and
// TitanEngine continues the target (this is how conditional filtering works).
static bool bp_hit(ULONG_PTR addr, int kind)
{
    Bpx* b = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_bp_mu);
        for (auto& kv : g_bps) {
            Bpx& c = kv.second;
            if (c.kind != kind) continue;
            if (kind == 3) {
                if (addr >= c.addr && addr < c.addr + c.memsize) { b = &c; break; }
            } else if (c.addr == addr) { b = &c; break; }
        }
        g_cond_failed = false;
        if (b) {
            b->hits++;
            g_hit_bp_id = b->id;
            g_hit_bp_hits = b->hits;
            g_hit_bp_oneshot = false;
            if (b->ignore > 0) { b->ignore--; g_hit_bp_id = 0; return false; }
            if (!b->condition.empty()) {
                bool v = false;
                if (eval_cond(b->condition, v)) {
                    if (!v) { g_hit_bp_id = 0; return false; }
                } else {
                    g_cond_failed = true;   // eval error: stop anyway (fail-safe)
                }
            }
            // we are going to stop on this breakpoint
            g_hit_bp_oneshot = b->oneshot;
            if (b->oneshot) {
                // GDB tbreak: remove after the first hit
                if (b->kind == 0 && b->addr) DeleteBPX(b->addr);
                else if (b->kind == 1) DeleteAPIBreakPoint(b->dll.c_str(), b->api.c_str(), UE_APISTART);
                else if (b->kind == 2) DeleteHardwareBreakPoint(b->hwreg);
                else if (b->kind == 3) RemoveMemoryBPX(b->addr, b->memsize);
                g_bps.erase(b->id);
            }
        } else {
            g_hit_bp_id = 0;
        }
    }
    return true;
}

static void __cdecl cb_bpx()
{
    ULONG_PTR rip = GetContextData(UE_CIP);
    if (bp_hit(rip, 0)) pause_until_continue("breakpoint");
}
static void __cdecl cb_hwbp(void*)
{
    ULONG_PTR rip = GetContextData(UE_CIP);
    if (bp_hit(rip, 2)) pause_until_continue("hardware");
}
static void __cdecl cb_membp(void*)
{
    auto* de = (DEBUG_EVENT*)GetDebugData();
    ULONG_PTR a = de ? (ULONG_PTR)de->u.Exception.ExceptionRecord.ExceptionAddress : 0;
    if (bp_hit(a, 3)) pause_until_continue("memory");
}
static void __cdecl cb_step()   { pause_until_continue("step"); }
static void __cdecl cb_attach() { pause_until_continue("attach"); }

// log-only handlers (DLL load/unload, thread create/exit, debug string)
static void __cdecl cb_log_event(void*)
{
    auto* de = (DEBUG_EVENT*)GetDebugData();
    if (!de) return;
    // exceptions are reported through the specific exception handlers
    if (de->dwDebugEventCode == EXCEPTION_DEBUG_EVENT) return;
    std::ostringstream o;
    switch (de->dwDebugEventCode) {
    case LOAD_DLL_DEBUG_EVENT: {
        o << "loaddll base=" << hex((ULONG_PTR)de->u.LoadDll.lpBaseOfDll);
        break;
    }
    case UNLOAD_DLL_DEBUG_EVENT:
        o << "unloaddll base=" << hex((ULONG_PTR)de->u.UnloadDll.lpBaseOfDll);
        break;
    case CREATE_THREAD_DEBUG_EVENT:
        o << "createthread tid=" << de->dwThreadId << " start=" << hex((ULONG_PTR)de->u.CreateThread.lpStartAddress);
        break;
    case EXIT_THREAD_DEBUG_EVENT:
        o << "exitthread tid=" << de->dwThreadId;
        break;
    case OUTPUT_DEBUG_STRING_EVENT:
        o << "outputdebugstring tid=" << de->dwThreadId;
        break;
    default:
        o << "event code=" << de->dwDebugEventCode;
    }
    std::lock_guard<std::mutex> lk(g_ev_mu);
    g_events.push_back(o.str());
    if (g_events.size() > 2000) g_events.erase(g_events.begin());
}

static void register_handlers()
{
    SetCustomHandler(UE_CH_SYSTEMBREAKPOINT, (void*)&cb_system_bp);
    // Leave UE_CH_BREAKPOINT unset. Stray STATUS_BREAKPOINT events flow through
    // UE_CH_UNHANDLEDEXCEPTION, where aidbg can choose the continue disposition
    // without changing TitanEngine's transparent default.
    SetCustomHandler(UE_CH_SINGLESTEP,       (void*)&cb_single_step);
    SetCustomHandler(UE_CH_ACCESSVIOLATION,  (void*)&cb_access_violation);
    SetCustomHandler(UE_CH_ILLEGALINSTRUCTION, (void*)&cb_exception_stop);
    SetCustomHandler(UE_CH_NONCONTINUABLEEXCEPTION, (void*)&cb_exception_stop);
    SetCustomHandler(UE_CH_ARRAYBOUNDSEXCEPTION, (void*)&cb_exception_stop);
    SetCustomHandler(UE_CH_FLOATDENORMALOPERAND, (void*)&cb_exception_stop);
    SetCustomHandler(UE_CH_FLOATDEVIDEBYZERO, (void*)&cb_exception_stop);
    SetCustomHandler(UE_CH_INTEGERDEVIDEBYZERO, (void*)&cb_exception_stop);
    SetCustomHandler(UE_CH_INTEGEROVERFLOW,   (void*)&cb_exception_stop);
    SetCustomHandler(UE_CH_PRIVILEGEDINSTRUCTION, (void*)&cb_exception_stop);
    SetCustomHandler(UE_CH_PAGEGUARD,         (void*)&cb_exception_stop);
    SetCustomHandler(UE_CH_EVERYTHINGELSE,    (void*)&cb_log_event);
    SetCustomHandler(UE_CH_UNHANDLEDEXCEPTION,(void*)&cb_exception_stop);
    SetCustomHandler(UE_CH_EXITPROCESS,       (void*)&cb_exit);
    SetCustomHandler(UE_CH_LOADDLL,           (void*)&cb_log_event);
    SetCustomHandler(UE_CH_UNLOADDLL,         (void*)&cb_log_event);
    SetCustomHandler(UE_CH_CREATETHREAD,      (void*)&cb_log_event);
    SetCustomHandler(UE_CH_EXITTHREAD,        (void*)&cb_log_event);
    SetCustomHandler(UE_CH_OUTPUTDEBUGSTRING, (void*)&cb_log_event);
}

// DebugLoop thread: InitDebugW and DebugLoop must run on the SAME thread
// (WaitForDebugEvent is tied to the thread that created the process).
static std::wstring g_init_target, g_init_cmdline;
static bool g_init_done = false, g_init_ok = false;

static void loop_thread_run()
{
    void* pi = InitDebugW(const_cast<wchar_t*>(g_init_target.c_str()),
                          g_init_cmdline.empty() ? nullptr : const_cast<wchar_t*>(g_init_cmdline.c_str()),
                          nullptr);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_init_done = true;
        g_init_ok = (pi != nullptr);
        g_running = g_init_ok;
        g_cv.notify_all();
    }
    if (g_init_ok) DebugLoop();
    std::lock_guard<std::mutex> lk(g_mu);
    g_running = false;
    if (!g_exited) g_reason = "ended";
    g_stopped = true;
    g_waiting = false;
    g_cv.notify_all();
}

// attach thread (AttachDebugger calls DebugLoop itself)
static void attach_thread_run(DWORD pid, bool kill)
{
    AttachDebugger(pid, kill, NULL, (void*)&cb_attach);
    std::lock_guard<std::mutex> lk(g_mu);
    g_running = false;
    if (!g_exited) g_reason = "ended";
    g_stopped = true;
    g_waiting = false;
    g_cv.notify_all();
}

// stop the debug session and join the loop thread (must be called from the REPL thread)
static void stop_session()
{
    if (!g_running) {
        if (g_loop_thread.joinable()) g_loop_thread.join();
        sym_end();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_quit = true;               // unblocks pause_until_continue
        g_waiting = false;
        g_cv.notify_all();
    }
    StopDebug();
    if (g_loop_thread.joinable()) g_loop_thread.join();
    sym_end();
}

static void reset_state()
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_stopped = false; g_waiting = false; g_exited = false; g_quit = false;
    g_reason.clear(); g_exception_code = 0; g_exception_addr = 0;
    g_handle_exception_on_resume = false;
    g_running = false;
    g_hit_bp_id = 0; g_hit_bp_hits = 0; g_cond_failed = false;
    g_hit_bp_oneshot = false;
    g_ctx_tid = 0; g_ctx_valid = false;
    std::lock_guard<std::mutex> lk2(g_ev_mu);
    g_events.clear();
    g_mods.clear();
}

// wait until the debugger stops at an event (or exits); consumes the flag
static std::string wait_stop_consume()
{
    std::unique_lock<std::mutex> lk(g_mu);
    g_cv.wait(lk, []{ return g_stopped; });
    g_stopped = false;
    std::string r = g_reason;
    return r;
}

// ----------------------------------------------------------- registers state ---

// thread that triggered the current debug stop
static DWORD ctx_event_tid()
{
    auto* de = (DEBUG_EVENT*)GetDebugData();
    return de ? de->dwThreadId : 0;
}

// fetch a thread's register context via the SDK (target must be paused)
static bool ctx_fetch(DWORD tid, CONTEXT& out)
{
    if (!tid) return false;
    HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME,
                           FALSE, tid);
    if (!th) return false;
    out = CONTEXT{};
    out.ContextFlags = CONTEXT_ALL;
    bool ok = GetThreadContext(th, &out) != FALSE;
    CloseHandle(th);
    return ok;
}

// build a CONTEXT from TitanEngine's cached context for the current event thread
// (we always compile for x64, so the CONTEXT is the AMD64 layout; 32-bit target
//  register names fall back to reg_get in ctx_reg below)
static CONTEXT ctx_from_titan()
{
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS;
    c.Rax = GetContextData(UE_RAX); c.Rbx = GetContextData(UE_RBX); c.Rcx = GetContextData(UE_RCX);
    c.Rdx = GetContextData(UE_RDX); c.Rdi = GetContextData(UE_RDI); c.Rsi = GetContextData(UE_RSI);
    c.Rbp = GetContextData(UE_RBP); c.Rsp = GetContextData(UE_RSP); c.Rip = GetContextData(UE_RIP);
    c.EFlags = (DWORD)GetContextData(UE_RFLAGS);
    c.R8 = GetContextData(UE_R8); c.R9 = GetContextData(UE_R9); c.R10 = GetContextData(UE_R10);
    c.R11 = GetContextData(UE_R11); c.R12 = GetContextData(UE_R12); c.R13 = GetContextData(UE_R13);
    c.R14 = GetContextData(UE_R14); c.R15 = GetContextData(UE_R15);
    c.SegGs = (WORD)GetContextData(SEG_IDX[0]); c.SegFs = (WORD)GetContextData(SEG_IDX[1]);
    c.SegEs = (WORD)GetContextData(SEG_IDX[2]); c.SegDs = (WORD)GetContextData(SEG_IDX[3]);
    c.SegCs = (WORD)GetContextData(SEG_IDX[4]); c.SegSs = (WORD)GetContextData(SEG_IDX[5]);
    // x64 user-mode segment selectors (RtlVirtualUnwind reads SegCs to decide kernel
    // vs user mode; a zero CS makes it treat us as kernel and crash)
    if (target_is64()) {
        if (!c.SegCs) c.SegCs = 0x33;
        if (!c.SegSs) c.SegSs = 0x2B;
        if (!c.SegDs) c.SegDs = 0x2B;
        if (!c.SegEs) c.SegEs = 0x2B;
        if (!c.SegFs) c.SegFs = 0x53;
        if (!c.SegGs) c.SegGs = 0x2B;
    }
    return c;
}

// CONTEXT of the thread whose registers we currently display (default: event thread)
static bool ctx_display(CONTEXT& out)
{
    DWORD evtid = ctx_event_tid();
    if (g_ctx_tid == 0 || g_ctx_tid == evtid) { out = ctx_from_titan(); return true; }
    if (g_ctx_valid) { out = g_ctx; return true; }
    return false;
}

// read one UE_* register out of a CONTEXT snapshot (32-bit target names fall back
// to TitanEngine's reg_get, which is the authoritative value for the event thread)
static ULONG_PTR ctx_reg(const CONTEXT& c, DWORD idx, bool is64)
{
    if (!is64) return reg_get(idx);
    switch (idx) {
        case UE_RAX: return c.Rax; case UE_RBX: return c.Rbx; case UE_RCX: return c.Rcx;
        case UE_RDX: return c.Rdx; case UE_RDI: return c.Rdi; case UE_RSI: return c.Rsi;
        case UE_RBP: return c.Rbp; case UE_RSP: return c.Rsp; case UE_RIP: return c.Rip;
        case UE_RFLAGS: return c.EFlags;
        case UE_R8: return c.R8; case UE_R9: return c.R9; case UE_R10: return c.R10;
        case UE_R11: return c.R11; case UE_R12: return c.R12; case UE_R13: return c.R13;
        case UE_R14: return c.R14; case UE_R15: return c.R15;
    }
    for (int i = 0; i < 6; i++) if (SEG_IDX[i] == idx) {
        switch (i) { case 0: return c.SegGs; case 1: return c.SegFs; case 2: return c.SegEs;
                     case 3: return c.SegDs; case 4: return c.SegCs; case 5: return c.SegSs; }
    }
    return 0;
}

// back to the debug-event thread view (called before continuing/stepping)
static void ctx_reset()
{
    g_ctx_tid = 0;
    g_ctx_valid = false;
}

static std::string regs_json()
{
    bool is64 = target_is64();
    CONTEXT c;
    if (!ctx_display(c)) return "{}";
    std::ostringstream o;
    o << "{";
    bool first = true;
    for (auto& r : REGS) {
        DWORD idx = is64 ? r.idx64 : r.idx32;
        if (!idx) continue;
        if (!first) o << ",";
        first = false;
        o << "\"" << (is64 ? r.n64 : r.n32) << "\":\"" << hex(ctx_reg(c, idx, is64)) << "\"";
    }
    o << "}";
    return o.str();
}

// GDB-style human-readable register table (non-JSON output)
static std::string regs_text()
{
    bool is64 = target_is64();
    CONTEXT c;
    if (!ctx_display(c)) return "";
    std::ostringstream o;
    for (auto& r : REGS) {
        DWORD idx = is64 ? r.idx64 : r.idx32;
        if (!idx) continue;
        ULONG_PTR v = ctx_reg(c, idx, is64);
        o << "  " << std::left << std::setw(8) << (is64 ? r.n64 : r.n32)
          << "  " << hex(v) << "  " << std::dec << (unsigned long long)v << "\n";
    }
    return o.str();
}

static std::string stop_json(const std::string& reason)
{
    auto* de = (DEBUG_EVENT*)GetDebugData();
    std::string thread = de ? std::to_string(de->dwThreadId) : "0";
    ULONG_PTR rip = reg_get(UE_CIP);
    std::ostringstream o;
    o << "{\"type\":\"stopped\",\"reason\":\"" << js_str(reason) << "\",\"thread\":" << thread
      << ",\"rip\":\"" << hex(rip) << "\",\"rip_symbol\":\"" << js_str(resolve(rip))
      << "\",\"registers\":" << regs_json();
    if (reason == "breakpoint" && g_hit_bp_id) {
        o << ",\"breakpoint_id\":" << g_hit_bp_id << ",\"hits\":" << g_hit_bp_hits;
        if (g_hit_bp_oneshot) o << ",\"temporary\":true";
        if (g_cond_failed) o << ",\"condition_error\":true";
    }
    if (reason == "exception") {
        o << ",\"exception\":{\"code\":\"0x" << std::hex << g_exception_code << std::dec
          << "\",\"address\":\"" << hex(g_exception_addr) << "\"}";
    }
    o << "}";
    return o.str();
}

static std::string stop_banner(const std::string& reason)
{
    std::ostringstream o;
    auto* de = (DEBUG_EVENT*)GetDebugData();
    std::string thread = de ? std::to_string(de->dwThreadId) : "?";
    if (reason == "breakpoint" && g_hit_bp_id) {
        // GDB-style: "Temporary breakpoint 1, main () at test_basic.c:29"
        o << (g_hit_bp_oneshot ? "Temporary breakpoint " : "Breakpoint ") << g_hit_bp_id << ", "
          << resolve_gdb(reg_get(UE_CIP))
          << (g_hit_bp_hits > 1 ? "  (hit " + std::to_string(g_hit_bp_hits) + ")" : "") << "\n";
        if (g_cond_failed) o << "  (warning: breakpoint condition failed to evaluate; stopped anyway)\n";
    }
    o << "Stopped: " << reason << "  [thread " << thread << "]\n";
    if (reason == "exception")
        o << "  exception 0x" << std::hex << g_exception_code << " at " << hex(g_exception_addr) << std::dec << "\n";
    o << "  rip = " << hex(reg_get(UE_CIP)) << "  (" << resolve(reg_get(UE_CIP)) << ")\n";
    o << "  registers:\n" << regs_text();
    return o.str();
}

static void emit_stop(const std::string& reason)
{
    sym_sync();   // pick up any DLLs loaded during execution so bt/resolve sees symbols
    if (g_silent) return;
    std::string out = g_json ? stop_json(reason) : stop_banner(reason);
    printf("%s\n", out.c_str());
    fflush(stdout);
    g_cond_failed = false;
    g_hit_bp_oneshot = false;
}

// ------------------------------------------------------------------ memory ---

static bool mem_read(ULONG_PTR addr, void* buf, SIZE_T n, SIZE_T* nr)
{
    PROCESS_INFORMATION* pi = TitanGetProcessInformation();
    if (!pi || !pi->hProcess) return false;
    return MemoryReadSafe(pi->hProcess, (void*)addr, buf, n, nr) != FALSE;
}

static bool mem_write(ULONG_PTR addr, const void* buf, SIZE_T n, SIZE_T* nw)
{
    PROCESS_INFORMATION* pi = TitanGetProcessInformation();
    if (!pi || !pi->hProcess) return false;
    return MemoryWriteSafe(pi->hProcess, (void*)addr, buf, n, nw) != FALSE;
}

// ---------------------------------------------------------------- modules ---

static void __cdecl enum_lib_cb(void* p);

static std::vector<ModInfo>& modules_refresh()
{
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    g_mods.clear();
    // main module first (Librarian list only tracks LOAD_DLL events)
    ULONG_PTR mainbase = GetDebuggedFileBaseAddress();
    if (mainbase) {
        ModInfo mi;
        mi.base = mainbase;
        mi.name = g_target.empty() ? "" : g_target.substr(g_target.find_last_of("\\/") + 1);
        mi.path = g_target;
        g_mods.push_back(mi);
    }
    LibrarianEnumLibraryInfoW((void*)&enum_lib_cb);
    std::sort(g_mods.begin(), g_mods.end(),
              [](const ModInfo& a, const ModInfo& b){ return a.base < b.base; });
    return g_mods;
}

static void __cdecl enum_lib_cb(void* p)
{
    auto* lib = (LIB_ITEM_W*)p;
    ModInfo mi;
    mi.base = (ULONG_PTR)lib->BaseOfDll;
    mi.name = utf8(lib->szLibraryName);
    mi.path = utf8(lib->szLibraryPath);
    g_mods.push_back(mi);
}

// ------------------------------------------------------ PDB symbol resolution ---

// register one module with dbghelp so SymFromAddr can resolve its PDB symbols
static void sym_load_module(ULONG_PTR base, const std::string& path)
{
    if (!g_sym_active || !g_sym_proc || !base) return;
    if (g_sym_loaded.count(base)) return;
    std::wstring wp;
    if (!path.empty()) {
        int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, NULL, 0);
        if (n > 1) {
            wp.resize(n - 1);
            MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wp[0], n);
        }
    }
    DWORD64 h = SymLoadModuleExW(g_sym_proc, NULL, wp.empty() ? NULL : wp.c_str(),
                                 NULL, (DWORD64)base, 0, NULL, 0);
    // modules present at init are already registered by fInvadeProcess; remember the
    // base so late-loaded DLLs are only handed over once
    g_sym_loaded.insert(base);
}

// refresh the module list and hand any new modules to dbghelp
static void sym_sync()
{
    if (!g_sym_active) return;
    const auto& mods = modules_refresh();
    for (auto& m : mods) sym_load_module(m.base, m.path);
}

// start symbol resolution for the current debugged process (call while stopped)
static void sym_begin(PROCESS_INFORMATION* pi)
{
    if (!pi || !pi->hProcess) return;
    if (g_sym_active && g_sym_proc == pi->hProcess) { sym_sync(); return; }
    sym_end();
    DWORD opts = SymGetOptions();
    opts |= SYMOPT_UNDNAME | SYMOPT_LOAD_LINES;
    opts &= ~SYMOPT_DEFERRED_LOADS;
    SymSetOptions(opts);
    if (SymInitializeW(pi->hProcess, NULL, TRUE)) {
        g_sym_active = true;
        g_sym_proc = pi->hProcess;
        g_sym_loaded.clear();
        sym_sync();
    }
}

static void sym_end()
{
    if (g_sym_active && g_sym_proc) SymCleanup(g_sym_proc);
    g_sym_active = false;
    g_sym_proc = NULL;
    g_sym_loaded.clear();
}

// Load PDB symbols for a target file BEFORE the target process exists, so that
// `break <symbol>` / `list <symbol>` work like GDB does (resolve from the symbol
// file on disk). We use the current process as the dbghelp host; once the target
// actually runs, sym_begin() re-initializes against the real process handle and
// re-resolves addresses (handling ASLR by rebasing).
static void sym_load_file(const std::string& path)
{
    if (path.empty()) return;
    sym_end();
    DWORD opts = SymGetOptions();
    opts |= SYMOPT_UNDNAME | SYMOPT_LOAD_LINES;
    opts &= ~SYMOPT_DEFERRED_LOADS;
    SymSetOptions(opts);
    HANDLE hHost = GetCurrentProcess();
    if (!SymInitializeW(hHost, NULL, TRUE)) return;
    g_sym_active = true;
    g_sym_proc = hHost;
    g_sym_loaded.clear();
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, NULL, 0);
    if (n > 1) {
        std::wstring wp(n - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wp[0], n);
        SymLoadModuleExW(hHost, NULL, wp.empty() ? NULL : wp.c_str(),
                         NULL, 0, 0, NULL, 0);
    }
}

// Resolve a symbol name to its address for display when no target is running.
// sym_load_file() must have been called so sym_lookup() works.
static bool sym_lookup_file(const std::string& name, ULONG_PTR& out)
{
    if (!g_sym_active || !g_sym_proc || name.empty()) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, NULL, 0);
    if (n <= 1) return false;
    std::wstring wname(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, &wname[0], n);
    char buf[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(WCHAR)];
    PSYMBOL_INFOW si = (PSYMBOL_INFOW)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFOW);
    si->MaxNameLen = MAX_SYM_NAME;
    if (SymFromNameW(g_sym_proc, wname.c_str(), si)) { out = (ULONG_PTR)si->Address; return true; }
    return false;
}

// resolve an address to "func+0x12 (file.c:line)" using PDB symbols, or "" if none
static std::string sym_resolve(ULONG_PTR addr)
{
    if (!g_sym_active || !g_sym_proc) return "";
    char buf[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(WCHAR)];
    PSYMBOL_INFOW si = (PSYMBOL_INFOW)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFOW);
    si->MaxNameLen = MAX_SYM_NAME;
    DWORD64 disp = 0;
    if (!SymFromAddrW(g_sym_proc, (DWORD64)addr, &disp, si)) return "";
    std::ostringstream o;
    o << utf8(si->Name);
    if (disp) o << "+0x" << std::hex << (ULONG_PTR)disp << std::dec;
    IMAGEHLP_LINEW64 li = {};
    li.SizeOfStruct = sizeof(IMAGEHLP_LINEW64);
    DWORD col = 0;
    if (SymGetLineFromAddrW64(g_sym_proc, (DWORD64)addr, &col, &li)) {
        std::string fn = utf8(li.FileName);
        size_t slash = fn.find_last_of("\\/");
        if (slash != std::string::npos) fn = fn.substr(slash + 1);
        o << " (" << fn << ":" << li.LineNumber << ")";
    }
    return o.str();
}

// look up a symbol name (or "module!func") and return its runtime address
struct SymLookupCtx { std::wstring want; ULONG_PTR hit = 0; };

static BOOL CALLBACK sym_lookup_all_cb(PSYMBOL_INFOW s, ULONG, PVOID c)
{
    auto* ctx = (SymLookupCtx*)c;
    if (!s) return TRUE;
    if (_wcsicmp(s->Name, ctx->want.c_str()) == 0) { ctx->hit = (ULONG_PTR)s->Address; return FALSE; }
    return TRUE;
}

static bool sym_lookup(const std::string& name, ULONG_PTR& out)
{
    if (!g_sym_active || !g_sym_proc || name.empty()) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, NULL, 0);
    if (n <= 1) return false;
    std::wstring wname(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, &wname[0], n);
    char buf[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(WCHAR)];
    PSYMBOL_INFOW si = (PSYMBOL_INFOW)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFOW);
    si->MaxNameLen = MAX_SYM_NAME;
    if (SymFromNameW(g_sym_proc, wname.c_str(), si)) { out = (ULONG_PTR)si->Address; return true; }
    // globals/statics are often absent from the SymFromName index: enumerate every
    // symbol and pick the first exact name match
    SymLookupCtx ctx{ wname, 0 };
    SymEnumSymbolsW(g_sym_proc, GetDebuggedFileBaseAddress(), NULL, sym_lookup_all_cb, &ctx);
    if (ctx.hit) { out = ctx.hit; return true; }
    return false;
}

// ----------------------------------------------------------- line locations ---
// GDB-style source-line breakpoints: `break 31` (current file) and
// `break file.c:31`. dbghelp resolves file:line to an address via
// SymGetLineFromNameW64; this works both before `run` (PDB preloaded with the
// current process as host) and while the target runs (real process handle).

// current source file of the stop location (or of `main` when nothing is stopped)
static std::string current_source_file()
{
    if (!g_sym_active || !g_sym_proc) return "";
    ULONG_PTR addr = 0;
    if (require_running()) {
        CONTEXT c;
        if (ctx_display(c)) addr = target_is64() ? (ULONG_PTR)c.Rip : (ULONG_PTR)reg_get(UE_EIP);
    }
    if (!addr) sym_lookup("main", addr);   // fallback: the file containing main
    if (!addr) return "";
    IMAGEHLP_LINEW64 li = {};
    li.SizeOfStruct = sizeof(li);
    DWORD col = 0;
    if (!SymGetLineFromAddrW64(g_sym_proc, (DWORD64)addr, &col, &li)) return "";
    return utf8(li.FileName);
}

// resolve a source line to its address; `file` empty means the current file.
// Cross-checks the returned line so an out-of-range request fails cleanly
// (dbghelp silently clamps to the last line of the file).
static bool resolve_line(const std::string& file, long line, ULONG_PTR& out)
{
    if (!g_sym_active || !g_sym_proc || line <= 0) return false;
    std::string f = file;
    if (f.empty()) f = current_source_file();
    if (f.empty()) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, f.c_str(), -1, NULL, 0);
    if (n <= 1) return false;
    std::wstring wf(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, f.c_str(), -1, &wf[0], n);
    IMAGEHLP_LINEW64 li = {};
    li.SizeOfStruct = sizeof(li);
    DWORD col = 0;
    // SymGetLineFromNameW64(hProcess, ModuleName, FileName, Line, Column, Line):
    // pass NULL module name so any loaded module is searched.
    if (!SymGetLineFromNameW64(g_sym_proc, NULL, wf.c_str(), (DWORD)line, (PLONG)&col, &li)) return false;
    // cross-check: SymGetLineFromNameW64 clamps an out-of-range request to the
    // last line of the file; reject unless it really is the requested line
    DWORD col2 = 0;
    IMAGEHLP_LINEW64 li2 = {};
    li2.SizeOfStruct = sizeof(li2);
    if (!SymGetLineFromAddrW64(g_sym_proc, li.Address, &col2, &li2)) return false;
    if ((long)li2.LineNumber != line) return false;
    out = (ULONG_PTR)li.Address;
    return out != 0;
}

// ----------------------------------------------------- source/PDB checksum ---
// Optional source-file verification (VS-style): compare the checksum recorded in
// the PDB with a local hash of the source file. `SymGetSourceFileChecksumW` returns
// the algorithm id in pCheckSumType (undocumented; empirically mapped on MSVC:
// 1=MD5(16B), 2=SHA1(20B), 3=SHA256(32B)). Local hashing uses BCrypt. Enabled via
// `set source-checksum on` (default off). Results are cached per file path and
// invalidated when size/mtime change.

static const char* cs_alg_name(DWORD type)
{
    switch (type) {
        case 1: return "MD5";
        case 2: return "SHA1";
        case 3: return "SHA256";
        default: return "unknown";
    }
}

// returns false when the algorithm is unsupported or the file cannot be read
static bool local_file_checksum(const std::wstring& path, DWORD type, std::vector<BYTE>& out)
{
    DWORD len = 0;
    LPCWSTR alg = NULL;
    switch (type) {
        case 1: len = 16; alg = BCRYPT_MD5_ALGORITHM; break;
        case 2: len = 20; alg = BCRYPT_SHA1_ALGORITHM; break;
        case 3: len = 32; alg = BCRYPT_SHA256_ALGORITHM; break;
        default: return false;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE ha = NULL;
    BCRYPT_HASH_HANDLE hh = NULL;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&ha, alg, NULL, 0));
    if (ok) ok = BCRYPT_SUCCESS(BCryptCreateHash(ha, &hh, NULL, 0, NULL, 0, 0));
    BYTE buf[65536];
    DWORD rd = 0;
    while (ok && ReadFile(h, buf, sizeof(buf), &rd, NULL) && rd)
        if (!BCRYPT_SUCCESS(BCryptHashData(hh, buf, rd, 0))) { ok = false; break; }
    if (ok) {
        out.resize(len);
        // BCryptFinishHash wants the exact digest length in cbOutput (64 is rejected)
        ok = BCRYPT_SUCCESS(BCryptFinishHash(hh, out.data(), len, 0));
    }
    if (hh) BCryptDestroyHash(hh);
    if (ha) BCryptCloseAlgorithmProvider(ha, 0);
    CloseHandle(h);
    return ok && out.size() == len;
}

// verify a source file against the PDB checksum; result is cached
static std::string source_checksum_verify(HANDLE hProc, ULONG64 base, const std::string& file,
                                         DWORD* out_type = NULL)
{
    if (!hProc || !g_sym_active || file.empty()) return "no-symbols";
    int n = MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, NULL, 0);
    if (n <= 1) return "bad-path";
    std::wstring wf(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, &wf[0], n);

    WIN32_FILE_ATTRIBUTE_DATA wfad;
    if (!GetFileAttributesExW(wf.c_str(), GetFileExInfoStandard, &wfad))
        return "file-not-found";
    ULONGLONG size = ((ULONGLONG)wfad.nFileSizeHigh << 32) | wfad.nFileSizeLow;

    auto it = g_cs_cache.find(file);
    if (it != g_cs_cache.end() && it->second.size == size &&
        CompareFileTime(&it->second.mtime, &wfad.ftLastWriteTime) == 0) {
        if (out_type) *out_type = it->second.type;
        return it->second.status;
    }

    DWORD type = 0, need = 0;
    if (!SymGetSourceFileChecksumW(hProc, base, wf.c_str(), &type, NULL, 0, &need) || !need)
        return "no-checksum";
    std::vector<BYTE> pdb(need);
    if (!SymGetSourceFileChecksumW(hProc, base, wf.c_str(), &type, pdb.data(), need, &need))
        return "pdb-error";

    std::string status;
    std::vector<BYTE> local;
    if (local_file_checksum(wf, type, local)) {
        status = (local.size() == pdb.size() && memcmp(local.data(), pdb.data(), local.size()) == 0)
                 ? "ok" : "mismatch";
    } else {
        status = cs_alg_name(type)[0] == 'u'
                 ? "unknown-algorithm(" + std::to_string(type) + ")" : "read-error";
    }
    CsCache c; c.status = status; c.type = type; c.mtime = wfad.ftLastWriteTime; c.size = size;
    g_cs_cache[file] = c;
    if (out_type) *out_type = type;
    return status;
}

// ------------------------------------------------------------------- expr eval ---
// Minimal C-like expression evaluator for breakpoint conditions. Supports:
//   0x.. / decimal literals, $registers, *addr dereference, identifiers (global
//   variables resolved via symbols, read from memory), + - * / % & | ^ ~ ! and
//   comparison / logical operators with parentheses.

static bool eval_cond(const std::string& expr, bool& out)
{
    struct P {
        const std::string& s;
        size_t p = 0;
        bool ok = true;
        explicit P(const std::string& str) : s(str) {}
        void skip() { while (p < s.size() && (s[p]==' '||s[p]=='\t')) p++; }
        bool fail() { ok = false; return false; }
        bool eof() { skip(); return p >= s.size(); }

        bool parse_or(unsigned long long& v) {
            if (!parse_and(v)) return false;
            for (;;) { skip(); if (p+1 < s.size() && s[p]=='|' && s[p+1]=='|') { p+=2; unsigned long long r; if(!parse_and(r)) return false; v = v || r; } else break; }
            return true;
        }
        bool parse_and(unsigned long long& v) {
            if (!parse_bitor(v)) return false;
            for (;;) { skip(); if (p+1 < s.size() && s[p]=='&' && s[p+1]=='&') { p+=2; unsigned long long r; if(!parse_bitor(r)) return false; v = v && r; } else break; }
            return true;
        }
        bool parse_bitor(unsigned long long& v) {
            if (!parse_bitxor(v)) return false;
            for (;;) { skip(); if (p < s.size() && s[p]=='|' && (p+1>=s.size()||s[p+1]!='|')) { p++; unsigned long long r; if(!parse_bitxor(r)) return false; v |= r; } else break; }
            return true;
        }
        bool parse_bitxor(unsigned long long& v) {
            if (!parse_bitand(v)) return false;
            for (;;) { skip(); if (p < s.size() && s[p]=='^') { p++; unsigned long long r; if(!parse_bitand(r)) return false; v ^= r; } else break; }
            return true;
        }
        bool parse_bitand(unsigned long long& v) {
            if (!parse_eq(v)) return false;
            for (;;) { skip(); if (p < s.size() && s[p]=='&' && (p+1>=s.size()||s[p+1]!='&')) { p++; unsigned long long r; if(!parse_eq(r)) return false; v &= r; } else break; }
            return true;
        }
        bool parse_eq(unsigned long long& v) {
            if (!parse_rel(v)) return false;
            for (;;) {
                skip();
                if (p+1 < s.size() && s[p]=='=' && s[p+1]=='=') { p+=2; unsigned long long r; if(!parse_rel(r)) return false; v = (v==r); }
                else if (p+1 < s.size() && s[p]=='!' && s[p+1]=='=') { p+=2; unsigned long long r; if(!parse_rel(r)) return false; v = (v!=r); }
                else break;
            }
            return true;
        }
        bool parse_rel(unsigned long long& v) {
            if (!parse_add(v)) return false;
            for (;;) {
                skip();
                if (p < s.size() && s[p]=='<') {
                    if (p+1 < s.size() && s[p+1]=='=') { p+=2; unsigned long long r; if(!parse_add(r)) return false; v = (v<=r); }
                    else { p++; unsigned long long r; if(!parse_add(r)) return false; v = (v<r); }
                } else if (p < s.size() && s[p]=='>') {
                    if (p+1 < s.size() && s[p+1]=='=') { p+=2; unsigned long long r; if(!parse_add(r)) return false; v = (v>=r); }
                    else { p++; unsigned long long r; if(!parse_add(r)) return false; v = (v>r); }
                } else break;
            }
            return true;
        }
        bool parse_add(unsigned long long& v) {
            if (!parse_mul(v)) return false;
            for (;;) {
                skip();
                if (p < s.size() && s[p]=='+') { p++; unsigned long long r; if(!parse_mul(r)) return false; v = (unsigned long long)((long long)v + (long long)r); }
                else if (p < s.size() && s[p]=='-') { p++; unsigned long long r; if(!parse_mul(r)) return false; v = (unsigned long long)((long long)v - (long long)r); }
                else break;
            }
            return true;
        }
        bool parse_mul(unsigned long long& v) {
            if (!parse_unary(v)) return false;
            for (;;) {
                skip();
                if (p < s.size() && s[p]=='*') { p++; unsigned long long r; if(!parse_unary(r)) return false; v = (unsigned long long)((long long)v * (long long)r); }
                else if (p < s.size() && s[p]=='/') { p++; unsigned long long r; if(!parse_unary(r)) return false; if(!r) return fail(); v /= r; }
                else if (p < s.size() && s[p]=='%') { p++; unsigned long long r; if(!parse_unary(r)) return false; if(!r) return fail(); v %= r; }
                else break;
            }
            return true;
        }
        bool parse_unary(unsigned long long& v) {
            skip();
            if (p >= s.size()) return fail();
            if (s[p]=='!') { p++; unsigned long long r; if(!parse_unary(r)) return false; v = !r; return true; }
            if (s[p]=='~') { p++; unsigned long long r; if(!parse_unary(r)) return false; v = ~r; return true; }
            if (s[p]=='-') { p++; unsigned long long r; if(!parse_unary(r)) return false; v = (unsigned long long)(-(long long)r); return true; }
            if (s[p]=='+') { p++; return parse_unary(v); }
            if (s[p]=='*') { p++; unsigned long long r; if(!parse_unary(r)) return false; SIZE_T n = sizeof(ULONG_PTR), nr = 0; if(!mem_read((ULONG_PTR)r, &v, n, &nr) || nr < n) return fail(); return true; }
            return parse_primary(v);
        }
        bool parse_primary(unsigned long long& v) {
            skip();
            if (p >= s.size()) return fail();
            if (s[p]=='(') { p++; if(!parse_or(v)) return false; skip(); if (p>=s.size()||s[p]!=')') return fail(); p++; return true; }
            if (s[p]=='$') {
                size_t e = p+1; while (e < s.size() && (isalnum((unsigned char)s[e]) || s[e]=='_')) e++;
                std::string rn = s.substr(p+1, e-p-1); p = e;
                DWORD idx = reg_index_for_name(rn, target_is64());
                if (!idx) return fail();
                v = GetContextData(idx); return true;
            }
            if (isdigit((unsigned char)s[p])) {
                size_t e = p; unsigned long long base = 10;
                if (s[p]=='0' && p+1 < s.size() && (s[p+1]=='x'||s[p+1]=='X')) { e = p+2; base = 16; }
                std::string tok;
                for (; e < s.size() && (isalnum((unsigned char)s[e]) || s[e]=='_'); e++) tok += s[e];
                p = e;
                try { v = std::stoull(tok, nullptr, (int)base); } catch (...) { return fail(); }
                return true;
            }
            if (isalpha((unsigned char)s[p]) || s[p]=='_') {
                size_t e = p; while (e < s.size() && (isalnum((unsigned char)s[e]) || s[e]=='_')) e++;
                std::string name = s.substr(p, e-p); p = e;
                if (name == "true") { v = 1; return true; }
                if (name == "false") { v = 0; return true; }
                ULONG_PTR addr = 0;
                if (!sym_lookup(name, addr)) return fail();
                SIZE_T n = sizeof(ULONG_PTR), nr = 0;
                if (!mem_read(addr, &v, n, &nr) || nr < n) return fail();
                return true;
            }
            return fail();
        }
    };
    P pa(expr);
    unsigned long long v = 0;
    if (!pa.parse_or(v) || !pa.eof()) return false;
    out = (v != 0);
    return true;
}

// --------------------------------------------- locals / args (SymEnumSymbols) ---
// BasicType values (cvconst.h) as returned by TI_GET_BASETYPE
enum { BT_VOID=1, BT_CHAR=2, BT_WCHAR=3, BT_INT=6, BT_UINT=7, BT_FLOAT=8, BT_BOOL=10,
       BT_LONG=13, BT_ULONG=14, BT_HRESULT=31, BT_CHAR16=32, BT_CHAR32=33, BT_CHAR8=34 };
// SymTagEnum / D3DATAKIND (cvconst.h)
enum { STAG_DATA=7, STAG_BASETYPE=16 };
enum { DK_LOCAL=1, DK_STATIC_LOCAL=2, DK_PARAM=3, DK_GLOBAL=6 };

struct ScopeVar {
    std::string name;
    ULONG_PTR addr;
    DWORD size;
    DWORD baseType;
    std::string typeName;
    bool isParam;
};

static std::string bt_name(DWORD bt)
{
    switch (bt) {
        case BT_VOID: return "void";
        case BT_CHAR: return "char"; case BT_CHAR8: return "char8_t";
        case BT_WCHAR: return "wchar_t"; case BT_CHAR16: return "char16_t"; case BT_CHAR32: return "char32_t";
        case BT_INT: return "int"; case BT_UINT: return "unsigned int";
        case BT_LONG: return "long"; case BT_ULONG: return "unsigned long";
        case BT_FLOAT: return "float"; case BT_BOOL: return "bool";
        case BT_HRESULT: return "HRESULT";
        default: return "";
    }
}

// chase a symbol's type to its leaf base type + size (through refs/typedefs/pointers)
static bool sym_base_type(HANDLE h, ULONG64 modBase, DWORD typeIndex, DWORD& bt, DWORD& len)
{
    DWORD tag = 0;
    if (!SymGetTypeInfo(h, modBase, typeIndex, TI_GET_SYMTAG, &tag)) return false;
    if (tag == STAG_BASETYPE) {
        if (!SymGetTypeInfo(h, modBase, typeIndex, TI_GET_BASETYPE, &bt)) return false;
        DWORD l = 0;
        if (SymGetTypeInfo(h, modBase, typeIndex, TI_GET_LENGTH, &l)) len = l;
        return true;
    }
    DWORD child = 0;
    if (SymGetTypeInfo(h, modBase, typeIndex, TI_GET_TYPE, &child) && child)
        return sym_base_type(h, modBase, child, bt, len);
    return false;
}

static BOOL CALLBACK enum_scope_cb(PSYMBOL_INFOW si, ULONG, PVOID ctx)
{
    auto* vars = (std::vector<ScopeVar>*)ctx;
    if (!si) return TRUE;
    bool isParam = (si->Flags & SYMFLAG_PARAMETER) != 0;
    bool isLocal = (si->Flags & SYMFLAG_LOCAL) != 0;
    if (!isParam && !isLocal) return TRUE;
    ScopeVar v;
    v.name = utf8(si->Name);
    v.addr = (ULONG_PTR)si->Address;
    v.isParam = isParam;
    v.size = 0; v.baseType = 0; v.typeName.clear();
    if (sym_base_type(g_sym_proc, si->ModBase, si->TypeIndex, v.baseType, v.size))
        v.typeName = bt_name(v.baseType);
    if (v.typeName.empty()) {
        if (v.size == 1) v.typeName = "byte";
        else if (v.size == 2) v.typeName = "word";
        else if (v.size == 4) v.typeName = "dword";
        else if (v.size == 8) v.typeName = "qword";
    }
    vars->push_back(v);
    return TRUE;
}

// read a variable's value from the frame base + (signed) offset, or the raw
// absolute address when dbghelp resolved it
static bool var_read(const ScopeVar& v, ULONG_PTR frameBase, bool is64, unsigned long long& out, ULONG_PTR& outaddr)
{
    if (!v.size) return false;
    SIZE_T n = v.size, nr = 0;
    if (n > sizeof out) n = sizeof out;
    ULONG_PTR a = v.addr;
    // dbghelp reports locals/params as offsets relative to the frame base
    ULONG_PTR cand = frameBase ? (ULONG_PTR)((LONG_PTR)frameBase + (LONG)v.addr) : v.addr;
    if (!cand) return false;
    if (mem_read(cand, &out, n, &nr) && nr >= n) { outaddr = cand; return true; }
    if (a && a != cand && a >= 0x10000) {
        if (mem_read(a, &out, n, &nr) && nr >= n) { outaddr = a; return true; }
    }
    return false;
}

static std::string resolve(ULONG_PTR addr)
{
    std::string s = sym_resolve(addr);
    if (!s.empty()) return s;
    if (g_mods.empty()) modules_refresh();
    for (auto& m : g_mods) {
        if (addr >= m.base && addr < m.base + 0x10000000) {
            return m.name + "+0x" + hex_brief(addr - m.base).substr(2);
        }
    }
    return "";
}

// GDB-style location for the stop banner: "func () at file.c:line" when symbols
// are available, else fall back to resolve().  e.g.
//   Temporary breakpoint 1, main () at test_basic.c:29
static std::string resolve_gdb(ULONG_PTR addr)
{
    if (g_sym_active && g_sym_proc) {
        char buf[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(WCHAR)];
        PSYMBOL_INFOW si = (PSYMBOL_INFOW)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFOW);
        si->MaxNameLen = MAX_SYM_NAME;
        DWORD64 disp = 0;
        if (SymFromAddrW(g_sym_proc, (DWORD64)addr, &disp, si)) {
            IMAGEHLP_LINEW64 li = {};
            li.SizeOfStruct = sizeof(IMAGEHLP_LINEW64);
            DWORD col = 0;
            std::ostringstream o;
            o << utf8(si->Name) << " ()";
            if (SymGetLineFromAddrW64(g_sym_proc, (DWORD64)addr, &col, &li)) {
                std::string fn = utf8(li.FileName);
                size_t slash = fn.find_last_of("\\/");
                if (slash != std::string::npos) fn = fn.substr(slash + 1);
                o << " at " << fn << ":" << li.LineNumber;
            }
            return o.str();
        }
    }
    return resolve(addr);
}


// ------------------------------------------------------------------ disasm ---

struct Insn { ULONG_PTR addr; int len; std::string text; std::string bytes; };

static std::vector<Insn> disasm(ULONG_PTR addr, int count)
{
    std::vector<Insn> out;
    PROCESS_INFORMATION* pi = TitanGetProcessInformation();
    if (!pi || !pi->hProcess) return out;
    ULONG_PTR a = addr;
    for (int i = 0; i < count; i++) {
        void* s = DisassembleEx(pi->hProcess, (void*)a, false);
        long ln = LengthDisassembleEx(pi->hProcess, (void*)a);
        if (!s || ln <= 0) break;
        Insn in;
        in.addr = a;
        in.len = (int)ln;
        in.text = (char*)s;
        std::vector<unsigned char> bytes(ln);
        SIZE_T nr = 0;
        if (mem_read(a, bytes.data(), ln, &nr)) {
            std::ostringstream o;
            for (size_t k = 0; k < nr && k < 16; k++) {
                char b[4]; snprintf(b,sizeof b,"%02x ", bytes[k]); o << b;
            }
            in.bytes = o.str();
        }
        out.push_back(in);
        a += ln;
    }
    return out;
}

static std::string disasm_json(const std::vector<Insn>& ins)
{
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < ins.size(); i++) {
        if (i) o << ",";
        o << "{\"address\":\"" << hex(ins[i].addr) << "\",\"bytes\":\""
          << js_str(ins[i].bytes) << "\",\"text\":\"" << js_str(ins[i].text) << "\"}";
    }
    o << "]";
    return o.str();
}

static std::string disasm_text(const std::vector<Insn>& ins)
{
    std::ostringstream o;
    for (auto& in : ins) {
        o << "  " << hex(in.addr) << "  " << std::left << std::setw(24) << in.bytes
          << in.text << "\n";
    }
    return o.str();
}

// ------------------------------------------------------------------ parsing ---

static std::vector<std::string> tokenize(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    bool inq = false;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '"') { inq = !inq; continue; }
        if (!inq && (c == ' ' || c == '\t')) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// parse "0x..", decimal, $reg, *addr, addr+off
static bool parse_addr(const std::string& tok, ULONG_PTR& out)
{
    std::string t = tok;
    bool is64 = target_is64();
    if (t.size() > 1 && t[0] == '*') {
        ULONG_PTR base = 0;
        if (!parse_addr(t.substr(1), base)) return false;
        ULONG_PTR val = 0;
        SIZE_T nr = 0;
        if (!mem_read(base, &val, sizeof(val), &nr)) return false;
        out = val;
        return true;
    }
    // module!offset  (e.g. test_target!0x11a0)
    size_t bang = t.find('!');
    if (bang != std::string::npos) {
        std::string mod = t.substr(0, bang);
        ULONG_PTR off = 0;
        if (!parse_addr(t.substr(bang + 1), off)) return false;
        const auto& mods = modules_refresh();
        for (auto& m : mods) {
            if (_stricmp(m.name.c_str(), mod.c_str()) == 0) {
                out = m.base + off;
                return true;
            }
            // also accept the module name without its extension
            std::string base = m.name;
            size_t dot = base.rfind('.');
            if (dot != std::string::npos) base.resize(dot);
            if (_stricmp(base.c_str(), mod.c_str()) == 0) {
                out = m.base + off;
                return true;
            }
        }
        return false;
    }
    if (t.size() > 1 && t[0] == '$') {
        DWORD idx = reg_index_for_name(t.substr(1), is64);
        if (!idx) return false;
        out = reg_get(idx);
        return true;
    }
    // strip optional '+' expression "addr+0x10"
    size_t plus = t.find('+');
    if (plus != std::string::npos) {
        ULONG_PTR a = 0, b = 0;
        if (parse_addr(t.substr(0, plus), a) && parse_addr(t.substr(plus + 1), b)) {
            out = a + b;
            return true;
        }
        return false;
    }
    try {
        size_t pos = 0;
        if (t.rfind("0x", 0) == 0) out = std::stoull(t, &pos, 16);
        else out = std::stoull(t, &pos, 10);
        return pos == t.size();
    } catch (...) { return false; }
}

// ------------------------------------------------------------------ commands ---

struct CmdResult {
    bool ok = true;
    std::string err;
    std::string text;   // human output (empty => nothing)
    std::string js;     // json "result" payload (empty => nothing)
};

static void print_result(const CmdResult& r)
{
    if (g_silent) {
        if (!r.ok) { fprintf(stderr, "error: %s\n", r.err.c_str()); fflush(stderr); }
        return;
    }
    if (g_json) {
        std::ostringstream o;
        if (r.ok) {
            o << "{\"ok\":true";
            if (!r.js.empty()) o << ",\"result\":" << r.js;
            o << "}";
        } else {
            o << "{\"ok\":false,\"error\":\"" << js_str(r.err) << "\"}";
        }
        printf("%s\n", o.str().c_str());
    } else if (r.ok) {
        if (!r.text.empty()) printf("%s\n", r.text.c_str());
    } else {
        printf("error: %s\n", r.err.c_str());
    }
    fflush(stdout);
}

static bool require_running()
{
    std::lock_guard<std::mutex> lk(g_mu);
    return g_running && !g_exited;
}

// errors that are informational in batch mode (like GDB's "No stack." /
// "The program is not being run.") and should not fail the batch exit code
static bool soft_error(const std::string& err)
{
    return err.find("no active debug session") != std::string::npos
        || err.find("target is not stopped") != std::string::npos;
}

// -- info locals / info args (GDB) --

// Compute the current frame's base (post-prolog RSP, or RBP when a frame pointer is
// used) via RtlVirtualUnwind. MSVC x64 addresses locals relative to RSP, so without
// this the variable offsets dbghelp reports are relative to 0 and unreadable.
// The unwind call is wrapped in SEH because it can fault on unusual frames; on
// failure the caller falls back to the current RSP.
static bool frame_base_rsp(CONTEXT& c, bool is64, ULONG_PTR& out)
{
    static auto RtlVirtualUnwindFn = (DWORD (WINAPI*)(ULONG, ULONG64, ULONG64, PRUNTIME_FUNCTION,
        PCONTEXT, PVOID*, PULONG64, PKNONVOLATILE_CONTEXT_POINTERS))
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlVirtualUnwind");
    if (!RtlVirtualUnwindFn || !g_sym_proc) return false;
    ULONG64 pc = is64 ? c.Rip : (ULONG64)reg_get(UE_EIP);
    PVOID entry = SymFunctionTableAccess64(g_sym_proc, pc);
    ULONG64 imgBase = SymGetModuleBase64(g_sym_proc, pc);
    if (!entry || !imgBase) return false;
    ULONG64 est = 0;
    __try {
        DWORD st = RtlVirtualUnwindFn(UNW_FLAG_NHANDLER, imgBase, pc,
                                      (PRUNTIME_FUNCTION)entry, &c, nullptr, &est, nullptr);
        if (st != ERROR_SUCCESS || !est) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    out = (ULONG_PTR)est;
    return true;
}

static CmdResult cmd_info_vars(bool args_only)
{
    if (!require_running()) return {false, "no active debug session"};
    if (!g_sym_active || !g_sym_proc) return {false, "no symbols loaded"};
    bool is64 = target_is64();
    CONTEXT c;
    if (!ctx_display(c)) return {false, "cannot read thread context"};

    // The frame base dbghelp should use as the reference point for variable offsets.
    ULONG_PTR frameBase = 0;
    if (frame_base_rsp(c, is64, frameBase)) {
        // fall through; frameBase is the established post-prolog frame pointer
    } else {
        frameBase = is64 ? (ULONG_PTR)c.Rsp : (ULONG_PTR)reg_get(UE_ESP);
    }

    IMAGEHLP_STACK_FRAME sf = {};
    sf.InstructionOffset = is64 ? c.Rip : (ULONG64)reg_get(UE_EIP);
    sf.FrameOffset = frameBase;
    sf.StackOffset = is64 ? c.Rsp : (ULONG64)reg_get(UE_ESP);

    bool scoped = false;
    if (SymSetContext(g_sym_proc, &sf, nullptr)) scoped = true;
    else if (SymSetScopeFromAddr(g_sym_proc, sf.InstructionOffset)) scoped = true;
    if (!scoped) return {false, "SymSetContext failed"};
    std::vector<ScopeVar> vars;
    if (!SymEnumSymbolsW(g_sym_proc, 0, nullptr, enum_scope_cb, &vars))
        return {false, "SymEnumSymbols failed"};

    // are we at the very first instruction of the enclosing function?
    bool atEntry = false;
    if (is64) {
        char b[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(WCHAR)];
        PSYMBOL_INFOW si = (PSYMBOL_INFOW)b;
        si->SizeOfStruct = sizeof(SYMBOL_INFOW); si->MaxNameLen = MAX_SYM_NAME;
        DWORD64 disp = 0;
        if (SymFromAddrW(g_sym_proc, (DWORD64)c.Rip, &disp, si))
            atEntry = (disp == 0);
    }

    // params are reported in home-slot order -> matches x64 register order
    std::vector<ScopeVar*> params;
    std::vector<ScopeVar*> locals;
    for (auto& v : vars) { (v.isParam ? params : locals).push_back(&v); }
    std::sort(params.begin(), params.end(), [](ScopeVar* x, ScopeVar* y){ return x->addr < y->addr; });
    std::sort(locals.begin(), locals.end(), [](ScopeVar* x, ScopeVar* y){ return x->addr < y->addr; });

    static const DWORD ARG_REGS[4] = { UE_RCX, UE_RDX, UE_R8, UE_R9 };

    std::ostringstream t, j;
    j << "[";
    int shown = 0;
    std::vector<ScopeVar*> sel = args_only ? params : locals;
    for (size_t idx = 0; idx < sel.size(); idx++) {
        ScopeVar* v = sel[idx];
        unsigned long long val = 0;
        ULONG_PTR at = 0;
        std::string valstr = "<unreadable>";
        bool ok = false;
        if (args_only && v->isParam && atEntry && idx < 4) {
            ok = true;   // value lives in the argument register at function entry
            val = GetContextData(ARG_REGS[idx]);
        } else if (var_read(*v, frameBase, is64, val, at)) {
            ok = true;
        }
        if (ok) valstr = hex((ULONG_PTR)val);
        if (shown) { t << "\n"; j << ","; }
        shown++;
        j << "{\"name\":\"" << js_str(v->name) << "\",\"type\":\"" << js_str(v->typeName)
          << "\",\"value\":\"" << js_str(valstr) << "\"}";
        t << "  " << std::left << std::setw(24) << v->name
          << std::setw(12) << v->typeName << " = " << valstr;
    }
    j << "]";
    if (!shown) {
        CmdResult cr;
        cr.js = "[]";
        cr.text = "no " + std::string(args_only ? "arguments" : "locals") + " (or no symbols)";
        return cr;
    }
    CmdResult cr;
    cr.js = j.str();
    cr.text = t.str();
    return cr;
}

// -- run / start --
static CmdResult cmd_run(const std::vector<std::string>& args, const std::string& raw_args, bool gdb_start = false)
{
    if (g_target.empty()) return {false, "no target file; use: file <path>"};
    if (g_running) { ForceClose(); if (g_loop_thread.joinable()) g_loop_thread.join(); }
    reset_state();

    // engine options
    SetEngineVariable(UE_ENGINE_PASS_ALL_EXCEPTIONS, false);
    SetEngineVariable(UE_ENGINE_DISABLE_ASLR, false);

    register_handlers();

    // GDB semantics: a bare "run" uses the inferior args from --args / "set args"
    std::string cmdline = !raw_args.empty() ? raw_args : g_default_args;
    g_init_target = g_target_w;
    g_init_cmdline = !cmdline.empty() ? std::wstring(cmdline.begin(), cmdline.end()) : std::wstring();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_init_done = false;
        g_init_ok = false;
    }
    g_loop_thread = std::thread(loop_thread_run);

    // wait for InitDebugW to finish (may fail, e.g. bad path)
    {
        std::unique_lock<std::mutex> lk(g_mu);
        g_cv.wait(lk, []{ return g_init_done; });
    }
    if (!g_init_ok) { if (g_loop_thread.joinable()) g_loop_thread.join(); return {false, "InitDebugW failed to create the target process"}; }

    std::string reason = wait_stop_consume();

    // Load symbols for the live process and arm breakpoints that were set
    // before `run` (GDB pending-breakpoint behaviour). Must happen before the
    // batch skip-loop below, otherwise run() would continue straight to exit.
    if (reason == "initial-break") {
        sym_begin(TitanGetProcessInformation());
        apply_pending_bps();
    }

    if (reason == "initial-break" && g_batch && !gdb_start) {
        // GDB batch semantics: run() continues past the internal initial system
        // breakpoint and only stops at a real breakpoint, a crash, or process exit.
        while (reason == "initial-break") {
            {
                std::lock_guard<std::mutex> lk(g_mu);
                g_waiting = false;
                g_cv.notify_all();
            }
            reason = wait_stop_consume();
        }
        if (reason == "exited" || reason == "ended") {
            if (g_json) printf("{\"type\":\"exited\",\"code\":%ld}\n", GetExitCode());
            else if (!g_silent) printf("Process exited with code %ld\n", GetExitCode());
            fflush(stdout);
            return {};
        }
    }
    if (!g_json && !g_silent) printf("Target started. pid=%lu base=%s\n",
        TitanGetProcessInformation()->dwProcessId,
        hex(GetDebuggedFileBaseAddress()).c_str());

    // GDB `start`: stop at the program's entry function (main/WinMain/...) via a
    // one-shot breakpoint, instead of pausing at the internal system breakpoint.
    // `start <func>` stops at the given function. Symbols are loaded by
    // sym_begin(); we are stopped at the initial break here, so the PDB is
    // available for sym_lookup().
    if (gdb_start && reason == "initial-break") {
        sym_begin(TitanGetProcessInformation());
        apply_pending_bps();
        ULONG_PTR entry = 0;
        std::string want = args.size() > 1 ? args[1] : "";
        if (!want.empty()) {
            if (!sym_lookup(want, entry))
                entry = 0;
        } else {
            const char* entry_names[] = { "main", "wmain", "WinMain", "wWinMain", "_main" };
            for (auto n : entry_names) {
                if (sym_lookup(n, entry)) break;
            }
        }
        if (!entry) {
            // GDB: "No symbol \"main\" in current context." Leave the target
            // paused at the initial break so the user can continue manually.
            emit_stop(reason);
            return {false, "No symbol \"" + (want.empty() ? std::string("main") : want)
                           + "\" in current context. (start: entry symbol not found)"};
        }
        if (!SetBPX(entry, UE_BREAKPOINT | UE_BREAKPOINT_TYPE_INT3, (void*)&cb_bpx))
            return {false, "SetBPX failed for entry symbol"};
        Bpx b; b.id = g_bp_next_id++; b.kind = 0; b.addr = entry; b.enabled = true;
        b.symbol = want.empty() ? "main" : want; b.oneshot = true;
        g_bps[b.id] = b;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_waiting = false;
            g_cv.notify_all();
        }
        reason = wait_stop_consume();
        if (reason == "exited" || reason == "ended") {
            if (g_json) printf("{\"type\":\"exited\",\"code\":%ld}\n", GetExitCode());
            else if (!g_silent) printf("Process exited with code %ld\n", GetExitCode());
            fflush(stdout);
            return {};
        }
        emit_stop(reason);
        return {};
    }

    emit_stop(reason);
    return {};
}

// -- attach --
static CmdResult cmd_attach(DWORD pid)
{
    if (g_running) return {false, "already debugging a process"};
    reset_state();
    register_handlers();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_running = true;
    }
    g_loop_thread = std::thread(attach_thread_run, pid, true);
    std::string reason = wait_stop_consume();
    sym_begin(TitanGetProcessInformation());
    emit_stop(reason);
    return {};
}

// -- detach --
static CmdResult cmd_detach()
{
    if (!require_running()) return {false, "no active debug session"};
    PROCESS_INFORMATION* pi = TitanGetProcessInformation();
    DWORD pid = pi ? pi->dwProcessId : 0;
    DetachDebuggerEx(pid);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_waiting = false;
        g_cv.notify_all();
    }
    std::string reason = wait_stop_consume();
    if (reason != "exited") {
        if (g_json) printf("{\"type\":\"detached\",\"pid\":%lu}\n", pid);
        else if (!g_silent) printf("Detached from process %lu\n", pid);
    }
    if (g_loop_thread.joinable()) g_loop_thread.join();
    sym_end();
    return {};
}

// -- continue --
static CmdResult cmd_continue()
{
    if (!require_running()) return {false, "no active debug session"};
    if (!g_waiting) return {false, "target is not stopped"};
    if (g_json) printf("{\"type\":\"running\"}\n"); else if (!g_silent) printf("Continuing.\n");
    fflush(stdout);
    ctx_reset();
    resume_waiting_callback();
    std::string reason = wait_stop_consume();
    if (reason == "exited") {
        if (g_json) printf("{\"type\":\"exited\",\"code\":%ld}\n", GetExitCode());
        else if (!g_silent) printf("Process exited with code %ld\n", GetExitCode());
        fflush(stdout);
        return {};
    }
    if (reason == "ended") {
        if (g_json) printf("{\"type\":\"exited\",\"code\":%ld}\n", GetExitCode());
        else if (!g_silent) printf("Debug session ended\n");
        fflush(stdout);
        return {};
    }
    emit_stop(reason);
    return {};
}

// -- stepi / nexti / finish --
// get the enclosing function symbol name for an address ("" if none)
static std::string sym_func_name(ULONG_PTR addr)
{
    if (!g_sym_active || !g_sym_proc) return "";
    char buf[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(WCHAR)];
    PSYMBOL_INFOW si = (PSYMBOL_INFOW)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFOW);
    si->MaxNameLen = MAX_SYM_NAME;
    DWORD64 disp = 0;
    if (!SymFromAddrW(g_sym_proc, (DWORD64)addr, &disp, si)) return "";
    return utf8(si->Name);
}

static CmdResult cmd_step(const char* kind, int n)
{
    if (!require_running()) return {false, "no active debug session"};
    if (!g_waiting) return {false, "target is not stopped"};
    if (n < 1) n = 1;
    // GDB `finish`: run to the return point of the current function, then stop
    // at the instruction AFTER the call in the caller. TitanEngine's StepOut
    // stops on the callee's `ret`; step into it so we land in the caller.
    std::string start_func;
    if (strcmp(kind, "out") == 0) start_func = sym_func_name(reg_get(UE_CIP));
    std::string reason;
    for (int i = 0; i < n; i++) {
        ctx_reset();
        if (strcmp(kind, "into") == 0) StepInto((void*)&cb_step);
        else if (strcmp(kind, "over") == 0) StepOver((void*)&cb_step);
        else StepOut((void*)&cb_step, false);
        resume_waiting_callback();
        reason = wait_stop_consume();
        if (reason == "exited") break;
        if (reason == "ended") break;
    }
    // finish: if we are still inside the same function (on the ret), step once
    // so rip points at the caller's next instruction (GDB semantics).
    if (strcmp(kind, "out") == 0 && !start_func.empty()) {
        for (int i = 0; i < 3 && reason != "exited" && reason != "ended"; i++) {
            if (sym_func_name(reg_get(UE_CIP)) != start_func) break;
            ctx_reset();
            StepInto((void*)&cb_step);
            resume_waiting_callback();
            reason = wait_stop_consume();
        }
    }
    if (reason == "exited") {
        if (g_json) printf("{\"type\":\"exited\",\"code\":%ld}\n", GetExitCode());
        else if (!g_silent) printf("Process exited with code %ld\n", GetExitCode());
        fflush(stdout);
        return {};
    }
    if (reason == "ended") {
        if (g_json) printf("{\"type\":\"exited\",\"code\":%ld}\n", GetExitCode());
        else if (!g_silent) printf("Debug session ended\n");
        fflush(stdout);
        return {};
    }
    emit_stop(reason);
    return {};
}

// -- break --
static std::string bp_list_json();
static CmdResult cmd_break(const std::vector<std::string>& args)
{
    if (args.empty()) {
        // list breakpoints
        return {true, "", bp_list_json()};
    }
    std::string spec = args[0];
    // GDB syntax: break *ADDR means "break at this address" (not a dereference)
    if (!spec.empty() && spec[0] == '*') spec = spec.substr(1);

    // GDB source-line breakpoints: `break 31` (current file) / `break file.c:31`
    {
        std::string linefile;
        long lineno = 0;
        bool isline = false;
        // file.c:NN  -- split on the last ':' when the tail is all digits
        size_t colon = spec.rfind(':');
        if (colon != std::string::npos && colon + 1 < spec.size()) {
            std::string tail = spec.substr(colon + 1);
            if (!tail.empty() && tail.find_first_not_of("0123456789") == std::string::npos) {
                linefile = spec.substr(0, colon);
                lineno = strtol(tail.c_str(), nullptr, 0);
                isline = true;
            }
        } else if (!spec.empty() && spec.find_first_not_of("0123456789") == std::string::npos) {
            // bare number = line of the current source file (do NOT treat 0x.. as a line)
            if (spec.find("0x") != 0 && spec.find("0X") != 0) {
                lineno = strtol(spec.c_str(), nullptr, 0);
                isline = true;
            }
        }
        if (isline) {
            ULONG_PTR la = 0;
            if (!resolve_line(linefile, lineno, la)) {
                std::string where = linefile.empty() ? current_source_file() : linefile;
                return {false, "No line " + std::to_string(lineno) + (where.empty() ? "" : " in " + where)
                               + " (no line info; ensure the target has PDB line numbers)"};
            }
            bool running = require_running();
            if (running) {
                bool r = SetBPX(la, UE_BREAKPOINT | UE_BREAKPOINT_TYPE_INT3, (void*)&cb_bpx);
                if (!r) return {false, "SetBPX failed"};
            }
            Bpx b; b.id = g_bp_next_id++; b.kind = 0; b.addr = la; b.enabled = true;
            b.symbol = spec; b.file = linefile; b.line = lineno;
            b.pending = !running;
            g_bps[b.id] = b;
            std::string disp = linefile.empty() ? (current_source_file() + ":" + std::to_string(lineno))
                                                : spec;
            CmdResult cr; cr.ok = true;
            cr.js = "{\"breakpoint\":{\"id\":" + std::to_string(b.id) + ",\"kind\":\"code\",\"address\":\"" + hex(la)
                  + "\",\"file\":\"" + js_str(b.file) + "\",\"line\":" + std::to_string(lineno)
                  + (b.pending ? ",\"pending\":true" : "") + "}}";
            cr.text = "Breakpoint " + std::to_string(b.id) + " at " + disp
                    + (b.pending ? " (pending: applied on run)" : "");
            if (!b.pending && g_source_checksum) {
                std::string srcfile = linefile.empty() ? current_source_file() : linefile;
                if (!linefile.empty()) {
                    // resolve the short name to the PDB-recorded full path
                    IMAGEHLP_LINEW64 li2 = {};
                    li2.SizeOfStruct = sizeof(li2);
                    DWORD col2 = 0;
                    if (SymGetLineFromAddrW64(g_sym_proc, (DWORD64)la, &col2, &li2))
                        srcfile = utf8(li2.FileName);
                }
                ULONG64 base = SymGetModuleBase64(g_sym_proc, (DWORD64)la);
                std::string status = source_checksum_verify(g_sym_proc, base, srcfile);
                if (status != "ok") cr.text += "  [!! Checksum mismatch: " + srcfile + " (" + status + ")]";
            }
            return cr;
        }
    }

    ULONG_PTR addr = 0;
    bool ok = parse_addr(spec, addr);
    if (ok && addr) {
        // SetBPX needs the debugged process; before `run` the breakpoint is pending
        bool running = require_running();
        if (running) {
            bool r = SetBPX(addr, UE_BREAKPOINT | UE_BREAKPOINT_TYPE_INT3, (void*)&cb_bpx);
            if (!r) return {false, "SetBPX failed"};
        }
        Bpx b; b.id = g_bp_next_id++; b.kind = 0; b.addr = addr; b.enabled = true; b.symbol = spec;
        b.pending = !running;
        g_bps[b.id] = b;
        CmdResult cr; cr.ok = true;
        cr.js = "{\"breakpoint\":{\"id\":" + std::to_string(b.id) + ",\"kind\":\"code\",\"address\":\"" + hex(addr) + "\""
              + (b.pending ? ",\"pending\":true" : "") + "}}";
        cr.text = "Breakpoint " + std::to_string(b.id) + " at " + hex(addr)
                + (b.pending ? " (pending: applied on run)" : "");
        return cr;
    }
    // PDB symbol name: break boom  or  break symtest!boom
    if (sym_lookup(spec, addr) && addr) {
        bool running = require_running();
        if (running) {
            bool r = SetBPX(addr, UE_BREAKPOINT | UE_BREAKPOINT_TYPE_INT3, (void*)&cb_bpx);
            if (!r) return {false, "SetBPX failed"};
        }
        Bpx b; b.id = g_bp_next_id++; b.kind = 0; b.addr = addr; b.enabled = true; b.symbol = spec;
        b.pending = !running;
        g_bps[b.id] = b;
        CmdResult cr; cr.ok = true;
        cr.js = "{\"breakpoint\":{\"id\":" + std::to_string(b.id) + ",\"kind\":\"code\",\"address\":\""
              + hex(addr) + "\",\"symbol\":\"" + js_str(spec) + "\""
              + (b.pending ? ",\"pending\":true" : "") + "}}";
        cr.text = "Breakpoint " + std::to_string(b.id) + " at " + spec + " (" + hex(addr) + ")"
                + (b.pending ? " (pending: applied on run)" : "");
        return cr;
    }
    // API breakpoint: module!api
    size_t bang = spec.find('!');
    if (bang != std::string::npos) {
        std::string dll = spec.substr(0, bang);
        std::string api = spec.substr(bang + 1);
        std::string dllname = dll;
        std::string lower = dllname;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.size() < 4 || lower.substr(lower.size() - 4) != ".dll") dllname += ".dll";
        bool r = SetAPIBreakPoint(dllname.c_str(), api.c_str(), UE_BREAKPOINT | UE_BREAKPOINT_TYPE_INT3,
                                   UE_APISTART, (void*)&cb_bpx);
        if (!r) return {false, "SetAPIBreakPoint failed"};
        Bpx b; b.id = g_bp_next_id++; b.kind = 1; b.dll = dllname; b.api = api; b.enabled = true; b.addr = 0; b.symbol = spec;
        g_bps[b.id] = b;
        CmdResult cr; cr.ok = true;
        cr.js = "{\"breakpoint\":{\"id\":" + std::to_string(b.id) + ",\"kind\":\"api\",\"dll\":\"" + js_str(dll) + "\",\"api\":\"" + js_str(api) + "\"}}";
        cr.text = "Breakpoint " + std::to_string(b.id) + " on " + dll + "!" + api;
        return cr;
    }
    return {false, "cannot parse breakpoint location: " + spec};
}

// Called from cmd_run/start/attach after symbols are loaded for the live
// process: resolve and arm any breakpoints that were set before `run` (GDB
// "pending breakpoint" behaviour). Only software breakpoints can be pending.
static void apply_pending_bps()
{
    std::lock_guard<std::mutex> lk(g_bp_mu);
    for (auto& kv : g_bps) {
        Bpx& b = kv.second;
        if (!b.pending || !b.enabled || b.kind != 0) continue;
        ULONG_PTR addr = 0;
        bool ok = false;
        if (!b.file.empty() || b.line) ok = resolve_line(b.file, b.line, addr);
        else if (!b.symbol.empty() && sym_lookup(b.symbol, addr)) ok = true;
        else if (b.addr) { addr = b.addr; ok = true; }
        if (!ok || !addr) continue;
        if (!SetBPX(addr, UE_BREAKPOINT | UE_BREAKPOINT_TYPE_INT3, (void*)&cb_bpx)) continue;
        b.addr = addr;
        b.pending = false;
    }
}

static std::string bp_list_json()
{
    std::lock_guard<std::mutex> lk(g_bp_mu);
    std::ostringstream o;
    o << "{\"breakpoints\":[";
    bool first = true;
    for (auto& kv : g_bps) {
        if (!first) o << ","; first = false;
        Bpx& b = kv.second;
        o << "{\"id\":" << b.id << ",\"kind\":";
        if (b.kind == 0) {
            o << "\"code\",\"address\":\"" << hex(b.addr) << "\"";
            if (!b.file.empty() || b.line)
                o << ",\"file\":\"" << js_str(b.file) << "\",\"line\":" << b.line;
        }
        else if (b.kind == 1) o << "\"api\",\"dll\":\"" << js_str(b.dll) << "\",\"api\":\"" << js_str(b.api) << "\"";
        else if (b.kind == 2) o << "\"hardware\",\"address\":\"" << hex(b.addr) << "\"";
        else o << "\"memory\",\"address\":\"" << hex(b.addr) << "\",\"size\":" << b.memsize;
        o << ",\"enabled\":" << (b.enabled ? "true" : "false")
          << ",\"hits\":" << b.hits
          << ",\"ignore\":" << b.ignore
          << ",\"pending\":" << (b.pending ? "true" : "false");
        if (!b.condition.empty()) o << ",\"condition\":\"" << js_str(b.condition) << "\"";
        o << "}";
    }
    o << "]}";
    return o.str();
}

static std::string bp_list_text()
{
    std::lock_guard<std::mutex> lk(g_bp_mu);
    std::ostringstream o;
    o << "Num  Type       Disp Enb Address/What\n";
    for (auto& kv : g_bps) {
        Bpx& b = kv.second;
        std::ostringstream line;
        char buf[16];
        snprintf(buf, sizeof buf, "%-3d", b.id);
        std::string disp = "keep";
        std::string what;
        if (b.kind == 0) {
            if (!b.file.empty() || b.line) {
                std::string file = b.file.empty() ? "<current>" : b.file;
                size_t slash = file.find_last_of("\\/");
                if (slash != std::string::npos) file = file.substr(slash + 1);
                what = file + ":" + std::to_string(b.line) + " (" + hex(b.addr) + ")";
            } else {
                what = b.symbol.empty() ? hex(b.addr) : (b.symbol + " (" + hex(b.addr) + ")");
            }
            line << buf << " breakpoint " << disp << " " << (b.enabled ? "y" : "n") << " " << what;
        } else if (b.kind == 1) {
            what = b.dll + "!" + b.api;
            line << buf << " api-bp      " << disp << " " << (b.enabled ? "y" : "n") << " " << what;
        } else if (b.kind == 2) {
            what = hex(b.addr);
            line << buf << " hbreak      " << disp << " " << (b.enabled ? "y" : "n") << " " << what;
        } else {
            what = hex(b.addr) + " size " + std::to_string(b.memsize);
            line << buf << " mem/watch   " << disp << " " << (b.enabled ? "y" : "n") << " " << what;
        }
        o << line.str() << "\n";
        if (b.pending) o << "    pending (not yet applied; will arm on run)\n";
        if (b.hits) o << "    breakpoint already hit " << b.hits << " time" << (b.hits == 1 ? "" : "s") << "\n";
        if (b.ignore) o << "    ignore next " << b.ignore << " hits\n";
        if (!b.condition.empty()) o << "    stop only if " << b.condition << "\n";
    }
    return o.str();
}

// -- delete/disable/enable breakpoints --
static CmdResult cmd_bp_ops(const std::string& op, const std::vector<std::string>& args)
{
    std::lock_guard<std::mutex> lk(g_bp_mu);
    if (op == "delete" && args.empty()) {
        // GDB: `delete` with no args removes all breakpoints
        for (auto it = g_bps.begin(); it != g_bps.end(); ) {
            Bpx& b = it->second;
            if (!b.pending) {
                if (b.kind == 0 && b.addr) DeleteBPX(b.addr);
                else if (b.kind == 1) DeleteAPIBreakPoint(b.dll.c_str(), b.api.c_str(), UE_APISTART);
                else if (b.kind == 2) DeleteHardwareBreakPoint(b.hwreg);
                else if (b.kind == 3) RemoveMemoryBPX(b.addr, b.memsize);
            }
            it = g_bps.erase(it);
        }
        CmdResult cr;
        cr.text = "deleted all breakpoints\n";
        return cr;
    }
    if (args.empty() && (op == "disable" || op == "enable")) {
        // GDB: `disable` / `enable` with no args applies to all breakpoints
        for (auto& kv : g_bps) {
            Bpx& b = kv.second;
            if (op == "disable") {
                if (!b.pending && b.kind == 0 && b.addr) DisableBPX(b.addr);
                b.enabled = false;
            } else {
                if (!b.pending && b.kind == 0 && b.addr) EnableBPX(b.addr);
                b.enabled = true;
            }
        }
        CmdResult cr;
        cr.text = (op == "disable" ? "disabled all breakpoints\n" : "enabled all breakpoints\n");
        return cr;
    }
    if (args.empty()) return {false, "usage: " + op + " <id ...>"};
    std::ostringstream done;
    for (auto& a : args) {
        int id = atoi(a.c_str());
        auto it = g_bps.find(id);
        if (it == g_bps.end()) { done << "no breakpoint " << id << "\n"; continue; }
        Bpx& b = it->second;
        if (op == "delete") {
            if (!b.pending) {
                if (b.kind == 0 && b.addr) DeleteBPX(b.addr);
                else if (b.kind == 1) DeleteAPIBreakPoint(b.dll.c_str(), b.api.c_str(), UE_APISTART);
                else if (b.kind == 2) DeleteHardwareBreakPoint(b.hwreg);
                else if (b.kind == 3) RemoveMemoryBPX(b.addr, b.memsize);
            }
            g_bps.erase(it);
            done << "deleted " << id << "\n";
        } else if (op == "disable") {
            if (!b.pending && b.kind == 0 && b.addr) DisableBPX(b.addr);
            b.enabled = false;
            done << "disabled " << id << "\n";
        } else { // enable
            if (!b.pending && b.kind == 0 && b.addr) EnableBPX(b.addr);
            b.enabled = true;
            done << "enabled " << id << "\n";
        }
    }
    CmdResult cr;
    cr.text = done.str();
    return cr;
}

// -- condition <id> [expr]  (GDB) --
static CmdResult cmd_condition(const std::vector<std::string>& args)
{
    if (args.empty()) return {false, "usage: condition <id> [expr]"};
    int id = atoi(args[0].c_str());
    std::string expr;
    for (size_t i = 1; i < args.size(); i++) { if (i > 1) expr += " "; expr += args[i]; }
    std::lock_guard<std::mutex> lk(g_bp_mu);
    auto it = g_bps.find(id);
    if (it == g_bps.end()) return {false, "no breakpoint number " + std::to_string(id)};
    it->second.condition = expr;
    CmdResult cr;
    cr.js = "{\"id\":" + std::to_string(id) + ",\"condition\":\"" + js_str(expr) + "\"}";
    cr.text = expr.empty()
        ? "Breakpoint " + std::to_string(id) + " condition cleared"
        : "Breakpoint " + std::to_string(id) + " condition: " + expr;
    return cr;
}

// -- ignore <id> [count]  (GDB) --
static CmdResult cmd_ignore(const std::vector<std::string>& args)
{
    if (args.empty()) return {false, "usage: ignore <id> [count]"};
    int id = atoi(args[0].c_str());
    int n = args.size() > 1 ? atoi(args[1].c_str()) : 0;
    std::lock_guard<std::mutex> lk(g_bp_mu);
    auto it = g_bps.find(id);
    if (it == g_bps.end()) return {false, "no breakpoint number " + std::to_string(id)};
    it->second.ignore = n;
    CmdResult cr;
    cr.js = "{\"id\":" + std::to_string(id) + ",\"ignore\":" + std::to_string(n) + "}";
    cr.text = n == 0
        ? "Will stop on every crossing of breakpoint " + std::to_string(id) + "."
        : "Will ignore next " + std::to_string(n) + " crossings of breakpoint " + std::to_string(id) + ".";
    return cr;
}

// -- hardware breakpoint --
static CmdResult cmd_hbreak(const std::vector<std::string>& args)
{
    if (args.empty()) return {false, "usage: hbreak <addr> [r|w|x] [1|2|4|8]"};
    ULONG_PTR addr = 0;
    if (!parse_addr(args[0], addr) && !sym_lookup(args[0], addr)) return {false, "bad address"};
    DWORD type = UE_HARDWARE_EXECUTE;
    DWORD size = UE_HARDWARE_SIZE_4;
    if (args.size() > 1) {
        std::string t = args[1];
        std::transform(t.begin(), t.end(), t.begin(), ::tolower);
        if (t == "r") type = UE_HARDWARE_READWRITE;
        else if (t == "w") type = UE_HARDWARE_WRITE;
        else if (t == "x") type = UE_HARDWARE_EXECUTE;
    }
    if (args.size() > 2) {
        int s = atoi(args[2].c_str());
        if (s == 1) size = UE_HARDWARE_SIZE_1;
        else if (s == 2) size = UE_HARDWARE_SIZE_2;
        else if (s == 4) size = UE_HARDWARE_SIZE_4;
        else if (s == 8) size = UE_HARDWARE_SIZE_8;
    }
    DWORD reg = 0;
    if (!GetUnusedHardwareBreakPointRegister(&reg)) return {false, "no free hardware breakpoint slot"};
    // reg is an index 0..3; map to UE_DR0..3
    DWORD drIdx = UE_DR0 + reg;
    if (!SetHardwareBreakPoint(addr, drIdx, type, size, (void*)&cb_hwbp))
        return {false, "SetHardwareBreakPoint failed"};
    Bpx b; b.id = g_bp_next_id++; b.kind = 2; b.addr = addr; b.hwreg = drIdx; b.hwtype = type; b.enabled = true;
    b.symbol = args[0];
    g_bps[b.id] = b;
    CmdResult cr;
    cr.js = "{\"breakpoint\":{\"id\":" + std::to_string(b.id) + ",\"kind\":\"hardware\",\"address\":\"" + hex(addr) + "\"}}";
    cr.text = "Hardware breakpoint " + std::to_string(b.id) + " at " + hex(addr) + " (DR" + std::to_string(reg) + ")";
    return cr;
}

// -- memory breakpoint --
static CmdResult cmd_mbreak(const std::vector<std::string>& args)
{
    if (args.size() < 2) return {false, "usage: mbreak <addr> <size> [r|w|x|a]"};
    ULONG_PTR addr = 0;
    if (!parse_addr(args[0], addr)) {
        if (sym_lookup(args[0], addr)) {
            // symbol name resolved
        } else return {false, "bad address"};
    }
    SIZE_T size = (SIZE_T)strtoull(args[1].c_str(), nullptr, 0);
    DWORD type = UE_MEMORY;
    if (args.size() > 2) {
        std::string t = args[2];
        std::transform(t.begin(), t.end(), t.begin(), ::tolower);
        if (t == "r") type = UE_MEMORY_READ;
        else if (t == "w") type = UE_MEMORY_WRITE;
        else if (t == "x") type = UE_MEMORY_EXECUTE;
        else if (t == "a") type = UE_MEMORY;   // access = read+write+execute (guard page)
    }
    if (!SetMemoryBPXEx(addr, size, type, false, (void*)&cb_membp))
        return {false, "SetMemoryBPXEx failed"};
    Bpx b; b.id = g_bp_next_id++; b.kind = 3; b.addr = addr; b.memsize = size; b.enabled = true;
    b.symbol = args[0];
    g_bps[b.id] = b;
    CmdResult cr;
    cr.js = "{\"breakpoint\":{\"id\":" + std::to_string(b.id) + ",\"kind\":\"memory\",\"address\":\"" + hex(addr) + "\",\"size\":" + std::to_string(size) + "}}";
    cr.text = "Memory breakpoint " + std::to_string(b.id) + " at " + hex(addr);
    return cr;
}

// -- watch / rwatch / awatch  (GDB aliases of the memory breakpoint) --
static CmdResult cmd_watch(const std::vector<std::string>& args, char mode)
{
    if (args.empty()) return {false, "usage: " + std::string(mode=='a'?"awatch":(mode=='r'?"rwatch":"watch")) + " <addr> [size]"};
    std::vector<std::string> margs;
    margs.push_back(args[0]);
    margs.push_back(args.size() > 1 ? args[1] : "8");
    margs.push_back(mode == 'w' ? "w" : (mode == 'r' ? "r" : "a"));
    return cmd_mbreak(margs);
}

// -- registers --
static CmdResult cmd_registers()
{
    if (!require_running()) return {false, "no active debug session"};
    CmdResult cr;
    cr.js = regs_json();
    cr.text = regs_text();
    return cr;
}

// -- set register / memory --
static CmdResult cmd_set(const std::vector<std::string>& args)
{
    if (args.empty()) return {false, "usage: set <reg> = <val> | set *addr = <val>"};
    std::string left = args[0];
    std::string right;
    // find '='
    size_t eq = std::string::npos;
    for (size_t i = 1; i < args.size(); i++) {
        if (args[i] == "=") { eq = i; break; }
    }
    if (eq == std::string::npos || eq + 1 >= args.size()) return {false, "usage: set <reg> = <val>"};
    right = args[eq + 1];
    bool is64 = target_is64();
    if (left.size() > 1 && left[0] == '*') {
        ULONG_PTR addr = 0;
        if (!parse_addr(left.substr(1), addr)) return {false, "bad address"};
        ULONG_PTR val = 0;
        if (!parse_addr(right, val)) return {false, "bad value"};
        // infer size from value
        SIZE_T n = 8;
        if (val <= 0xFF) n = 1; else if (val <= 0xFFFF) n = 2; else if (val <= 0xFFFFFFFF) n = 4;
        SIZE_T nw = 0;
        if (!mem_write(addr, &val, n, &nw)) return {false, "MemoryWriteSafe failed"};
        CmdResult cr;
        cr.js = "{\"address\":\"" + hex(addr) + "\",\"value\":\"" + hex(val) + "\",\"size\":" + std::to_string(n) + "}";
        cr.text = "Wrote " + hex(val) + " (" + std::to_string(n) + " bytes) to " + hex(addr);
        return cr;
    }
    std::string regname = left;
    if (regname.size() > 1 && regname[0] == '$') regname = regname.substr(1);
    DWORD idx = reg_index_for_name(regname, is64);
    if (!idx) {
        // not a register: try a bare memory address (module!off, 0x..., etc.)
        ULONG_PTR a = 0;
        if (parse_addr(left, a)) {
            ULONG_PTR val = 0;
            if (!parse_addr(right, val)) return {false, "bad value"};
            SIZE_T n = 8;
            if (val <= 0xFF) n = 1; else if (val <= 0xFFFF) n = 2; else if (val <= 0xFFFFFFFF) n = 4;
            SIZE_T nw = 0;
            if (!mem_write(a, &val, n, &nw)) return {false, "MemoryWriteSafe failed"};
            CmdResult cr;
            cr.js = "{\"address\":\"" + hex(a) + "\",\"value\":\"" + hex(val) + "\",\"size\":" + std::to_string(n) + "}";
            cr.text = "Wrote " + hex(val) + " (" + std::to_string(n) + " bytes) to " + hex(a);
            return cr;
        }
        return {false, "unknown register: " + left};
    }
    ULONG_PTR val = 0;
    if (!parse_addr(right, val)) return {false, "bad value"};
    if (!SetContextData(idx, val)) return {false, "SetContextData failed"};
    CmdResult cr;
    cr.js = "{\"register\":\"" + js_str(left) + "\",\"value\":\"" + hex(val) + "\"}";
    cr.text = left + " = " + hex(val);
    return cr;
}

// -- examine memory (x) --
static CmdResult cmd_x(const std::string& arg)
{
    if (!require_running()) return {false, "no active debug session"};
    std::string a = arg;
    int count = 8;
    char fmt = 'x';
    char size = 'g';
    std::string addr_str = a;
    if (!a.empty() && a[0] == '/') {
        size_t slash = a.find(' ');
        std::string spec = slash == std::string::npos ? a.substr(1) : a.substr(1, slash - 1);
        addr_str = slash == std::string::npos ? "" : a.substr(slash + 1);
        std::string digits;
        for (char c : spec) {
            if (isdigit((unsigned char)c)) digits += c;
            else if (strchr("bhdwg", c)) size = c;          // b=byte h=half w=word g=giant
            else if (strchr("xduicsfi", c)) fmt = c;        // 'i' = instruction disassembly
            else if (strchr("o", c)) fmt = 'o';
        }
        if (!digits.empty()) count = atoi(digits.c_str());
        if (!target_is64() && size == 'g') size = 'w';
    }
    ULONG_PTR addr = 0;
    if (addr_str.empty() || !parse_addr(addr_str, addr)) {
        addr = reg_get(UE_CIP);
    }

    if (fmt == 'i') {
        auto ins = disasm(addr, count);
        CmdResult cr;
        cr.js = disasm_json(ins);
        cr.text = disasm_text(ins);
        return cr;
    }

    int bytes_per = 1;
    if (size == 'b') bytes_per = 1; else if (size == 'h') bytes_per = 2;
    else if (size == 'w') bytes_per = 4; else if (size == 'g') bytes_per = 8;

    size_t total = (size_t)count * bytes_per;
    if (total > 4096) total = 4096;
    std::vector<unsigned char> buf(total);
    SIZE_T nr = 0;
    if (!mem_read(addr, buf.data(), total, &nr)) return {false, "memory read failed"};

    std::ostringstream j;
    j << "[";
    std::ostringstream t;
    t << std::uppercase << std::hex;
    size_t i = 0;
    bool first = true;
    while (i + bytes_per <= nr) {
        uint64_t v = 0;
        for (int k = 0; k < bytes_per; k++) v |= (uint64_t)buf[i + k] << (8 * k);
        if (!first) j << ",";
        first = false;
        j << "{\"address\":\"" << hex(addr + i) << "\",\"value\":\"" << hex_brief(v) << "\"}";
        if (i % 16 == 0) t << hex(addr + i) << ":  ";
        for (int k = 0; k < bytes_per; k++) t << std::setw(2) << std::setfill('0') << (int)buf[i + k] << " ";
        if ((i + bytes_per) % 16 == 0) t << "\n";
        i += bytes_per;
    }
    if (fmt == 's') {
        // ascii string
        std::string s;
        for (size_t k = 0; k < nr && buf[k] != 0 && k < 512; k++)
            s += (buf[k] >= 32 && buf[k] < 127) ? (char)buf[k] : '.';
        CmdResult cr;
        cr.js = "{\"address\":\"" + hex(addr) + "\",\"string\":\"" + js_str(s) + "\"}";
        cr.text = hex(addr) + ": \"" + s + "\"";
        return cr;
    }
    j << "]";
    CmdResult cr;
    cr.js = j.str();
    cr.text = t.str();
    return cr;
}

// -- dump raw --
static CmdResult cmd_dump(const std::vector<std::string>& args)
{
    if (!require_running()) return {false, "no active debug session"};
    ULONG_PTR addr = 0;
    if (args.empty() || !parse_addr(args[0], addr)) return {false, "usage: dump <addr> [size]"};
    size_t n = args.size() > 1 ? (size_t)strtoull(args[1].c_str(), nullptr, 0) : 256;
    if (n > 65536) n = 65536;
    std::vector<unsigned char> buf(n);
    SIZE_T nr = 0;
    if (!mem_read(addr, buf.data(), n, &nr)) return {false, "memory read failed"};
    std::ostringstream o;
    for (size_t i = 0; i < nr; i += 16) {
        o << hex(addr + i) << ": ";
        std::string ascii;
        for (size_t k = 0; k < 16; k++) {
            if (i + k < nr) {
                char b[4]; snprintf(b, sizeof b, "%02x ", buf[i + k]); o << b;
                ascii += (buf[i + k] >= 32 && buf[i + k] < 127) ? (char)buf[i + k] : '.';
            } else o << "   ";
        }
        o << " |" << ascii << "|\n";
    }
    CmdResult cr;
    cr.text = o.str();
    return cr;
}

// -- disas --
static CmdResult cmd_disas(const std::vector<std::string>& args)
{
    if (!require_running()) return {false, "no active debug session"};
    ULONG_PTR addr = args.empty() ? reg_get(UE_CIP) : 0;
    int count = 8;
    if (!args.empty()) {
        // GDB: disas start, end  |  disas start, +len  |  disas start [count]
        std::string joined;
        for (auto& a : args) joined += a;
        size_t comma = joined.find(',');
        if (comma != std::string::npos) {
            std::string lhs = joined.substr(0, comma);
            std::string rhs = joined.substr(comma + 1);
            if (!parse_addr(lhs, addr)) return {false, "bad address"};
            if (!rhs.empty() && rhs[0] == '+') rhs = rhs.substr(1);
            ULONG_PTR end = 0;
            if (parse_addr(rhs, end) && end > addr) {
                count = (int)((end - addr) / 16) + 1;
                if (count > 1024) count = 1024;
            }
        } else {
            if (!parse_addr(args[0], addr)) return {false, "bad address"};
            if (args.size() > 1) count = atoi(args[1].c_str());
        }
    }
    auto ins = disasm(addr, count);
    CmdResult cr;
    cr.js = disasm_json(ins);
    cr.text = disasm_text(ins);
    return cr;
}

// -- backtrace (StackWalk64; works regardless of frame-pointer omission) --
static CmdResult cmd_bt(const std::vector<std::string>& args)
{
    if (!require_running()) return {false, "no active debug session"};
    int max = args.empty() ? 64 : atoi(args[0].c_str());
    if (max <= 0) max = 64;      // tolerate GDB's "bt full" etc.
    if (max > 256) max = 256;
    bool is64 = target_is64();
    PROCESS_INFORMATION* pi = TitanGetProcessInformation();
    if (!pi || !pi->hProcess) return {false, "no debuggee process"};

    CONTEXT ctx;
    if (!ctx_display(ctx)) return {false, "cannot read thread context"};

    STACKFRAME64 sf = {};
    DWORD mach = is64 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
    if (is64) {
        sf.AddrPC.Offset = ctx.Rip; sf.AddrFrame.Offset = ctx.Rbp; sf.AddrStack.Offset = ctx.Rsp;
    } else {
        sf.AddrPC.Offset = reg_get(UE_EIP); sf.AddrFrame.Offset = reg_get(UE_EBP); sf.AddrStack.Offset = reg_get(UE_ESP);
    }
    sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;

    DWORD tid = ctx_event_tid();
    if (g_ctx_tid && g_ctx_tid != tid) tid = g_ctx_tid;
    // x64 ignores hThread; x86 needs it to read stack memory
    HANDLE hth = NULL;
    if (!is64) hth = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);

    std::vector<ULONG_PTR> addrs;
    std::set<ULONG_PTR> seen;
    for (int frame = 0; frame < max; frame++) {
        if (!StackWalk64(mach, pi->hProcess, hth, &sf, &ctx, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL))
            break;
        ULONG_PTR pc = (ULONG_PTR)sf.AddrPC.Offset;
        if (!pc) break;
        if (seen.count(pc)) break;   // unwinder loop guard
        seen.insert(pc);
        addrs.push_back(pc);
        if (!sf.AddrReturn.Offset) break;
    }
    if (hth) CloseHandle(hth);

    if (addrs.empty()) addrs.push_back(is64 ? ctx.Rip : reg_get(UE_EIP));

    std::ostringstream o, j;
    j << "[";
    for (size_t i = 0; i < addrs.size(); i++) {
        ULONG_PTR a = addrs[i];
        if (i) j << ",";
        j << "{\"frame\":" << i << ",\"rip\":\"" << hex(a) << "\"}";
        std::string r = resolve(a);
        o << "#" << i << "  " << hex(a) << " in " << (r.empty() ? "??" : r) << "\n";
    }
    j << "]";
    CmdResult cr;
    cr.js = j.str();
    cr.text = o.str();
    return cr;
}

// -- list (source lines, GDB) --
static CmdResult list_file_lines(const std::string& file, long curline)
{
    std::ifstream f(file);
    if (!f) return {false, "cannot open source file: " + file};
    long from = (std::max)(1L, curline - 5);
    long to = curline + 5;
    long ln = 0;
    std::ostringstream t, j;
    j << "{\"lines\":[";
    bool first = true;
    std::string line;
    while (std::getline(f, line)) {
        ln++;
        if (ln < from) continue;
        if (ln > to) break;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!first) j << ","; first = false;
        j << "{\"line\":" << ln << ",\"text\":\"" << js_str(line)
          << "\",\"current\":" << (ln == curline ? "true" : "false") << "}";
        t << (ln == curline ? ">" : " ") << std::setw(5) << ln << " | " << line << "\n";
    }
    j << "]}";
    if (first) return {false, "line " + std::to_string(curline) + " outside file"};
    CmdResult cr;
    cr.js = j.str();
    cr.text = t.str();
    return cr;
}

static CmdResult cmd_list(const std::vector<std::string>& args)
{
    if (!require_running()) return {false, "no active debug session"};
    if (!g_sym_active || !g_sym_proc) return {false, "no symbols loaded"};
    bool is64 = target_is64();
    ULONG_PTR addr = 0;
    bool have = false;
    if (!args.empty()) {
        std::string a = args[0];
        if (isdigit((unsigned char)a[0])) {
            // "list N": line N of the current file -> keep current file, override line
        } else {
            // function name or address
            if (sym_lookup(a, addr)) have = true;
            else if (parse_addr(a, addr)) have = true;
            else return {false, "cannot parse list location: " + a};
        }
    }
    if (!have) {
        CONTEXT c;
        if (!ctx_display(c)) return {false, "cannot read thread context"};
        addr = is64 ? (ULONG_PTR)c.Rip : (ULONG_PTR)reg_get(UE_EIP);
    }
    IMAGEHLP_LINEW64 li = {};
    li.SizeOfStruct = sizeof(li);
    DWORD col = 0;
    if (!SymGetLineFromAddrW64(g_sym_proc, (DWORD64)addr, &col, &li))
        return {false, "no line info for " + hex(addr)};
    std::string file = utf8(li.FileName);
    long line = (long)li.LineNumber;
    if (!args.empty() && isdigit((unsigned char)args[0][0])) line = (long)strtol(args[0].c_str(), nullptr, 0);
    CmdResult cr = list_file_lines(file, line);
    if (cr.ok && g_source_checksum) {
        ULONG64 base = SymGetModuleBase64(g_sym_proc, (DWORD64)addr);
        std::string status = source_checksum_verify(g_sym_proc, base, file);
        if (status != "ok") {
            cr.text += "!! Checksum mismatch: " + file + " (" + status + ")\n";
            if (!cr.js.empty() && cr.js.front() == '{')
                cr.js.insert(cr.js.size() - 1, ",\"checksum\":\"" + status + "\"");
        }
    }
    return cr;
}

// -- search --
static CmdResult cmd_search(const std::vector<std::string>& args)
{
    if (!require_running()) return {false, "no active debug session"};
    if (args.size() < 3) return {false, "usage: search <addr> <size> <hex pattern> [wildcard byte]"};
    ULONG_PTR start = 0;
    if (!parse_addr(args[0], start)) return {false, "bad address"};
    DWORD size = (DWORD)strtoul(args[1].c_str(), nullptr, 0);
    std::string pat = args[2];
    std::vector<BYTE> pattern;
    BYTE wild = 0xFF;
    if (args.size() > 3) wild = (BYTE)strtoul(args[3].c_str(), nullptr, 0);
    for (size_t i = 0; i < pat.size(); i++) {
        char c = pat[i];
        if (c == '?') { pattern.push_back(wild); continue; }
        if (isspace((unsigned char)c)) continue;
        if (!isxdigit((unsigned char)c)) return {false, "invalid pattern"};
        int hi = isdigit((unsigned char)c) ? c - '0' : (tolower(c) - 'a' + 10);
        int lo = 0;
        if (i + 1 < pat.size() && isxdigit((unsigned char)pat[i + 1])) {
            lo = isdigit((unsigned char)pat[i + 1]) ? pat[i + 1] - '0' : (tolower(pat[i + 1]) - 'a' + 10);
            i++;
        }
        pattern.push_back((BYTE)((hi << 4) | lo));
    }
    if (pattern.empty()) return {false, "empty pattern"};
    PROCESS_INFORMATION* pi = TitanGetProcessInformation();
    std::vector<ULONG_PTR> hits;
    ULONG_PTR cur = start;
    ULONG_PTR end = start + size;
    while (cur + pattern.size() <= end) {
        ULONG_PTR hit = FindEx(pi->hProcess, (void*)cur, (DWORD)(end - cur),
                               pattern.data(), (DWORD)pattern.size(), &wild);
        if (!hit) break;
        hits.push_back(hit);
        cur = hit + 1;
    }
    std::ostringstream j;
    j << "[";
    std::ostringstream t;
    for (size_t i = 0; i < hits.size(); i++) {
        if (i) j << ",";
        j << "\"" << hex(hits[i]) << "\"";
        t << "  " << hex(hits[i]) << "\n";
    }
    j << "]";
    CmdResult cr;
    cr.js = "{\"count\":" + std::to_string(hits.size()) + ",\"hits\":" + j.str() + "}";
    cr.text = hits.empty() ? "no match" : t.str();
    return cr;
}

// -- strings --
static CmdResult cmd_strings(const std::vector<std::string>& args)
{
    if (!require_running()) return {false, "no active debug session"};
    ULONG_PTR addr = 0;
    if (args.empty() || !parse_addr(args[0], addr)) return {false, "usage: strings <addr> [size]"};
    size_t n = args.size() > 1 ? (size_t)strtoull(args[1].c_str(), nullptr, 0) : 0x1000;
    if (n > 0x100000) n = 0x100000;
    std::vector<unsigned char> buf(n);
    SIZE_T nr = 0;
    if (!mem_read(addr, buf.data(), n, &nr)) return {false, "memory read failed"};
    std::ostringstream j;
    j << "[";
    std::ostringstream t;
    bool first = true;
    std::string cur;
    ULONG_PTR cur_start = 0;
    for (size_t i = 0; i <= nr; i++) {
        unsigned char c = i < nr ? buf[i] : 0;
        bool printable = c >= 32 && c < 127;
        if (printable) {
            if (cur.empty()) cur_start = addr + i;
            cur += (char)c;
        } else {
            if (cur.size() >= 4) {
                if (!first) j << ",";
                first = false;
                j << "{\"address\":\"" << hex(cur_start) << "\",\"string\":\"" << js_str(cur) << "\"}";
                t << hex(cur_start) << ": " << cur << "\n";
            }
            cur.clear();
        }
    }
    j << "]";
    CmdResult cr;
    cr.js = j.str();
    cr.text = t.str();
    return cr;
}

// -- info modules --
static CmdResult cmd_info_modules()
{
    if (!require_running()) return {false, "no active debug session"};
    modules_refresh();
    std::ostringstream j;
    j << "[";
    std::ostringstream t;
    bool first = true;
    for (auto& m : g_mods) {
        if (!first) j << ",";
        first = false;
        j << "{\"base\":\"" << hex(m.base) << "\",\"name\":\"" << js_str(m.name)
          << "\",\"path\":\"" << js_str(m.path) << "\"}";
        t << hex(m.base) << "  " << m.name << "\n";
    }
    j << "]";
    CmdResult cr;
    cr.js = j.str();
    cr.text = t.str();
    return cr;
}

// read the PE entry point (base + AddressOfEntryPoint) of the loaded image
static ULONG_PTR pe_entry_point(ULONG_PTR base)
{
    if (!base) return 0;
    IMAGE_DOS_HEADER dos = {};
    SIZE_T nr = 0;
    if (!mem_read(base, &dos, sizeof(dos), &nr) || nr < sizeof(dos)) return 0;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS64 nth = {};
    if (!mem_read(base + dos.e_lfanew, &nth, sizeof(nth), &nr) || nr < sizeof(nth)) return 0;
    if (nth.Signature != IMAGE_NT_SIGNATURE) return 0;
    // OptionalHeader is the 32-bit layout in a 32-bit image; the Magic field
    // and AddressOfEntryPoint share the same offset in both layouts.
    if (nth.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_NT_HEADERS32 nt32 = {};
        if (!mem_read(base + dos.e_lfanew, &nt32, sizeof(nt32), &nr) || nr < sizeof(nt32)) return 0;
        if (nt32.OptionalHeader.AddressOfEntryPoint)
            return base + nt32.OptionalHeader.AddressOfEntryPoint;
        return 0;
    }
    if (nth.OptionalHeader.AddressOfEntryPoint)
        return base + nth.OptionalHeader.AddressOfEntryPoint;
    return 0;
}

// GDB `info files`: symbol file + exec file + entry point + loaded files.
// (info modules stays a concise base/name list; info files is the verbose form.)
static CmdResult cmd_info_files()
{
    if (!require_running()) return {false, "no active debug session"};
    modules_refresh();
    std::string exe = g_target.empty() ? std::string("(unknown)") : g_target;
    ULONG_PTR base = GetDebuggedFileBaseAddress();
    ULONG_PTR entry = pe_entry_point(base);
    std::ostringstream j, t;
    j << "{\"symbols\":\"" << js_str(exe) << "\",\"exec\":\"" << js_str(exe)
      << "\",\"entry\":\"" << hex(entry) << "\",\"files\":[";
    t << "Symbols from \"" << exe << "\".\n";
    t << "Local exec file:\n";
    t << "\t`" << exe << "', file type " << (target_is64() ? "pei-x86-64" : "pei-i386") << ".\n";
    t << "\tEntry point: " << hex(entry) << "\n";
    t << "\tLoaded files:\n";
    bool first = true;
    for (auto& m : g_mods) {
        if (!first) j << ",";
        first = false;
        j << "{\"base\":\"" << hex(m.base) << "\",\"name\":\"" << js_str(m.name)
          << "\",\"path\":\"" << js_str(m.path) << "\"}";
        t << "\t  " << hex(m.base) << "  " << m.name
          << (m.path.empty() ? "" : "  " + m.path) << "\n";
    }
    j << "]}";
    CmdResult cr;
    cr.js = j.str();
    cr.text = t.str();
    return cr;
}

// -- info threads / thread switching --
static std::vector<THREAD_ITEM_W> g_thr_scratch;

static void __cdecl enum_thread_cb(void* p)
{
    auto* th = (THREAD_ITEM_W*)p;
    g_thr_scratch.push_back(*th);
}

// enumerate threads (single-threaded REPL; the callback runs synchronously)
static void enum_threads(std::vector<THREAD_ITEM_W>& out)
{
    g_thr_scratch.clear();
    ThreaderEnumThreadInfo((void*)&enum_thread_cb);
    out = g_thr_scratch;
}

static CmdResult cmd_info_threads()
{
    if (!require_running()) return {false, "no active debug session"};
    std::vector<THREAD_ITEM_W> list;
    enum_threads(list);
    DWORD evtid = ctx_event_tid();
    DWORD cur = g_ctx_tid ? g_ctx_tid : evtid;
    std::ostringstream j;
    j << "[";
    std::ostringstream t;
    for (size_t i = 0; i < list.size(); i++) {
        if (i) j << ",";
        THREAD_ITEM_W& th = list[i];
        bool iscur = (th.dwThreadId == cur);
        j << "{\"id\":" << th.dwThreadId
          << ",\"num\":" << (i + 1)
          << ",\"start\":\"" << hex((ULONG_PTR)th.ThreadStartAddress)
          << "\",\"current\":" << (iscur ? "true" : "false") << "}";
        t << (iscur ? "* " : "  ") << (i + 1) << "  " << th.dwThreadId
          << "  start=" << hex((ULONG_PTR)th.ThreadStartAddress) << "\n";
    }
    j << "]";
    CmdResult cr;
    cr.js = j.str();
    cr.text = t.str();
    return cr;
}

// -- thread [id]  (GDB) --
static CmdResult cmd_thread(const std::vector<std::string>& args)
{
    if (!require_running()) return {false, "no active debug session"};
    if (args.empty()) {
        DWORD evtid = ctx_event_tid();
        DWORD cur = g_ctx_tid ? g_ctx_tid : evtid;
        CmdResult cr;
        cr.js = "{\"thread\":" + std::to_string(cur) + "}";
        cr.text = "[Current thread is " + std::to_string(cur) + "]";
        return cr;
    }
    DWORD arg = (DWORD)strtoul(args[0].c_str(), nullptr, 0);
    std::vector<THREAD_ITEM_W> list;
    enum_threads(list);
    // GDB: `thread <id>` uses the internal thread number (1..N). We also accept
    // the raw OS tid. Prefer an exact tid match, then a 1-based internal number.
    DWORD tid = 0;
    for (auto& th : list) if (th.dwThreadId == arg) { tid = th.dwThreadId; break; }
    if (!tid && arg >= 1 && arg <= (DWORD)list.size()) tid = list[arg - 1].dwThreadId;
    if (!tid) return {false, "no thread " + std::to_string(arg)};

    if (tid == ctx_event_tid()) {
        ctx_reset();
    } else {
        if (!ctx_fetch(tid, g_ctx)) return {false, "cannot read thread context"};
        g_ctx_tid = tid;
        g_ctx_valid = true;
    }
    CmdResult cr;
    cr.js = "{\"thread\":" + std::to_string(tid) + "}";
    cr.text = "[Switching to thread " + std::to_string(tid) + "]\n" + regs_text();
    return cr;
}

// -- info proc --
static CmdResult cmd_info_proc()
{
    PROCESS_INFORMATION* pi = TitanGetProcessInformation();
    std::string pid = pi ? std::to_string(pi->dwProcessId) : "0";
    std::string base = g_running ? hex(GetDebuggedFileBaseAddress()) : "0x0";
    std::string is64 = target_is64() ? "true" : "false";
    CmdResult cr;
    cr.js = "{\"pid\":" + pid + ",\"image_base\":\"" + base + "\",\"is64\":" + is64 + ",\"running\":" + (g_running ? "true" : "false") + "}";
    cr.text = "pid       = " + pid + "\nimagebase = " + base + "\n64-bit    = " + (is64 == "true" ? "yes" : "no");
    return cr;
}

// -- info events --
static CmdResult cmd_info_events()
{
    std::lock_guard<std::mutex> lk(g_ev_mu);
    std::ostringstream j;
    j << "[";
    std::ostringstream t;
    for (size_t i = 0; i < g_events.size(); i++) {
        if (i) j << ",";
        j << "\"" << js_str(g_events[i]) << "\"";
        t << g_events[i] << "\n";
    }
    j << "]";
    CmdResult cr;
    cr.js = j.str();
    cr.text = t.str();
    return cr;
}

// -- set args / show args (GDB) --
static CmdResult cmd_set_args(const std::vector<std::string>& args)
{
    std::string joined;
    for (size_t i = 0; i < args.size(); i++) {
        if (i) joined += " ";
        joined += args[i];
    }
    g_default_args = joined;
    CmdResult cr;
    cr.js = "{\"args\":\"" + js_str(joined) + "\"}";
    cr.text = "args = " + (joined.empty() ? std::string("(none)") : joined);
    return cr;
}

static CmdResult cmd_show_args()
{
    CmdResult cr;
    cr.js = "{\"args\":\"" + js_str(g_default_args) + "\"}";
    cr.text = "args = " + (g_default_args.empty() ? std::string("(none)") : g_default_args);
    return cr;
}

// -- kill (GDB) --
static CmdResult cmd_kill()
{
    if (!require_running()) return {false, "no active debug session"};
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_waiting = false;
        g_cv.notify_all();
    }
    stop_session();
    CmdResult cr;
    cr.js = "{\"killed\":true}";
    cr.text = "Process killed.";
    return cr;
}

// -- print / p (GDB) --
static CmdResult cmd_print(const std::string& raw_args)
{
    if (!require_running()) return {false, "no active debug session"};
    std::string arg = raw_args;
    size_t b = arg.find_first_not_of(" \t");
    if (b == std::string::npos) return {false, "usage: print [/fmt] expr"};
    size_t e = arg.find_last_not_of(" \t");
    arg = arg.substr(b, e - b + 1);
    char fmt = 'x';
    if (arg[0] == '/') {
        size_t sp = arg.find(' ');
        std::string spec = sp == std::string::npos ? arg.substr(1) : arg.substr(1, sp - 1);
        arg = sp == std::string::npos ? "" : arg.substr(sp + 1);
        if (!spec.empty()) fmt = spec.back();
        b = arg.find_first_not_of(" \t");
        if (b == std::string::npos) return {false, "usage: print [/fmt] expr"};
        arg = arg.substr(b);
    }
    bool is64 = target_is64();
    bool deref = false;
    std::string expr = arg;
    if (!expr.empty() && expr[0] == '*') { deref = true; expr = expr.substr(1); }
    ULONG_PTR val = 0;
    if (!expr.empty() && expr[0] == '$') {
        DWORD idx = reg_index_for_name(expr.substr(1), is64);
        if (!idx) return {false, "unknown register: " + expr.substr(1)};
        val = reg_get(idx);
    } else {
        ULONG_PTR a = 0;
        if (!parse_addr(expr, a)) return {false, "cannot parse expression: " + expr};
        val = a;
    }
    if (deref) {
        SIZE_T n = is64 ? 8 : 4, nr = 0;
        if (!mem_read(val, &val, n, &nr) || nr < n) return {false, "memory read failed"};
    }
    std::string h = hex(val);
    std::string dv = std::to_string((unsigned long long)val);
    CmdResult cr;
    cr.js = "{\"expression\":\"" + js_str(arg) + "\",\"value\":\"" + h + "\",\"decimal\":\"" + dv + "\"}";
    if (fmt == 'd') {
        cr.text = "value = " + dv;
    } else if (fmt == 't') {
        std::string bits;
        unsigned long long v = (unsigned long long)val;
        for (int k = 0; k < (is64 ? 64 : 32); k++) { bits.insert(bits.begin(), (v & 1) ? '1' : '0'); v >>= 1; }
        cr.text = "value = " + bits;
    } else if (fmt == 'c') {
        cr.text = "value = '" + std::string(1, (char)val) + "' (0x" + std::to_string(val) + ")";
    } else {
        cr.text = "value = " + h + "  (" + dv + ")";
    }
    return cr;
}

// -- set engine var --
static CmdResult cmd_set_engine(const std::vector<std::string>& args)
{
    if (args.size() < 2) return {false, "usage: set engine <aslr|console|passexc> on|off"};
    std::string var = args[0];
    bool on = args[1] == "on" || args[1] == "1" || args[1] == "true";
    DWORD id = 0;
    std::transform(var.begin(), var.end(), var.begin(), ::tolower);
    if (var == "aslr") id = UE_ENGINE_DISABLE_ASLR;
    else if (var == "console") id = UE_ENGINE_NO_CONSOLE_WINDOW;
    else if (var == "passexc") id = UE_ENGINE_PASS_ALL_EXCEPTIONS;
    else return {false, "unknown engine variable"};
    SetEngineVariable(id, on);
    CmdResult cr;
    cr.js = "{\"variable\":\"" + js_str(var) + "\",\"value\":" + (on ? "true" : "false") + "}";
    cr.text = std::string("engine.") + var + " = " + (on ? "on" : "off");
    return cr;
}

// -- source checksum toggle / query --
static CmdResult cmd_set_checksum(const std::vector<std::string>& args)
{
    if (args.empty()) return {false, "usage: set source-checksum on|off"};
    bool on = args[0] == "on" || args[0] == "1" || args[0] == "true";
    g_source_checksum = on;
    CmdResult cr;
    cr.js = std::string("{\"source-checksum\":") + (on ? "true" : "false") + "}";
    cr.text = std::string("source-checksum = ") + (on ? "on" : "off");
    return cr;
}

static CmdResult cmd_show_checksum()
{
    CmdResult cr;
    cr.js = std::string("{\"source-checksum\":") + (g_source_checksum ? "true" : "false") + "}";
    cr.text = std::string("source-checksum = ") + (g_source_checksum ? "on" : "off");
    return cr;
}

// `info source`: current stop's source file + PDB checksum verification.
// Always verifies (explicit diagnostic), regardless of the source-checksum toggle.
static CmdResult cmd_info_source()
{
    if (!require_running()) return {false, "no active debug session"};
    if (!g_sym_active || !g_sym_proc) return {false, "no symbols loaded"};
    CONTEXT c;
    if (!ctx_display(c)) return {false, "cannot read thread context"};
    ULONG_PTR addr = target_is64() ? (ULONG_PTR)c.Rip : (ULONG_PTR)reg_get(UE_EIP);
    if (!addr) return {false, "cannot determine stop location"};
    IMAGEHLP_LINEW64 li = {};
    li.SizeOfStruct = sizeof(li);
    DWORD col = 0;
    if (!SymGetLineFromAddrW64(g_sym_proc, addr, &col, &li))
        return {false, "no line info at " + hex(addr)};
    std::string file = utf8(li.FileName);
    ULONG64 base = SymGetModuleBase64(g_sym_proc, addr);
    DWORD type = 0;
    std::string status = source_checksum_verify(g_sym_proc, base, file, &type);
    std::string alg = (type && status != "no-checksum") ? cs_alg_name(type) : "-";
    CmdResult cr;
    cr.js = "{\"file\":\"" + js_str(file) + "\",\"checksum\":\"" + status
          + "\",\"algorithm\":\"" + alg + "\",\"enabled\":" + (g_source_checksum ? "true" : "false") + "}";
    cr.text = "Source file: " + file + "\n"
            + "Checksum: " + status
            + (alg != "-" ? " (" + alg + ")" : "")
            + "  [source-checksum: " + (g_source_checksum ? "on" : "off") + "]";
    return cr;
}

// -- help --
static const char* HELP =
"aidbg - GDB-style debugger on TitanEngine\n"
"\n"
"GDB-compatible invocation:\n"
"  aidbg [--batch] [-ex \"cmd\" ...] [-x script] [--args prog arg ...]\n"
"  aidbg --batch -ex \"run\" -ex \"bt\" --args myprog.exe arg1 arg2\n"
"  (exit code is 0 on success, nonzero if the inferior crashed or a command errored)\n"
"\n"
"  file <path>                 set target executable\n"
"  run [args] / r              start debugging (stops at initial breakpoint)\n"
"  start [func]                like run, but stop at the entry function (main by default)\n"
"  attach <pid>                attach to a running process\n"
"  detach                      detach and leave target running\n"
"  kill                        terminate the current run\n"
"  continue / c / cont         resume execution\n"
"  stepi / si / s / step       single-step instructions (s/step = source-line fallback)\n"
"  nexti / ni / n / next       step over calls\n"
"  finish / fin                run until the current function returns (stops in the caller)\n"
"  break <addr> / b            set software breakpoint (*addr | addr | $reg | mod!api)\n"
"  break <symbol>              set breakpoint by PDB symbol name (e.g. break main)\n"
"  break <line>                set breakpoint at a source line (e.g. break 31)\n"
"  break <file.c>:<line>       set breakpoint at file.c:line (e.g. break main.c:13)\n"
"  hbreak <addr|sym> [r|w|x]   hardware breakpoint (address or symbol)\n"
"  mbreak <addr> <size> [r|w|x|a] memory breakpoint\n"
"  watch / rwatch / awatch     write / read / access watchpoint on an address or symbol\n"
"  condition <id> [expr]       set a stop condition (empty expr clears it)\n"
"  ignore <id> <count>         ignore the next <count> hits of a breakpoint\n"
"  info break / modules / threads / proc / events / regs\n"
"  info locals / info args     show local variables / function parameters (PDB)\n"
"  info files / target         info files = symbol/exec files; target = modules\n"
"  info source                 current source file + PDB checksum verification\n"
"  delete / d <id...>          remove breakpoints (no ids = delete all)\n"
"  disable/enable <id...>      toggle breakpoints\n"
"  thread [id]                 show current thread / switch (id = internal # or tid)\n"
"  registers / regs            show registers\n"
"  list / l [func|line]        show source lines around the current stop (PDB)\n"
"  print / p [/fmt] expr       print $reg | *addr | addr (fmt: x/d/t/c)\n"
"  set <reg> = <val>           write register\n"
"  set *<addr> = <val>         write memory\n"
"  set args <...>              set inferior arguments used by a bare \"run\"\n"
"  show args                   show inferior arguments\n"
"  x/<n><fmt> <addr>           examine memory (fmt: b/h/w/g + x/d/u/i/s/c/f)\n"
"  dump <addr> [size]          raw hex dump\n"
"  disas / disassemble [a] [n] disassemble (GDB range: disas start, end)\n"
"  bt [max] / backtrace / where stack backtrace\n"
"  search <addr> <size> <pat>  find byte pattern (? = wildcard)\n"
"  strings <addr> [size]       scan for ascii strings\n"
"  echo <text>                 print text\n"
"  set engine <var> on|off     aslr / console / passexc\n"
"  set source-checksum on|off  verify source files against PDB checksums (default off)\n"
"  show source-checksum        show the source-checksum toggle\n"
"  help / quit / q\n";

static CmdResult cmd_help()
{
    CmdResult cr;
    cr.text = HELP;
    return cr;
}

// ----------------------------------------------------------------- dispatch ---

static CmdResult execute(const std::string& line)
{
    std::string trimmed = line;
    while (!trimmed.empty() && (trimmed.front()==' '||trimmed.front()=='\t')) trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && (trimmed.back()==' '||trimmed.back()=='\t'||trimmed.back()=='\r')) trimmed.pop_back();
    if (trimmed.empty() || trimmed[0] == '#') return {};
    auto tok = tokenize(trimmed);
    if (tok.empty()) return {};
    std::string cmd = tok[0];
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

    // raw args for 'run' (keep command line as-is)
    std::string raw_args;
    size_t sp = trimmed.find(' ');
    if (sp != std::string::npos) raw_args = trimmed.substr(sp + 1);

    if (cmd == "quit" || cmd == "q" || cmd == "exit") {
        g_quit = true;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_waiting = false;
            g_cv.notify_all();
        }
        if (g_running) {
            StopDebug();
            if (g_loop_thread.joinable()) g_loop_thread.join();
        }
        exit(0);
    }
    if (cmd == "help" || cmd == "?") return cmd_help();
    if (cmd == "echo") {
        CmdResult cr;
        cr.js = "{\"text\":\"" + js_str(raw_args) + "\"}";
        cr.text = raw_args;
        return cr;
    }
    if (cmd == "file") {
        if (tok.size() < 2) return {false, "usage: file <path>"};
        set_target_path(tok[1]);
        sym_load_file(g_target);   // preload PDB so `break <symbol>` works before run
        CmdResult cr;
        cr.js = "{\"file\":\"" + js_str(g_target) + "\"}";
        cr.text = "target = " + g_target;
        return cr;
    }
    if (cmd == "run" || cmd == "r") return cmd_run(tok, raw_args);
    if (cmd == "start") return cmd_run(tok, raw_args, true);
    if (cmd == "attach") {
        if (tok.size() < 2) return {false, "usage: attach <pid>"};
        return cmd_attach((DWORD)strtoul(tok[1].c_str(), nullptr, 0));
    }
    if (cmd == "detach") return cmd_detach();
    if (cmd == "kill") return cmd_kill();
    if (cmd == "continue" || cmd == "c" || cmd == "cont") return cmd_continue();
    if (cmd == "stepi" || cmd == "si") {
        int n = tok.size() > 1 ? atoi(tok[1].c_str()) : 1;
        return cmd_step("into", n);
    }
    if (cmd == "nexti" || cmd == "ni") {
        int n = tok.size() > 1 ? atoi(tok[1].c_str()) : 1;
        return cmd_step("over", n);
    }
    // GDB: step/next are source-line steps; without source symbols they fall
    // back to instruction stepping (stepi/nexti)
    if (cmd == "step" || cmd == "s") {
        int n = tok.size() > 1 ? atoi(tok[1].c_str()) : 1;
        return cmd_step("into", n);
    }
    if (cmd == "next" || cmd == "n") {
        int n = tok.size() > 1 ? atoi(tok[1].c_str()) : 1;
        return cmd_step("over", n);
    }
    if (cmd == "finish" || cmd == "fin") return cmd_step("out", 1);
    if (cmd == "break" || cmd == "b" || cmd == "br") return cmd_break(tok.size() > 1 ? std::vector<std::string>(tok.begin()+1, tok.end()) : std::vector<std::string>());
    if (cmd == "hbreak" || cmd == "hb") return cmd_hbreak(std::vector<std::string>(tok.begin()+1, tok.end()));
    if (cmd == "mbreak" || cmd == "mb") return cmd_mbreak(std::vector<std::string>(tok.begin()+1, tok.end()));
    if (cmd == "delete" || cmd == "del" || cmd == "d") return cmd_bp_ops("delete", std::vector<std::string>(tok.begin()+1, tok.end()));
    if (cmd == "disable") return cmd_bp_ops("disable", std::vector<std::string>(tok.begin()+1, tok.end()));
    if (cmd == "enable") return cmd_bp_ops("enable", std::vector<std::string>(tok.begin()+1, tok.end()));
    if (cmd == "condition") return cmd_condition(std::vector<std::string>(tok.begin()+1, tok.end()));
    if (cmd == "ignore") return cmd_ignore(std::vector<std::string>(tok.begin()+1, tok.end()));
    if (cmd == "watch") return cmd_watch(std::vector<std::string>(tok.begin()+1, tok.end()), 'w');
    if (cmd == "rwatch") return cmd_watch(std::vector<std::string>(tok.begin()+1, tok.end()), 'r');
    if (cmd == "awatch") return cmd_watch(std::vector<std::string>(tok.begin()+1, tok.end()), 'a');
    if (cmd == "registers" || cmd == "regs" || cmd == "info" && tok.size() >= 2 && tok[1] == "registers") return cmd_registers();
    if (cmd == "set") {
        if (tok.size() >= 2 && tok[1] == "engine") {
            return cmd_set_engine(std::vector<std::string>(tok.begin()+2, tok.end()));
        }
        if (tok.size() >= 2 && tok[1] == "args") {
            return cmd_set_args(std::vector<std::string>(tok.begin()+2, tok.end()));
        }
        if (tok.size() >= 2 && tok[1] == "source-checksum") {
            return cmd_set_checksum(std::vector<std::string>(tok.begin()+2, tok.end()));
        }
        return cmd_set(std::vector<std::string>(tok.begin()+1, tok.end()));
    }
    if (cmd == "show") {
        if (tok.size() < 2) return {false, "usage: show args|source-checksum"};
        if (tok[1] == "args") return cmd_show_args();
        if (tok[1] == "source-checksum") return cmd_show_checksum();
        return {false, "unknown show topic: " + tok[1]};
    }
    if (cmd == "print" || cmd == "p") {
        std::string rest = raw_args;
        return cmd_print(rest);
    }
    if (cmd.rfind("p/", 0) == 0) {
        // GDB format attached to the command: p/x $rax, p/d $rsp
        std::string arg = "/" + cmd.substr(2);
        if (!raw_args.empty()) arg += " " + raw_args;
        return cmd_print(arg);
    }
    if (cmd.rfind("print/", 0) == 0) {
        std::string arg = "/" + cmd.substr(6);
        if (!raw_args.empty()) arg += " " + raw_args;
        return cmd_print(arg);
    }
    if (cmd == "x" || cmd == "examine") {
        std::string rest = raw_args;
        return cmd_x(rest);
    }
    if (cmd.size() > 2 && cmd[0] == 'x' && cmd[1] == '/') {
        // GDB-style count/format with no space: x/4gx <addr>
        std::string spec = cmd.substr(2);
        std::string rest = raw_args;
        std::string arg = "/" + spec;
        if (!rest.empty()) arg += " " + rest;
        return cmd_x(arg);
    }
    if (cmd.rfind("examine/", 0) == 0) {
        // GDB-style count/format with no space: examine/4gx <addr>
        std::string spec = cmd.substr(8);
        std::string rest = raw_args;
        std::string arg = "/" + spec;
        if (!rest.empty()) arg += " " + rest;
        return cmd_x(arg);
    }
    if (cmd == "dump") return cmd_dump(std::vector<std::string>(tok.begin()+1, tok.end()));
    if (cmd == "disas" || cmd == "u" || cmd == "disassemble") return cmd_disas(std::vector<std::string>(tok.begin()+1, tok.end()));
    if (cmd == "bt" || cmd == "backtrace" || cmd == "where") return cmd_bt(std::vector<std::string>(tok.begin()+1, tok.end()));
    if (cmd == "list" || cmd == "l") return cmd_list(std::vector<std::string>(tok.begin()+1, tok.end()));
    if (cmd == "thread") return cmd_thread(std::vector<std::string>(tok.begin()+1, tok.end()));
    if (cmd == "search") return cmd_search(std::vector<std::string>(tok.begin()+1, tok.end()));
    if (cmd == "strings") return cmd_strings(std::vector<std::string>(tok.begin()+1, tok.end()));
    if (cmd == "info") {
        if (tok.size() < 2) return {false, "usage: info <break|locals|args|modules|threads|proc|events|registers>"};
        std::string what = tok[1];
        std::transform(what.begin(), what.end(), what.begin(), ::tolower);
        if (what == "break" || what == "b" || what == "breakpoints") {
            CmdResult cr; cr.js = bp_list_json(); cr.text = bp_list_text(); return cr;
        }
        if (what == "locals" || what == "local") return cmd_info_vars(false);
        if (what == "args" || what == "arguments") return cmd_info_vars(true);
        if (what == "modules" || what == "target") return cmd_info_modules();
        if (what == "files") return cmd_info_files();
        if (what == "threads") return cmd_info_threads();
        if (what == "proc") return cmd_info_proc();
        if (what == "events") return cmd_info_events();
        if (what == "source") return cmd_info_source();
        if (what == "registers" || what == "regs" || what == "reg" || what == "r") return cmd_registers();
        return {false, "unknown info topic: " + what};
    }
    return {false, "unknown command: " + cmd + " (try: help)"};
}

// ---------------------------------------------------------------------- main ---

struct BatchItem { std::string text; bool is_file; };

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetBPXOptions(UE_BREAKPOINT_TYPE_INT3);

    std::string single_cmd, script_file, target;
    std::vector<BatchItem> batch;
    bool batch_mode = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need_arg = [&](const char* opt) -> std::string {
            if (i + 1 < argc) return argv[++i];
            fprintf(stderr, "option %s requires an argument\n", opt);
            return std::string();
        };
        if (a == "--json") {
            g_json = true;
        } else if (a == "--batch" || a == "-batch") {
            batch_mode = true;
        } else if (a == "--batch-silent" || a == "-batch-silent") {
            batch_mode = true; g_silent = true;
        } else if (a == "-q" || a == "-quiet" || a == "--quiet" || a == "-silent" || a == "--silent") {
            g_quiet = true;
        } else if (a == "-ex" || a == "--eval-command" || a.rfind("--eval-command=", 0) == 0) {
            std::string v = a.rfind("--eval-command=", 0) == 0
                ? a.substr(std::string("--eval-command=").size())
                : need_arg(a.c_str());
            batch.push_back({v, false});
        } else if (a == "-x" || a == "--command-file" || a.rfind("--command-file=", 0) == 0) {
            std::string v = a.rfind("--command-file=", 0) == 0
                ? a.substr(std::string("--command-file=").size())
                : need_arg(a.c_str());
            batch.push_back({v, true});
        } else if (a == "-e" || a == "--executable" || a.rfind("--executable=", 0) == 0) {
            std::string v = a.rfind("--executable=", 0) == 0
                ? a.substr(std::string("--executable=").size())
                : need_arg(a.c_str());
            if (!v.empty()) set_target_path(v);
        } else if (a == "--args" || a == "-args") {
            // GDB --args: the next arg is the target, everything after is inferior args
            if (i + 1 < argc) target = argv[++i];
            std::string rest;
            for (int k = i + 1; k < argc; k++) {
                if (!rest.empty()) rest += " ";
                rest += argv[k];
            }
            g_default_args = rest;
            i = argc; // --args consumes the remainder of the command line
        } else if (a == "--command" || a.rfind("--command=", 0) == 0) {
            // GDB: `--command=FILE` runs a command file (-x). aidbg also accepts
            // a single command string for AI use. If the argument names an
            // existing file, honour the GDB meaning (run it as a command file).
            std::string v;
            if (a.rfind("--command=", 0) == 0) v = a.substr(10);
            else if (i + 1 < argc) v = argv[++i];
            if (!v.empty()) {
                std::ifstream probe(v);
                if (probe.good()) { script_file = v; probe.close(); }
                else { single_cmd = v; }
            }
        } else if (a == "--commands") {
            if (i + 1 < argc) script_file = argv[++i];
        } else if (a == "--help" || a == "-h") {
            printf("%s", HELP);
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "unknown option: %s\n", a.c_str());
            return 1;
        } else {
            target = a;
        }
    }
    if (!target.empty()) set_target_path(target);

    // ---- GDB-style batch mode: execute -ex/-x commands in order, then exit ----
    if (batch_mode || !batch.empty()) {
        g_batch = true;   // bare "run" continues past the initial break, like GDB
        int rc = 0;
        for (auto& item : batch) {
            if (item.is_file) {
                std::ifstream f(item.text);
                if (!f) { fprintf(stderr, "cannot open %s\n", item.text.c_str()); rc = 1; continue; }
                std::string line;
                while (std::getline(f, line)) {
                    CmdResult r = execute(line);
                    print_result(r);
                    if (!r.ok && !soft_error(r.err)) rc = 1;
                }
            } else {
                CmdResult r = execute(item.text);
                print_result(r);
                if (!r.ok && !soft_error(r.err)) rc = 1;
            }
        }
        std::string final_reason = g_reason;
        stop_session();
        // GDB batch exits nonzero when the inferior stopped on a crash/signal
        if (final_reason == "exception") rc = 1;
        return rc;
    }

    // single command mode (AI-friendly)
    if (!single_cmd.empty()) {
        CmdResult r = execute(single_cmd);
        print_result(r);
        stop_session();
        return r.ok ? 0 : 1;
    }

    // script mode
    if (!script_file.empty()) {
        std::ifstream f(script_file);
        if (!f) { fprintf(stderr, "cannot open %s\n", script_file.c_str()); return 1; }
        std::string line;
        while (std::getline(f, line)) {
            CmdResult r = execute(line);
            print_result(r);
            if (!r.ok && r.err == "unknown command") continue; // tolerate
        }
        stop_session();
        return 0;
    }

    // stdin batch mode
    if (!_isatty(_fileno(stdin))) {
        std::string line;
        while (std::getline(std::cin, line)) {
            CmdResult r = execute(line);
            print_result(r);
        }
        if (g_running) { stop_session(); }
        return 0;
    }

    // interactive REPL
    if (g_target.empty() && !g_quiet) printf("%s", HELP);
    std::string line;
    while (true) {
        printf("(aidbg) ");
        fflush(stdout);
        if (!std::getline(std::cin, line)) break;
        CmdResult r = execute(line);
        print_result(r);
    }
    if (g_running) { stop_session(); }
    return 0;
}
