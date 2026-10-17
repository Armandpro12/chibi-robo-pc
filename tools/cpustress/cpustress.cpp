// cpustress: a focused CPU stress test for statically recompiled code (Windows x64).
//
// It generates, at run time, x86-64 code with the shapes that a static recompiler emits for guest function
// prologues: long runs of `mov dword ptr [reg+0x280], imm32` (guest program counter updates) interleaved with
// register-file copies (`mov eax,[reg+a]` / `mov [reg+b],eax`), byte-swapped stores to guest memory and immediate
// stores, at every code alignment modulo 64 bytes and with several run lengths. One thread per logical processor
// (pinned with SetThreadGroupAffinity) executes them millions of times and checks every result. Faults are caught
// with structured exception handling (the generated code is registered with RtlAddFunctionTable).
//
// Verdict: PASS (no fault, no wrong result) or FAIL (logical processor, pattern, alignment, iteration count).
// A PASS does not prove that the processor is sound; see README.md.
//
// Build: see README.md (cl /O2 /EHsc cpustress.cpp, or clang-cl, or the CMakeLists.txt next to this file).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Model of what the generated code touches: a 0x400-byte "CPU state" block (base register), 0x100 bytes of "guest
// memory" (rdx) and a stack slot.
// ---------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kCtxSize = 0x400;
constexpr uint32_t kRamSize = 0x100;
constexpr uint32_t kPcOff = 0x280;      // displacement of the program-counter field (as in the observed code)
constexpr uint32_t kLrOff = 0x284;
constexpr uint32_t kStackCopyOff = 0x288;
constexpr uint32_t kFlagOff = 0x344;    // byte tested before each guest store (always 0: branch never taken)
constexpr int kInputs = 4;              // random input sets, rotated

enum Base : uint8_t { RAX = 0, RCX = 1, RSI = 6 };

enum OpKind : uint8_t {
  ST_IMM32,     // mov dword [B+disp], imm
  LD_EAX,       // mov eax, [B+disp]
  ST_EAX,       // mov [B+disp], eax
  GUEST_STORE,  // mov r8d,[B+disp]; cmp byte [B+0x344],0; jne +10; bswap r8d; mov [rdx+ram], r8d
  RELOAD,       // mov B, [rsp+0x38]   (B = rcx or rax)
  ST_IMM64,     // movabs rax, imm64; mov [B+disp], rax
  STK_IMM32,    // mov dword [rsp+0x44], imm
  STK_TO_CTX,   // mov eax, [rsp+0x44]; mov [B+disp], eax
};

struct Op {
  OpKind kind;
  Base base;
  uint32_t disp;
  uint64_t imm;  // immediate, or guest-memory offset for GUEST_STORE
};

struct Encoder {
  std::vector<uint8_t> b;
  void u8(uint8_t v) { b.push_back(v); }
  void u32(uint32_t v) { for (int i = 0; i < 4; i++) u8(uint8_t(v >> (8 * i))); }
  void u64(uint64_t v) { for (int i = 0; i < 8; i++) u8(uint8_t(v >> (8 * i))); }
  // ModRM + displacement for [base+disp], reg field `reg` (low 3 bits).
  void mem(uint8_t reg, Base base, uint32_t disp) {
    if (disp < 0x80) { u8(uint8_t(0x40 | (reg << 3) | base)); u8(uint8_t(disp)); }
    else { u8(uint8_t(0x80 | (reg << 3) | base)); u32(disp); }
  }
  void encode(const Op& o) {
    switch (o.kind) {
      case ST_IMM32: u8(0xC7); mem(0, o.base, o.disp); u32(uint32_t(o.imm)); break;
      case LD_EAX: u8(0x8B); mem(0, o.base, o.disp); break;
      case ST_EAX: u8(0x89); mem(0, o.base, o.disp); break;
      case GUEST_STORE:
        u8(0x44); u8(0x8B); mem(0, o.base, o.disp);                       // mov r8d, [B+disp]
        u8(0x80); mem(7, o.base, kFlagOff); u8(0x00);                     // cmp byte [B+0x344], 0
        u8(0x75); u8(10);                                                 // jne +10 (never taken)
        u8(0x41); u8(0x0F); u8(0xC8);                                     // bswap r8d
        u8(0x44); u8(0x89); u8(0x82); u32(uint32_t(o.imm));               // mov [rdx+ram], r8d
        break;
      case RELOAD: u8(0x48); u8(0x8B); u8(uint8_t(0x44 | (o.base << 3))); u8(0x24); u8(0x38); break;
      case ST_IMM64: u8(0x48); u8(0xB8); u64(o.imm); u8(0x48); u8(0x89); mem(0, o.base, o.disp); break;
      case STK_IMM32: u8(0xC7); u8(0x44); u8(0x24); u8(0x44); u32(uint32_t(o.imm)); break;
      case STK_TO_CTX: u8(0x8B); u8(0x44); u8(0x24); u8(0x44); u8(0x89); mem(0, o.base, o.disp); break;
    }
  }
};

