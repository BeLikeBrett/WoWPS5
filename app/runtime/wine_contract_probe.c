// What Wine's native side needs from the console, measured. MIT license.
// Every line reports the kernel's own result; nothing here emulates Windows.
// Tests run from safe to risky and each risky step is announced first, so a
// crash still leaves the answer in klog.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#ifndef __PROSPERO__
bool wowps5WineContractProbe(const char* reportPath) { (void)reportPath; return true; }
#else
#include <cpuid.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/event.h>
#include <sys/select.h>
#include <sys/filio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <machine/sysarch.h>
#include <ps5platform/context.h>
#include <ps5platform/exec.h>
#include <ps5platform/kernel.h>
#include <ps5platform/shm.h>

// The public PS4 SDK's record, which the console keeps.
struct VirtualQueryInfo {
    void *start, *end; int64_t offset; int protection, memoryType;
    unsigned flexible:1, direct:1, stack:1, pooled:1, committed:1; char name[32];
};
int32_t sceKernelVirtualQuery(const void*, int, struct VirtualQueryInfo*, size_t);

// Leaf routines whose one faulting instruction the handler steps past: each
// returns 0, or 1 through wowps5GuardLanding when its site faulted.
int wowps5GuardLoad(const void* address, uint64_t* value);
int wowps5GuardStore(void* address, uint64_t value);
int wowps5GuardGsLoad(uint64_t offset, uint64_t* value);
int wowps5GuardRdgsbase(uint64_t* value);
int wowps5GuardWrgsbase(uint64_t value);
int wowps5GuardXsave(void* area, uint64_t mask);
int wowps5GuardXsavec(void* area, uint64_t mask);
int wowps5GuardXrstor(const void* area, uint64_t mask);
extern const char wowps5GuardLoadSite[], wowps5GuardStoreSite[], wowps5GuardGsLoadSite[],
    wowps5GuardRdgsbaseSite[], wowps5GuardWrgsbaseSite[], wowps5GuardXsaveSite[], wowps5GuardXsavecSite[],
    wowps5GuardXrstorSite[], wowps5GuardLanding[];
__asm__(".text\n"
    ".global wowps5GuardLoad, wowps5GuardLoadSite\nwowps5GuardLoad:\nwowps5GuardLoadSite:\n"
    " movq (%rdi),%rax\n movq %rax,(%rsi)\n xorl %eax,%eax\n ret\n"
    ".global wowps5GuardStore, wowps5GuardStoreSite\nwowps5GuardStore:\nwowps5GuardStoreSite:\n"
    " movq %rsi,(%rdi)\n xorl %eax,%eax\n ret\n"
    ".global wowps5GuardGsLoad, wowps5GuardGsLoadSite\nwowps5GuardGsLoad:\nwowps5GuardGsLoadSite:\n"
    " movq %gs:(%rdi),%rax\n movq %rax,(%rsi)\n xorl %eax,%eax\n ret\n"
    // rdgsbase %rax and wrgsbase %rdi as bytes: the assembler refuses them without -mfsgsbase
    ".global wowps5GuardRdgsbase, wowps5GuardRdgsbaseSite\nwowps5GuardRdgsbase:\nwowps5GuardRdgsbaseSite:\n"
    " .byte 0xf3,0x48,0x0f,0xae,0xc8\n movq %rax,(%rdi)\n xorl %eax,%eax\n ret\n"
    ".global wowps5GuardWrgsbase, wowps5GuardWrgsbaseSite\nwowps5GuardWrgsbase:\nwowps5GuardWrgsbaseSite:\n"
    " .byte 0xf3,0x48,0x0f,0xae,0xdf\n xorl %eax,%eax\n ret\n"
    // xsave64, xsavec64 and xrstor64 on (%rdi), with the mask in %rsi
    ".global wowps5GuardXsave, wowps5GuardXsaveSite\nwowps5GuardXsave:\n movq %rsi,%rax\n movq %rsi,%rdx\n shrq $32,%rdx\n"
    "wowps5GuardXsaveSite:\n .byte 0x48,0x0f,0xae,0x27\n xorl %eax,%eax\n ret\n"
    ".global wowps5GuardXsavec, wowps5GuardXsavecSite\nwowps5GuardXsavec:\n movq %rsi,%rax\n movq %rsi,%rdx\n shrq $32,%rdx\n"
    "wowps5GuardXsavecSite:\n .byte 0x48,0x0f,0xc7,0x27\n xorl %eax,%eax\n ret\n"
    ".global wowps5GuardXrstor, wowps5GuardXrstorSite\nwowps5GuardXrstor:\n movq %rsi,%rax\n movq %rsi,%rdx\n shrq $32,%rdx\n"
    "wowps5GuardXrstorSite:\n .byte 0x48,0x0f,0xae,0x2f\n xorl %eax,%eax\n ret\n"
    ".global wowps5GuardLanding\nwowps5GuardLanding:\n movl $1,%eax\n ret\n");

static FILE* report;
static unsigned failures;
static void say(const char* format, ...) {
    char line[600]; va_list arguments;
    va_start(arguments, format); vsnprintf(line, sizeof(line), format, arguments); va_end(arguments);
    fprintf(stderr, "[WoWPS5 Wine] %s\n", line); fflush(stderr);
    if(report) { fprintf(report, "%s\n", line); fflush(report); }
}
static void verdict(bool passed, const char* what) {
    if(!passed) failures++;
    say("%s: %s", passed ? "PASS" : "FAIL", what);
}

static struct {
    volatile int count, signal, code, onAltStack, user1Count;
    volatile uintptr_t address, rip, mcGs, mcFs, gsInHandler, rdiInContext, user1Thread;
    volatile uintptr_t fixPage, callTarget; volatile int fixResult;
    // the floating-point state the kernel put in the signal context
    volatile uint32_t realControl, realMxcsr; volatile uint64_t headerBv, headerCompact, ymm6High, xmm6Real;
    volatile uint64_t findMarker, dump[24]; volatile int markerOffset; volatile size_t contextBytes;
    volatile long fpFormat, fpOwned, mcFlags; volatile uint32_t fpMxcsr, fpMxcsrMask, fpControl; volatile uint64_t fpXmm6, fpExtraAddress, fpExtraBytes;
    uintptr_t altBase, altEnd;
} seen;
static struct sigaction oldActions[32];

static uint64_t gsBase(void) { uint64_t value = ~0ull; return sysarch(AMD64_GET_GSBASE, &value) ? ~0ull : value; }

