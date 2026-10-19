// gcextract: native extractor for an uncompressed GameCube disc image (.iso / .gcm). Generic: no game is named here;
// the accepted images come from the command line (--game-toml or --hashes).
//
// Same output as tools/extract.py + tools/gcdisc.py + tools/u8arc.py, without Python, so that a player's installer can
// run it: sys/ (boot.bin, bi2.bin, apploader.img, main.dol, fst.bin), files/ (the disc file system) and, on request,
// unpacked/<archive name>/ (U8 archives found in the extracted files).
//
// Disc layout (sources: YAGCD, disc section; Dolphin Source/Core/DiscIO/VolumeGC.cpp, as in tools/gcdisc.py):
//   0x0000 boot.bin (0x440): game ID (6 bytes), magic 0xC2339F3D at 0x1C, DOL offset 0x420, FST offset 0x424,
//          FST size 0x428; 0x0440 bi2.bin (0x2000); 0x2440 apploader (0x20-byte header + code + trailer).
//   FST: 12-byte big-endian entries [type(1) | name offset(3)] [offset | parent] [size | next], then the names;
//   entry 0 is the root, its third field is the number of entries.
// U8 archive (source: wiki.tockdom.com "U8 (File Format)", as in tools/u8arc.py): magic 0x55AA382D, offset of the
// root node at 4, 12-byte nodes like the FST, offsets relative to the start of the archive.
//
// Safety: every offset and size read from the image is bounds-checked before anything is written; every name is
// checked to be a single, plain path component (no "..", separators, drive letters, device names), so nothing can
// be written outside the output folder. Nothing is ever deleted.
//
// Machine-readable output on stdout, one line each, flushed (for an installer):
//   INFO <key> <value>                      e.g. INFO game_id XXXX01, INFO sha1 <hex>, INFO match <label>
//   PROGRESS <stage> <done> <total>         stage = verify (bytes hashed) | extract (bytes written)
//   DONE <files> <bytes>
//   ERROR <exit code> <message>             also printed to stderr
// Exit codes: see enum Exit below.

#include "sha1.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

enum Exit {
  kOk = 0,
  kUsage = 2,          // bad command line, unreadable hash list
  kReadError = 3,      // image missing, unreadable, truncated
  kNotGameCube = 4,    // empty file, other format (archive, compressed image, Wii disc, anything else)
  kUnknownImage = 5,   // GameCube image whose SHA-1 is not in the accepted list (or whose ID does not match it)
  kBadLayout = 6,      // inconsistent FST / DOL / U8 data, or a name that is not a safe path component
  kWriteError = 7,     // output folder not writable, disk full, existing files without --overwrite
};

constexpr const char* kVersion = "1.0";

std::string g_last_progress;

