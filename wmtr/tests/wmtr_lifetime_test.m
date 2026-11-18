/* Real-object regression for cross-context pools and pre-dispatch handle validation.
 * Unlike the marshalling loopback's fake integer handles, provenance checks are enabled here. */
#define WMTR_TESTING 1
#include "../src/wmtr_host.m"
static int alive, dispatched, failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
@interface LifetimeProbe : NSObject
@end
@implementation LifetimeProbe
- (instancetype)init { if ((self = [super init])) alive++; return self; }
- (void)dealloc { alive--; [super dealloc]; }
@end
static NTSTATUS make_probe(void *p) { ((struct unixcall_generic_obj_ret *)p)->ret = (uint64_t)[[[LifetimeProbe alloc] init] autorelease]; return 0; }
static NTSTATUS make_owned(void *p) { ((struct unixcall_generic_obj_ret *)p)->ret = (uint64_t)[[LifetimeProbe alloc] init]; return 0; }
static NTSTATUS query(void *p) { dispatched++; ((struct unixcall_generic_obj_uint64_ret *)p)->ret = 42; return 0; }
static NTSTATUS retain_object(void *p) { [(id)*(uint64_t *)p retain]; return 0; }
static NTSTATUS release_object(void *p) { [(id)*(uint64_t *)p release]; return 0; }
static NTSTATUS noop(void *p) { dispatched++; return 0; }
static NTSTATUS command_buffer(void *p) {
  ((struct unixcall_generic_obj_obj_ret *)p)->ret = (uint64_t)[(id<MTLCommandQueue>)((struct unixcall_generic_obj_obj_ret *)p)->handle commandBuffer]; return 0;
}
static NTSTATUS queue(void *p) { ((struct unixcall_generic_obj_ret *)p)->ret = (uint64_t)[[MTLCreateSystemDefaultDevice() newCommandQueue] autorelease]; return 0; }
static NTSTATUS blit_encoder(void *p) {
  dispatched++;
  struct unixcall_generic_obj_obj_ret *args = p;
  args->ret = (uint64_t)[(id<MTLCommandBuffer>)args->handle blitCommandEncoder];
  return 0;
}
static NTSTATUS bad_string(void *p) { char buffer[8]; [(id)((struct unixcall_nsstring_getcstring *)p)->str getCString:buffer maxLength:sizeof buffer encoding:NSUTF8StringEncoding]; return 0; }
#ifdef WMTR_MULLION
/* newMTL4CommandQueue: a +1 object in the uint64 ret of a generic struct */
static NTSTATUS make_owned_u64(void *p) { ((struct unixcall_generic_obj_uint64_ret *)p)->ret = (uint64_t)[[LifetimeProbe alloc] init]; return 0; }
#endif
const void *wmtr_impl_funcs[WMTR_NCALLS] = {[0]=retain_object, [1]=release_object, [4]=make_owned, [5]=query, [53]=make_probe, [62]=queue, [11]=command_buffer, [30]=blit_encoder, [38]=noop, [8]=bad_string, [92]=query,
#ifdef WMTR_MULLION
  [154]=make_owned_u64, [158]=noop, [161]=noop, [162]=noop, [163]=noop, [168]=noop,
  [180]=noop, [185]=noop, [193]=noop, [196]=noop,
#endif
};
uint64_t wmtr_test_present_create_view(void *c, uint64_t hwnd, void *dev, uint64_t *layer) { return 0; }
void wmtr_test_present_release_view(void *c, uint64_t v) {}
static void *blob(void *ctx, uint32_t n, uint64_t *size) { *size = WMTR_RING_BLOB_BYTES; return n ? NULL : ctx; }
static void wake(void *ctx, uint32_t n) {}
static const struct wmtr_host_ops ops = {blob, wake};
static struct wmtr_host *new_host(void) {
  struct wmtr_ring *ring = calloc(1, WMTR_RING_BLOB_BYTES);
  ring->magic = WMTR_RING_MAGIC; ring->version = WMTR_VERSION;
  return wmtr_host_create(ring, &ops, ring);
}
static void destroy_host(struct wmtr_host *h) { void *ring = h->ring; wmtr_host_destroy(h); free(ring); }
static NTSTATUS call(struct wmtr_host *h, uint16_t n, void *params, uint32_t size) {
  uint8_t record[8192] = {0};
  struct wmtr_req_ext *r = (void *)record;
  r->h.call=n; r->h.flags=WMTR_F_SYNC; r->h.slot=0; r->h.params_size=size;
  r->payload_size=size; uint32_t off=wmtr_req_payload_offset(r); r->h.size=off+size;
  memcpy(record+off, params, size); execute(h, record, off+size);
  memcpy(params, h->slots[0].data, size); return h->slots[0].status;
}
int main(void) { @autoreleasepool {
  setenv("WMTR_TEST_STRICT_OBJECTS", "1", 1);
  struct wmtr_host *a=new_host(), *b=new_host(); CHECK(a && b);
  struct unixcall_generic_obj_uint64_ret color={WMTColorSpaceHDR10,0};
  CHECK(call(a,92,&color,sizeof color)==0 && color.ret==42); /* enum, not an object handle */
  struct unixcall_generic_obj_ret pa={0}, pb={0}, oa={0}, ob={0};
  CHECK(call(a,10,&pa,sizeof pa)==0); CHECK(call(a,53,&oa,sizeof oa)==0);
  CHECK(call(b,10,&pb,sizeof pb)==0); CHECK(call(b,53,&ob,sizeof ob)==0); CHECK(alive==2);
  uint64_t pool=pa.ret; CHECK(call(a,1,&pool,8)==0);
  CHECK(alive==1); /* old globals drain b's slot-0 pool as well */
  struct unixcall_generic_obj_uint64_ret q={ob.ret,0}; CHECK(call(b,5,&q,sizeof q)==0 && q.ret==42);
  q.handle=oa.ret; CHECK(call(a,5,&q,sizeof q)==STATUS_INVALID_HANDLE); /* freed with a's pool */
  q.handle=0x365a41f3321aa8ull; int before=dispatched;
  CHECK(call(a,5,&q,sizeof q)==STATUS_INVALID_HANDLE && dispatched==before); /* garbage (STEAMAC-5W) */
  /* A live object the registry does not know (another context's, or made by a call the metadata does
   * not cover, like DispatchData_alloc_init) goes through: refusing it gave DXMT nil libraries. */
  q.handle=ob.ret; before=dispatched; CHECK(call(a,5,&q,sizeof q)==0 && q.ret==42 && dispatched==before+1);
  LifetimeProbe *uncovered=[[LifetimeProbe alloc] init];
  q.handle=(uint64_t)uncovered; CHECK(call(a,5,&q,sizeof q)==0 && q.ret==42);
  [uncovered release];
  pool=pb.ret; CHECK(call(b,1,&pool,8)==0); CHECK(alive==0);
  /* Deterministic worker-versus-pool-drain interleaving: the call's lease survives its owner. */
  CHECK(call(b,10,&pb,sizeof pb)==0); CHECK(call(b,53,&ob,sizeof ob)==0);
  uint64_t borrowed=ob.ret; CHECK(call(b,1,&borrowed,8)==STATUS_INVALID_HANDLE && alive==1);
  struct unixcall_generic_obj_uint64_ret leased_query={ob.ret,0};
  struct object_walk lease={b,(uint8_t *)&leased_query,sizeof leased_query,OBJECT_LEASE,5};
  pthread_mutex_lock(&b->object_lock);
  CHECK(objects_walk(&lease,(void *)&leased_query,0));
  pthread_mutex_unlock(&b->object_lock);
  pool=pb.ret; CHECK(call(b,1,&pool,8)==0); CHECK(alive==1);
  lease.mode=OBJECT_UNLEASE; CHECK(objects_walk(&lease,(void *)&leased_query,0)); CHECK(alive==0);
  struct unixcall_generic_obj_ret owned={0}; CHECK(call(a,4,&owned,sizeof owned)==0);
  uint64_t handle=owned.ret; CHECK(call(a,0,&handle,8)==0); CHECK(call(a,1,&handle,8)==0); CHECK(alive==1);
  CHECK(call(a,1,&handle,8)==0); CHECK(alive==0); before=dispatched;
  CHECK(call(a,1,&handle,8)==STATUS_INVALID_HANDLE && dispatched==before);
  /* Embedded command resources are validated, not merely the receiver. */
  CHECK(call(a,4,&owned,sizeof owned)==0);
  struct { struct unixcall_generic_obj_cmd_noret p; struct wmtcmd_render_settexture c; } cmd={0};
  cmd.p.encoder=owned.ret; cmd.p.cmd_head.ptr=&cmd.c; cmd.c.type=WMTRenderCommandSetFragmentTexture; cmd.c.texture=0xdeadbeef;
  struct object_walk walk={a,(uint8_t *)&cmd,sizeof cmd,OBJECT_VALIDATE,38};
  CHECK(!objects_walk(&walk,(void *)&cmd,0));
  /* DXMT heads every blit, compute and render list with a Nop (dxmt_context.cpp). 1.9.5 refused
   * every blit and compute list: Death's Door drew a black frame with magenta menu arrows. */
  for (uint16_t kind = 36; kind <= 38; kind++) {
    struct { struct unixcall_generic_obj_cmd_noret p; struct wmtcmd_base head; } list={0};
    list.p.encoder=owned.ret; list.p.cmd_head.ptr=&list.head;
    struct object_walk nop={a,(uint8_t *)&list,sizeof list,OBJECT_VALIDATE,kind};
    CHECK(objects_walk(&nop,(void *)&list,0));
  }
  handle=owned.ret; CHECK(call(a,1,&handle,8)==0);
  /* A known but wrong object type is caught at the Objective-C boundary, not a VM abort. */
  CHECK(call(a,4,&owned,sizeof owned)==0);
  struct unixcall_nsstring_getcstring s={.str=owned.ret};
  CHECK(call(a,8,&s,sizeof s)==STATUS_INVALID_PARAMETER && a->faulted);
#ifdef WMTR_MULLION
  { /* Mullion's own calls: objects behind pointers are checked like DXMT's, pointer fields must be marshalled, and
       calls that write host files are refused */
    struct wmtr_host *m=new_host();
    struct unixcall_generic_obj_ret known={0}; CHECK(call(m,4,&known,sizeof known)==0);
    /* the queue newMTL4CommandQueue returns in a uint64 ret is registered */
    struct unixcall_generic_obj_uint64_ret mq={known.ret,0}; CHECK(call(m,154,&mq,sizeof mq)==0 && mq.ret);
    CHECK(object_find(m,mq.ret,0) && object_find(m,mq.ret,0)->refs);
    /* an acceleration-structure build's geometries (161) and a descriptor's (158) */
    struct WMTAccelerationStructureGeometry geo[2]; memset(geo,0,sizeof geo); geo[0].buffer=known.ret; geo[1].index_buffer=0xdeadbeef;
    struct { struct unixcall_generic_obj_cmd_noret p; struct wmtcmd_accelerationstructure_build c; } as={0};
    as.p.encoder=known.ret; as.p.cmd_head.ptr=&as.c; as.c.type=WMTAccelerationStructureCommandBuild; as.c.dst=known.ret;
    as.c.info.geometries.ptr=geo; as.c.info.geometry_count=2;
    struct object_walk aw={m,(uint8_t *)&as,sizeof as,OBJECT_VALIDATE,161};
    CHECK(!objects_walk(&aw,(void *)&as,0)); /* not in the payload */
    struct { struct unixcall_generic_obj_cmd_noret p; struct wmtcmd_accelerationstructure_build c; struct WMTAccelerationStructureGeometry g[2]; } as2={0};
    as2.p=as.p; as2.p.cmd_head.ptr=&as2.c; as2.c=as.c; as2.c.info.geometries.ptr=as2.g; memcpy(as2.g,geo,sizeof geo);
    struct object_walk aw2={m,(uint8_t *)&as2,sizeof as2,OBJECT_VALIDATE,161};
    CHECK(!objects_walk(&aw2,(void *)&as2,0)); /* garbage index buffer of geometry 1 */
    as2.g[1].index_buffer=known.ret; CHECK(objects_walk(&aw2,(void *)&as2,0));
    struct { struct unixcall_mtldevice_accelerationstructuresizes p; struct WMTAccelerationStructureInfo i; struct WMTAccelerationStructureGeometry g[1]; } sz={0};
    sz.p.device=known.ret; sz.p.info.ptr=&sz.i; sz.i.geometries.ptr=sz.g; sz.i.geometry_count=1; sz.g[0].transform_buffer=0xdeadbeef;
    struct object_walk sw={m,(uint8_t *)&sz,sizeof sz,OBJECT_VALIDATE,158};
    CHECK(!objects_walk(&sw,(void *)&sz,0)); sz.g[0].transform_buffer=known.ret; CHECK(objects_walk(&sw,(void *)&sz,0));
    /* function arrays of linked pipelines (162) and visible function tables (163) */
    struct { struct unixcall_mtldevice_newlinkedcomputepso p; struct WMTLinkedComputePipelineInfo i; obj_handle_t f[2]; } lk={0};
    lk.p.device=known.ret; lk.p.info.ptr=&lk.i; lk.i.compute_function=known.ret; lk.i.functions.ptr=lk.f; lk.i.function_count=2;
    lk.f[0]=known.ret; lk.f[1]=0xdeadbeef;
    struct object_walk lw={m,(uint8_t *)&lk,sizeof lk,OBJECT_VALIDATE,162};
    CHECK(!objects_walk(&lw,(void *)&lk,0)); lk.f[1]=known.ret; CHECK(objects_walk(&lw,(void *)&lk,0));
    struct { struct unixcall_mtlcomputepso_newvisiblefunctiontable p; obj_handle_t f[1]; } vt={0};
    vt.p.pso=known.ret; vt.p.functions.ptr=vt.f; vt.p.function_count=1; vt.f[0]=0xdeadbeef;
    struct object_walk vw={m,(uint8_t *)&vt,sizeof vt,OBJECT_VALIDATE,163};
    CHECK(!objects_walk(&vw,(void *)&vt,0)); vt.f[0]=known.ret; CHECK(objects_walk(&vw,(void *)&vt,0));
    /* a pointer field the client did not marshal is a guest address: refused before the implementation runs */
    struct unixcall_mtlcomputepso_newvisiblefunctiontable raw={known.ret,{(void *)0x10000},1,0}; before=dispatched;
    CHECK(call(m,163,&raw,sizeof raw)==STATUS_INVALID_PARAMETER && dispatched==before);
    raw.functions.ptr=NULL; raw.function_count=0; CHECK(call(m,163,&raw,sizeof raw)==0 && dispatched==before+1);
    /* host files under guest paths (WMTDiagWrite) */
    struct unixcall_wmtdiag_write dw; memset(&dw,0,sizeof dw); before=dispatched;
    CHECK(call(m,168,&dw,sizeof dw)==STATUS_NOT_SUPPORTED && dispatched==before);
    /* v0.5: flight records and traces (host files), bootstrap names */
    struct unixcall_wmtflight_write fw; memset(&fw,0,sizeof fw); before=dispatched;
    CHECK(call(m,180,&fw,sizeof fw)==STATUS_NOT_SUPPORTED && dispatched==before);
    struct unixcall_wmttrace_write tw; memset(&tw,0,sizeof tw);
    CHECK(call(m,193,&tw,sizeof tw)==STATUS_NOT_SUPPORTED && dispatched==before);
    struct unixcall_bootstrap bk; memset(&bk,0,sizeof bk); bk.object=known.ret;
    CHECK(call(m,196,&bk,sizeof bk)==STATUS_NOT_SUPPORTED && dispatched==before);
    /* a Metal 4 command buffer's commit takes the receiver alone: a garbage one never runs */
    uint64_t cb4=0xdeadbeef; CHECK(call(m,185,&cb4,8)==STATUS_INVALID_HANDLE && dispatched==before);
    cb4=known.ret; CHECK(call(m,185,&cb4,8)==0 && dispatched==before+1);
    /* a Metal 4 encoder's command list is walked as the stream it names (here: render) */
    struct { struct unixcall_metal4_commands p; struct wmtcmd_render_settexture c; } l4={0};
    l4.p.encoder=known.ret; l4.p.head.ptr=&l4.c; l4.p.kind=WMTMetal4Render;
    l4.c.type=WMTRenderCommandSetFragmentTexture; l4.c.texture=0xdeadbeef;
    struct object_walk lw4={m,(uint8_t *)&l4,sizeof l4,OBJECT_VALIDATE,183};
    CHECK(!objects_walk(&lw4,(void *)&l4,0)); l4.c.texture=known.ret; CHECK(objects_walk(&lw4,(void *)&l4,0));
    l4.p.kind=7; CHECK(!objects_walk(&lw4,(void *)&l4,0));
    /* the denoised scaler's textures */
    struct { struct unixcall_mtlcommandbuffer_denoised_scale p; struct WMTFXDenoisedScaleArgs a; } dn={0};
    dn.p.cmdbuf=known.ret; dn.p.scaler=known.ret; dn.p.args.ptr=&dn.a; dn.a.color=known.ret; dn.a.output=0xdeadbeef;
    struct object_walk dw4={m,(uint8_t *)&dn,sizeof dn,OBJECT_VALIDATE,204};
    CHECK(!objects_walk(&dw4,(void *)&dn,0)); dn.a.output=known.ret; CHECK(objects_walk(&dw4,(void *)&dn,0));
    uint64_t q4=mq.ret; CHECK(call(m,1,&q4,8)==0); q4=known.ret; CHECK(call(m,1,&q4,8)==0);
    destroy_host(m);
  }
#endif
  destroy_host(a); destroy_host(b);
  CHECK(alive==0); /* context loss also releases caller-owned objects */
  /* A committed real Metal buffer must not reach encoder creation (Metal would abort). */
  a=new_host(); struct unixcall_generic_obj_ret qr={0}; CHECK(call(a,62,&qr,sizeof qr)==0);
  struct unixcall_generic_obj_obj_ret cb={qr.ret,0}; CHECK(call(a,11,&cb,sizeof cb)==0);
  [(id<MTLCommandBuffer>)cb.ret commit]; [(id<MTLCommandBuffer>)cb.ret waitUntilCompleted];
  struct unixcall_generic_obj_obj_ret enc={cb.ret,0}; before=dispatched;
  CHECK(call(a,30,&enc,sizeof enc)==STATUS_INVALID_PARAMETER && enc.ret==0 && dispatched==before && a->faulted);
  destroy_host(a);
  /* 1.9.8 silently stopped registering objects at 65536 live handles. An encoder created after
   * that limit still entered the virtual pool, but teardown could not find and end it. Use real
   * objects and a real Metal encoder: before the fix destroy aborts in vpool_pop with
   * "Command encoder released without endEncoding", exactly STEAMAC-6Z/70. */
  a=new_host();
  CHECK(call(a,10,&pa,sizeof pa)==0);
  for (uint32_t i=0; i<WMTR_HANDLE_SLOTS; i++) CHECK(call(a,4,&owned,sizeof owned)==0);
  CHECK(alive==WMTR_HANDLE_SLOTS);
  qr=(struct unixcall_generic_obj_ret){0}; CHECK(call(a,62,&qr,sizeof qr)==0);
  cb=(struct unixcall_generic_obj_obj_ret){qr.ret,0}; CHECK(call(a,11,&cb,sizeof cb)==0);
  enc=(struct unixcall_generic_obj_obj_ret){cb.ret,0}; CHECK(call(a,30,&enc,sizeof enc)==0 && enc.ret);
  destroy_host(a);
  CHECK(alive==0);
  printf("wmtr_lifetime_test: %s\n", failures ? "FAILED" : "all passed");
  return failures != 0;
} }
