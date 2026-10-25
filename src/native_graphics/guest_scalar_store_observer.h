#pragma once
#include "edf2017_pch.h"
#include <cstdint>
#include <source_location>

// Compile-time diagnostic adapter, not a complete writer-ownership contract.
// Append after the generated PCH; never edit generated macros in place.
extern "C" void edf_native_observe_guest_scalar_store(uint8_t* base,uint32_t address,uint32_t bytes,const char* file,uint32_t line);
namespace edf::native {
inline void ObserveRawPhysicalStore(uint8_t* base,const void* pointer,uint32_t bytes,
    std::source_location site=std::source_location::current()) {
  if(!bytes) return;
  const auto origin=reinterpret_cast<uintptr_t>(base);
  const auto target=reinterpret_cast<uintptr_t>(pointer);
  if(target<origin) return;
  auto offset=target-origin;
  if(offset<0xa0000000ull || offset>=0x100001000ull) return;
  // Invert the generated Windows physical E-alias offset, not a host pointer
  // truncation. The gap before E0001000 is not a valid generated E-alias store.
#if defined(_WIN32)
  if(offset>=0xe0000000ull) {
    if(offset<0xe0001000ull) return;
    offset-=0x1000;
  }
#endif
  if(offset>UINT32_MAX) return;
  edf_native_observe_guest_scalar_store(base,static_cast<uint32_t>(offset),bytes,site.file_name(),site.line());
}
template<class T,class Old,class New>
inline bool ObservedAtomicStore(uint8_t* base,T* destination,Old expected,New desired,
    std::source_location site=std::source_location::current()) {
  static_assert(sizeof(T)==4 || sizeof(T)==8);
  const bool written=__sync_bool_compare_and_swap(destination,expected,desired);
  if(written) ObserveRawPhysicalStore(base,destination,sizeof(T),site);
  return written;
}
// Expand the original generated MMIO macros before replacing their call sites.
// In particular, preserve the 64-bit MMIO branch's two ordered CheckStore calls
// and the full 32-bit MMIO value for the byte/halfword helpers.
template<uint32_t Bytes,class T> inline void ObservedMmioStore(uint8_t* base,uint32_t address,T value,
    std::source_location site=std::source_location::current()) {
  if constexpr(Bytes==1) { REX_MM_STORE_U8(address,value); }
  else if constexpr(Bytes==2) { REX_MM_STORE_U16(address,value); }
  else if constexpr(Bytes==4) { REX_MM_STORE_U32(address,value); }
  else { static_assert(Bytes==8); REX_MM_STORE_U64(address,value); }
  // The MMIO address range is below physical aliases, hence is never notified.
  if(address>=0xa0000000u)
    edf_native_observe_guest_scalar_store(base,address,Bytes,site.file_name(),site.line());
}
template<class T> inline T ObservedScalarStore(uint8_t* base,uint32_t address,T encoded,
    std::source_location site=std::source_location::current()) {
  // Preserve the generated Windows E-alias host offset and volatile access.
  *reinterpret_cast<volatile T*>(base+address+REX_PHYS_HOST_OFFSET(address))=encoded;
  if((address>=0xa0000000u) || (address>=0x40000000u && address<0x50000000u))
    edf_native_observe_guest_scalar_store(base,address,sizeof(T),site.file_name(),site.line());
  return encoded;
}
}
#undef REX_STORE_U8
#undef REX_STORE_U16
#undef REX_STORE_U32
#undef REX_STORE_U64
#define REX_STORE_U8(x,y) edf::native::ObservedScalarStore<uint8_t>(base,uint32_t(x),uint8_t(y))
#define REX_STORE_U16(x,y) edf::native::ObservedScalarStore<uint16_t>(base,uint32_t(x),__builtin_bswap16(uint16_t(y)))
#define REX_STORE_U32(x,y) edf::native::ObservedScalarStore<uint32_t>(base,uint32_t(x),__builtin_bswap32(uint32_t(y)))
#define REX_STORE_U64(x,y) edf::native::ObservedScalarStore<uint64_t>(base,uint32_t(x),__builtin_bswap64(uint64_t(y)))
#undef REX_MM_STORE_U8
#undef REX_MM_STORE_U16
#undef REX_MM_STORE_U32
#undef REX_MM_STORE_U64
#define REX_MM_STORE_U8(x,y) edf::native::ObservedMmioStore<1>(base,uint32_t(x),(y))
#define REX_MM_STORE_U16(x,y) edf::native::ObservedMmioStore<2>(base,uint32_t(x),(y))
#define REX_MM_STORE_U32(x,y) edf::native::ObservedMmioStore<4>(base,uint32_t(x),(y))
#define REX_MM_STORE_U64(x,y) edf::native::ObservedMmioStore<8>(base,uint32_t(x),(y))
#define __sync_bool_compare_and_swap(p,o,n) edf::native::ObservedAtomicStore(base,(p),(o),(n))