// Reference semantics of the ops (the expected result is computed once per instance and input set).
struct Model {
  uint8_t ctx[kCtxSize];
  uint8_t ram[kRamSize];
  uint32_t eax = 0, stack44 = 0;
  uint32_t rd(uint32_t off) const { uint32_t v; memcpy(&v, ctx + off, 4); return v; }
  void wr(uint32_t off, uint32_t v) { memcpy(ctx + off, &v, 4); }
  void apply(const Op& o) {
    switch (o.kind) {
      case ST_IMM32: wr(o.disp, uint32_t(o.imm)); break;
      case LD_EAX: eax = rd(o.disp); break;
      case ST_EAX: wr(o.disp, eax); break;
      case GUEST_STORE: if (ctx[kFlagOff] == 0) { uint32_t v = _byteswap_ulong(rd(o.disp)); memcpy(ram + o.imm, &v, 4); } break;
      case RELOAD: break;
      case ST_IMM64: memcpy(ctx + o.disp, &o.imm, 8); eax = uint32_t(o.imm); break;
      case STK_IMM32: stack44 = uint32_t(o.imm); break;
      case STK_TO_CTX: eax = stack44; wr(o.disp, eax); break;
    }
  }
};

// ---------------------------------------------------------------------------------------------------------------------
// Pattern templates
// ---------------------------------------------------------------------------------------------------------------------
enum Pattern { P_PC_RUN, P_PC_ONLY, P_STACK_RELOAD, P_MIXED, P_COUNT };
const char* kPatternName[P_COUNT] = {"pc-run", "pc-only", "stack-reload", "mixed"};
const char* kPatternDesc[P_COUNT] = {
    "8 byte-swapped guest stores, then N x {mov [rsi+0x280],imm32; mov eax,[rsi+a]; mov [rsi+b],eax}, immediate tail",
    "N back-to-back mov dword [rsi+0x280], imm32, immediate tail",
    "context pointer reloaded from [rsp+0x38] before each copy, one pc store, [rax+0x284] and [rsp+0x44] stores",
    "pseudo-random mix of the above instruction forms, rsi base"};

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
  uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
  uint32_t pc() { return 0x80000000u | (uint32_t(next()) & 0x00FFFFFCu); }
};

// Register-file slots (guest r3..r10 style sources, r24..r31 style destinations), as 4-byte offsets.
const uint32_t kSrc[8] = {0x10, 0x14, 0x0C, 0x18, 0x1C, 0x20, 0x24, 0x28};
const uint32_t kDst[8] = {0x6C, 0x70, 0x7C, 0x74, 0x78, 0x60, 0x64, 0x68};