void line(const std::string& text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

[[noreturn]] void fail(int code, const std::string& message) {
  line("ERROR " + std::to_string(code) + " " + message);
  std::fprintf(stderr, "gcextract: %s\n", message.c_str());
  std::exit(code);
}

std::string utf8(const fs::path& p) {
  auto s = p.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

fs::path from_utf8(const std::string& s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

std::string hex32(uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "0x%llX", (unsigned long long)v);
  return buf;
}

// ------------------------------------------------------------------------------------------------ image reader
class Image {
 public:
  explicit Image(const fs::path& path) : path_(path) {
#ifdef _WIN32
    f_ = _wfopen(path.c_str(), L"rb");
#else
    f_ = std::fopen(path.c_str(), "rb");
#endif
    if (!f_)
      fail(kReadError, "cannot open the image: " + utf8(path));
    std::error_code ec;
    size_ = fs::file_size(path, ec);
    if (ec)
      fail(kReadError, "cannot read the size of the image: " + utf8(path));
  }
  ~Image() {
    if (f_)
      std::fclose(f_);
  }
  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;

  uint64_t size() const { return size_; }

  // Reads exactly `size` bytes at `offset`; any short read or I/O error is fatal (exit kReadError).
  void read(uint64_t offset, void* out, size_t size) {
    if (offset > size_ || size > size_ - offset)
      fail(kReadError, "read past the end of the image at " + hex32(offset) + " (" + std::to_string(size) +
                           " bytes; image of " + std::to_string(size_) + " bytes: truncated?)");
#ifdef _WIN32
    int seek = _fseeki64(f_, (long long)offset, SEEK_SET);
#else
    int seek = fseeko(f_, (off_t)offset, SEEK_SET);
#endif
    if (seek != 0 || std::fread(out, 1, size, f_) != size)
      fail(kReadError, "read error in the image at " + hex32(offset) + " (" + std::to_string(size) + " bytes)");
  }

  std::vector<uint8_t> read(uint64_t offset, size_t size) {
    std::vector<uint8_t> v(size);
    if (size)
      read(offset, v.data(), size);
    return v;
  }

 private:
  fs::path path_;
  std::FILE* f_ = nullptr;
  uint64_t size_ = 0;
};

// ------------------------------------------------------------------------------------------------ names and paths
bool is_reserved_windows_name(std::string stem) {
  // Device names are reserved whatever the extension (CON.txt too). Case-insensitive.
  auto dot = stem.find('.');
  if (dot != std::string::npos)
    stem.resize(dot);
  for (auto& c : stem)
    c = (char)std::toupper((unsigned char)c);
  static const char* names[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
  for (const char* n : names)
    if (stem == n)
      return true;
  if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) && stem[3] >= '0' &&
      stem[3] <= '9')
    return true;
  return false;
}

// Converts a raw name (Shift-JIS on Japanese discs, ASCII otherwise) to UTF-8 and checks that it is one plain path
// component. Returns false with a reason if it is not safe.
bool safe_component(const std::string& raw, std::string& out, std::string& why) {
  if (raw.empty()) {
    why = "empty name";
    return false;
  }
  bool ascii = true;
  for (unsigned char c : raw) {
    if (c < 0x20 || c == 0x7F) {
      why = "control character in a name";
      return false;
    }
    if (c >= 0x80)
      ascii = false;
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      why = "forbidden character in a name";
      return false;
    }
  }
  if (raw == "." || raw == "..") {
    why = "relative name";
    return false;
  }
  if (raw.back() == '.' || raw.back() == ' ') {
    why = "name ending with a dot or a space";
    return false;
  }
  if (is_reserved_windows_name(raw)) {
    why = "reserved device name";
    return false;
  }
  if (ascii) {
    out = raw;
    return true;
  }
#ifdef _WIN32
  // Code page 932 = Shift-JIS (Windows), like the Python tools' "shift_jis" codec; invalid bytes are refused here
  // (Python replaces them) because a replaced name could collide with another one.
  int wn = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, raw.data(), (int)raw.size(), nullptr, 0);
  if (wn > 0) {
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, raw.data(), (int)raw.size(), w.data(), wn);
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    out.assign((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, out.data(), n, nullptr, nullptr);
    return true;
  }
#endif
  why = "name that is not valid Shift-JIS";
  return false;
}

struct Entry {
  std::string path;  // relative to the output folder, '/' separator, UTF-8
  uint64_t offset;   // in the image
  uint64_t size;
};

std::string name_at(const uint8_t* names, size_t names_size, uint32_t offset, const char* what) {
  if (offset >= names_size)
    fail(kBadLayout, std::string(what) + ": name offset " + hex32(offset) + " outside the name table");
  const void* end = std::memchr(names + offset, 0, names_size - offset);
  if (!end)
    fail(kBadLayout, std::string(what) + ": unterminated name at " + hex32(offset));
  return std::string(reinterpret_cast<const char*>(names + offset), static_cast<const uint8_t*>(end) - (names + offset));
}