static void faultHandler(int number, siginfo_t* info, void* opaque) {
    ucontext_t* context = opaque;
    const uintptr_t rip = context->uc_mcontext.mc_rip, here = (uintptr_t)&context;
    seen.count++; seen.signal = number; seen.code = info->si_code; seen.address = (uintptr_t)info->si_addr;
    seen.rip = rip; seen.rdiInContext = context->uc_mcontext.mc_rdi;
    seen.mcGs = context->uc_mcontext.mc_gsbase; seen.mcFs = context->uc_mcontext.mc_fsbase;
    seen.onAltStack = here >= seen.altBase && here < seen.altEnd;
    const uint8_t* state = (const uint8_t*)context->uc_mcontext.mc_fpstate;      // an fxsave image when the format is XMM
    seen.fpFormat = context->uc_mcontext.mc_fpformat; seen.fpOwned = context->uc_mcontext.mc_ownedfp; seen.mcFlags = context->uc_mcontext.mc_flags;
    seen.fpControl = state[0] | state[1] << 8;
    memcpy((void*)&seen.fpMxcsr, state + 24, 4); memcpy((void*)&seen.fpMxcsrMask, state + 28, 4); memcpy((void*)&seen.fpXmm6, state + 160 + 6*16, 8);
    seen.fpExtraAddress = context->uc_mcontext.mc_xfpustate; seen.fpExtraBytes = context->uc_mcontext.mc_xfpustate_len;
    // where the state really is: the first fault looks for a marker left in xmm6
    if(seen.findMarker && seen.markerOffset < 0) {
        const uint64_t* words = opaque;
        for(int w = 34; w < 400; w++) if(words[w] == seen.findMarker) { seen.markerOffset = w * 8; break; }   // past the general registers
        seen.realControl = (uint32_t)words[40]; seen.realMxcsr = (uint32_t)words[43];          // an fxsave image at offset 320
        seen.headerBv = words[104]; seen.headerCompact = words[105]; seen.ymm6High = words[104 + 8 + 12]; seen.xmm6Real = words[72];   // its XSAVE header at 320+512, AVX state after it
        for(int w = 0; w < 24; w++) seen.dump[w] = words[(offsetof(ucontext_t, uc_mcontext.mc_fpformat) / 8) - 4 + w];
        seen.contextBytes = sizeof(ucontext_t);
    }
    seen.gsInHandler = gsBase();
    // Wine's guard-page pattern: make the page accessible and rerun the instruction.
    if(seen.fixPage && (seen.address & ~(uintptr_t)0x3fff) == seen.fixPage) {
        seen.fixResult = sceKernelMprotect((void*)seen.fixPage, 0x4000, PS5_KERNEL_PROT_CPU_READ|PS5_KERNEL_PROT_CPU_WRITE);
        seen.fixPage = 0;
        if(!seen.fixResult) return;
    }
    static const char* const sites[] = { wowps5GuardLoadSite, wowps5GuardStoreSite, wowps5GuardGsLoadSite,
                                         wowps5GuardRdgsbaseSite, wowps5GuardWrgsbaseSite, wowps5GuardXsaveSite,
                                         wowps5GuardXsavecSite, wowps5GuardXrstorSite };
    for(unsigned i = 0; i < sizeof(sites)/sizeof(*sites); i++) if(rip == (uintptr_t)sites[i]) {
        context->uc_mcontext.mc_rip = (uintptr_t)wowps5GuardLanding;
        return;
    }
    // A call into memory that turned out not to be executable: return to the caller with -1.
    if(seen.callTarget && rip == seen.callTarget) {
        context->uc_mcontext.mc_rip = *(uint64_t*)context->uc_mcontext.mc_rsp;
        context->uc_mcontext.mc_rsp += 8; context->uc_mcontext.mc_rax = (uint64_t)-1;
        return;
    }
    // Not one of ours: hand the fault back, so the console writes its crash record.
    sigaction(number, &oldActions[number], NULL);
}
static void user1Handler(int number, siginfo_t* info, void* opaque) {
    ucontext_t* context = opaque; (void)number; (void)info;
    seen.user1Count++; seen.user1Thread = (uintptr_t)pthread_self();
    seen.gsInHandler = gsBase(); seen.mcGs = context->uc_mcontext.mc_gsbase;
}

static void probeCpu(void) {
    unsigned a, b, c, d, a7 = 0, b7 = 0, c7 = 0, d7 = 0;
    __get_cpuid(1, &a, &b, &c, &d);
    __get_cpuid_count(7, 0, &a7, &b7, &c7, &d7);
    say("cpu family_model=0x%x xsave=%u osxsave=%u avx=%u fma=%u avx2=%u bmi2=%u fsgsbase_cpuid=%u",
        a, c>>26&1, c>>27&1, c>>28&1, c>>12&1, b7>>5&1, b7>>8&1, b7&1);
    size_t flexible = 0; int64_t start = 0; size_t direct = 0;
    sceKernelAvailableFlexibleMemorySize(&flexible);
    sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), 0, &start, &direct);
    struct ps5_kernel_sw_version version = { .size = sizeof(version) };
    sceKernelGetSystemSwVersion(&version);
    say("system firmware=%s page=%ld flexible_free_mib=%zu direct_total_mib=%lld direct_largest_free_mib=%zu pid=%d",
        version.text, sysconf(_SC_PAGESIZE), flexible>>20, (long long)(sceKernelGetDirectMemorySize()>>20), direct>>20, (int)getpid());
}

static void probeMap(void) {
    struct VirtualQueryInfo info; const void* at = 0; unsigned entries = 0;
    struct { uintptr_t start, end; int protection; unsigned kind; char name[32]; } run = {0};
    for(; entries < 2000; entries++) {
        memset(&info, 0, sizeof(info));
        const int32_t result = sceKernelVirtualQuery(at, 1, &info, sizeof(info));
        const unsigned kind = result ? 99 : info.flexible | info.direct<<1 | info.stack<<2 | info.pooled<<3 | info.committed<<4;
        // adjacent entries alike in everything but address are one line
        if(!result && run.end == (uintptr_t)info.start && run.protection == info.protection && run.kind == kind &&
           !strncmp(run.name, info.name, sizeof(run.name))) { run.end = (uintptr_t)info.end; at = info.end; continue; }
        if(run.end) say("map %012lx-%012lx prot=0x%02x kind=0x%02x kib=%lu %.32s", (unsigned long)run.start, (unsigned long)run.end,
                        run.protection, run.kind, (unsigned long)((run.end-run.start)>>10), run.name);
        if(result) { say("map end entries=%u last_result=0x%08x", entries, (unsigned)result); break; }
        run.start = (uintptr_t)info.start; run.end = (uintptr_t)info.end; run.protection = info.protection; run.kind = kind;
        memcpy(run.name, info.name, sizeof(run.name));
        at = info.end;
    }
}

static bool installHandlers(void) {
    const size_t bytes = 256*1024; void* stack = malloc(bytes);
    stack_t alternate = { .ss_sp = stack, .ss_size = bytes, .ss_flags = 0 };
    const int alt = stack ? sigaltstack(&alternate, NULL) : -1;
    seen.altBase = (uintptr_t)stack; seen.altEnd = seen.altBase + bytes;
    say("signal sigaltstack result=%d errno=%d", alt, alt ? errno : 0);
    struct sigaction action; memset(&action, 0, sizeof(action)); sigemptyset(&action.sa_mask);
    action.sa_sigaction = faultHandler; action.sa_flags = SA_SIGINFO | SA_NODEFER | (alt ? 0 : SA_ONSTACK);
    int result = 0;
    static const int numbers[] = { SIGSEGV, SIGBUS, SIGILL };
    for(unsigned i = 0; i < 3; i++) if(sigaction(numbers[i], &action, &oldActions[numbers[i]])) result = errno;
    action.sa_sigaction = user1Handler;
    if(sigaction(SIGUSR1, &action, &oldActions[SIGUSR1])) result = errno;
    say("signal sigaction errno=%d previous_segv_handler=%p", result, (void*)oldActions[SIGSEGV].sa_sigaction);
    return !result;
}
static void removeHandlers(void) {
    static const int numbers[] = { SIGSEGV, SIGBUS, SIGILL, SIGUSR1 };
    for(unsigned i = 0; i < 4; i++) sigaction(numbers[i], &oldActions[numbers[i]], NULL);
    stack_t off = { .ss_flags = SS_DISABLE }; sigaltstack(&off, NULL);
}