std::vector<Op> make_ops(Pattern p, int n, uint64_t seed) {
  Rng r(seed);
  std::vector<Op> v;
  uint32_t pc = r.pc();
  auto pcstore = [&](Base b) { v.push_back({ST_IMM32, b, kPcOff, pc}); pc += 4; };
  auto tail = [&](Base b) {
    pcstore(b);
    v.push_back({ST_IMM32, b, 0x10, 0});
    pcstore(b);
    v.push_back({ST_IMM32, b, 0x14, 0x20});
    v.push_back({ST_IMM64, b, kPcOff, (uint64_t(pc + 4) << 32) | pc});
  };
  switch (p) {
    case P_PC_RUN:
      for (int i = 0; i < 8; i++) v.push_back({GUEST_STORE, RSI, 0x60 + 4 * uint32_t(i), uint64_t(0x40 + 4 * i)});
      for (int i = 0; i < n; i++) {
        pcstore(RSI);
        v.push_back({LD_EAX, RSI, kSrc[i % 8], 0});
        v.push_back({ST_EAX, RSI, kDst[i % 8], 0});
      }
      tail(RSI);
      break;
    case P_PC_ONLY:
      for (int i = 0; i < n; i++) pcstore(RSI);
      tail(RSI);
      break;
    case P_STACK_RELOAD:
      v.push_back({RELOAD, RCX, 0, 0});
      for (int i = 0; i < 3; i++) v.push_back({GUEST_STORE, RCX, 0x60 + 4 * uint32_t(i), uint64_t(0x80 + 4 * i)});
      pcstore(RCX);
      for (int i = 0; i < n; i++) {
        v.push_back({LD_EAX, RCX, kSrc[i % 8], 0});
        v.push_back({ST_EAX, RCX, kDst[i % 8], 0});
        v.push_back({RELOAD, RCX, 0, 0});
      }
      v.push_back({RELOAD, RAX, 0, 0});
      v.push_back({ST_IMM32, RAX, 0x10, 0});
      v.push_back({RELOAD, RAX, 0, 0});
      v.push_back({ST_IMM32, RAX, 0x14, 0x20});
      v.push_back({RELOAD, RAX, 0, 0});
      v.push_back({ST_IMM32, RAX, kLrOff, pc});
      v.push_back({STK_IMM32, RAX, 0, r.pc()});
      v.push_back({RELOAD, RCX, 0, 0});
      v.push_back({STK_TO_CTX, RCX, kStackCopyOff, 0});
      break;
    case P_MIXED:
      for (int i = 0; i < n; i++) {
        switch (r.next() % 6) {
          case 0: case 1: pcstore(RSI); break;
          case 2: v.push_back({LD_EAX, RSI, kSrc[r.next() % 8], 0}); v.push_back({ST_EAX, RSI, kDst[r.next() % 8], 0}); break;
          case 3: v.push_back({GUEST_STORE, RSI, kDst[r.next() % 8], uint64_t(4 * (r.next() % 60))}); break;
          case 4: v.push_back({ST_IMM32, RSI, kSrc[r.next() % 8], uint32_t(r.next())}); break;
          case 5: v.push_back({ST_IMM64, RSI, kPcOff, r.next()}); break;
        }
      }
      tail(RSI);
      break;
    default: break;
  }
  return v;
}

// Prologue: push rsi; sub rsp,0x50 (described by the shared UNWIND_INFO); then mov rsi,rcx; mov [rsp+0x38],rcx.
const uint8_t kPrologue[] = {0x56, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x89, 0xCE, 0x48, 0x89, 0x4C, 0x24, 0x38};
const uint8_t kEpilogue[] = {0x48, 0x83, 0xC4, 0x50, 0x5E, 0xC3};

struct Instance {
  uint32_t off = 0, size = 0;     // in the code region
  uint32_t first_pc_off = 0;      // offset of the first `mov [B+0x280], imm32` inside the instance
  Pattern pattern{};
  int length = 0, align = 0;
  uint32_t pc_stores = 0;
  std::vector<uint8_t> expected;  // kInputs x (ctx + ram)
};

typedef void(__cdecl* GenFn)(uint8_t* ctx, uint8_t* ram);

// ---------------------------------------------------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------------------------------------------------
uint8_t* g_code = nullptr;
size_t g_code_size = 0;
std::vector<Instance> g_inst;
std::vector<uint8_t> g_inputs;  // kInputs x (ctx + ram)
std::atomic<bool> g_stop{false};

enum InjectMode { INJ_NONE, INJ_GP, INJ_VALUE };
InjectMode g_inject = INJ_NONE;
int g_inject_pattern = P_PC_ONLY;  // the first instruction of this pattern is a pc store
std::atomic<bool> g_injected{false};
int g_inject_thread = 0;
double g_inject_after_s = 2.0;

struct ErrorRec {
  int kind = 0;  // 1 fault, 2 wrong result
  uint32_t code = 0;
  uint64_t fault_addr = 0, rip = 0, base_reg = 0;
  int rw = -1;
  int inst = -1, input = -1, ctx_off = 0;
  uint64_t iteration = 0;
  uint32_t first_diff = 0;
  uint32_t got = 0, want = 0;
};

struct Worker {
  int index = 0;
  WORD group = 0;
  BYTE bit = 0;
  int core = -1, eff_class = -1;
  std::atomic<uint64_t> iterations{0};
  std::atomic<uint64_t> pc_stores{0};
  std::atomic<uint32_t> faults{0}, wrong{0};
  ErrorRec first;
  bool have_first = false;
  bool affinity_ok = false;
};

