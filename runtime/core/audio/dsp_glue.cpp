// HLE DSP microcodes (GameCube AX, CARD, ROM, INIT) plugged into the SDK's native DSP library
// (hle/native_audio.c), without any register: see dsp_ucode.h.
//
// Microcode selection and switching follow Dolphin's logic (commit e6f3ae17, GPL-2.0-or-later):
// Source/Core/Core/HW/DSPHLE/DSPHLE.cpp (SetUCode, SwapUCode: the "persistent" microcode, e.g. AX, is set aside
// and resumed as is if the game comes back to the same hash), UCodes/UCodes.cpp (UCodeFactory: unknown hash
// -> AX on GameCube), UCodes/ROM.cpp (mail 0x8071FEED, "ector" hash of the task's IRAM code) and
// UCodes/INIT.cpp (mail 0x80544348). Up to step 10c, this role was held by hw/dsp.cpp (emulated
// registers, removed): identical behaviour, same mails at the same time.
//
// Microcode switches requested by a microcode (new task, back to the ROM) are applied after its processing
// returns, as the very last action: the microcode's state may then be destroyed or set aside.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "../platform/gcrt_path.h"
#include "audio_hw.h"
#include "audio_trace.hpp"
#include "dsp_ucode.h"
#include "ucode.hpp"

namespace gcrt::audio {
namespace {

constexpr uint32_t ROM_READY_MAIL = 0x8071FEEDu;  // Dolphin ROMUCode::Initialize
constexpr uint32_t INIT_READY_MAIL = 0x80544348u; // Dolphin INITUCode::Initialize

int16_t g_coefs[0x800];
bool g_coefs_valid = false;

GcrtDspMailSink g_sink{};
GuestMemory g_mem{};

class GlueHost final : public UcodeHost {
public:
  std::unique_ptr<Ucode> ucode;

  const GuestMemory& memory() const override { return g_mem; }
  void push_mail(uint32_t mail, bool interrupt, uint32_t delay_cycles) override {
    glue_stats().mails_from_dsp++;
    if (interrupt) {
      glue_stats().interrupts++;
    }
    if (g_sink.push != nullptr) {
      g_sink.push(g_sink.user, mail, interrupt, delay_cycles);
    }
  }
  void request_set_ucode(uint32_t crc) override {
    m_pending = Pending::Set;
    m_pending_crc = crc;
  }
  void request_swap_ucode(uint32_t crc) override {
    m_pending = Pending::Swap;
    m_pending_crc = crc;
  }
  const int16_t* polyphase_coefs() const override { return g_coefs_valid ? g_coefs : nullptr; }
  DspStats& stats() override { return glue_stats(); }