// Walks a FST-like node table (disc FST or U8 archive). `base`: offset added to file offsets; `limit`: size of the
// container the files must fit in. Directories are visited depth-first exactly like tools/gcdisc.py / u8arc.py.
std::vector<Entry> walk_nodes(const std::vector<uint8_t>& table, size_t nodes_at, const std::string& root,
                              uint64_t base, uint64_t limit, const char* what) {
  if (nodes_at > table.size() || table.size() - nodes_at < 12)
    fail(kBadLayout, std::string(what) + ": table too small");
  const uint8_t* nodes = table.data() + nodes_at;
  const uint32_t count = be32(nodes + 8);
  if (count == 0 || (uint64_t)count * 12u > table.size() - nodes_at)
    fail(kBadLayout, std::string(what) + ": " + std::to_string(count) + " entries do not fit in the table");
  const uint8_t* names = nodes + (size_t)count * 12u;
  const size_t names_size = table.size() - nodes_at - (size_t)count * 12u;

  std::vector<Entry> out;
  struct Dir {
    uint32_t end;
    std::string path;
  };
  std::vector<Dir> stack{{count, root}};
  for (uint32_t i = 1; i < count; i++) {
    while (!stack.empty() && i >= stack.back().end)
      stack.pop_back();
    if (stack.empty())
      fail(kBadLayout, std::string(what) + ": entry " + std::to_string(i) + " outside the root directory");
    const uint8_t* e = nodes + (size_t)i * 12u;
    const uint32_t word0 = be32(e), a = be32(e + 4), b = be32(e + 8);
    const std::string raw = name_at(names, names_size, word0 & 0xFFFFFFu, what);
    std::string name, why;
    if (!safe_component(raw, name, why))
      fail(kBadLayout, std::string(what) + ": entry " + std::to_string(i) + " refused (" + why + ")");
    const std::string& parent = stack.back().path;
    const std::string path = parent.empty() ? name : parent + "/" + name;
    if (word0 >> 24) {
      // A directory ends at `b` (index of the next entry after its contents): it must lie inside its parent.
      if (b <= i || b > stack.back().end)
        fail(kBadLayout, std::string(what) + ": directory entry " + std::to_string(i) + " with an invalid end " +
                             std::to_string(b));
      stack.push_back({b, path});
    } else {
      const uint64_t off = base + a;
      if (a > limit || b > limit - a)
        fail(kBadLayout, std::string(what) + ": file " + path + " (" + hex32(a) + ", " + std::to_string(b) +
                             " bytes) outside its container");
      out.push_back({path, off, b});
    }
  }
  return out;
}

// ------------------------------------------------------------------------------------------------ output
class Writer {
 public:
  Writer(const fs::path& root, bool overwrite) : root_(fs::absolute(root).lexically_normal()), overwrite_(overwrite) {}

  // Full path of a relative entry path; checks lexically that it stays below the output folder.
  fs::path target(const std::string& rel) const {
    fs::path p = (root_ / from_utf8(rel)).lexically_normal();
    auto r = p.lexically_relative(root_);
    if (r.empty() || *r.begin() == "..")
      fail(kBadLayout, "path outside the output folder refused: " + rel);
    return p;
  }

  void check_new(const std::string& rel) const {
    std::error_code ec;
    if (!overwrite_ && fs::exists(target(rel), ec))
      fail(kWriteError, "file already present (use --overwrite to replace the files of the image): " +
                            utf8(target(rel)));
  }

  std::FILE* open(const std::string& rel) {
    fs::path p = target(rel);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (ec)
      fail(kWriteError, "cannot create the folder " + utf8(p.parent_path()) + ": " + ec.message());
#ifdef _WIN32
    std::FILE* f = _wfopen(p.c_str(), L"wb");
#else
    std::FILE* f = std::fopen(p.c_str(), "wb");
#endif
    if (!f)
      fail(kWriteError, "cannot create " + utf8(p));
    return f;
  }

  static void put(std::FILE* f, const void* data, size_t size, const std::string& rel) {
    if (size && std::fwrite(data, 1, size, f) != size)
      fail(kWriteError, "write error (disk full?) in " + rel);
  }

  static void close(std::FILE* f, const std::string& rel) {
    if (std::fclose(f) != 0)
      fail(kWriteError, "write error (disk full?) when closing " + rel);
  }

  const fs::path& root() const { return root_; }

 private:
  fs::path root_;
  bool overwrite_;
};

struct Progress {
  const char* stage;
  uint64_t total;
  uint64_t done = 0;
  int last_percent = -1;
  void add(uint64_t n) {
    done += n;
    int percent = total ? (int)(done * 100u / total) : 100;
    if (percent != last_percent) {
      last_percent = percent;
      line(std::string("PROGRESS ") + stage + " " + std::to_string(done) + " " + std::to_string(total));
    }
  }
};

// ------------------------------------------------------------------------------------------------ accepted images
struct Accepted {
  std::string sha1;
  std::string game_id;  // may be empty (no ID check)
  std::string label;
};