BOOL WINAPI on_ctrl(DWORD) { g_stop = true; return TRUE; }

int instance_at(uint64_t rip) {
  if (rip < uint64_t(g_code) || rip >= uint64_t(g_code) + g_code_size) return -1;
  uint32_t off = uint32_t(rip - uint64_t(g_code));
  size_t lo = 0, hi = g_inst.size();
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (g_inst[mid].off + g_inst[mid].size <= off) lo = mid + 1; else hi = mid;
  }
  if (lo < g_inst.size() && g_inst[lo].off <= off) return int(lo);
  return -1;
}

int filter(EXCEPTION_POINTERS* ep, ErrorRec* out) {
  out->kind = 1;
  out->code = ep->ExceptionRecord->ExceptionCode;
  out->rip = uint64_t(ep->ExceptionRecord->ExceptionAddress);
  if (out->code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
    out->rw = int(ep->ExceptionRecord->ExceptionInformation[0]);
    out->fault_addr = ep->ExceptionRecord->ExceptionInformation[1];
  }
  out->base_reg = ep->ContextRecord->Rsi;
  return EXCEPTION_EXECUTE_HANDLER;
}

// One call, guarded. Returns false on a fault (details in *err).
bool guarded_call(GenFn fn, uint8_t* ctx, uint8_t* ram, ErrorRec* err) {
  __try {
    fn(ctx, ram);
    return true;
  } __except (filter(GetExceptionInformation(), err)) {
    return false;
  }
}

void record(Worker& w, const ErrorRec& e) {
  if (e.kind == 1) w.faults++; else w.wrong++;
  if (!w.have_first) { w.first = e; w.have_first = true; }
}

void worker_main(Worker* w, uint64_t seed) {
  if (w->group != 0xFFFF) {
    GROUP_AFFINITY ga{};
    ga.Group = w->group;
    ga.Mask = KAFFINITY(1) << w->bit;
    w->affinity_ok = SetThreadGroupAffinity(GetCurrentThread(), &ga, nullptr) != 0;
  }
  // Own buffers: a CPU-state block at several alignments (one crossing a 4 KiB page), guest memory.
  uint8_t* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x3000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  uint8_t* ram = page + 0x2000;
  const uint32_t ctx_offs[4] = {0x000, 0x010, 0x7F0, 0xFC0};
  const size_t n = g_inst.size();
  Rng r(seed);
  // Visit order: a per-thread permutation (cold code), each instance run in bursts of 1, 8 or 64 (hot code).
  std::vector<uint32_t> order(n);
  for (size_t i = 0; i < n; i++) order[i] = uint32_t(i);
  for (size_t i = n; i > 1; i--) std::swap(order[i - 1], order[r.next() % i]);
  const int bursts[3] = {1, 8, 64};
  auto t0 = std::chrono::steady_clock::now();
  bool injected = false;
  uint64_t local_iter = 0, local_pc = 0, round = 0;
  while (!g_stop.load(std::memory_order_relaxed)) {
    for (size_t k = 0; k < n && !g_stop.load(std::memory_order_relaxed); k++) {
      const Instance& in = g_inst[order[k]];
      GenFn fn = reinterpret_cast<GenFn>(g_code + in.off);
      int burst = bursts[(k + round) % 3];
      uint32_t ctx_off = ctx_offs[(k + round) & 3];
      uint8_t* ctx = page + ctx_off;
      for (int b = 0; b < burst; b++) {
        int input = int((local_iter + b) % kInputs);
        const uint8_t* src = &g_inputs[size_t(input) * (kCtxSize + kRamSize)];
        const uint8_t* want = &in.expected[size_t(input) * (kCtxSize + kRamSize)];
        memcpy(ctx, src, kCtxSize);
        memcpy(ram, src + kCtxSize, kRamSize);
        uint8_t* call_ctx = ctx;
        bool inject_now = false;
        if (g_inject != INJ_NONE && !injected && w->index == g_inject_thread &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= g_inject_after_s &&
            in.pattern == g_inject_pattern) {
          inject_now = true;
          injected = true;
          g_injected = true;
          // Deliberate software fault: a non-canonical base pointer, so the first memory access through it (in the
          // pc-only pattern, `mov dword ptr [rsi+0x280], imm32`) raises a general-protection fault.
          if (g_inject == INJ_GP) call_ctx = reinterpret_cast<uint8_t*>(0x8000000000000000ull);
        }
        ErrorRec e;
        e.inst = int(order[k]); e.input = input; e.ctx_off = int(ctx_off); e.iteration = local_iter + b;
        if (!guarded_call(fn, call_ctx, ram, &e)) {
          int at = instance_at(e.rip);
          if (at >= 0) e.inst = at;
          record(*w, e);
          continue;
        }
        if (inject_now && g_inject == INJ_VALUE) ctx[kPcOff] ^= 0x10;  // deliberate wrong result, after the call
        if (memcmp(ctx, want, kCtxSize) != 0 || memcmp(ram, want + kCtxSize, kRamSize) != 0) {
          e.kind = 2;
          for (uint32_t i = 0; i < kCtxSize + kRamSize; i++) {
            uint8_t g = i < kCtxSize ? ctx[i] : ram[i - kCtxSize];
            if (g != want[i]) { e.first_diff = i & ~3u; break; }
          }
          const uint8_t* gp = e.first_diff < kCtxSize ? ctx + e.first_diff : ram + (e.first_diff - kCtxSize);
          memcpy(&e.got, gp, 4);
          memcpy(&e.want, want + e.first_diff, 4);
          record(*w, e);
        }
      }
      local_iter += uint64_t(burst);
      local_pc += uint64_t(burst) * in.pc_stores;
      w->iterations.store(local_iter, std::memory_order_relaxed);
      w->pc_stores.store(local_pc, std::memory_order_relaxed);
      if (w->faults + w->wrong >= 1000) { g_stop = true; break; }
    }
    round++;
  }
  VirtualFree(page, 0, MEM_RELEASE);
}