  // Pending request (None if there is none), cleared.
  bool take_pending(bool& swap, uint32_t& crc) {
    if (m_pending == Pending::None) {
      return false;
    }
    swap = m_pending == Pending::Swap;
    crc = m_pending_crc;
    m_pending = Pending::None;
    return true;
  }

private:
  enum class Pending { None, Set, Swap };
  Pending m_pending = Pending::None;
  uint32_t m_pending_crc = 0;
};

// Current microcode and set-aside microcode (Dolphin m_ucode / m_last_ucode). Without an instance: ROM or INIT.
struct Slot {
  bool valid = false;
  std::unique_ptr<GlueHost> host;
  uint32_t crc = 0;
  const char* name = "ROM";
};
Slot g_current;
Slot g_last;

// DSP ROM (Dolphin ROMUCode::HandleMail): task parameters announced by 0x80F3xxxx, then the value.
struct RomState {
  uint32_t next_param = 0;
  uint32_t ram_address = 0;
  uint32_t length = 0;
};
RomState g_rom{};

void clear_mails() {
  if (g_sink.clear != nullptr) {
    g_sink.clear(g_sink.user);
  }
}

void push_plain(uint32_t mail) {
  glue_stats().mails_from_dsp++;
  if (g_sink.push != nullptr) {
    g_sink.push(g_sink.user, mail, false, 0);
  }
}

void switch_ucode(uint32_t crc, bool keep_previous);
void set_ucode(uint32_t crc);

// Applies the switch request left by the processing that has just finished. `host` may be destroyed.
void finish(GlueHost* host) {
  bool swap = false;
  uint32_t crc = 0;
  if (host == nullptr || !host->take_pending(swap, crc)) {
    return;
  }
  glue_stats().ucode_switches++;
  if (!swap && crc == UCODE_ROM) {
    set_ucode(UCODE_ROM); // back to the ROM: current microcode destroyed, not set aside (Dolphin SetUCode)
  } else {
    switch_ucode(crc, true);
  }
}

// Dolphin SetUCode: the current microcode is replaced (destroyed), pending mails are discarded.
void set_ucode(uint32_t crc) {
  g_current = Slot{};
  clear_mails();
  g_current.valid = true;
  g_current.crc = crc;
  if (crc == UCODE_ROM) {
    g_current.name = "ROM";
    g_rom = RomState{};
    GCRT_AUDIO_TRACE(TraceCat::Dsp, "microcode 0x%08X: ROM (waiting for a task)", crc);
    push_plain(ROM_READY_MAIL);
    return;
  }
  if (crc == UCODE_INIT_AUDIO_SYSTEM) {
    g_current.name = "init-audio";
    GCRT_AUDIO_TRACE(TraceCat::Dsp, "microcode 0x%08X: OS audio initialisation", crc);
    push_plain(INIT_READY_MAIL);
    return;
  }
  auto host = std::make_unique<GlueHost>();
  host->ucode = make_audio_ucode(*host, crc, true); // unknown hash -> AX, like Dolphin on GameCube
  g_current.name = classify_ucode(crc) == UcodeKind::Card ? "CARD (HLE)" : "AX GameCube (HLE audio)";
  GCRT_AUDIO_TRACE(TraceCat::Dsp, "microcode 0x%08X -> HLE %s", crc, host->ucode->name());
  GlueHost* raw = host.get();
  g_current.host = std::move(host);
  raw->ucode->initialize();
  finish(raw);
}

// Dolphin SwapUCode: a non-ROM microcode is set aside (a single slot) and resumed as is if the game comes back to
// the same hash.
void switch_ucode(uint32_t crc, bool keep_previous) {
  clear_mails();
  if (keep_previous && g_last.valid && g_last.crc == crc) {
    g_current = std::move(g_last);
    g_last = Slot{};
    GCRT_AUDIO_TRACE(TraceCat::Dsp, "resuming microcode 0x%08X: %s", crc, g_current.name);
    return;
  }
  if (keep_previous && !g_last.valid && g_current.valid && g_current.crc != UCODE_ROM) {
    g_last = std::move(g_current);
    g_current = Slot{};
  }
  set_ucode(crc);
}

void load_coefficients() {
  g_coefs_valid = false;
  const char* poly = std::getenv("GCRT_DSP_POLYPHASE");
  if (poly != nullptr && std::strcmp(poly, "0") == 0) {
    GCRT_AUDIO_TRACE(TraceCat::Dsp, "polyphase sample rate conversion disabled (GCRT_DSP_POLYPHASE=0)");
    return;
  }
  if (const char* path = std::getenv("GCRT_DSP_COEF")) {
    char utf8[4096]; // the same value in UTF-8 (wide-character environment on Windows), opened as such
    if (gcrt_getenv_utf8("GCRT_DSP_COEF", utf8, sizeof(utf8))) {
      path = utf8;
    }
    if (std::FILE* f = gcrt_fopen_utf8(path, "rb")) {
      uint8_t raw[0x1000];
      const size_t n = std::fread(raw, 1, sizeof(raw), f);
      std::fclose(f);
      if (n == sizeof(raw)) {
        for (int i = 0; i < 0x800; ++i) {
          g_coefs[i] = static_cast<int16_t>((raw[2 * i] << 8) | raw[2 * i + 1]);
        }
        g_coefs_valid = true;
        GCRT_AUDIO_TRACE(TraceCat::Dsp, "polyphase table read from %s", path);
        return;
      }
    }
    GCRT_AUDIO_TRACE(TraceCat::Warn, "GCRT_DSP_COEF=%s unreadable (4096 bytes expected), free table used", path);
  }
  std::memcpy(g_coefs, free_polyphase_coefs(), sizeof(g_coefs));
  g_coefs_valid = true;
}

} // namespace
} // namespace gcrt::audio