static void probeFaults(void) {
    void* range = NULL;
    const int reserve = ps5_vrange_reserve(4*0x10000, (void*)0x1000000000ull, 0, &range);
    say("fault reserve result=0x%08x at=%p", (unsigned)reserve, range);
    if(reserve) { verdict(false, "a reserved range for the fault tests"); return; }
    uint8_t* base = range; uint64_t value = 0;
    say("next: deliberate read of reserved, unbacked memory at %p", (void*)(base+0x20000));
    seen.count = 0; seen.signal = 0; seen.code = 0;
    const uint64_t marker[2] = { 0x5750533566707866ull, 0 };
    uint32_t mxcsr = 0;
    const uint64_t wide[4] = { 0x5750533566707866ull, 0, 0x5750533579686967ull, 0 };
    __asm__("vmovdqu %0,%%ymm6\n stmxcsr %1" : : "m"(wide), "m"(mxcsr) : "xmm6");
    seen.findMarker = marker[0]; seen.markerOffset = -1;
    int faulted = wowps5GuardLoad(base+0x20000, &value);
    seen.findMarker = 0;
    say("fault fpstate xmm6_marker_at_context_offset=%d sdk_mc_fpstate_offset=%zu sdk_fpformat_offset=%zu sizeof_ucontext=%zu", seen.markerOffset,
        offsetof(ucontext_t, uc_mcontext.mc_fpstate), offsetof(ucontext_t, uc_mcontext.mc_fpformat), seen.contextBytes);
    say("fault fpstate at_offset_320 fcw=0x%x mxcsr=0x%x thread_mxcsr=0x%x xstate_bv=0x%llx xcomp_bv=0x%llx xmm6_matches=%d ymm6_high_matches=%d", seen.realControl, seen.realMxcsr, mxcsr,
        (unsigned long long)seen.headerBv, (unsigned long long)seen.headerCompact, seen.xmm6Real == wide[0], seen.ymm6High == wide[2]);
    verdict(seen.xmm6Real == wide[0] && seen.realMxcsr == mxcsr && seen.realControl == 0x37f && (seen.headerBv & 7) == 7 && !seen.headerCompact,
            "the signal context holds the thread's full XSAVE image, 32 bytes after where the SDK header puts mc_fpstate");
    for(int w = 0; w < 24; w += 4)
        say("fault fpstate words@%zu: %016llx %016llx %016llx %016llx", offsetof(ucontext_t, uc_mcontext.mc_fpformat) - 32 + (size_t)w*8,
            (unsigned long long)seen.dump[w], (unsigned long long)seen.dump[w+1], (unsigned long long)seen.dump[w+2], (unsigned long long)seen.dump[w+3]);
    say("fault fpstate format=0x%lx owned=0x%lx mc_flags=0x%lx fcw=0x%x mxcsr=0x%x (thread's is 0x%x) mxcsr_mask=0x%x xmm6_matches=%d xfpustate=0x%llx len=%llu",
        seen.fpFormat, seen.fpOwned, seen.mcFlags, seen.fpControl, seen.fpMxcsr, mxcsr, seen.fpMxcsrMask, seen.fpXmm6 == marker[0],
        (unsigned long long)seen.fpExtraAddress, (unsigned long long)seen.fpExtraBytes);
    say("fault unbacked_read faulted=%d signal=%d code=%d address_matches=%d rip_matches=%d rdi_matches=%d on_alt_stack=%d handler_runs=%d",
        faulted, seen.signal, seen.code, seen.address == (uintptr_t)(base+0x20000), seen.rip == (uintptr_t)wowps5GuardLoadSite,
        seen.rdiInContext == (uintptr_t)(base+0x20000), seen.onAltStack, seen.count);
    verdict(faulted == 1 && seen.count == 1 && seen.address == (uintptr_t)(base+0x20000) && seen.rip == (uintptr_t)wowps5GuardLoadSite,
            "a handler receives the fault's address and instruction, and resumes elsewhere by editing the saved rip");
    int commit = ps5_vrange_commit(base, 0x4000, PS5_SHM_READ|PS5_SHM_WRITE);
    int stored = commit ? -1 : wowps5GuardStore(base, 0x1122334455667788ull);
    int readOnly = commit ? -1 : ps5_vrange_commit(base, 0x4000, PS5_SHM_READ);
    seen.count = 0;
    say("next: deliberate write to a read-only page at %p", (void*)base);
    faulted = wowps5GuardStore(base, 1);
    int loaded = wowps5GuardLoad(base, &value);
    say("fault readonly_write commit=0x%08x first_store=%d protect=0x%08x faulted=%d signal=%d code=%d kept_value=%d",
        (unsigned)commit, stored, (unsigned)readOnly, faulted, seen.signal, seen.code, !loaded && value == 0x1122334455667788ull);
    verdict(faulted == 1 && seen.count == 1 && !loaded && value == 0x1122334455667788ull, "a write to a read-only page faults and changes nothing");
    seen.count = 0; seen.fixResult = -1; seen.fixPage = (uintptr_t)base;
    say("next: write fault repaired inside the handler, instruction rerun");
    faulted = wowps5GuardStore(base, 0xfeedfacecafebeefull);
    loaded = wowps5GuardLoad(base, &value);
    say("fault repair faulted=%d handler_runs=%d mprotect_in_handler=0x%08x value_written=%d",
        faulted, seen.count, (unsigned)seen.fixResult, !loaded && value == 0xfeedfacecafebeefull);
    verdict(!faulted && seen.count == 1 && !seen.fixResult && value == 0xfeedfacecafebeefull,
            "a handler changes the page's protection and the faulting write reruns (guard pages, write watches)");
    seen.fixPage = 0;
    say("fault cleanup decommit=0x%08x release=0x%08x", (unsigned)ps5_vrange_decommit(base, 0x10000), (unsigned)ps5_vrange_release(base, 4*0x10000));
}

struct ThreadTest { uint64_t block[16]; volatile int stage; uint64_t initialGs, readBack, afterSleep; int setResult; pthread_t self; };
static void* threadMain(void* argument) {
    struct ThreadTest* test = argument; uint64_t value = (uint64_t)test->block;
    test->self = pthread_self(); test->block[6] = (uint64_t)test->block;
    test->initialGs = gsBase();
    test->setResult = sysarch(AMD64_SET_GSBASE, &value) ? errno : 0;
    if(!test->setResult && gsBase() == value) wowps5GuardGsLoad(0x30, &test->readBack);
    test->stage = 1;
    for(int i = 0; i < 400 && test->stage < 2; i++) usleep(5000);   // the main thread signals this one here
    if(!test->setResult && gsBase() == value) wowps5GuardGsLoad(0x30, &test->afterSleep);
    return NULL;
}

