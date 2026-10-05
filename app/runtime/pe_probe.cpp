// Native Windows-image bringup. MIT license.
#include "pe_image.hpp"
#include <cerrno>
#include <ctime>

extern "C" uint64_t wowps5NativeTick() {
    timespec time{};
    if(clock_gettime(CLOCK_MONOTONIC,&time)) return 0;
    return uint64_t(time.tv_sec)*1000+uint64_t(time.tv_nsec)/1000000;
}
extern "C" void wowps5NativeSleep(uint32_t milliseconds) {
    timespec delay{static_cast<time_t>(milliseconds/1000),static_cast<long>(milliseconds%1000)*1000000};
    while(nanosleep(&delay,&delay) && errno==EINTR) {}
}
extern "C" uint64_t wowps5WinTick();
extern "C" void wowps5WinSleep(uint32_t);
// Prospero clang refuses ms_abi. These explicit ABI bridges preserve Windows'
// additional nonvolatile registers and supply its required 32-byte shadow space.
#define WIN_SAVE "push %rdi\n push %rsi\n sub $168,%rsp\n" \
 "movdqu %xmm6,0(%rsp)\n movdqu %xmm7,16(%rsp)\n movdqu %xmm8,32(%rsp)\n movdqu %xmm9,48(%rsp)\n" \
 "movdqu %xmm10,64(%rsp)\n movdqu %xmm11,80(%rsp)\n movdqu %xmm12,96(%rsp)\n movdqu %xmm13,112(%rsp)\n" \
 "movdqu %xmm14,128(%rsp)\n movdqu %xmm15,144(%rsp)\n"
#define WIN_RESTORE "movdqu 0(%rsp),%xmm6\n movdqu 16(%rsp),%xmm7\n movdqu 32(%rsp),%xmm8\n movdqu 48(%rsp),%xmm9\n" \
 "movdqu 64(%rsp),%xmm10\n movdqu 80(%rsp),%xmm11\n movdqu 96(%rsp),%xmm12\n movdqu 112(%rsp),%xmm13\n" \
 "movdqu 128(%rsp),%xmm14\n movdqu 144(%rsp),%xmm15\n add $168,%rsp\n pop %rsi\n pop %rdi\n ret\n"
asm(".text\n"
    ".global wowps5CallWin64\n.type wowps5CallWin64,@function\nwowps5CallWin64:\n"
    "mov %rdi,%rax\n sub $40,%rsp\n call *%rax\n add $40,%rsp\n ret\n"
    ".global wowps5WinTick\n.type wowps5WinTick,@function\nwowps5WinTick:\n" WIN_SAVE
    "call wowps5NativeTick\n" WIN_RESTORE
    ".global wowps5WinSleep\n.type wowps5WinSleep,@function\nwowps5WinSleep:\n" WIN_SAVE
    "mov %ecx,%edi\n call wowps5NativeSleep\n" WIN_RESTORE);
#undef WIN_SAVE
#undef WIN_RESTORE
static void* resolveSmoke(const std::string& dll,const std::string& symbol) {
    if(dll!="kernel32.dll") return nullptr;
    if(symbol=="GetTickCount64") return reinterpret_cast<void*>(&wowps5WinTick);
    if(symbol=="Sleep") return reinterpret_cast<void*>(&wowps5WinSleep);
    return nullptr;
}
static bool mapClient(const char* path,const char* label,FILE* report) {
    try {
        wowps5::PeImage image; image.load(path,false);
        fprintf(stderr,"[WoWPS5 PE] %s mapped: bytes=%u sections=%u relocations=%u imports=%zu TLS=%u delay=%u; execution blocked: Windows runtime required\n",label,image.imageSize,image.sectionCount,image.relocations,image.imports.size(),image.dirs[9][1],image.dirs[13][1]);
        if(report) fprintf(report,"{\"file\":\"%s\",\"mapped\":true,\"imageBytes\":%u,\"relocations\":%u,\"imports\":%zu,\"tlsBytes\":%u,\"delayImportBytes\":%u,\"entryCalled\":false}",label,image.imageSize,image.relocations,image.imports.size(),image.dirs[9][1],image.dirs[13][1]);
        std::string previous;
        for(const auto& import:image.imports) if(import.module!=previous) {
            fprintf(stderr,"[WoWPS5 PE] %s needs %s (example: %s)\n",label,import.module.c_str(),import.symbol.c_str()); previous=import.module;
        }
        return true;
    } catch(const std::exception& error) {
        fprintf(stderr,"[WoWPS5 PE] %s: %s\n",label,error.what());
        if(report) fprintf(report,"{\"file\":\"%s\",\"mapped\":false,\"entryCalled\":false}",label);
        return false;
    }
}
extern "C" bool wowps5PeProbe(const char* smoke,const char* clientDirectory,const char* reportPath) {
    FILE* report=fopen(reportPath,"w"); bool passed=false;
    try {
        wowps5::PeImage image; image.load(smoke,true); image.bindSmoke(resolveSmoke);
        const uint64_t result=image.runSmoke(); passed=result==42;
        fprintf(stderr,"[WoWPS5 PE] Windows x64 smoke %s: result=%llu imports=%zu relocations=%u\n",passed?"PASS":"FAIL",static_cast<unsigned long long>(result),image.imports.size(),image.relocations);
    } catch(const std::exception& error) { fprintf(stderr,"[WoWPS5 PE] Windows x64 smoke FAIL: %s\n",error.what()); }
    if(report) fprintf(report,"{\"schema\":1,\"smokePassed\":%s,\"woWRunning\":false,\"client\":[",passed?"true":"false");
    const std::string root(clientDirectory);
    mapClient((root+"/WowB.exe").c_str(),"WowB.exe",report);
    if(report) fputc(',',report);
    mapClient((root+"/WowB_loader.dll").c_str(),"WowB_loader.dll",report);
    if(report) { fputs("]}\n",report); fclose(report); }
    return passed;
}
#ifdef WOWPS5_PE_HOST_TEST
int main(int argc,char** argv) {
    if(argc!=4) return 2;
    return wowps5PeProbe(argv[1],argv[2],argv[3]) ? 0:1;
}
#endif
