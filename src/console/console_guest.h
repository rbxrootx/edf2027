// EDF2027 console - guest memory and guest calls from host code on the engine thread.
//
// Guest memory is big-endian; addresses at or above 0xE0000000 carry the SDK's physical
// offset (edf2017_pch.h REX_PHYS_HOST_OFFSET). Pointers read from game structures are
// checked against the title heap before they are followed, so a stale or foreign pointer
// reads as "nothing there" instead of faulting.
//
// GuestCalls runs recompiled guest functions from inside a hook: it reserves scratch
// memory on the guest stack below the hooked frame's stack pointer (which the hooked
// function has not used yet), builds a fresh context per call as rex::CallFrame does
// (stack pointer, r13 and FPSCR from the parent), and returns r3.
#pragma once

#include <rex/ppc/context.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>

namespace edf::console {

class GuestMemory {
 public:
  explicit GuestMemory(uint8_t* base) : base_(base) {}
  uint8_t* Ptr(uint32_t address) const { return base_ + address + (address >= 0xE0000000u ? 0x1000u : 0u); }
  uint32_t U32(uint32_t a) const { uint32_t v; std::memcpy(&v, Ptr(a), 4); return std::byteswap(v); }
  uint16_t U16(uint32_t a) const { uint16_t v; std::memcpy(&v, Ptr(a), 2); return std::byteswap(v); }
  uint8_t U8(uint32_t a) const { return *Ptr(a); }
  float F32(uint32_t a) const { return std::bit_cast<float>(U32(a)); }
  void W32(uint32_t a, uint32_t v) const { v = std::byteswap(v); std::memcpy(Ptr(a), &v, 4); }
  void W16(uint32_t a, uint16_t v) const { v = std::byteswap(v); std::memcpy(Ptr(a), &v, 2); }
  void W8(uint32_t a, uint8_t v) const { *Ptr(a) = v; }
  void WF32(uint32_t a, float v) const { W32(a, std::bit_cast<uint32_t>(v)); }
  void Zero(uint32_t a, uint32_t size) const { std::memset(Ptr(a), 0, size); }

  // Game objects, list nodes and their buffers live in the title heap (0x40000000, 43 MB
  // in retail; allow its whole 256 MB window).
  static bool Heap(uint32_t a, uint32_t size = 4) {
    return a >= 0x40000000u && a < 0x50000000u && a + size <= 0x50000000u && (a & 3) == 0;
  }
  // Code, vtables, RTTI and globals: the executable image.
  static bool Image(uint32_t a, uint32_t size = 4) {
    return a >= 0x82000000u && uint64_t(a) + size <= 0x82600000u;
  }

  // The RTTI class name of an object ("clGiantAnt"), or "" when its vtable does not lead
  // to a well-formed MSVC type descriptor: vtable[-1] is the complete-object locator,
  // locator+12 the type descriptor, and descriptor+8 the decorated name ".?AVclGiantAnt@@".
  std::string ClassName(uint32_t object) const {
    if (!Heap(object)) return {};
    return ClassNameOfVtable(U32(object));
  }
  std::string ClassNameOfVtable(uint32_t vtable) const {
    if (!Image(vtable) || !Image(vtable - 4)) return {};
    const uint32_t locator = U32(vtable - 4);
    if (!Image(locator, 16)) return {};
    const uint32_t descriptor = U32(locator + 12);
    if (!Image(descriptor, 12)) return {};
    const uint32_t name_address = descriptor + 8;
    if (!Image(name_address, 4)) return {};
    const size_t name_bytes = (std::min)(size_t{96}, size_t(0x82600000u - name_address));
    const char* name = reinterpret_cast<const char*>(Ptr(name_address));
    if (std::strncmp(name, ".?AV", 4) != 0) return {};
    std::string out;
    for (size_t i = 4; i < name_bytes && name[i] && name[i] != '@'; ++i) out += name[i];
    return out;
  }

 private:
  uint8_t* base_;
};

class GuestCalls {
 public:
  // Scratch memory: `scratch_bytes` below the parent's stack pointer, after a gap the
  // parent's callee (the hooked function) would use for its register saves.
  GuestCalls(PPCContext& parent, uint8_t* base, uint32_t scratch_bytes = 0x2000)
      : parent_(parent), base_(base), memory_(base) {
    top_ = (parent.r1.u32 - 0x200u) & ~0xFu;
    bottom_ = top_ - scratch_bytes;
    next_ = top_;
    // Calls run below the scratch area, with room for their own frames.
    call_sp_ = (bottom_ - 0x100u) & ~0xFu;
  }
  const GuestMemory& memory() const { return memory_; }
  uint8_t* base() const { return base_; }

  // Zero-filled scratch memory, 16-byte aligned. Throws when the scratch area is used up.
  uint32_t Alloc(uint32_t size, uint32_t align = 16) {
    uint32_t address = (next_ - size) & ~(align - 1);
    if (address < bottom_) throw std::runtime_error("console guest scratch exhausted");
    next_ = address;
    memory_.Zero(address, size);
    return address;
  }
  // Scratch marks: release everything allocated after a mark (per-iteration reuse).
  uint32_t Mark() const { return next_; }
  void Release(uint32_t mark) { next_ = mark; }

  // A big-endian UTF-16 copy of an ASCII string, NUL terminated. Returns its address.
  uint32_t WideString(std::string_view text) {
    const uint32_t address = Alloc(uint32_t(text.size() + 1) * 2, 4);
    for (size_t i = 0; i < text.size(); ++i) memory_.W16(address + uint32_t(i) * 2, uint16_t(uint8_t(text[i])));
    return address;
  }

  struct Args {
    std::array<uint64_t, 8> r{};  // r3..r10
    std::array<double, 4> f{};    // f1..f4
    size_t nr = 0, nf = 0;
  };
  static Args R(std::initializer_list<uint32_t> ints, std::initializer_list<double> floats = {}) {
    Args args;
    for (uint32_t v : ints) if (args.nr < args.r.size()) args.r[args.nr++] = v;
    for (double v : floats) if (args.nf < args.f.size()) args.f[args.nf++] = v;
    return args;
  }
  // Calls a recompiled function. Returns r3 (and f1 through `f1` when asked).
  uint32_t Call(PPCFunc* function, const Args& args, double* f1 = nullptr) {
    PPCContext ctx{};
    ctx.r1.u64 = call_sp_;
    ctx.r13 = parent_.r13;
    ctx.fpscr = parent_.fpscr;
    PPCRegister* ints[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    for (size_t i = 0; i < args.nr; ++i) ints[i]->u64 = args.r[i];
    PPCRegister* floats[] = {&ctx.f1, &ctx.f2, &ctx.f3, &ctx.f4};
    for (size_t i = 0; i < args.nf; ++i) floats[i]->f64 = args.f[i];
    function(ctx, base_);
    parent_.fpscr = ctx.fpscr;
    if (f1) *f1 = ctx.f1.f64;
    return ctx.r3.u32;
  }
  // An indirect call through a guest address (a vtable slot).
  uint32_t CallAddress(uint32_t address, const Args& args) {
    PPCFunc* function = rex::runtime::ResolveIndirectFunction(address);
    if (!function) throw std::runtime_error("no recompiled function at a guest call target");
    return Call(function, args);
  }

 private:
  PPCContext& parent_;
  uint8_t* base_;
  GuestMemory memory_;
  uint32_t top_ = 0, bottom_ = 0, next_ = 0, call_sp_ = 0;
};

}  // namespace edf::console