// Logical processor -> physical core index and efficiency class (hybrid processors), from
// GetLogicalProcessorInformationEx(RelationProcessorCore).
void map_cores(std::vector<Worker*>& ws) {
  DWORD len = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
  std::vector<uint8_t> buf(len);
  auto* p = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore, p, &len)) return;
  int core = 0;
  for (DWORD off = 0; off < len; core++) {
    auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
    for (WORD g = 0; g < e->Processor.GroupCount; g++) {
      for (Worker* w : ws) {
        if (w->group == e->Processor.GroupMask[g].Group && (e->Processor.GroupMask[g].Mask >> w->bit) & 1) {
          w->core = core;
          w->eff_class = e->Processor.EfficiencyClass;
        }
      }
    }
    off += e->Size;
  }
}

std::string describe(const Instance& in) {
  char s[160];
  snprintf(s, sizeof s, "pattern %s, length %d, alignment %d mod 64 (first pc store at code offset 0x%X)",
           kPatternName[in.pattern], in.length, in.align, in.off + in.first_pc_off);
  return s;
}

void usage() {
  printf(
      "cpustress: stress test of recompiled-code instruction patterns on every logical processor.\n"
      "usage: cpustress [--seconds N] [--threads N] [--code-mb N] [--patterns list] [--seed N]\n"
      "                 [--inject gp|value] [--inject-thread N] [--list] [--dump]\n"
      "  --seconds N      run time (default 180); Ctrl+C stops early and still prints the verdict\n"
      "  --threads N      only the first N logical processors (default: all)\n"
      "  --code-mb N      spread the generated code over N MiB (default 32: instruction-cache and TLB pressure)\n"
      "  --patterns list  comma-separated subset of: pc-run,pc-only,stack-reload,mixed (default: all)\n"
      "  --inject gp      self-test: one deliberate general-protection fault (non-canonical base register)\n"
      "  --inject value   self-test: one deliberately corrupted result\n"
      "  --list           print the patterns and instance counts, then exit\n"
      "  --dump           print the bytes of the first instance of each pattern, then exit\n"
      "exit code: 0 PASS, 1 FAIL, 2 usage or setup error\n");
}

}  // namespace

