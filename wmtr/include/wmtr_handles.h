/* wmtr object handles: what the host executor accepts where a winemetal call carries a host object.
 *
 * The guest never sees a host pointer. Every object the executor hands out (a call's result, an object written
 * into an output struct or array) enters the executor context's handle table and the guest gets
 *
 *   WMTR_OBJ_TAG | generation << 24 | index        (bits 63..48 "WO", 24-bit generation, 24-bit index)
 *
 * Every handle field of a request (src/wmtr_handles.inc, generated from DXMT's headers by gen/gen_handles.py)
 * is looked up before the call: the index must name a live entry of the same generation whose kind is one the
 * field accepts, else the call is refused (STATUS_INVALID_HANDLE) and nothing is executed. The lookup is an
 * array index and one compare of a 64-bit key; the generation changes when an entry is freed, so an old handle
 * of a reused slot is refused too. An entry is freed when its Objective-C object is deallocated (an associated
 * watcher object), or by the call that destroys a non-Objective-C object (SM50 shaders, bitcode, errors, views,
 * virtual autorelease pools). Releases are counted per entry: the guest cannot release more references than
 * calls gave it, so it cannot free an object behind another user's back.
 *
 * Pointer fields are checked as well: a pointer field is NULL or one the request relocated (WMTR_REF_*), never a
 * value the guest wrote, and structs that carry handles (render pass, pipeline infos, command lists, handle
 * arrays) must lie in the request's private copy, not in shared arena memory the guest could change during the
 * call. */
#ifndef WMTR_HANDLES_H
#define WMTR_HANDLES_H
#include <stdint.h>

#define WMTR_OBJ_TAG 0x574F000000000000ull /* "WO" */
#define WMTR_OBJ_INDEX_BITS 24
#define WMTR_OBJ_MAX (1u << WMTR_OBJ_INDEX_BITS)

/* entry kinds; Objective-C kinds first (WMTR_M_OBJC) */
enum wmtr_kind {
  WMTR_K_NONE,
  WMTR_K_OBJC, /* any other Objective-C object (strings for HUD labels, log containers, caches, ...) */
  WMTR_K_DEVICE, WMTR_K_QUEUE, WMTR_K_CMDBUF, WMTR_K_BUFFER, WMTR_K_TEXTURE, WMTR_K_SAMPLER, WMTR_K_DSSO,
  WMTR_K_LIBRARY, WMTR_K_FUNCTION, WMTR_K_CPSO, WMTR_K_RPSO, WMTR_K_ENC_BLIT, WMTR_K_ENC_COMPUTE,
  WMTR_K_ENC_RENDER, WMTR_K_SHARED_EVENT, WMTR_K_EVENT, WMTR_K_FENCE, WMTR_K_STRING, WMTR_K_ARRAY, WMTR_K_ERROR,
  WMTR_K_DATA, WMTR_K_ARCHIVE, WMTR_K_HEAP, WMTR_K_ICB, WMTR_K_COUNTERBUF, WMTR_K_RESSET, WMTR_K_FXT, WMTR_K_FXS,
  WMTR_K_LAYER, WMTR_K_DRAWABLE, WMTR_K_CAPTURE,
  WMTR_K_LAST_OBJC = WMTR_K_CAPTURE,
  /* host-made, not Objective-C: freed by the call that destroys them */
  WMTR_K_VIEW, WMTR_K_POOL, WMTR_K_LISTENER, WMTR_K_SM50_SHADER, WMTR_K_SM50_BITCODE, WMTR_K_SM50_ERROR,
  WMTR_K_BITCODE_DATA, /* SM50_COMPILED_BITCODE.Data: host bytes, only usable as DispatchData_alloc_init's source */
  WMTR_K_COUNT
};
_Static_assert(WMTR_K_COUNT <= 64, "kind masks are 64 bits");

