"""Exercise the actual dispatcher source with a minimal recursive guest/scheduler fixture.
Generate with: python scripts/test_dispatch_yield.py SOURCE OUTPUT
Compile OUTPUT with a C++17 compiler, then run it. The original dispatcher must fail.
"""
import pathlib, sys
source=pathlib.Path(sys.argv[1]).read_text()
start=source.index('bool PS2Runtime::dispatchGuestBranch(')
end=source.index('\nvoid PS2Runtime::SignalException',start)
dispatch=source[start:end]
a=source.index('bool PS2Runtime::eeCheckpointDue(')
b=source.index('\n[[noreturn]]',a)
checkpoint=source[a:b]
prefix=r'''
#include <iostream>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
struct R5900Context { uint32_t pc=0; int frames=0; };
struct EeScheduler {
 static constexpr uint32_t kGuestDispatchCycles=8;
 bool due=false;
 bool checkpointDue(uint32_t) noexcept { return due; }
};
uint32_t Ps2FastRead32(uint8_t*,uint32_t) { return 0; }
constexpr uint32_t PS2_RAM_SIZE=0x2000000;
class PS2Runtime {
public:
 enum class GuestBranchKind { DirectCall,IndirectCall,DirectJump,IndirectJump,Return };
 enum class MissingFunctionPolicy { SkipCallDebug,ContinueToTarget,Stop };
 using RecompiledFunction=void(*)(uint8_t*,R5900Context*,PS2Runtime*);
 EeScheduler scheduler;
 EeScheduler* m_eeScheduler=&scheduler;
 RecompiledFunction fn=nullptr;
 bool hasFunction(uint32_t) { return true; }
 RecompiledFunction lookupFunction(uint32_t) { return fn; }
 void reportMissingFunction(uint8_t*,R5900Context*,uint32_t,uint32_t,GuestBranchKind,const char*) {}
 MissingFunctionPolicy missingFunctionPolicy() { return MissingFunctionPolicy::Stop; }
 bool isStopRequested() { return false; }
 bool dispatchGuestBranch(uint8_t*,R5900Context*,uint32_t,uint32_t,uint32_t,GuestBranchKind,const char*);
 bool eeCheckpointDue(uint32_t cycles=32) noexcept;
};
static thread_local uint64_t s_guestYieldGeneration=0;
int continued=0, mode=0;
void guest(uint8_t* ram,R5900Context* ctx,PS2Runtime* rt) {
 ++ctx->frames;
 if(ctx->frames==3) {
   rt->scheduler.due=true;
   if(mode==0 || mode==2) {
     // Backedge yield at a PC identical to an ancestor's return or entry PC.
     ctx->pc=mode==0 ? 0x200 : 0x100;
     if(rt->eeCheckpointDue()) return;
   } else {
     // Inter-function safe point, also colliding with an ancestor's return PC.
     if(!rt->dispatchGuestBranch(ram,ctx,0x200,0,0x200,PS2Runtime::GuestBranchKind::DirectCall,"test")) return;
   }
 }
 if(!rt->dispatchGuestBranch(ram,ctx,0x100,0,0x200,PS2Runtime::GuestBranchKind::DirectCall,"test")) return;
 ++continued;
 --ctx->frames;
 ctx->pc=0x200;
}
void normal(uint8_t*,R5900Context* ctx,PS2Runtime*) { ctx->pc=0x200; }
void legacyStub(uint8_t*,R5900Context*,PS2Runtime*) {}
'''
suffix=r'''
int main() {
 int failures=0;
 for(mode=0;mode<3;++mode) {
   PS2Runtime rt; R5900Context ctx; continued=0; rt.fn=guest;
   bool returned=rt.dispatchGuestBranch(nullptr,&ctx,0x100,0,0x200,PS2Runtime::GuestBranchKind::DirectCall,"test");
   if(returned || continued || ctx.frames!=3) {
     std::printf("FAIL mode=%d returned=%d continued=%d frames=%d\n",mode,returned,continued,ctx.frames);
     ++failures;
   }
 }
 for(auto fn : {normal,legacyStub}) {
   PS2Runtime rt; R5900Context ctx; rt.fn=fn;
   if(!rt.dispatchGuestBranch(nullptr,&ctx,0x100,0,0x200,PS2Runtime::GuestBranchKind::DirectCall,"test") || ctx.pc!=0x200) return 2;
 }
 if(failures) return 1;
 std::puts("PASS: recursive backedge/call/entry-PC yields; normal and legacy stub returns");
}
'''
pathlib.Path(sys.argv[2]).write_text('#include <initializer_list>\n'+prefix+dispatch+checkpoint+suffix)

