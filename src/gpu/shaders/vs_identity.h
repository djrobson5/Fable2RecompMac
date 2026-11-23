// gpu/shaders/vs_identity.h
//
// Fable 2: the game rewrites a vertex shader object in place when it binds a
// declaration (sub_82BA40F0 and the device's patch, both through
// sub_821D3570): vfetch instructions from the declaration, exports and
// interpolants from the pixel shader. Every (shader, declaration, pixel shader)
// then hashes to a shader of its own although the bank holds one original.
// vs_identity.cpp remembers, per object, the container as the bank loader made
// it (the pristine one) and the hashes of the rewritten forms, so the
// renderer can translate and key the original once.

#pragma once

#include <memory>
#include <vector>

#include <rex/types.h>

#include "gpu/d3d.h"

namespace eot::gpu {

// A vertex shader object's container: virtual part ++ physical part.
struct VsContainer {
  std::vector<u8> bytes;
  u64 hash = 0;  // XXH3-64 of bytes, as RegisterGuestShader hashes it
  u32 virtualSize = 0, physicalSize = 0;
  u32 flags = 0;
  bool ok = false;
};
VsContainer ReadVsContainer(u32 object_va, bool is_pixel);

// The declaration the game rewrote an object with (guest form, as it holds it) and its per-stream strides in
// dwords: what that rewritten form's microcode was bound to, for draws that bind no declaration. One record per
// rewritten form (the same hash is the same binding).
struct VsRecordedDecl {
  std::vector<DeclElement> decl;
  u8 strides[16] = {};
};

struct VsPristine {
  u64 hash = 0;                                      // hash of the pristine container
  std::shared_ptr<const std::vector<u8>> bytes;      // virtual ++ physical
  u32 virtualSize = 0, physicalSize = 0;
  std::shared_ptr<const VsRecordedDecl> rec;         // of the rewritten form classified (VsClassify)
};

// fable2_vs_pristine: the switch (default on). Off: every function below says "not known".
bool VsPristineEnabled();

// What the hook knows about an object whose container hashes to live_hash.
enum class VsRecord : u8 {
  None,       // no record (never rewritten as far as the hook saw)
  Pristine,   // the container is the pristine one
  Rewritten,  // the container is one of the forms the rewrite produced for it
  Unknown,    // a record exists but this hash is neither (the memory holds another shader, a rewrite
              // the hook did not see, or more rewritten forms than it keeps)
};
// Classifies the object's current container and, for Rewritten, fills *out with the pristine one and the
// declaration recorded for that rewritten form. Only a remembered rewritten form is ever normalised: freed shader
// memory the game reuses holds other shaders.
VsRecord VsClassify(u32 object_va, u64 live_hash, VsPristine *out);

// True when hash is the hash of a vertex shader container of the shader bank (a generated table,
// tools/mac/gen_bank_vs_hashes.py): only such a container is ever taken for a pristine original.
bool IsBankVsHash(u64 hash);

// The pipeline precache and predictor rebuild layouts from recorded rows (a vertex shader hash and a raw
// declaration): with the switch on, a bank hash is the key of a normalised shader (the rewritten objects it
// stands for), so they build the layout the live draw does (the layout key carries it).
bool VsHashKeyedPristine(u64 vs_hash);

// The bind hook (vs_rewrite_log.cpp) around sub_82BA40F0: before the original
// runs, and after it returned with the container hashed again.
void VsIdentityBeforeBind(u32 object_va, const VsContainer &before);
void VsIdentityAfterBind(u32 object_va, const VsContainer &after);
// The bind hook brackets the original with these (this thread is inside a bind: sub_821D3570 only
// records the declaration).
void VsIdentityEnterBind();
void VsIdentityLeaveBind();

}  // namespace eot::gpu
