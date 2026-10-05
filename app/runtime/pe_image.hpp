// WoWPS5's bounded PE64 loading experiment. MIT license.
// This maps images and measures dependencies; it is not a Wine replacement.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef __PROSPERO__
#include <ps5platform/exec.h>
#else
#include <sys/mman.h>
#endif
extern "C" uint64_t wowps5CallWin64(void* entry);

namespace wowps5 {
struct Import { std::string module, symbol; uint32_t slot; };
class PeImage {
    std::vector<uint8_t> file;
    uint8_t* mapped = nullptr;
    size_t allocated = 0;
    bool smokeExecutable = false, smokeBound = false;
#ifdef __PROSPERO__
    ps5_exec_region region{};
#endif
    static void check(size_t at, size_t length, size_t limit) {
        if (at > limit || length > limit - at) throw std::runtime_error("PE range outside image");
    }
    template<class T> T read(size_t at) const {
        check(at, sizeof(T), file.size()); T value; memcpy(&value, file.data()+at,sizeof(T)); return value;
    }
    template<class T> T memory(size_t at) const {
        check(at,sizeof(T),imageSize); T value; memcpy(&value,mapped+at,sizeof(T)); return value;
    }
    std::string name(size_t at) const {
        check(at,1,imageSize);
        const size_t available = std::min<size_t>(4096,imageSize-at);
        const void* end = memchr(mapped+at,0,available);
        if (!end) throw std::runtime_error("Unterminated PE name");
        return std::string(reinterpret_cast<char*>(mapped+at),static_cast<const uint8_t*>(end)-(mapped+at));
    }
public:
    uint64_t preferredBase = 0;
    uint32_t imageSize = 0, entryRva = 0, relocations = 0;
    uint16_t sectionCount = 0;
    uint32_t dirs[16][2]{};
    std::vector<Import> imports;
    PeImage() = default;
    PeImage(const PeImage&) = delete;
    PeImage& operator=(const PeImage&) = delete;
    ~PeImage() {
#ifdef __PROSPERO__
        ps5_exec_free(&region);
#else
        if(mapped) munmap(mapped,allocated);
#endif
    }
    void load(const char* path, bool executable) {
        if(mapped || !file.empty()) throw std::runtime_error("PE image already loaded");
        FILE* input = fopen(path,"rb");
        if(!input) throw std::runtime_error("PE input not present");
        if(fseek(input,0,SEEK_END)) { fclose(input); throw std::runtime_error("PE seek failed"); }
        long length=ftell(input);
        if(length<64 || length>256L*1024*1024) { fclose(input); throw std::runtime_error("PE input size refused"); }
        rewind(input); file.resize(static_cast<size_t>(length));
        const size_t got=fread(file.data(),1,file.size(),input); fclose(input);
        if(got!=file.size()) throw std::runtime_error("PE short read");
        if(read<uint16_t>(0)!=0x5a4d) throw std::runtime_error("Missing MZ signature");
        const size_t pe=read<uint32_t>(0x3c);
        if(read<uint32_t>(pe)!=0x4550 || read<uint16_t>(pe+4)!=0x8664) throw std::runtime_error("Requires x64 Windows PE");
        sectionCount=read<uint16_t>(pe+6);
        const uint16_t optSize=read<uint16_t>(pe+20);
        const size_t opt=pe+24;
        check(opt,optSize,file.size());
        if(optSize<112 || sectionCount<1 || sectionCount>96 || read<uint16_t>(opt)!=0x20b) throw std::runtime_error("Invalid PE64 optional header");
        entryRva=read<uint32_t>(opt+16); preferredBase=read<uint64_t>(opt+24);
        imageSize=read<uint32_t>(opt+56); const uint32_t headers=read<uint32_t>(opt+60);
        if(!imageSize || imageSize>512u*1024*1024 || entryRva>=imageSize || headers>imageSize) throw std::runtime_error("PE image dimensions refused");
        check(0,headers,file.size());
        const uint32_t count=read<uint32_t>(opt+108);
        if(count>16 || 112+count*8>optSize) throw std::runtime_error("Invalid PE directory count");
        for(uint32_t i=0;i<count;i++) {
            dirs[i][0]=read<uint32_t>(opt+112+i*8); dirs[i][1]=read<uint32_t>(opt+116+i*8);
            if(i!=4 && dirs[i][1]) check(dirs[i][0],dirs[i][1],imageSize); // certificate table is a file offset
        }
        const size_t table=opt+optSize; check(table,size_t(sectionCount)*40,file.size());
#ifdef __PROSPERO__
        ps5_exec_request request{}; request.bytes=imageSize;
        request.flags=executable ? 0 : PS5_EXEC_TOGGLED;
        const int result=ps5_exec_alloc(&request,&region);
        if(result) throw std::runtime_error("PS5 direct-memory PE allocation refused");
        mapped=static_cast<uint8_t*>(region.base); allocated=region.bytes;
#else
        allocated=imageSize;
        void* address=mmap(nullptr,allocated,PROT_READ|PROT_WRITE|(executable ? PROT_EXEC : 0),MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(address==MAP_FAILED) throw std::runtime_error("Host PE allocation refused");
        mapped=static_cast<uint8_t*>(address);
#endif
        smokeExecutable=executable;
        memcpy(mapped,file.data(),headers);
        struct Range { size_t start,end; }; std::vector<Range> ranges{{0,headers}};
        for(uint16_t i=0;i<sectionCount;i++) {
            const size_t s=table+i*40;
            const uint32_t virtualSize=read<uint32_t>(s+8), rva=read<uint32_t>(s+12), rawSize=read<uint32_t>(s+16), raw=read<uint32_t>(s+20);
            const size_t extent=std::max(virtualSize,rawSize);
            check(rva,extent,imageSize); check(raw,rawSize,file.size());
            if(extent) {
                for(const auto& range:ranges) if(rva<range.end && size_t(rva)+extent>range.start) throw std::runtime_error("Overlapping PE sections");
                ranges.push_back({rva,size_t(rva)+extent});
            }
            if(rawSize) memcpy(mapped+rva,file.data()+raw,rawSize);
        }
        const uint64_t delta=reinterpret_cast<uintptr_t>(mapped)-preferredBase;
        if(delta && !dirs[5][1]) throw std::runtime_error("Image lacks relocation directory");
        for(size_t at=dirs[5][0], end=at+dirs[5][1];at<end;) {
            check(at,8,end); const uint32_t page=memory<uint32_t>(at), bytes=memory<uint32_t>(at+4);
            if(bytes<8 || bytes%2 || bytes>end-at) throw std::runtime_error("Invalid relocation block");
            for(size_t p=at+8;p<at+bytes;p+=2) {
                const uint16_t value=memory<uint16_t>(p); const unsigned type=value>>12;
                if(!type) continue;
                if(type!=10) throw std::runtime_error("Unsupported PE64 relocation type");
                const uint64_t target=uint64_t(page)+(value&0xfff); check(target,8,imageSize);
                uint64_t address=memory<uint64_t>(target)+delta; memcpy(mapped+target,&address,8); relocations++;
            }
            at+=bytes;
        }
        if(dirs[1][1]) {
            const size_t end=size_t(dirs[1][0])+dirs[1][1]; bool terminated=false;
            for(size_t at=dirs[1][0];at+20<=end;at+=20) {
                const uint32_t lookup=memory<uint32_t>(at), dll=memory<uint32_t>(at+12), iat=memory<uint32_t>(at+16);
                if(!lookup && !dll && !iat) { terminated=true; break; }
                if(!dll || !iat) throw std::runtime_error("Invalid import descriptor");
                std::string module=name(dll);
                std::transform(module.begin(),module.end(),module.begin(),[](unsigned char c){return c>='A'&&c<='Z' ? c+32:c;});
                bool done=false;
                for(size_t n=0;n<100000;n++) {
                    const uint64_t symbol=memory<uint64_t>(size_t(lookup ? lookup:iat)+8*n);
                    if(!symbol) { done=true; break; }
                    check(size_t(iat)+n*8,8,imageSize);
                    std::string function;
                    if(symbol>>63) function="#"+std::to_string(symbol&0xffff);
                    else { check(symbol,2,imageSize); function=name(symbol+2); }
                    imports.push_back({module,function,static_cast<uint32_t>(size_t(iat)+n*8)});
                }
                if(!done) throw std::runtime_error("Import table not terminated");
            }
            if(!terminated) throw std::runtime_error("Import descriptors not terminated");
        }
        file.clear(); file.shrink_to_fit();
    }
    template<class Resolver> void bindSmoke(Resolver resolve) {
        // Only the project's CRT-free test executable may be run here.
        // WoW remains non-executable until a complete Windows runtime exists.
        if(!smokeExecutable || dirs[9][1] || dirs[13][1]) throw std::runtime_error("Smoke image is not executable or uses unsupported TLS/delayed imports");
        for(const auto& import:imports) {
            void* function=resolve(import.module,import.symbol);
            if(!function) throw std::runtime_error("Smoke import not implemented");
            memcpy(mapped+import.slot,&function,sizeof(function));
        }
        smokeBound=true;
    }
    uint64_t runSmoke() const {
        if(!smokeExecutable || !smokeBound || !entryRva) throw std::runtime_error("Smoke execution prerequisites incomplete");
        return wowps5CallWin64(mapped+entryRva);
    }
};
}