static void probeGs(void) {
    static uint64_t block[16];
    uint64_t fs = 0, original = 0, value = (uint64_t)block, read = 0;
    block[6] = (uint64_t)block;                         // the TEB's self pointer, at %gs:0x30
    const int getFs = sysarch(AMD64_GET_FSBASE, &fs) ? errno : 0;
    const int getGs = sysarch(AMD64_GET_GSBASE, &original) ? errno : 0;
    say("gs sysarch_get_fsbase errno=%d value=0x%lx get_gsbase errno=%d value=0x%lx", getFs, (unsigned long)fs, getGs, (unsigned long)original);
    say("next: sysarch(AMD64_SET_GSBASE) to %p, then libc calls with it set", (void*)block);
    const int set = sysarch(AMD64_SET_GSBASE, &value) ? errno : 0;
    const uint64_t back = gsBase();
    say("gs sysarch_set_gsbase errno=%d read_back_matches=%d", set, back == value);
    if(set || back != value) {
        verdict(false, "sysarch sets the GS base");
    } else {
        int faulted = wowps5GuardGsLoad(0x30, &read);
        say("gs load_through_segment faulted=%d self_pointer_matches=%d", faulted, read == value);
        bool held = !faulted && read == value;
        verdict(held, "Windows code can reach a TEB through %gs:0x30");
        for(int i = 0; i < 200; i++) { sched_yield(); if(i % 50 == 0) usleep(10000); }
        read = 0; wowps5GuardGsLoad(0x30, &read);
        say("gs after_yield_and_sleep matches=%d", read == value);
        verdict(read == value, "the GS base survives context switches");
        seen.user1Count = 0; seen.gsInHandler = seen.mcGs = 0;
        raise(SIGUSR1);
        say("gs in_signal_handler runs=%d sysarch_matches=%d mcontext_gsbase=0x%lx mcontext_matches=%d", seen.user1Count,
            seen.gsInHandler == value, (unsigned long)seen.mcGs, seen.mcGs == value);
        read = 0; wowps5GuardGsLoad(0x30, &read);
        verdict(seen.user1Count == 1 && seen.gsInHandler == value && read == value, "the GS base holds inside and after a signal handler");

        struct ThreadTest test; memset(&test, 0, sizeof(test));
        pthread_attr_t attributes; pthread_attr_init(&attributes); pthread_attr_setstacksize(&attributes, 1024*1024);
        pthread_t thread; const int created = pthread_create(&thread, &attributes, threadMain, &test);
        if(!created) {
            for(int i = 0; i < 400 && !test.stage; i++) usleep(5000);
            seen.user1Count = 0; seen.user1Thread = 0;
            const int killed = pthread_kill(thread, SIGUSR1);
            for(int i = 0; i < 400 && !seen.user1Count; i++) usleep(5000);
            const bool delivered = seen.user1Count == 1 && seen.user1Thread == (uintptr_t)test.self;
            const bool threadGs = seen.gsInHandler == (uint64_t)test.block;
            test.stage = 2; pthread_join(thread, NULL);
            read = 0; wowps5GuardGsLoad(0x30, &read);
            say("gs second_thread inherits_creator=%d set_errno=%d read_back_matches=%d after_signal_matches=%d main_still_matches=%d",
                test.initialGs == value, test.setResult, test.readBack == (uint64_t)test.block, test.afterSleep == (uint64_t)test.block, read == value);
            say("signal pthread_kill result=%d delivered_to_target=%d handler_saw_target_gs=%d", killed, delivered, threadGs);
            // A new thread starts with its creator's base (FreeBSD copies the pcb); what matters is that setting one leaves the other.
            verdict(!test.setResult && test.readBack == (uint64_t)test.block && test.afterSleep == (uint64_t)test.block && read == value,
                    "each thread keeps a GS base of its own");
            verdict(delivered && threadGs, "a signal sent to one thread runs there, as Wine's suspend and context requests need");
        } else { say("gs second_thread create_error=%d", created); verdict(false, "a second thread for the GS test"); }
        pthread_attr_destroy(&attributes);
    }
    // FSGSBASE: the instructions FreeBSD's libc uses when the kernel enables them
    uint64_t direct = 0;
    say("next: rdgsbase and wrgsbase instructions (SIGILL when the kernel leaves them off)");
    seen.count = 0;
    const int readFault = wowps5GuardRdgsbase(&direct);
    const int writeFault = readFault ? 1 : wowps5GuardWrgsbase(direct);
    say("gs fsgsbase_instructions rdgsbase_faulted=%d signal=%d matches_sysarch=%d wrgsbase_faulted=%d",
        readFault, readFault ? seen.signal : 0, !readFault && direct == gsBase(), writeFault);
    const int restore = sysarch(AMD64_SET_GSBASE, &original) ? errno : 0;
    say("gs restored errno=%d value=0x%lx", restore, (unsigned long)gsBase());
}

