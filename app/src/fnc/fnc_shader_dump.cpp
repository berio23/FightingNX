// Stage 2: FNC's shader container dump (cvar fnc_dump_shaders), for Renut's translator.
//
// FNC's executable holds no shader container (tools/sa2_dump_shaders.py over its image found 0): every shader comes
// from the game's data files (the .fxo files in its .big archives). So the containers are taken where the game creates
// them: D3DDevice_CreateVertexShader (sub_82CE8360) and D3DDevice_CreatePixelShader (sub_82CE8178) get the container
// in r3 (docs/fnc/stage2.md, "Shaders"). A container is its virtual part then its physical part (sizes at +4 / +8, the
// physical part right after the virtual one, as both functions read it), which is the layout the dump files have.
//
// Output, next to the executable: <fnc_dump_shaders_dir>/{vs,ps}_<xxh3_64 of the container>.bin and manifest.csv, the
// naming of reNut's tools/dump_shaders.py and sa2-nx's tools/sa2_dump_shaders.py. The files are game data: they stay in
// the build folder and are never committed. Only the scenes that are played are dumped.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DEFINE_BOOL(fnc_dump_shaders, false, "FNC",
                    "Stage 2: write every shader container the game creates (CreateVertexShader / CreatePixelShader) "
                    "to fnc_dump_shaders_dir next to the executable, for Renut's translator");
REXCVAR_DEFINE_STRING(fnc_dump_shaders_dir, "shader_dump", "FNC",
                      "Folder (next to the executable) of the shader dump (fnc_dump_shaders)");

namespace {

std::mutex g_mutex;
std::unordered_set<uint64_t> g_seen;

uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

void Dump(const char* kind, uint8_t* base, uint32_t container) {
  static const bool on = REXCVAR_GET(fnc_dump_shaders);
  if (!on || !container) return;
  const uint8_t* p = base + container;
  const uint32_t virt = LoadBE32(p + 4), phys = LoadBE32(p + 8);
  // The size limits of the container check in tools/sa2_dump_shaders.py (reNut's).
  if ((LoadBE32(p) & 0xFFFFFF00u) != 0x102A1100u || virt < 0x24 || virt >= 0x10000 || phys == 0 ||
      phys >= 0x100000) {
    REXLOG_WARN("[fnc-shaders] {} container at {:08X} rejected (virtual {}, physical {})", kind, container, virt, phys);
    return;
  }
  const size_t size = size_t(virt) + phys;
  const uint64_t hash = XXH3_64bits(p, size);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_seen.insert(hash).second) return;
  const auto dir = rex::filesystem::GetExecutableFolder() / REXCVAR_GET(fnc_dump_shaders_dir);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string name = fmt::format("{}_{:016x}", kind, hash);
  const auto path = dir / (name + ".bin");
  if (!std::filesystem::exists(path, ec)) {
    if (std::FILE* f = std::fopen(path.string().c_str(), "wb")) {
      std::fwrite(p, 1, size, f);
      std::fclose(f);
    }
  }
  const auto manifest = dir / "manifest.csv";
  const bool fresh = !std::filesystem::exists(manifest, ec);
  if (std::FILE* f = std::fopen(manifest.string().c_str(), "ab")) {
    if (fresh) std::fputs("name,type,virtual_size,physical_size,source,asset\n", f);
    std::fputs(fmt::format("{},{},{},{},runtime,0x{:08x}\n", name, kind, virt, phys, container).c_str(), f);
    std::fclose(f);
  }
}

}  // namespace

// D3DDevice_CreateVertexShader(pFunction, ppShader): the container in r3.
extern "C" REX_FUNC(__imp__sub_82CE8360);
REX_HOOK_RAW(sub_82CE8360) {
  Dump("vs", base, ctx.r3.u32);
  __imp__sub_82CE8360(ctx, base);
}

// D3DDevice_CreatePixelShader(pFunction, ppShader): the container in r3.
extern "C" REX_FUNC(__imp__sub_82CE8178);
REX_HOOK_RAW(sub_82CE8178) {
  Dump("ps", base, ctx.r3.u32);
  __imp__sub_82CE8178(ctx, base);
}
