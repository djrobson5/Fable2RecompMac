// gpu/vfetch_perm.h
//
// Fable 2: the component permutation of vertex inputs (header only, no renderer dependencies: the offline
// check in the T89 notes builds it alone).
//
// The game's bind-time rewrite of a vertex shader (sub_821D3570) composes the declaration type's own swizzle
// into every vfetch destination swizzle: the four 3-bit selects at type bits 10..21 (x lowest; 0..3 a
// component, 4 zero, 5 one), and for a type with the 8-in-16 endian field (bits 6..7 = 1) the low bit of every
// select that names a component is flipped (the half swap of 16-bit data). An instruction select s <= 3 then
// reads select[s] of the fetched value; constants stay. A usage the declaration lacks gets (0, 0, 0, 1).
// Translating the unrewritten bank code instead, the renderer supplies this as data: one code per input in
// the shared constants (XenosRecomp vfetchPerm), stored xor kVfetchPermIdentity so that zero means none.

#pragma once

#include <cstdint>

namespace eot::gpu {

constexpr std::uint32_t kVfetchPermIdentity = 0x688;  // x, y, z, w: 3-bit selects, output x lowest
constexpr std::uint32_t kMissingInputPermutation = 4 | (4 << 3) | (4 << 6) | (5 << 9);
constexpr std::uint32_t kVfetchPermSlots = 48;

inline std::uint32_t DeclTypePermutation(std::uint32_t type) {
  std::uint32_t selects[4];
  for (std::uint32_t j = 0; j < 4; ++j) {
    selects[j] = (type >> (10 + 3 * j)) & 7;
    if ((type & 0xC0) == 0x40 && selects[j] <= 3)
      selects[j] ^= 1;
  }
  return selects[0] | (selects[1] << 3) | (selects[2] << 6) | (selects[3] << 9);
}

// The entry of an input (D3DDECLUSAGE, usage index) in the table, -1 when it has none. XenosRecomp's
// vfetchPermSlot (shader_recompiler.cpp) is the same table.
inline int VfetchPermSlot(std::uint32_t usage, std::uint32_t index) {
  switch (usage) {
  case 0: return index < 8 ? static_cast<int>(index) : -1;           // Position
  case 1: return index < 2 ? 8 + static_cast<int>(index) : -1;       // BlendWeight
  case 2: return index < 2 ? 10 + static_cast<int>(index) : -1;      // BlendIndices
  case 3: return index < 2 ? 12 + static_cast<int>(index) : -1;      // Normal
  case 6: return index < 2 ? 14 + static_cast<int>(index) : -1;      // Tangent
  case 7: return index < 2 ? 16 + static_cast<int>(index) : -1;      // Binormal
  case 10: return index < 4 ? 18 + static_cast<int>(index) : -1;     // Color
  case 5: return index < 16 ? 22 + static_cast<int>(index) : -1;     // TexCoord
  case 4: return index == 0 ? 38 : -1;                               // PointSize
  case 8: return index == 0 ? 39 : -1;                               // TessFactor
  case 9: return index == 0 ? 40 : -1;                               // PositionT
  case 11: return index == 0 ? 41 : -1;                              // Fog
  case 12: return index == 0 ? 42 : -1;                              // Depth
  case 13: return index == 0 ? 43 : -1;                              // Sample
  default: return -1;
  }
}

}  // namespace eot::gpu