// The extended register state Wine saves and restores around exceptions.
static void probeXstate(void) {
    unsigned a = 0, b = 0, c = 0, d = 0, a1 = 0, b1 = 0, c1 = 0, d1 = 0, low, high;
    __get_cpuid_count(0xd, 0, &a, &b, &c, &d);
    __get_cpuid_count(0xd, 1, &a1, &b1, &c1, &d1);
    __asm__("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    const uint64_t xcr0 = (uint64_t)high << 32 | low, cpu = (uint64_t)d << 32 | a;
    say("xstate xcr0=0x%llx cpuid_d0_features=0x%llx size_enabled=%u size_all=%u xsaveopt=%u xsavec=%u xgetbv1=%u xsaves=%u compact_size=%u",
        (unsigned long long)xcr0, (unsigned long long)cpu, b, c, a1 & 1, a1 >> 1 & 1, a1 >> 2 & 1, a1 >> 3 & 1, b1);
    static uint8_t area[4096] __attribute__((aligned(64)));
    uint64_t header[3];
    memset(area, 0, sizeof(area)); seen.count = 0;
    const int saved = wowps5GuardXsave(area, cpu); memcpy(header, area + 512, sizeof(header));
    const int restored = saved ? -1 : wowps5GuardXrstor(area, cpu);
    say("xstate standard xsave_faulted=%d signal=%d xstate_bv=0x%llx xcomp_bv=0x%llx xrstor_faulted=%d", saved, saved ? seen.signal : 0,
        (unsigned long long)header[0], (unsigned long long)header[1], restored);
    memset(area, 0, sizeof(area)); seen.count = 0;
    const int compacted = wowps5GuardXsavec(area, cpu); memcpy(header, area + 512, sizeof(header));
    const int compactRestored = compacted ? -1 : wowps5GuardXrstor(area, cpu);
    say("xstate compacted xsavec_faulted=%d signal=%d xstate_bv=0x%llx xcomp_bv=0x%llx xrstor_faulted=%d signal=%d", compacted, compacted ? seen.signal : 0,
        (unsigned long long)header[0], (unsigned long long)header[1], compactRestored, compactRestored > 0 ? seen.signal : 0);
    // what Wine builds for a context with no AVX state in use: compaction mask for every CPU feature, nothing present
    memset(area, 0, sizeof(area)); header[0] = 0; header[1] = 0x8000000000000000ull | (cpu & ~3ull); header[2] = 0;
    memcpy(area + 512, header, sizeof(header)); area[0] = 0x7f; area[1] = 0x03; area[24] = 0x80; area[25] = 0x1f;   // fcw 0x37f, mxcsr 0x1f80
    seen.count = 0;
    const int built = wowps5GuardXrstor(area, cpu);
    say("xstate built_compacted mask_all_cpu_features xrstor_faulted=%d signal=%d", built, built ? seen.signal : 0);
    memset(area + 512, 0, 64); header[1] = 0x8000000000000000ull | (xcr0 & ~3ull); memcpy(area + 512, header, sizeof(header));
    seen.count = 0;
    const int builtEnabled = wowps5GuardXrstor(area, xcr0);
    say("xstate built_compacted mask_enabled_features xrstor_faulted=%d signal=%d", builtEnabled, builtEnabled ? seen.signal : 0);
    memset(area + 512, 0, 64); seen.count = 0;
    const int builtStandard = wowps5GuardXrstor(area, xcr0);
    say("xstate built_standard empty_header xrstor_faulted=%d signal=%d", builtStandard, builtStandard ? seen.signal : 0);
}

static void probePlacement(void) {
    static const struct { uintptr_t address; size_t bytes; const char* name; } places[] = {
        { 0x10000, 0x10000, "lowest_64k" },
        { 0x110000, 0x100000, "low_1mib" },
        { 0x7ffe0000, 0x10000, "KUSER_SHARED_DATA" },
        { 0x140000000ull, 0x8590000, "WowB.exe_preferred_base" },
        { 0x180000000ull, 0x29a0000, "WowB_loader.dll_preferred_base" },
        { 0x1000000000ull, 0x100000000ull, "4gib_at_64gib" },
        { 0x7ffffffe0000ull, 0x10000, "top_of_windows_user_space" },
    };
    for(unsigned i = 0; i < sizeof(places)/sizeof(*places); i++) {
        void* at = (void*)places[i].address; uint64_t value = 0;
        const int reserve = ps5_vrange_reserve_at(at, places[i].bytes);
        int commit = -1, readOnly = -1, stored = -1, blocked = -1, release = -1;
        if(!reserve) {
            commit = ps5_vrange_commit(at, 0x4000, PS5_SHM_READ|PS5_SHM_WRITE);
            if(!commit) {
                stored = wowps5GuardStore(at, 0x5750533500000000ull | i) || wowps5GuardLoad(at, &value) || value != (0x5750533500000000ull | i);
                readOnly = ps5_vrange_commit(at, 0x4000, PS5_SHM_READ);
                blocked = wowps5GuardStore(at, 0);
                ps5_vrange_decommit(at, 0x10000);
            }
            release = ps5_vrange_release(at, places[i].bytes);
        }
        say("place %s at=0x%lx bytes=0x%lx reserve=0x%08x commit=0x%08x write_read_failed=%d readonly=0x%08x readonly_write_faulted=%d release=0x%08x",
            places[i].name, (unsigned long)places[i].address, (unsigned long)places[i].bytes, (unsigned)reserve, (unsigned)commit, stored,
            (unsigned)readOnly, blocked, (unsigned)release);
    }
    // Code at the game's preferred base: direct memory, execute added after the map
    struct ps5_exec_request request = { .bytes = 0x10000, .address = 0x140000000ull, .flags = PS5_EXEC_AT };
    struct ps5_exec_region region; memset(&region, 0, sizeof(region));
    const int allocate = ps5_exec_alloc(&request, &region);
    int answer = -1;
    if(!allocate) {
        static const uint8_t code[] = { 0xb8, 0x2a, 0, 0, 0, 0xc3 };   // mov eax,42; ret
        memcpy(region.write_view, code, sizeof(code));
        answer = ((int(*)(void))region.base)();
        ps5_exec_free(&region);
    }
    say("place code_at_0x140000000 alloc=0x%08x returned=%d", (unsigned)allocate, answer);
    verdict(!allocate && answer == 42, "code runs at WowB.exe's preferred base");

    // The plain BSD calls Wine's virtual.c is written against
    void* anonymous = mmap(NULL, 0x10000, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
    say("mmap anonymous_rw result=%p errno=%d", anonymous, anonymous == MAP_FAILED ? errno : 0);
    if(anonymous != MAP_FAILED) {
        const int execute = mprotect(anonymous, 0x4000, PROT_READ|PROT_EXEC) ? errno : 0;
        const int none = mprotect(anonymous, 0x4000, PROT_NONE) ? errno : 0;
        say("mmap anonymous mprotect_rx_errno=%d mprotect_none_errno=%d munmap=%d", execute, none, munmap(anonymous, 0x10000));
    }
    void* executable = mmap(NULL, 0x10000, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANON, -1, 0);
    say("mmap anonymous_rwx result=%p errno=%d", executable, executable == MAP_FAILED ? errno : 0);
    if(executable != MAP_FAILED) {
        static const uint8_t code[] = { 0xb8, 0x2a, 0, 0, 0, 0xc3 };
        memcpy(executable, code, sizeof(code));
        say("next: call into plain mmap memory mapped read-write-execute at %p", executable);
        seen.count = 0; seen.callTarget = (uintptr_t)executable;
        const int returned = ((int(*)(void))executable)();
        seen.callTarget = 0;
        say("mmap anonymous_rwx_call returned=%d faults=%d signal=%d code=%d", returned, seen.count, seen.count ? seen.signal : 0, seen.count ? seen.code : 0);
        munmap(executable, 0x10000);
    }
    void* fixed = mmap((void*)0x7ffd0000, 0x10000, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON|MAP_FIXED|MAP_EXCL, -1, 0);
    say("mmap anonymous_fixed_excl_0x7ffd0000 result=%p errno=%d", fixed, fixed == MAP_FAILED ? errno : 0);
    if(fixed != MAP_FAILED) munmap(fixed, 0x10000);
    void* noReserve = mmap((void*)0x2000000000ull, 0x40000000, PROT_NONE, MAP_PRIVATE|MAP_ANON, -1, 0);
    say("mmap reserve_1gib_prot_none hint=0x2000000000 result=%p errno=%d", noReserve, noReserve == MAP_FAILED ? errno : 0);
    if(noReserve != MAP_FAILED) munmap(noReserve, 0x40000000);
    const int file = open("/data/wowps5/client/WowB.exe", O_RDONLY);
    say("file open_client_exe fd=%d errno=%d", file, file < 0 ? errno : 0);
    if(file >= 0) {
        uint8_t* view = mmap(NULL, 0x10000, PROT_READ, MAP_PRIVATE, file, 0);
        say("mmap file_private_readonly result=%p errno=%d header_ok=%d", (void*)view, view == MAP_FAILED ? errno : 0,
            view != MAP_FAILED && view[0] == 'M' && view[1] == 'Z');
        if(view != MAP_FAILED) munmap(view, 0x10000);
        view = mmap(NULL, 0x10000, PROT_READ|PROT_WRITE, MAP_PRIVATE, file, 0);
        say("mmap file_private_copy_on_write result=%p errno=%d", (void*)view, view == MAP_FAILED ? errno : 0);
        if(view != MAP_FAILED) { view[0] = 'm'; say("mmap file_private_copy_on_write stored=%d", view[0] == 'm'); munmap(view, 0x10000); }
        close(file);
    }
    const int scratch = open("/data/wowps5/contract-scratch.bin", O_RDWR|O_CREAT|O_TRUNC, 0600);
    say("file create_scratch fd=%d errno=%d", scratch, scratch < 0 ? errno : 0);
    if(scratch >= 0) {
        const int sized = ftruncate(scratch, 0x10000) ? errno : 0;
        uint8_t* view = mmap(NULL, 0x10000, PROT_READ|PROT_WRITE, MAP_SHARED, scratch, 0);
        uint8_t byte = 0; int reads = -1;
        if(view != MAP_FAILED) { view[5] = 0x5a; msync(view, 0x10000, MS_SYNC); reads = pread(scratch, &byte, 1, 5); munmap(view, 0x10000); }
        say("mmap file_shared_readwrite ftruncate_errno=%d result=%p errno=%d written_through=%d", sized, (void*)view,
            view == MAP_FAILED ? errno : 0, reads == 1 && byte == 0x5a);
        close(scratch); unlink("/data/wowps5/contract-scratch.bin");
    }
}

static void probeIpc(void) {
    int pair[2] = {-1,-1}, pipes[2] = {-1,-1};
    const int paired = socketpair(AF_UNIX, SOCK_STREAM, 0, pair) ? errno : 0;
    const int piped = pipe(pipes) ? errno : 0;
    say("ipc socketpair errno=%d pipe errno=%d", paired, piped);
    if(paired || piped) { verdict(false, "a socket pair and a pipe"); return; }
    // one descriptor passed over the socket, as every Wine server reply that carries a handle does
    char byte = 'W', control[CMSG_SPACE(sizeof(int))]; memset(control, 0, sizeof(control));
    struct iovec vector = { &byte, 1 };
    struct msghdr message; memset(&message, 0, sizeof(message));
    message.msg_iov = &vector; message.msg_iovlen = 1; message.msg_control = control; message.msg_controllen = sizeof(control);
    struct cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS; header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &pipes[1], sizeof(int));
    const int sent = sendmsg(pair[0], &message, 0) == 1 ? 0 : errno;
    struct pollfd waiting = { pair[1], POLLIN, 0 };
    const int polled = poll(&waiting, 1, 1000);
    const int queue = kqueue(); struct kevent change, event; memset(&event, 0, sizeof(event));
    EV_SET(&change, pair[1], EVFILT_READ, EV_ADD, 0, 0, 0);
    const struct timespec second = { 1, 0 };
    const int queued = queue < 0 ? -1 : kevent(queue, &change, 1, &event, 1, &second);
    char got = 0; memset(control, 0, sizeof(control)); vector.iov_base = &got;
    message.msg_controllen = sizeof(control);
    const int received = recvmsg(pair[1], &message, 0) == 1 ? 0 : errno;
    int passed = -1; header = CMSG_FIRSTHDR(&message);
    if(!received && header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) memcpy(&passed, CMSG_DATA(header), sizeof(int));
    char through = 0;
    const bool works = passed >= 0 && write(passed, "P", 1) == 1 && read(pipes[0], &through, 1) == 1 && through == 'P';
    say("ipc sendmsg_errno=%d poll=%d revents=0x%x kqueue=%d kevent=%d filter=%d recvmsg_errno=%d passed_fd=%d passed_fd_works=%d",
        sent, polled, waiting.revents, queue, queued, (int)event.filter, received, passed, works);
    verdict(!sent && polled == 1 && queued == 1 && !received && got == 'W' && works,
            "a descriptor passes over a Unix socket pair, and poll and kqueue see the socket readable");
    if(passed >= 0) close(passed);
    if(queue >= 0) close(queue);
    close(pair[0]); close(pair[1]); close(pipes[0]); close(pipes[1]);
}

// How a descriptor can be duplicated. Wine's server and client both hold
// copies of every file's descriptor.
static int passOverSocket(int fd) {
    int pair[2], copy = -1; char byte = 'D', control[CMSG_SPACE(sizeof(int))];
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return -1;
    struct iovec vector = { &byte, 1 };
    struct msghdr message; memset(&message, 0, sizeof(message)); memset(control, 0, sizeof(control));
    message.msg_iov = &vector; message.msg_iovlen = 1; message.msg_control = control; message.msg_controllen = sizeof(control);
    struct cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS; header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &fd, sizeof(int));
    if(sendmsg(pair[0], &message, 0) == 1) {
        memset(control, 0, sizeof(control)); message.msg_controllen = sizeof(control);
        if(recvmsg(pair[1], &message, 0) == 1 && (header = CMSG_FIRSTHDR(&message)) && header->cmsg_type == SCM_RIGHTS)
            memcpy(&copy, CMSG_DATA(header), sizeof(int));
    }
    const int saved = errno; close(pair[0]); close(pair[1]); errno = saved;
    return copy;
}
static void probeDuplicate(const char* name, int fd) {
    if(fd < 0) { say("dup %s: not opened errno=%d", name, errno); return; }
    int results[6], errors[6];
    errno = 0; results[0] = dup(fd); errors[0] = errno;
    errno = 0; results[1] = dup2(fd, 400); errors[1] = errno;
    errno = 0; results[2] = fcntl(fd, F_DUPFD, 0); errors[2] = errno;
    errno = 0; results[3] = fcntl(fd, F_DUPFD, 500); errors[3] = errno;
    errno = 0; results[4] = fcntl(fd, F_DUPFD_CLOEXEC, 0); errors[4] = errno;
    errno = 0; results[5] = passOverSocket(fd); errors[5] = errno;
    say("dup %s fd=%d: dup=%d/%d dup2_to_400=%d/%d F_DUPFD_0=%d/%d F_DUPFD_500=%d/%d F_DUPFD_CLOEXEC=%d/%d SCM_RIGHTS=%d/%d (result/errno)", name, fd,
        results[0], results[0] < 0 ? errors[0] : 0, results[1], results[1] < 0 ? errors[1] : 0, results[2], results[2] < 0 ? errors[2] : 0,
        results[3], results[3] < 0 ? errors[3] : 0, results[4], results[4] < 0 ? errors[4] : 0, results[5], results[5] < 0 ? errors[5] : 0);
    // a copy made over the socket shares the file position, as dup's would
    if(results[5] >= 0 && lseek(fd, 0, SEEK_CUR) >= 0) {
        const off_t before = lseek(fd, 0, SEEK_CUR); char two[2];
        const ssize_t got = read(results[5], two, 2);
        say("dup %s SCM_RIGHTS copy reads=%zd shares_position=%d", name, got, got == 2 && lseek(fd, 0, SEEK_CUR) == before + 2);
        lseek(fd, before, SEEK_SET);
    }
    for(int i = 0; i < 6; i++) if(results[i] >= 0) close(results[i]);
}
// Descriptor flags: Wine's server makes every socket and pipe non-blocking.
static void probeFlags(const char* name, int fd) {
    if(fd < 0) return;
    errno = 0; const int flags = fcntl(fd, F_GETFL); const int getError = errno;
    errno = 0; const int set = fcntl(fd, F_SETFL, flags | O_NONBLOCK); const int setError = errno;
    errno = 0; const int setPlain = fcntl(fd, F_SETFL, O_NONBLOCK); const int setPlainError = errno;
    const int after = fcntl(fd, F_GETFL);
    errno = 0; const int descriptorFlags = fcntl(fd, F_GETFD); const int getFdError = errno;
    errno = 0; const int close_on_exec = fcntl(fd, F_SETFD, FD_CLOEXEC); const int setFdError = errno;
    int one = 1, zero = 0;
    errno = 0; const int control = ioctl(fd, FIONBIO, &one); const int controlError = errno;
    const int afterControl = fcntl(fd, F_GETFL);
    ioctl(fd, FIONBIO, &zero);
    say("flags %s: F_GETFL=0x%x/%d F_SETFL(flags|O_NONBLOCK)=%d/%d F_SETFL(O_NONBLOCK)=%d/%d then=0x%x F_GETFD=%d/%d F_SETFD=%d/%d FIONBIO=%d/%d then=0x%x",
        name, flags, getError, set, setError, setPlain, setPlainError, after, descriptorFlags, getFdError, close_on_exec, setFdError, control, controlError, afterControl);
}