bool is_hex40(const std::string& s) {
  if (s.size() != 40)
    return false;
  for (char c : s)
    if (!std::isxdigit((unsigned char)c))
      return false;
  return true;
}

std::string lower(std::string s) {
  for (auto& c : s)
    c = (char)std::tolower((unsigned char)c);
  return s;
}

std::vector<std::string> read_lines(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    fail(kUsage, "cannot read " + utf8(path));
  std::vector<std::string> lines;
  std::string l;
  while (std::getline(in, l)) {
    if (!l.empty() && l.back() == '\r')
      l.pop_back();
    lines.push_back(l);
  }
  return lines;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Plain list: "<sha1> [game id] [label...]" per line, '#' starts a comment.
void load_hash_list(const fs::path& path, std::vector<Accepted>& out) {
  int n = 0;
  for (auto& raw : read_lines(path)) {
    n++;
    std::string l = trim(raw.substr(0, raw.find('#')));
    if (l.empty())
      continue;
    size_t sp = l.find_first_of(" \t");
    Accepted a{lower(l.substr(0, sp)), "", ""};
    if (!is_hex40(a.sha1))
      fail(kUsage, utf8(path) + ":" + std::to_string(n) + ": expected a 40-digit SHA-1");
    if (sp != std::string::npos) {
      std::string rest = trim(l.substr(sp));
      size_t sp2 = rest.find_first_of(" \t");
      a.game_id = rest.substr(0, sp2);
      if (sp2 != std::string::npos)
        a.label = trim(rest.substr(sp2));
    }
    out.push_back(a);
  }
}

// Minimal reader for the [[versions]] tables of a games/<game>/game.toml (the project's game description): only the
// keys game_id, region and iso_sha1, as basic strings on one line. Anything else is ignored.
void load_game_toml(const fs::path& path, std::vector<Accepted>& out) {
  bool in_versions = false;
  Accepted cur;
  auto flush = [&] {
    if (in_versions && !cur.sha1.empty())
      out.push_back(cur);
    cur = Accepted{};
  };
  for (auto& raw : read_lines(path)) {
    std::string l = trim(raw);
    if (l.empty() || l[0] == '#')
      continue;
    if (l[0] == '[') {
      flush();
      in_versions = l == "[[versions]]";
      continue;
    }
    if (!in_versions)
      continue;
    size_t eq = l.find('=');
    if (eq == std::string::npos)
      continue;
    std::string key = trim(l.substr(0, eq)), value = trim(l.substr(eq + 1));
    if (value.size() < 2 || value[0] != '"')
      continue;
    size_t close = value.find('"', 1);
    if (close == std::string::npos)
      continue;
    value = value.substr(1, close - 1);
    if (key == "iso_sha1")
      cur.sha1 = lower(value);
    else if (key == "game_id")
      cur.game_id = value;
    else if (key == "region")
      cur.label = value;
  }
  flush();
  if (out.empty())
    fail(kUsage, utf8(path) + ": no [[versions]] table with an iso_sha1 key");
  for (auto& a : out)
    if (!is_hex40(a.sha1))
      fail(kUsage, utf8(path) + ": invalid iso_sha1 \"" + a.sha1 + "\"");
}

// ------------------------------------------------------------------------------------------------ format detection
// Recognises what a player might pick instead of a plain image, to give a clear message.
void refuse_other_formats(const uint8_t* h, size_t n, uint64_t size) {
  auto starts = [&](const char* magic, size_t len) { return n >= len && std::memcmp(h, magic, len) == 0; };
  const std::string convert =
      " Only uncompressed GameCube images (.iso, .gcm) are supported: convert it to a plain ISO first.";
  if (size == 0)
    fail(kNotGameCube, "the file is empty: not a disc image.");
  if (starts("PK\x03\x04", 4) || starts("7z\xBC\xAF\x27\x1C", 6) || starts("Rar!\x1A\x07", 6))
    fail(kNotGameCube, "this is an archive (ZIP, 7z or RAR): extract the .iso it contains and pick that file.");
  if (starts("RVZ\x01", 4))
    fail(kNotGameCube, "this is a compressed RVZ image." + convert);
  if (starts("WIA\x01", 4))
    fail(kNotGameCube, "this is a compressed WIA image." + convert);
  if (starts("\x01\xC0\x0B\xB1", 4))  // GCZ: little-endian magic 0xB10BC001
    fail(kNotGameCube, "this is a compressed GCZ image." + convert);
  if (starts("CISO", 4))
    fail(kNotGameCube, "this is a compressed CISO image." + convert);
  if (n >= 0x20 && be32(h + 0x18) == 0x5D1C9EA3u)
    fail(kNotGameCube, "this is a Wii disc image, not a GameCube one.");
  if (n < 0x440 || size < 0x2460)
    fail(kNotGameCube, "the file is too small to be a GameCube disc image.");
  if (be32(h + 0x1C) != 0xC2339F3Du)
    fail(kNotGameCube, "not a GameCube disc image (no disc magic at 0x1C).");
}

// ------------------------------------------------------------------------------------------------ main logic
struct Options {
  fs::path image, out, game_toml, hashes, manifest;
  std::vector<std::string> u8;
  bool overwrite = false, system_only = false, list = false, verify_only = false;
};

void usage() {
  std::printf(
      "gcextract %s: extracts an uncompressed GameCube disc image (.iso / .gcm).\n"
      "\n"
      "Usage: gcextract <image> <output folder> [options]\n"
      "       gcextract <image> --verify-only [--game-toml FILE | --hashes FILE]\n"
      "\n"
      "  --game-toml FILE   accepted images = the [[versions]] tables (game_id, iso_sha1) of a game description\n"
      "  --hashes FILE      accepted images = lines \"<sha1> [game id] [label]\" ('#' starts a comment)\n"
      "                     (without either option, any GameCube image is accepted and a warning is printed)\n"
      "  --u8 PATH          also unpack the U8 archive PATH (relative to the output, e.g. files/x.bin) under\n"
      "                     unpacked/<archive name>/; repeatable\n"
      "  --system-only      extract sys/ only (boot.bin, bi2.bin, apploader.img, main.dol, fst.bin)\n"
      "  --overwrite        replace files already present in the output folder (nothing is ever deleted)\n"
      "  --manifest FILE    write a small JSON manifest (game_id, image, iso_sha1, disc_root)\n"
      "  --verify-only      check the image (format, SHA-1, game ID) and write nothing\n"
      "  --list             print each extracted file (FILE <path> <size>)\n"
      "\n"
      "Exit codes: 0 ok, 2 usage, 3 read error, 4 not a GameCube image, 5 unknown image,\n"
      "            6 inconsistent disc data, 7 write error.\n",
      kVersion);
}

std::string json_escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\')
      o += '\\', o += c;
    else if ((unsigned char)c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", (unsigned char)c);
      o += buf;
    } else
      o += c;
  }
  return o;
}