using namespace gcrt::audio;

extern "C" void gcrt_audio_register_ucodes(void) {
  trace_init_from_env();
  load_coefficients();
  GCRT_AUDIO_TRACE(TraceCat::Dsp, "HLE audio microcodes ready (GameCube AX, CARD; AX for an unknown hash)");
}

extern "C" void gcrt_audio_dsp_attach(const GcrtDspMailSink* sink, uint8_t* ram, uint32_t ram_size, uint8_t* aram,
                                      uint32_t aram_size) {
  g_sink = sink != nullptr ? *sink : GcrtDspMailSink{};
  g_mem.ram = ram;
  g_mem.ram_size = ram_size;
  g_mem.aram = aram;
  g_mem.aram_size = aram_size;
  g_last = Slot{};
  set_ucode(UCODE_ROM); // initial DSP state (Dolphin DSPHLE::Initialize): ROM, mail 0x8071FEED
}

extern "C" void gcrt_audio_dsp_detach(void) {
  g_current = Slot{};
  g_last = Slot{};
  g_sink = GcrtDspMailSink{};
  g_mem = GuestMemory{};
}

extern "C" uint32_t gcrt_audio_dsp_boot(uint32_t iram_ram_addr, uint32_t iram_length, uint32_t start_pc) {
  // Dolphin ROMUCode (mail 0x80F3D001): hash of the IRAM code taken from main memory, then SwapUCode.
  const uint32_t phys = iram_ram_addr & 0x01FFFFFFu;
  uint32_t crc = 0;
  if (g_mem.ram != nullptr && phys < g_mem.ram_size && iram_length <= g_mem.ram_size - phys) {
    crc = gcrt_audio_hash_ector(g_mem.ram + phys, iram_length);
  }
  GCRT_AUDIO_TRACE(TraceCat::Dsp, "ROM: starting task 0x%08X (%u bytes, entry %04X), hash %08X",
                   iram_ram_addr, iram_length, start_pc & 0xFFFFu, crc);
  switch_ucode(crc, true);
  return crc;
}

extern "C" void gcrt_audio_dsp_mail(uint32_t mail) {
  glue_stats().mails_to_dsp++;
  GlueHost* host = g_current.host.get();
  if (host != nullptr) {
    host->ucode->handle_mail(mail);
    finish(host);
    return;
  }
  if (!g_current.valid || g_current.crc != UCODE_ROM) {
    return; // OS initialisation microcode: expects no mail
  }
  // ROM (Dolphin ROMUCode::HandleMail): path of a game that would announce its task to the ROM itself; the SDK's
  // native DSP library goes through gcrt_audio_dsp_boot. [hypothesis]: path not exercised by the reference game,
  // covered only by the unit test (tests/audio, test_dsp_glue).
  if (g_rom.next_param == 0) {
    if ((mail & 0xFFFF0000u) != 0x80F30000u) {
      push_plain(0xFEEE0000u | (mail & 0xFFFFu));
    } else {
      g_rom.next_param = mail;
    }
    return;
  }
  const uint32_t param = g_rom.next_param;
  g_rom.next_param = 0;
  switch (param) {
  case 0x80F3A001u:
    g_rom.ram_address = mail;
    break;
  case 0x80F3A002u:
    g_rom.length = mail & 0xFFFFu;
    break;
  case 0x80F3D001u:
    gcrt_audio_dsp_boot(g_rom.ram_address, g_rom.length, mail & 0xFFFFu);
    break;
  default: // 0x80F3B002 (DRAM length), 0x80F3C002 (IRAM address): no effect
    break;
  }
}

extern "C" void gcrt_audio_dsp_update(void) {
  GlueHost* host = g_current.host.get();
  if (host == nullptr) {
    return;
  }
  host->ucode->update();
  finish(host);
}

extern "C" const char* gcrt_audio_dsp_name(void) { return g_current.name; }

extern "C" uint32_t gcrt_audio_hash_ector(const uint8_t* data, uint32_t length) { return hash_ector(data, length); }