int main(int argc, char** argv) {
  double seconds = 180;
  int max_threads = 0, code_mb = 32;
  uint64_t seed = 12345;
  bool list = false, dump = false;
  bool want_pattern[P_COUNT] = {true, true, true, true};
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto val = [&]() -> const char* { if (i + 1 >= argc) { usage(); exit(2); } return argv[++i]; };
    if (a == "--seconds") seconds = atof(val());
    else if (a == "--threads") max_threads = atoi(val());
    else if (a == "--code-mb") code_mb = atoi(val());
    else if (a == "--seed") seed = strtoull(val(), nullptr, 0);
    else if (a == "--inject") {
      std::string m = val();
      if (m == "gp") g_inject = INJ_GP; else if (m == "value") g_inject = INJ_VALUE; else { usage(); return 2; }
    } else if (a == "--inject-thread") g_inject_thread = atoi(val());
    else if (a == "--patterns") {
      std::string l = val();
      for (int p = 0; p < P_COUNT; p++) want_pattern[p] = false;
      size_t pos = 0;
      while (pos <= l.size()) {
        size_t c = l.find(',', pos);
        std::string name = l.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
        bool found = false;
        for (int p = 0; p < P_COUNT; p++) if (name == kPatternName[p]) { want_pattern[p] = true; found = true; }
        if (!found) { fprintf(stderr, "unknown pattern: %s\n", name.c_str()); return 2; }
        if (c == std::string::npos) break;
        pos = c + 1;
      }
    } else if (a == "--list") list = true;
    else if (a == "--dump") dump = true;
    else { usage(); return a == "--help" || a == "-h" ? 0 : 2; }
  }
  if (code_mb < 1) code_mb = 1;
  if (code_mb > 1024) code_mb = 1024;

  // Random input sets (flag byte at 0x344 kept at 0, so the guarded branch is never taken).
  g_inputs.resize(size_t(kInputs) * (kCtxSize + kRamSize));
  {
    Rng r(seed ^ 0xABCDEF);
    for (auto& x : g_inputs) x = uint8_t(r.next());
    for (int k = 0; k < kInputs; k++) g_inputs[size_t(k) * (kCtxSize + kRamSize) + kFlagOff] = 0;
  }

  // Build the instances: every pattern x length x alignment (0..63) of the first pc store.
  const std::vector<int> lengths[P_COUNT] = {{1, 2, 4, 8, 10, 12, 16}, {2, 4, 8, 16, 32}, {1, 4, 7, 10}, {16, 32, 64}};
  struct Built { Instance in; std::vector<uint8_t> bytes; };
  std::vector<Built> built;
  for (int p = 0; p < P_COUNT; p++) {
    if (!want_pattern[p]) continue;
    for (int len : lengths[p]) {
      for (int align = 0; align < 64; align++) {
        uint64_t iseed = seed * 1000003u + uint64_t(p) * 7919u + uint64_t(len) * 131u + uint64_t(align);
        std::vector<Op> ops = make_ops(Pattern(p), len, iseed);
        Encoder enc;
        for (uint8_t c : kPrologue) enc.u8(c);
        uint32_t first_pc = 0;
        for (const Op& o : ops) {
          size_t before = enc.b.size();
          enc.encode(o);
          if (!first_pc && o.kind == ST_IMM32 && o.disp == kPcOff) first_pc = uint32_t(before);
        }
        for (uint8_t c : kEpilogue) enc.u8(c);
        Built bt;
        bt.in.pattern = Pattern(p);
        bt.in.length = len;
        bt.in.align = align;
        bt.in.first_pc_off = first_pc;
        bt.in.size = uint32_t(enc.b.size());
        for (const Op& o : ops) bt.in.pc_stores += (o.kind == ST_IMM32 || o.kind == ST_IMM64) && o.disp == kPcOff;
        bt.in.expected.resize(g_inputs.size());
        for (int k = 0; k < kInputs; k++) {
          Model m;
          const uint8_t* src = &g_inputs[size_t(k) * (kCtxSize + kRamSize)];
          memcpy(m.ctx, src, kCtxSize);
          memcpy(m.ram, src + kCtxSize, kRamSize);
          for (const Op& o : ops) m.apply(o);
          memcpy(&bt.in.expected[size_t(k) * (kCtxSize + kRamSize)], m.ctx, kCtxSize);
          memcpy(&bt.in.expected[size_t(k) * (kCtxSize + kRamSize) + kCtxSize], m.ram, kRamSize);
        }
        bt.bytes = std::move(enc.b);
        built.push_back(std::move(bt));
      }
    }
  }
  if (built.empty()) { fprintf(stderr, "no pattern selected\n"); return 2; }

  if (list) {
    for (int p = 0; p < P_COUNT; p++) {
      size_t c = 0;
      for (auto& b : built) c += b.in.pattern == p;
      printf("%-13s %4zu instances: %s\n", kPatternName[p], c, kPatternDesc[p]);
    }
    return 0;
  }
  if (dump) {
    for (int p = 0; p < P_COUNT; p++) {
      for (auto& b : built) {
        if (b.in.pattern != p || b.in.align != 0) continue;
        printf("%s, length %d (%u bytes):\n", kPatternName[p], b.in.length, b.in.size);
        for (size_t i = 0; i < b.bytes.size(); i++) printf("%02X%s", b.bytes[i], (i % 32 == 31) ? "\n" : " ");
        printf("\n");
        break;
      }
    }
    return 0;
  }

  // Code region: [0, 64) holds the shared UNWIND_INFO; the instances are spread over the region with int3 padding.
  size_t total = 0;
  for (auto& b : built) total += b.bytes.size() + 64;
  size_t region = std::max(total + 4096, size_t(code_mb) << 20);
  size_t stride = region / built.size();
  g_code = static_cast<uint8_t*>(VirtualAlloc(nullptr, region, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!g_code) { fprintf(stderr, "VirtualAlloc failed (%lu)\n", GetLastError()); return 2; }
  g_code_size = region;
  memset(g_code, 0xCC, region);
  // UNWIND_INFO version 1, prologue 5 bytes, 2 codes: [5] UWOP_ALLOC_SMALL (0x50 = (9+1)*8), [1] UWOP_PUSH_NONVOL rsi.
  const uint8_t unwind[8] = {0x01, 0x05, 0x02, 0x00, 0x05, uint8_t((9 << 4) | 2), 0x01, uint8_t((6 << 4) | 0)};
  memcpy(g_code, unwind, sizeof unwind);
  std::vector<RUNTIME_FUNCTION> table;
  size_t cursor = 64;
  for (size_t i = 0; i < built.size(); i++) {
    Instance in = built[i].in;
    size_t slot = std::max(cursor, 64 + i * stride);
    size_t start = slot + ((size_t(in.align) - (slot + in.first_pc_off)) & 63);
    if (start + in.size + 1 > region) { fprintf(stderr, "internal error: code region overflow\n"); return 2; }
    in.off = uint32_t(start);
    memcpy(g_code + start, built[i].bytes.data(), built[i].bytes.size());
    cursor = start + in.size + 1;
    RUNTIME_FUNCTION rf;
    rf.BeginAddress = in.off;
    rf.EndAddress = in.off + in.size;
    rf.UnwindData = 0;
    table.push_back(rf);
    built[i].bytes.clear();
    g_inst.push_back(std::move(in));
  }
  DWORD old = 0;
  if (!VirtualProtect(g_code, region, PAGE_EXECUTE_READ, &old)) { fprintf(stderr, "VirtualProtect failed\n"); return 2; }
  FlushInstructionCache(GetCurrentProcess(), g_code, region);
  if (!RtlAddFunctionTable(table.data(), DWORD(table.size()), DWORD64(g_code))) {
    fprintf(stderr, "RtlAddFunctionTable failed\n");
    return 2;
  }

  // One worker per logical processor, in every processor group.
  std::vector<Worker*> ws;
  WORD groups = GetActiveProcessorGroupCount();
  for (WORD g = 0; g < groups; g++) {
    DWORD cnt = GetActiveProcessorCount(g);
    for (DWORD b = 0; b < cnt && b < 64; b++) {
      if (max_threads > 0 && int(ws.size()) >= max_threads) break;
      Worker* w = new Worker;
      w->index = int(ws.size());
      w->group = g;
      w->bit = BYTE(b);
      ws.push_back(w);
    }
  }
  map_cores(ws);
  if (g_inject_thread >= int(ws.size())) g_inject_thread = 0;
  if (!want_pattern[g_inject_pattern])
    for (int p = P_COUNT - 1; p >= 0; p--) if (want_pattern[p]) g_inject_pattern = p;
  SetConsoleCtrlHandler(on_ctrl, TRUE);

  char cpu[49] = {0};
  {
    int regs[4];
    for (int i = 0; i < 3; i++) { __cpuid(regs, int(0x80000002 + i)); memcpy(cpu + 16 * i, regs, 16); }
  }
  printf("cpustress: %s\n", cpu);
  printf("%zu logical processors, %zu code instances over %zu MiB, %.0f s%s\n", ws.size(), g_inst.size(),
         region >> 20, seconds,
         g_inject == INJ_GP ? ", SELF-TEST: one deliberate fault will be injected"
         : g_inject == INJ_VALUE ? ", SELF-TEST: one deliberate wrong result will be injected" : "");
  printf("All cores are fully loaded while this runs. Ctrl+C stops early.\n");
  fflush(stdout);

  std::vector<std::thread> th;
  for (Worker* w : ws) th.emplace_back(worker_main, w, seed + 0x1000u * uint64_t(w->index));
  auto t0 = std::chrono::steady_clock::now();
  double next_report = 10;
  for (;;) {
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (el >= seconds || g_stop) break;
    if (el >= next_report) {
      uint64_t it = 0; uint32_t errs = 0;
      for (Worker* w : ws) { it += w->iterations; errs += w->faults + w->wrong; }
      printf("  %4.0f s: %llu iterations, %u error(s)\n", el, (unsigned long long)it, errs);
      fflush(stdout);
      next_report += 10;
    }
    Sleep(100);
  }
  g_stop = true;
  for (auto& t : th) t.join();
  double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  uint64_t total_it = 0, total_pc = 0;
  bool fail = false, aff = true;
  printf("\nlogical processor (group:bit)  core  class  iterations      pc stores        faults  wrong\n");
  for (Worker* w : ws) {
    total_it += w->iterations; total_pc += w->pc_stores;
    fail |= (w->faults + w->wrong) != 0;
    aff &= w->affinity_ok;
    printf("  %3d (%u:%2u)%s               %4d  %5d  %-14llu  %-14llu  %6u  %5u\n", w->index, w->group, w->bit,
           w->affinity_ok ? " " : "*", w->core, w->eff_class, (unsigned long long)w->iterations.load(),
           (unsigned long long)w->pc_stores.load(), w->faults.load(), w->wrong.load());
  }
  if (!aff) printf("  * thread could not be pinned to its logical processor (SetThreadGroupAffinity failed)\n");
  printf("\n%llu iterations, %llu program-counter stores, %.1f s\n", (unsigned long long)total_it,
         (unsigned long long)total_pc, el);
  if (g_inject != INJ_NONE && !g_injected) {
    printf("SELF-TEST NOT DONE: the run ended before the injection (needs more than %.0f s)\n", g_inject_after_s);
    return 2;
  }
  if (!fail) {
    printf("VERDICT: PASS (no fault, no wrong result).\n"
           "A PASS does not prove that the processor is sound: it only means these patterns ran correctly for this long, "
           "at this temperature and with these settings.\n");
    return 0;
  }
  printf("VERDICT: FAIL\n");
  for (Worker* w : ws) {
    if (!w->have_first) continue;
    const ErrorRec& e = w->first;
    const Instance* in = e.inst >= 0 ? &g_inst[size_t(e.inst)] : nullptr;
    printf("  logical processor %d (group %u, bit %u, core %d, class %d): %u fault(s), %u wrong result(s); first:\n",
           w->index, w->group, w->bit, w->core, w->eff_class, w->faults.load(), w->wrong.load());
    if (e.kind == 1) {
      printf("    exception 0x%08X at rip %+lld from the instance start, %s address 0x%016llX, base register 0x%016llX\n",
             e.code, in ? (long long)(e.rip - uint64_t(g_code) - in->off) : 0LL,
             e.rw == 0 ? "read" : e.rw == 1 ? "write" : e.rw == 8 ? "execute" : "access",
             (unsigned long long)e.fault_addr, (unsigned long long)e.base_reg);
    } else {
      printf("    wrong result at %s offset 0x%X: got 0x%08X, expected 0x%08X\n",
             e.first_diff < kCtxSize ? "state" : "memory",
             e.first_diff < kCtxSize ? e.first_diff : e.first_diff - kCtxSize, e.got, e.want);
    }
    if (in) printf("    %s, input set %d, state block at page offset 0x%X, after %llu iterations on this thread\n",
                   describe(*in).c_str(), e.input, e.ctx_off, (unsigned long long)e.iteration);
  }
  if (g_inject != INJ_NONE) printf("(self-test run: the error above was injected on purpose by --inject)\n");
  return 1;
}