int run(const Options& o) {
  std::vector<Accepted> accepted;
  if (!o.game_toml.empty())
    load_game_toml(o.game_toml, accepted);
  if (!o.hashes.empty())
    load_hash_list(o.hashes, accepted);

  Image img(o.image);
  uint8_t header[0x440] = {};
  size_t head = img.size() < sizeof header ? (size_t)img.size() : sizeof header;
  if (head)
    img.read(0, header, head);
  refuse_other_formats(header, head, img.size());

  const std::string game_id(reinterpret_cast<const char*>(header), 6);
  for (char c : game_id)
    if (c < 0x20 || c > 0x7E)
      fail(kNotGameCube, "not a GameCube disc image (invalid game ID).");
  line("INFO game_id " + game_id);
  line("INFO size " + std::to_string(img.size()));

  // --- 1. Fingerprint of the whole image, before anything is written.
  gcx::Sha1 sha;
  {
    Progress p{"verify", img.size()};
    std::vector<uint8_t> buf(4u << 20);
    for (uint64_t off = 0; off < img.size();) {
      size_t n = (size_t)std::min<uint64_t>(buf.size(), img.size() - off);
      img.read(off, buf.data(), n);
      sha.update(buf.data(), n);
      off += n;
      p.add(n);
    }
  }
  const std::string digest = sha.hex();
  line("INFO sha1 " + digest);
  if (accepted.empty()) {
    line("INFO warning no list of accepted images: the image is not checked");
  } else {
    const Accepted* match = nullptr;
    for (auto& a : accepted)
      if (a.sha1 == digest)
        match = &a;
    if (!match) {
      std::string known;
      for (auto& a : accepted)
        known += (known.empty() ? "" : ", ") + (a.game_id.empty() ? a.sha1 : a.game_id);
      fail(kUnknownImage, "unknown disc image (game ID " + game_id + ", SHA-1 " + digest +
                              "). Accepted: " + known +
                              ". A modified, trimmed (NKit), patched or damaged dump is refused.");
    }
    if (!match->game_id.empty() && match->game_id != game_id)
      fail(kUnknownImage, "the image's game ID " + game_id + " differs from the expected " + match->game_id);
    line("INFO match " + (match->game_id.empty() ? digest : match->game_id) +
         (match->label.empty() ? "" : " " + match->label));
  }
  if (o.verify_only) {
    line("DONE 0 0");
    return kOk;
  }

  // --- 2. Plan everything (bounds and names checked) before writing.
  const uint32_t dol_off = be32(header + 0x420), fst_off = be32(header + 0x424), fst_size = be32(header + 0x428);
  std::vector<Entry> plan;
  plan.push_back({"sys/boot.bin", 0, 0x440});
  plan.push_back({"sys/bi2.bin", 0x440, 0x2000});
  {
    auto apl = img.read(0x2440, 0x20);
    uint64_t size = 0x20u + (uint64_t)be32(apl.data() + 0x14) + be32(apl.data() + 0x18);
    plan.push_back({"sys/apploader.img", 0x2440, size});
  }
  {
    auto dol = img.read(dol_off, 0x100);
    uint64_t end = 0;
    for (int i = 0; i < 18; i++) {
      uint64_t off = be32(dol.data() + 4 * i), size = be32(dol.data() + 0x90 + 4 * i);
      if (size && off + size > end)
        end = off + size;
    }
    if (end == 0)
      fail(kBadLayout, "main DOL without any segment");
    plan.push_back({"sys/main.dol", dol_off, end});
  }
  plan.push_back({"sys/fst.bin", fst_off, fst_size});
  for (auto& e : plan)
    if (e.offset > img.size() || e.size > img.size() - e.offset)
      fail(kBadLayout, e.path + " (" + hex32(e.offset) + ", " + std::to_string(e.size) +
                           " bytes) lies outside the image: truncated image?");
  if (!o.system_only) {
    auto fst = img.read(fst_off, fst_size);
    auto files = walk_nodes(fst, 0, "files", 0, img.size(), "FST");
    plan.insert(plan.end(), files.begin(), files.end());
  }

  // U8 archives: their own file list comes from their data, read now (from the image) so the plan is complete.
  struct U8Job {
    std::string archive;
    std::vector<Entry> entries;  // offsets relative to the image
  };
  std::vector<U8Job> u8jobs;
  for (auto& rel : o.u8) {
    const Entry* src = nullptr;
    for (auto& e : plan)
      if (e.path == rel)
        src = &e;
    if (!src)
      fail(kUsage, "--u8 " + rel + ": no such file in the image" + (o.system_only ? " (--system-only)" : ""));
    if (src->size < 0x20)
      fail(kBadLayout, rel + ": too small for a U8 archive");
    auto hdr = img.read(src->offset, 0x20);
    if (be32(hdr.data()) != 0x55AA382Du)
      fail(kBadLayout, rel + ": not a U8 archive");
    const uint64_t root = be32(hdr.data() + 4), table_size = be32(hdr.data() + 8);
    if (root > src->size || table_size > src->size - root)
      fail(kBadLayout, rel + ": U8 node table outside the archive");
    auto table = img.read(src->offset + root, (size_t)table_size);
    const std::string name = rel.substr(rel.find_last_of('/') + 1);
    auto entries = walk_nodes(table, 0, "unpacked/" + name, src->offset, src->size, rel.c_str());
    u8jobs.push_back({rel, entries});
  }
  for (auto& j : u8jobs)
    plan.insert(plan.end(), j.entries.begin(), j.entries.end());

  // Duplicate destinations (case-insensitive, as on Windows) would silently overwrite each other.
  {
    std::vector<std::string> keys;
    for (auto& e : plan)
      keys.push_back(lower(e.path));
    std::vector<std::string> sorted = keys;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 1; i < sorted.size(); i++)
      if (sorted[i] == sorted[i - 1])
        fail(kBadLayout, "two files of the image have the same path: " + sorted[i]);
  }

  // --- 3. Write.
  Writer w(o.out, o.overwrite);
  for (auto& e : plan)
    w.check_new(e.path);
  uint64_t total = 0;
  for (auto& e : plan)
    total += e.size;
  line("INFO files " + std::to_string(plan.size()));
  line("INFO output " + utf8(w.root()));
  Progress p{"extract", total};
  std::vector<uint8_t> buf(4u << 20);
  for (auto& e : plan) {
    std::FILE* f = w.open(e.path);
    for (uint64_t done = 0; done < e.size;) {
      size_t n = (size_t)std::min<uint64_t>(buf.size(), e.size - done);
      img.read(e.offset + done, buf.data(), n);
      Writer::put(f, buf.data(), n, e.path);
      done += n;
      p.add(n);
    }
    Writer::close(f, e.path);
    if (o.list)
      line("FILE " + e.path + " " + std::to_string(e.size));
  }
  if (total == 0)
    p.add(0);

  if (!o.manifest.empty()) {
    fs::path abs_image = fs::absolute(o.image).lexically_normal();
    std::string text = "{\n  \"game_id\": \"" + json_escape(game_id) + "\",\n  \"image\": \"" +
                       json_escape(utf8(abs_image)) + "\",\n  \"iso_sha1\": \"" + digest + "\",\n  \"disc_root\": \"" +
                       json_escape(utf8(w.root())) + "\",\n  \"file_count\": " + std::to_string(plan.size()) +
                       ",\n  \"extractor\": \"gcextract " + kVersion + "\"\n}\n";
    std::ofstream m(o.manifest, std::ios::binary | std::ios::trunc);
    m << text;
    m.close();
    if (!m)
      fail(kWriteError, "cannot write the manifest " + utf8(o.manifest));
    line("INFO manifest " + utf8(fs::absolute(o.manifest)));
  }
  line("DONE " + std::to_string(plan.size()) + " " + std::to_string(total));
  return kOk;
}