#define WMTR_KM(k) (1ull << WMTR_K_##k)
#define WMTR_M_OBJC (((2ull << WMTR_K_LAST_OBJC) - 1) & ~1ull)
#define WMTR_M_DEVICE WMTR_KM(DEVICE)
#define WMTR_M_QUEUE WMTR_KM(QUEUE)
#define WMTR_M_CMDBUF WMTR_KM(CMDBUF)
#define WMTR_M_BUFFER WMTR_KM(BUFFER)
#define WMTR_M_TEXTURE WMTR_KM(TEXTURE)
#define WMTR_M_SAMPLER WMTR_KM(SAMPLER)
#define WMTR_M_DSSO WMTR_KM(DSSO)
#define WMTR_M_LIBRARY WMTR_KM(LIBRARY)
#define WMTR_M_FUNCTION WMTR_KM(FUNCTION)
#define WMTR_M_CPSO WMTR_KM(CPSO)
#define WMTR_M_RPSO WMTR_KM(RPSO)
#define WMTR_M_ENC_BLIT WMTR_KM(ENC_BLIT)
#define WMTR_M_ENC_COMPUTE WMTR_KM(ENC_COMPUTE)
#define WMTR_M_ENC_RENDER WMTR_KM(ENC_RENDER)
#define WMTR_M_ENCODER (WMTR_M_ENC_BLIT | WMTR_M_ENC_COMPUTE | WMTR_M_ENC_RENDER)
#define WMTR_M_SHARED_EVENT WMTR_KM(SHARED_EVENT)
#define WMTR_M_EVENT WMTR_KM(EVENT)
#define WMTR_M_FENCE WMTR_KM(FENCE)
#define WMTR_M_STRING WMTR_KM(STRING)
#define WMTR_M_ARRAY WMTR_KM(ARRAY)
#define WMTR_M_ERROR WMTR_KM(ERROR)
#define WMTR_M_DATA WMTR_KM(DATA)
#define WMTR_M_ARCHIVE WMTR_KM(ARCHIVE)
#define WMTR_M_HEAP WMTR_KM(HEAP)
#define WMTR_M_ICB WMTR_KM(ICB)
#define WMTR_M_COUNTERBUF WMTR_KM(COUNTERBUF)
#define WMTR_M_RESSET WMTR_KM(RESSET)
#define WMTR_M_FXT WMTR_KM(FXT)
#define WMTR_M_FXS WMTR_KM(FXS)
#define WMTR_M_FX (WMTR_M_FXT | WMTR_M_FXS)
#define WMTR_M_LAYER WMTR_KM(LAYER)
#define WMTR_M_DRAWABLE WMTR_KM(DRAWABLE)
#define WMTR_M_CAPTURE WMTR_KM(CAPTURE)
#define WMTR_M_RESOURCE (WMTR_M_BUFFER | WMTR_M_TEXTURE | WMTR_M_HEAP | WMTR_M_ICB)
#define WMTR_M_VIEW WMTR_KM(VIEW)
#define WMTR_M_POOL WMTR_KM(POOL)
#define WMTR_M_LISTENER WMTR_KM(LISTENER)
#define WMTR_M_SM50_SHADER WMTR_KM(SM50_SHADER)
#define WMTR_M_SM50_BITCODE WMTR_KM(SM50_BITCODE)
#define WMTR_M_SM50_ERROR WMTR_KM(SM50_ERROR)
#define WMTR_M_BITCODE_DATA WMTR_KM(BITCODE_DATA)

/* generated field lists (src/wmtr_handles.inc) */
enum {
  WMTR_HF_END,
  WMTR_HF_IN,   /* a handle the call reads: translated, kind-checked */
  WMTR_HF_OUT0, /* a result the guest does not own (+0): enters the table */
  WMTR_HF_OUT1, /* a result the guest owns (+1): enters the table with one guest reference */
  WMTR_HF_PTR,  /* NULL or relocated; `sub` describes what it points to (if it carries handles or pointers) */
};
struct wmtr_hptee;
struct wmtr_hfield {
  uint32_t off;
  uint8_t flags;
  uint64_t mask;
  const struct wmtr_hptee *sub;
};
enum {
  WMTR_HP_STRUCT,       /* `count` structs (`fields`, `elem_size` each) */
  WMTR_HP_HANDLES_IN,   /* `count` handles, kinds `mask` */
  WMTR_HP_HANDLES_OUT0, /* up to `count` results (+0) */
  WMTR_HP_HANDLE_OUT1,  /* one result (+1) of kind `mask` (SM50 out-params) */
  WMTR_HP_CMDLIST,      /* a winemetal command list (call 36/37/38) */
  WMTR_HP_SM50ARGS,     /* an SM50 compile-argument list */
};
struct wmtr_hptee {
  uint8_t type;
  const struct wmtr_hfield *fields;
  uint32_t elem_size;
  uint32_t count_off, count_bytes; /* count field in the struct holding the pointer; 0 bytes: one element */
  uint64_t mask;
};
struct wmtr_hcmd { uint8_t kind; uint16_t type; uint32_t size; const struct wmtr_hfield *fields; };

#endif