// Whether a socket really stops blocking. A helper thread sends a byte after
// 300 ms: a receive that comes back at once with EAGAIN did not block; one
// that returns the byte waited for it.
static void* lateSender(void* argument) { usleep(300000); const int fd = *(int*)argument; (void)write(fd, "L", 1); return NULL; }
static void probeNonblocking(const char* method, int kind) {
    int pair[2] = { -1, -1 }, applied = 0, appliedError = 0, receiveFlags = 0, one = 1;
    if(socketpair(AF_UNIX, SOCK_STREAM | (kind == 3 ? SOCK_NONBLOCK : 0), 0, pair)) { say("nonblocking %s: socketpair errno=%d", method, errno); return; }
    errno = 0;
    if(kind == 0) applied = fcntl(pair[0], F_SETFL, O_NONBLOCK);
    if(kind == 1) applied = setsockopt(pair[0], SOL_SOCKET, 0x1200 /* SO_NBIO, Sony's */, &one, sizeof(one));
    if(kind == 2) applied = ioctl(pair[0], FIONBIO, &one);
    if(kind == 4) receiveFlags = MSG_DONTWAIT;
    appliedError = errno;
    pthread_t thread; pthread_create(&thread, NULL, lateSender, &pair[1]);
    struct timespec before, after; char byte = 0;
    clock_gettime(CLOCK_MONOTONIC, &before);
    errno = 0; const ssize_t got = recv(pair[0], &byte, 1, receiveFlags); const int receiveError = errno;
    clock_gettime(CLOCK_MONOTONIC, &after);
    const long waited = (after.tv_sec - before.tv_sec) * 1000 + (after.tv_nsec - before.tv_nsec) / 1000000;
    pthread_join(thread, NULL);
    say("nonblocking %s: set=%d/%d recv=%zd/%d after_ms=%ld really_nonblocking=%d", method, applied, appliedError, got, receiveError, waited,
        got == -1 && receiveError == EAGAIN && waited < 100);
    close(pair[0]); close(pair[1]);
}

static void probeDescriptors(void) {
    probeNonblocking("fcntl_F_SETFL", 0);
    probeNonblocking("setsockopt_SO_NBIO", 1);
    probeNonblocking("ioctl_FIONBIO", 2);
    probeNonblocking("socket_SOCK_NONBLOCK", 3);
    probeNonblocking("recv_MSG_DONTWAIT", 4);
    int pipes[2] = {-1, -1}; pipe(pipes);
    const int data = open("/data/wowps5/client/WowB.exe", O_RDONLY);
    const int title = open("/app0/sce_sys/param.json", O_RDONLY);
    const int directory = open("/data/wowps5", O_RDONLY);
    const int null = open("/dev/null", O_RDWR);
    const int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    probeDuplicate("stderr", 2);
    probeDuplicate("pipe", pipes[0]);
    probeDuplicate("socket", sock);
    probeDuplicate("data_file", data);
    probeDuplicate("title_file", title);
    probeDuplicate("data_directory", directory);
    probeDuplicate("dev_null", null);
    int pair[2] = {-1, -1}; socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
    probeFlags("socketpair", pair[0]);
    probeFlags("pipe", pipes[0]);
    probeFlags("data_file", data);
    errno = 0;
    const int nonblocking = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    say("flags socket(SOCK_NONBLOCK|SOCK_CLOEXEC)=%d/%d F_GETFL=0x%x", nonblocking, nonblocking < 0 ? errno : 0, nonblocking < 0 ? 0 : fcntl(nonblocking, F_GETFL));
    if(nonblocking >= 0) close(nonblocking);
    if(pair[0] >= 0) { close(pair[0]); close(pair[1]); }
    const int fds[] = { pipes[0], pipes[1], data, title, directory, null, sock };
    for(unsigned i = 0; i < sizeof(fds)/sizeof(*fds); i++) if(fds[i] >= 0) close(fds[i]);
}