int main_utf8(std::vector<std::string> args) {
  Options o;
  std::vector<std::string> positional;
  for (size_t i = 1; i < args.size(); i++) {
    const std::string& a = args[i];
    auto value = [&](const char* opt) -> std::string {
      if (i + 1 >= args.size())
        fail(kUsage, std::string(opt) + " needs a value");
      return args[++i];
    };
    if (a == "--help" || a == "-h") {
      usage();
      return kOk;
    } else if (a == "--version") {
      std::printf("gcextract %s\n", kVersion);
      return kOk;
    } else if (a == "--game-toml") {
      o.game_toml = from_utf8(value("--game-toml"));
    } else if (a == "--hashes") {
      o.hashes = from_utf8(value("--hashes"));
    } else if (a == "--u8") {
      o.u8.push_back(value("--u8"));
    } else if (a == "--manifest") {
      o.manifest = from_utf8(value("--manifest"));
    } else if (a == "--overwrite") {
      o.overwrite = true;
    } else if (a == "--system-only") {
      o.system_only = true;
    } else if (a == "--verify-only") {
      o.verify_only = true;
    } else if (a == "--list") {
      o.list = true;
    } else if (a.size() > 1 && a[0] == '-' && a[1] == '-') {
      fail(kUsage, "unknown option " + a + " (--help)");
    } else {
      positional.push_back(a);
    }
  }
  if (positional.size() != (o.verify_only ? 1u : 2u))
    fail(kUsage, o.verify_only ? "expected one image path (--help)" : "expected an image path and an output folder (--help)");
  o.image = from_utf8(positional[0]);
  if (!o.verify_only)
    o.out = from_utf8(positional[1]);
  return run(o);
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  // Arguments as UTF-16 (any folder name works), converted once to UTF-8; console output in UTF-8.
  SetConsoleOutputCP(CP_UTF8);
  std::vector<std::string> args;
  for (int i = 0; i < argc; i++) {
    int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? (size_t)n - 1 : 0, '\0');
    if (n > 1)
      WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), n, nullptr, nullptr);
    args.push_back(s);
  }
  return main_utf8(args);
}
#else
int main(int argc, char** argv) { return main_utf8(std::vector<std::string>(argv, argv + argc)); }
#endif