// Every name Wine's native side imports must be backed by a loaded module on
// this firmware. The stub libraries list what newer firmware exports; a name
// this console's modules lack is left null and faults at its first call.
extern const struct { const char* name; const void* address; } wowps5WineImports[];
extern const size_t wowps5WineImportCount;
static void probeImports(void) {
    unsigned missing = 0;
    for(size_t i = 0; i < wowps5WineImportCount; i++)
        if(!wowps5WineImports[i].address) { missing++; say("import %s unresolved", wowps5WineImports[i].name); }
    say("imports checked=%zu unresolved=%u", wowps5WineImportCount, missing);
    verdict(!missing, "every import of Wine's native side resolves on this firmware");
    // resolved, but built on a kernel call the title's libkernel lacks: it jumps to an unmapped address
    say("imports note getcwd resolves into libSceLibcInternal and faults when called (run wine-20261004-104738)");
}

// Directory listing: Wine finds a file whose name differs in case by reading its directory.
static void probeDirectory(const char* path) {
    errno = 0;
    DIR* directory = opendir(path);
    const int openError = errno;
    unsigned entries = 0, files = 0; char first[64] = "", wanted = 0;
    struct dirent* entry;
    errno = 0;
    while(directory && (entry = readdir(directory))) {
        if(!entries) snprintf(first, sizeof(first), "%s", entry->d_name);
        entries++; files += entry->d_type == DT_REG;
        wanted |= !strcmp(entry->d_name, "advapi32.dll") || !strcmp(entry->d_name, "system.reg");
    }
    const int readError = errno;
    say("directory %s: opendir=%s/%d entries=%u regular=%u first=%s readdir_errno=%d finds_known_name=%d",
        path, directory ? "ok" : "null", openError, entries, files, first, readError, wanted);   // statfs is null here: only libkernel_sys has it
    if(directory) closedir(directory);
}

// The file queries Wine makes for every path it resolves.
// What the console charges for the calls Wine makes on every Windows request:
// nanoseconds each, the median of five rounds.
int sceKernelUsleep(unsigned microseconds);
static double costNow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
#define COST(name, count, body) do { double rounds[5]; for(int r = 0; r < 5; r++) { const double start = costNow(); \
    for(int i = 0; i < (count); i++) { body; } rounds[r] = (costNow() - start) / (count); } \
    for(int a = 0; a < 5; a++) for(int b = a + 1; b < 5; b++) if(rounds[b] < rounds[a]) { const double t = rounds[a]; rounds[a] = rounds[b]; rounds[b] = t; } \
    say("cost %-44s %9.0f ns", name, rounds[2]); } while(0)
static void* costEcho(void* argument) { int* fds = argument; char byte; while(read(fds[0], &byte, 1) == 1 && byte != 'q') (void)write(fds[3], &byte, 1); return NULL; }
static void probeCosts(void) {
    struct timespec t; sigset_t set, old; sigemptyset(&set); sigaddset(&set, SIGUSR1);
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER; static char block[65536]; char byte = 'x';
    COST("clock_gettime(CLOCK_MONOTONIC)", 200000, clock_gettime(CLOCK_MONOTONIC, &t));
    COST("clock_gettime(CLOCK_REALTIME)", 200000, clock_gettime(CLOCK_REALTIME, &t));
    COST("pthread_sigmask block + restore", 200000, (pthread_sigmask(SIG_BLOCK, &set, &old), pthread_sigmask(SIG_SETMASK, &old, NULL)));
    COST("pthread_mutex lock + unlock", 1000000, (pthread_mutex_lock(&mutex), pthread_mutex_unlock(&mutex)));
    COST("getpid", 200000, (void)getpid());
    COST("usleep(1000)", 100, usleep(1000));
    COST("usleep(100)", 200, usleep(100));
    COST("nanosleep 1 ms", 100, ({ struct timespec ms = { 0, 1000000 }; nanosleep(&ms, NULL); }));
    COST("select with a 1 ms timeout", 100, ({ struct timeval tv = { 0, 1000 }; select(0, NULL, NULL, NULL, &tv); }));
    COST("poll with a 1 ms timeout", 100, poll(NULL, 0, 1));
    { const int queue = kqueue(); struct kevent event; struct timespec ms = { 0, 1000000 };
      COST("kevent with a 1 ms timeout", 100, kevent(queue, NULL, 0, &event, 1, &ms)); close(queue); }
    COST("sceKernelUsleep(1000)", 100, sceKernelUsleep(1000));
    COST("sched_yield", 100000, sched_yield());
    int fds[4]; if(pipe(fds) == 0 && pipe(fds + 2) == 0) {
        COST("pipe write + read, same thread", 100000, ((void)write(fds[1], &byte, 1), (void)read(fds[0], &byte, 1)));
        pthread_t echo; pthread_create(&echo, NULL, costEcho, fds);
        COST("pipe round trip through another thread", 20000, ((void)write(fds[1], &byte, 1), (void)read(fds[2], &byte, 1)));
        byte = 'q'; (void)write(fds[1], &byte, 1); pthread_join(echo, NULL);
    }
    const int file = open("/data/wowps5/test/cost-probe.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if(file >= 0) {
        COST("pwrite 64 bytes", 2000, (void)pwrite(file, block, 64, 0));
        COST("pwrite 4 KiB", 2000, (void)pwrite(file, block, 4096, 0));
        COST("pwrite 64 KiB", 1000, (void)pwrite(file, block, 65536, 0));
        COST("append 64 bytes (growing file)", 2000, (void)write(file, block, 64));
        COST("pread 64 bytes", 20000, (void)pread(file, block, 64, 0));
        COST("pread 64 KiB", 5000, (void)pread(file, block, 65536, 0));
        COST("fstat", 50000, ({ struct stat info; fstat(file, &info); }));
        COST("lseek", 100000, (void)lseek(file, 0, SEEK_SET));
        close(file);
    }
    COST("open + close an existing file", 2000, ({ const int fd = open("/data/wowps5/test/cost-probe.bin", O_RDONLY); if(fd >= 0) close(fd); }));
    COST("stat by path", 5000, ({ struct stat info; stat("/data/wowps5/test/cost-probe.bin", &info); }));
    unlink("/data/wowps5/test/cost-probe.bin");
}

// How many descriptors a title may hold, by kind: the client holds about 660
// files, 500 pipes and a few sockets at its login screen.
static void probeDescriptorLimits(void) {
    enum { MOST = 4096 };
    static int held[MOST];
    struct rlimit limit; memset(&limit, 0, sizeof(limit));
    const int limited = getrlimit(RLIMIT_NOFILE, &limit);
    say("descriptors: RLIMIT_NOFILE %d cur=%lld max=%lld sysconf(_SC_OPEN_MAX)=%ld", limited, (long long)limit.rlim_cur, (long long)limit.rlim_max, sysconf(_SC_OPEN_MAX));
    static const struct { const char* what; const char* path; } files[] = {
        { "one file of the title, read", "/app0/sce_sys/param.json" },
        { "one file on /data, read", "/data/wowps5/prefix/system.reg" },
    };
    for(unsigned kind = 0; kind < sizeof(files)/sizeof(*files); kind++) {
        int count = 0, highest = -1; errno = 0;
        while(count < MOST && (held[count] = open(files[kind].path, O_RDONLY)) >= 0) { if(held[count] > highest) highest = held[count]; count++; }
        const int error = count < MOST ? errno : 0;
        for(int i = 0; i < count; i++) close(held[i]);
        say("descriptors: %s opened %d times, then errno %d (highest descriptor %d)", files[kind].what, count, error, highest);
    }
    int count = 0; errno = 0;
    while(count + 1 < MOST && pipe(&held[count]) == 0) count += 2;
    int error = count + 1 < MOST ? errno : 0;
    // with the pipes still held: how many files on top of them?
    int extra = 0; static int more[MOST]; int fileError = 0; errno = 0;
    while(extra < MOST && (more[extra] = open("/app0/sce_sys/param.json", O_RDONLY)) >= 0) extra++;
    fileError = extra < MOST ? errno : 0;
    for(int i = 0; i < extra; i++) close(more[i]);
    for(int i = 0; i < count; i++) close(held[i]);
    say("descriptors: %d pipe ends, then errno %d; with those held, %d more files, then errno %d", count, error, extra, fileError);
    count = 0; errno = 0;
    while(count + 1 < MOST && socketpair(AF_UNIX, SOCK_STREAM, 0, &held[count]) == 0) count += 2;
    error = count + 1 < MOST ? errno : 0;
    for(int i = 0; i < count; i++) close(held[i]);
    say("descriptors: %d socket-pair ends, then errno %d", count, error);
}

// Anonymous shared memory objects, for the memory behind Windows sections that
// have no file: whether the kernel gives them to a title, whether two can be
// told apart, how they are read and written, and what memory they draw on.
static void probeSharedObjects(void) {
    size_t before = 0, mapped = 0, shrunk = 0;
    sceKernelAvailableFlexibleMemorySize(&before);
    errno = 0; const int first = shm_open(SHM_ANON, O_RDWR, 0600); const int firstError = errno;
    errno = 0; const int second = shm_open(SHM_ANON, O_RDWR, 0600); const int secondError = errno;
    struct stat one, two; memset(&one, 0, sizeof(one)); memset(&two, 0, sizeof(two));
    const int statOne = first >= 0 ? fstat(first, &one) : -1, statTwo = second >= 0 ? fstat(second, &two) : -1;
    say("shm anonymous: fds %d/%d errno %d/%d fstat %d/%d dev %llx/%llx ino %llu/%llu mode %o", first, second, firstError, secondError, statOne, statTwo,
        (unsigned long long)one.st_dev, (unsigned long long)two.st_dev, (unsigned long long)one.st_ino, (unsigned long long)two.st_ino, (unsigned)one.st_mode);
    if(first < 0) return;
    const size_t bytes = (size_t)64 << 20;
    errno = 0; const int sized = ftruncate(first, (off_t)bytes); const int sizeError = errno;
    fstat(first, &one);
    errno = 0; unsigned char* a = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, first, 0); const int mapError = errno;
    errno = 0; unsigned char* b = mmap(NULL, 0x10000, PROT_READ, MAP_SHARED, first, 0x10000); const int secondMapError = errno;
    bool shared = false, zero = false;
    if(a != MAP_FAILED) {
        zero = a[0] == 0 && a[bytes - 1] == 0;
        for(size_t at = 0; at < bytes; at += 0x4000) a[at] = (unsigned char)(at >> 14);
        a[0x10123] = 0x5a;
        shared = b != MAP_FAILED && b[0x123] == 0x5a;
    }
    sceKernelAvailableFlexibleMemorySize(&mapped);
    unsigned char byte = 0;
    errno = 0; const ssize_t got = pread(first, &byte, 1, 0x10123); const int readError = errno;
    errno = 0; const ssize_t put = pwrite(first, "\x77", 1, 0x20000); const int writeError = errno;
    say("shm 64 MiB: ftruncate %d/%d size %lld mmap %s/%d second view %s/%d zero %d shared %d pread %zd/%d (0x%02x) pwrite %zd/%d (0x%02x) flexible MiB %zu -> %zu",
        sized, sizeError, (long long)one.st_size, a != MAP_FAILED ? "ok" : "failed", mapError, b != MAP_FAILED ? "ok" : "failed", secondMapError, zero, shared,
        got, readError, byte, put, writeError, a != MAP_FAILED ? a[0x20000] : 0, before >> 20, mapped >> 20);
    // does emptying the object give its memory back while views of it exist, and afterwards?
    errno = 0; const int emptied = ftruncate(first, 0); const int emptyError = errno;
    sceKernelAvailableFlexibleMemorySize(&shrunk);
    if(a != MAP_FAILED) munmap(a, bytes);
    if(b != MAP_FAILED) munmap(b, 0x10000);
    close(first); if(second >= 0) close(second);
    size_t after = 0; sceKernelAvailableFlexibleMemorySize(&after);
    say("shm released: ftruncate(0) with views %d/%d flexible MiB %zu, after unmap and close %zu", emptied, emptyError, shrunk >> 20, after >> 20);
}

static void probeFiles(void) {
    probeDirectory("/data/wowps5/prefix");
    probeDirectory("/data/wowps5/wine/lib/wine/x86_64-windows");
    probeDirectory("/app0");
    // the working directory: Wine lists a directory by changing into it
    struct stat inside; memset(&inside, 0, sizeof(inside));
    errno = 0; const int changed = chdir("/data/wowps5"); const int changeError = errno;
    errno = 0; const int relative = stat("prefix/system.reg", &inside); const int relativeError = errno;
    errno = 0; const int dot = open(".", O_RDONLY); const int dotError = errno;
    errno = 0; DIR* here = opendir("."); const int hereError = errno;
    unsigned names = 0; while(here && readdir(here)) names++;
    errno = 0; const int back = chdir("/"); const int backError = errno;
    say("cwd chdir=%d/%d relative_stat=%d/%d open_dot=%d/%d opendir_dot=%s/%d entries=%u chdir_root=%d/%d", changed, changeError, relative, relativeError,
        dot >= 0 ? 0 : -1, dotError, here ? "ok" : "null", hereError, names, back, backError);
    if(here) closedir(here);
    if(dot >= 0) close(dot);
    static const char* const paths[] = { "/data/wowps5/prefix/dosdevices/c:/users", "/data/wowps5/prefix/dosdevices/c:/users/",
        "/data/wowps5/prefix/dosdevices/c:/users/..", "/data/wowps5/prefix/system.reg", "/data/wowps5/client/WowB.exe", "/data/wowps5/missing" };
    for(unsigned i = 0; i < sizeof(paths)/sizeof(*paths); i++) {
        struct stat st; memset(&st, 0, sizeof(st));
        errno = 0; const int plain = stat(paths[i], &st); const int plainError = errno;
        errno = 0; const int link = lstat(paths[i], &st); const int linkError = errno;
        errno = 0; const int reach = access(paths[i], R_OK); const int reachError = errno;
        errno = 0; const int fd = open(paths[i], O_RDONLY); const int openError = errno;
        int handle = -1, handleError = 0;
        if(fd >= 0) { errno = 0; handle = fstat(fd, &st); handleError = errno; close(fd); }
        say("file %s: stat=%d/%d lstat=%d/%d access=%d/%d open=%d/%d fstat=%d/%d mode=0%o", paths[i], plain, plainError, link, linkError, reach, reachError,
            fd >= 0 ? 0 : -1, openError, handle, handleError, (unsigned)st.st_mode);
    }
}

bool wowps5WineContractProbe(const char* reportPath) {
    report = fopen(reportPath, "w"); failures = 0;
    say("contract schema=1 begins");
    probeCpu();
    probeMap();
    probeIpc();
    probeDescriptors();
    probeImports();
    probeCosts();
    probeFiles();
    if(installHandlers()) {
        probeFaults();
        probeXstate();
        probePlacement();
        probeGs();
        removeHandlers();
    } else verdict(false, "signal handlers install");
    say("contract ends failures=%u", failures);
    if(report) { fclose(report); report = NULL; }
    return !failures;
}
#endif
