#!/usr/bin/env python3
"""Generate src/wmtr_handles.inc: where every winemetal call carries object handles and pointers, from DXMT's
headers (the struct layouts) plus the call table (src/wmtr_calls.inc) and command table (src/wmtr_cmds.inc).

The host executor uses it to translate every handle the guest sends through its per-context handle table and to
refuse raw values (wmtr_proto.h, "Object handles"). Field layouts come from clang's AST of the DXMT headers, so a
DXMT update that adds or moves a handle field changes this file; what a field *means* (which object type it holds,
which uint64 field is really a pointer, what a pointer points to) is the small rule set below.

Usage (reproduces src/wmtr_handles.inc byte-for-byte from a DXMT tree at the pinned revision):

  python3 gen/gen_handles.py <dxmt>/src src/wmtr_calls.inc src/wmtr_cmds.inc src/wmtr_handles.inc
"""
import json, os, re, subprocess, sys, tempfile

dxmt_src, calls_inc, cmds_inc, out_path = sys.argv[1:5]

# ---------------------------------------------------------------------------------------------------------------
# struct layouts: field names and types (offsets are left to the C compiler through offsetof)
with tempfile.TemporaryDirectory() as td:
    tu = os.path.join(td, 'tu.c')
    open(tu, 'w').write('#define WINEMETAL_API\n#include "winemetal_thunks.h"\n#include "airconv_thunks.h"\n')
    ast = json.loads(subprocess.run(['clang', '-x', 'objective-c', '-fsyntax-only', '-Xclang', '-ast-dump=json',
                                     '-I' + dxmt_src + '/winemetal', '-I' + dxmt_src + '/airconv', tu],
                                    check=True, capture_output=True, text=True).stdout)
recs = {}
for n in ast['inner']:
    if n.get('kind') == 'RecordDecl' and n.get('completeDefinition') and n.get('name'):
        recs[n['name']] = [(f.get('name', ''), f['type']['qualType']) for f in n.get('inner', []) if f.get('kind') == 'FieldDecl']

calls = []  # (index, name, struct or None)
for line in open(calls_inc):
    m = re.match(r'X\((\d+), (\w+), (?:struct (\w+)|(\w+)), (\w)\)', line)
    if m: calls.append((int(m.group(1)), m.group(2), m.group(3) or m.group(4)))
cmds = []  # (kind, enum, struct)
for line in open(cmds_inc):
    m = re.match(r'CMD\((\w+), (\w+), (\w+)\)', line)
    if m: cmds.append(m.groups())

# ---------------------------------------------------------------------------------------------------------------
# meaning rules
RECEIVER = [  # call-name prefix -> kinds of the first handle field
    ('NSObject_', 'OBJC'), ('NSArray_', 'ARRAY'), ('NSString_', 'STRING'), ('MTLDevice_', 'DEVICE'),
    ('MTLCommandQueue_', 'QUEUE'), ('MTLCommandBuffer_', 'CMDBUF'), ('MTLSharedEvent_', 'SHARED_EVENT'),
    ('MTLTexture_', 'TEXTURE'), ('MTLBuffer_', 'BUFFER'), ('MTLLibrary_', 'LIBRARY'),
    ('MTLCommandEncoder_', 'ENCODER'), ('MTLBlitCommandEncoder_', 'ENC_BLIT'),
    ('MTLComputeCommandEncoder_', 'ENC_COMPUTE'), ('MTLRenderCommandEncoder_', 'ENC_RENDER'),
    ('MTLCaptureManager_', 'CAPTURE'), ('MetalDrawable_', 'DRAWABLE'), ('MetalLayer_', 'LAYER'),
    ('DeveloperHUDProperties_', 'OBJC'), ('MTLLogContainer_', 'OBJC'), ('SharedEventListener_', 'LISTENER'),
    ('MTLBinaryArchive_', 'ARCHIVE'), ('CacheReader_', 'OBJC'), ('CacheWriter_', 'OBJC'),
    ('MTLCounterSampleBuffer_', 'COUNTERBUF'), ('MTLResidencySet_', 'RESSET'), ('MTLHeap_', 'HEAP'),
    ('ReleaseMetalView', 'VIEW'), ('CGColorSpace_', 'NONE'),
]
BY_NAME = {  # field name -> kinds (any struct)
    'texture': 'TEXTURE', 'resolve_texture': 'TEXTURE', 'color': 'TEXTURE', 'output': 'TEXTURE', 'motion': 'TEXTURE',
    'exposure': 'TEXTURE', 'buffer': 'BUFFER', 'index_buffer': 'BUFFER', 'indirect_args_buffer': 'BUFFER',
    'dispatch_args_buffer': 'BUFFER', 'imm_draw_arguments': 'BUFFER', 'dst_buffer': 'BUFFER',
    'visibility_buffer': 'BUFFER', 'resource': 'RESOURCE', 'dsso': 'DSSO', 'depth_stencil_state': 'DSSO',
    'fence': 'FENCE', 'sample_buffer': 'COUNTERBUF', 'indirect_command_buffer': 'ICB',
    'compute_function': 'FUNCTION', 'vertex_function': 'FUNCTION', 'fragment_function': 'FUNCTION',
    'object_function': 'FUNCTION', 'mesh_function': 'FUNCTION', 'tile_function': 'FUNCTION',
    'binary_archive_for_serialization': 'ARCHIVE', 'device': 'DEVICE', 'capture_object': 'OBJC',
    'shared_event': 'SHARED_EVENT', 'wait_event': 'SHARED_EVENT', 'event': 'SHARED_EVENT',
    'shared_event_listener': 'LISTENER', 'cmdbuf': 'CMDBUF', 'heap': 'HEAP', 'set': 'RESSET', 'archive': 'ARCHIVE',
    'library': 'LIBRARY', 'layer': 'LAYER', 'str': 'STRING', 'enumerable': 'OBJC', 'cache': 'OBJC',
    'value_data': 'DATA', 'data': 'DATA', 'encoder': 'ENCODER',
}
BY_STRUCT = {  # (struct, field) -> kinds; NOHANDLE: an obj_handle_t field that is not a host object
    ('unixcall_mtlsharedevent_setevent', 'event_handle'): 'NOHANDLE',  # a Win32 event handle (guest value)
    ('unixcall_mtlcommandbuffer_temporal_scale', 'scaler'): 'FX',
    ('unixcall_mtlcommandbuffer_spatial_scale', 'scaler'): 'FX',
    ('unixcall_mtlcommandbuffer_temporal_scale', 'depth'): 'TEXTURE',
    ('wmtcmd_compute_setpso', 'pso'): 'CPSO', ('wmtcmd_render_setpso', 'pso'): 'RPSO',
    ('wmtcmd_blit_copy_from_buffer_to_buffer', 'src'): 'BUFFER', ('wmtcmd_blit_copy_from_buffer_to_buffer', 'dst'): 'BUFFER',
    ('wmtcmd_blit_copy_from_buffer_to_texture', 'src'): 'BUFFER', ('wmtcmd_blit_copy_from_buffer_to_texture', 'dst'): 'TEXTURE',
    ('wmtcmd_blit_copy_from_buffer_to_texture_withblitoption', 'src'): 'BUFFER',
    ('wmtcmd_blit_copy_from_buffer_to_texture_withblitoption', 'dst'): 'TEXTURE',
    ('wmtcmd_blit_copy_from_texture_to_buffer', 'src'): 'TEXTURE', ('wmtcmd_blit_copy_from_texture_to_buffer', 'dst'): 'BUFFER',
    ('wmtcmd_blit_copy_from_texture_to_buffer_withblitoption', 'src'): 'TEXTURE',
    ('wmtcmd_blit_copy_from_texture_to_buffer_withblitoption', 'dst'): 'BUFFER',
    ('wmtcmd_blit_copy_from_texture_to_texture', 'src'): 'TEXTURE', ('wmtcmd_blit_copy_from_texture_to_texture', 'dst'): 'TEXTURE',
    ('wmtcmd_blit_copy_texture', 'src'): 'TEXTURE', ('wmtcmd_blit_copy_texture', 'dst'): 'TEXTURE',
}
# per call: field -> kinds for the generic structs' arguments, NOHANDLE, or PTR (a uint64 that carries a pointer)
CALL_FIELDS = {
    8: {'buffer_ptr': 'PTR'},
    17: {'arg0': 'SHARED_EVENT|EVENT'}, 102: {'arg0': 'SHARED_EVENT|EVENT'},
    26: {'arg': 'PTR'}, 32: {'arg': 'PTR:WMTRenderPassInfo'}, 47: {'arg': 'DRAWABLE'}, 48: {'arg0': 'DRAWABLE'},
    63: {'arg0': 'STRING', 'arg1': 'STRING'}, 64: {'arg0': 'STRING', 'arg1': 'STRING'}, 65: {'arg': 'STRING'},
    86: {'arg': 'STRING'}, 137: {'arg': 'RESSET'}, 94: {'ret': 'NOHANDLE'}, 95: {'ret': 'NOHANDLE'},
    # handled by wmtr_host.m itself: 114's first word is a relocated pointer to guest bytes or a bitcode token;
    # views, listeners and pools are host-made objects with their own kinds
    114: {'handle': 'NOHANDLE'}, 72: {'ret_view': 'NOHANDLE', 'ret_layer': 'NOHANDLE'}, 108: {'ret': 'NOHANDLE'},
    10: {'ret': 'NOHANDLE'}, 96: {'handle': 'NOHANDLE'}, 25: {'data': 'DATA'}, 77: {},
}
# pointer fields -> what they point to: (struct, count field or None) / 'HANDLES_IN:<kinds>' / cmd lists
POINTEES = {
    ('unixcall_mtldevice_newbuffer', 'info'): ('WMTBufferInfo', None),
    ('unixcall_mtldevice_newcomputepso', 'info'): ('WMTComputePipelineInfo', None),
    ('unixcall_mtldevice_newmeshrenderpso', 'info'): ('WMTMeshRenderPipelineInfo', None),
    ('unixcall_mtlcapturemanager_startcapture', 'info'): ('WMTCaptureInfo', None),
    ('unixcall_mtlcommandbuffer_blitcommandencoderwithsamplebuffers', 'attachments'):
        ('WMTSampleBufferAttachmentInfo', 'num_attachments'),
    ('unixcall_mtlresidencyset_addallocations', 'allocations'): ('HANDLES_IN:RESOURCE', 'count'),
    ('WMTComputePipelineInfo', 'binary_archives_for_lookup'): ('HANDLES_IN:ARCHIVE', 'num_binary_archives_for_lookup'),
    ('WMTRenderPipelineInfo', 'binary_archives_for_lookup'): ('HANDLES_IN:ARCHIVE', 'num_binary_archives_for_lookup'),
    ('WMTMeshRenderPipelineInfo', 'binary_archives_for_lookup'): ('HANDLES_IN:ARCHIVE', 'num_binary_archives_for_lookup'),
    ('WMTTileRenderPipelineInfo', 'binary_archives_for_lookup'): ('HANDLES_IN:ARCHIVE', 'num_binary_archives_for_lookup'),
    ('unixcall_enumerate', 'buffer'): ('HANDLES_OUT0', 'buffer_size'),
    ('unixcall_generic_obj_cmd_noret', 'cmd_head'): ('CMDLIST', None),
    ('unixcall_mtllibrary_newfunction_with_constants', 'constants'): ('WMTFunctionConstant', 'num_constants'),
    ('sm50_compile_params', 'args'): ('SM50ARGS', None),
    ('sm50_compile_geometry_pipeline_vertex_params', 'vertex_args'): ('SM50ARGS', None),
    ('sm50_compile_geometry_pipeline_geometry_params', 'geometry_args'): ('SM50ARGS', None),
    ('sm50_compile_tessellation_pipeline_hull_params', 'hull_args'): ('SM50ARGS', None),
    ('sm50_compile_tessellation_pipeline_domain_params', 'domain_args'): ('SM50ARGS', None),
}
# per call pointee overrides (one params struct, different pointees)
CALL_POINTEES = {
    34: {'info': ('WMTRenderPipelineInfo', None)}, 131: {'info': ('WMTTileRenderPipelineInfo', None)},
    70: {'arg': ('WMTLayerProps', None)}, 71: {'arg': ('OUT0:WMTLayerProps', None)},
    74: {'shader': ('OUT1:SM50_SHADER', None), 'error': ('OUT1:SM50_ERROR', None)},
    76: {'bitcode': ('OUT1:SM50_BITCODE', None), 'error': ('OUT1:SM50_ERROR', None)},
}
for c in (81, 82, 84, 85): CALL_POINTEES[c] = CALL_POINTEES[76]
# results returned autoreleased (+0): the guest owns no reference (wmtr_host.m keep_autoreleased)
PLUS0 = {2, 7, 11, 28, 30, 31, 32, 53, 60, 62, 66, 67, 89, 90, 129}
SM50_KINDS = {'sm50_shader_t': 'SM50_SHADER', 'sm50_bitcode_t': 'SM50_BITCODE', 'sm50_error_t': 'SM50_ERROR'}
PTR_TYPES = ('struct WMTMemoryPointer', 'struct WMTConstMemoryPointer')
# the kind a result is expected to have, from the call's name (used when the object's class declares none of the
# protocols wmtr_host.m checks): `ret` of <prefix>new<Thing> / <Thing>-returning calls, and named results
RET_HINT = [('newCommandQueue', 'QUEUE'), ('commandBuffer', 'CMDBUF'), ('newSharedEvent', 'SHARED_EVENT'),
            ('newEvent', 'EVENT'), ('newFence', 'FENCE'), ('newBuffer', 'BUFFER'), ('Texture', 'TEXTURE'),
            ('texture', 'TEXTURE'), ('SamplerState', 'SAMPLER'), ('DepthStencilState', 'DSSO'),
            ('newLibrary', 'LIBRARY'), ('newFunction', 'FUNCTION'), ('ComputePipelineState', 'CPSO'),
            ('PipelineState', 'RPSO'), ('blitCommandEncoder', 'ENC_BLIT'), ('computeCommandEncoder', 'ENC_COMPUTE'),
            ('renderCommandEncoder', 'ENC_RENDER'), ('TemporalScaler', 'FXT'), ('SpatialScaler', 'FXS'),
            ('nextDrawable', 'DRAWABLE'), ('BinaryArchive', 'ARCHIVE'), ('TimestampBuffer', 'COUNTERBUF'),
            ('ResidencySet', 'RESSET'), ('newHeap', 'HEAP'), ('IndirectCommandBuffer', 'ICB'),
            ('WMTCopyAllDevices', 'ARRAY'), ('NSString_', 'STRING'), ('name', 'STRING'), ('description', 'STRING'),
            ('DispatchData_', 'DATA'), ('sharedCaptureManager', 'CAPTURE'), ('error', 'ERROR')]
NAMED_RET = {'ret_error': 'ERROR', 'ret_library': 'LIBRARY', 'ret_archive': 'ARCHIVE', 'ret_set': 'RESSET',
             'ret_event': 'SHARED_EVENT', 'ret_data': 'DATA'}
call_names = {}

def ret_hint(call, fname):
    if fname in NAMED_RET: return NAMED_RET[fname]
    name = call_names.get(call, '')
    if not name.startswith(('NSString_', 'DispatchData_')) and '_' in name: name = name.split('_', 1)[1]
    if fname == 'ret_pso': return 'CPSO' if 'Compute' in name else 'RPSO'
    for k, v in RET_HINT:
        if k in name: return v
    return 'OBJC'

def receiver(name):
    for p, k in RECEIVER:
        if name.startswith(p): return k
    return None

out = ['/* generated by gen/gen_handles.py from DXMT src/winemetal headers + wmtr_calls.inc/wmtr_cmds.inc; do not edit */']
emitted = {}

def emit_struct(sname, call=None, plus0=False, first_kind=None, as_out=False):
    """field list of struct `sname` (as params of `call`, or as a pointee); returns the C array name"""
    key = (sname, call, as_out)
    if key in emitted: return emitted[key]
    cname = 'wf_%s%s%s' % (sname, '_c%d' % call if call is not None else '', '_out' if as_out else '')
    rows = []
    def walk(st, prefix):
        for i, (fname, ftype) in enumerate(recs[st]):
            path = prefix + fname
            m = re.match(r'struct (\w+)\[(\d+)\]$', ftype)
            if m and m.group(1) in recs:
                for k in range(int(m.group(2))): walk(m.group(1), '%s[%d].' % (path, k))
                continue
            m = re.match(r'struct (\w+)$', ftype)
            if m and m.group(1) in recs and ftype not in PTR_TYPES:
                walk(m.group(1), path + '.'); continue
            over = CALL_FIELDS.get(call, {}).get(fname) if prefix == '' else None
            cp = CALL_POINTEES.get(call, {}).get(fname) if prefix == '' else None
            pt = cp or POINTEES.get((st, fname))
            if over == 'NOHANDLE' or BY_STRUCT.get((st, fname)) == 'NOHANDLE': continue
            if ftype in PTR_TYPES or ftype.endswith('*') or (over and over.startswith('PTR')):
                if over and over.startswith('PTR:'): pt = (over[4:], None)
                rows.append(('WMTR_HF_PTR', path, '0', pointee(pt, st)))
            elif ftype == 'obj_handle_t' or ftype in SM50_KINDS or over:
                if as_out:
                    rows.append(('WMTR_HF_OUT0', path, 'WMTR_M_OBJC', 'NULL')); continue
                if fname.startswith('ret') and call is not None:
                    kinds = over or ret_hint(call, fname)
                    rows.append(('WMTR_HF_OUT0' if plus0 else 'WMTR_HF_OUT1', path, mask(kinds), 'NULL'))
                    continue
                kinds = over or BY_STRUCT.get((st, fname))
                if not kinds and ftype in SM50_KINDS: kinds = SM50_KINDS[ftype]
                if not kinds and call is not None and prefix == '' and not any(r[0] == 'WMTR_HF_IN' for r in rows) and first_kind:
                    kinds = first_kind
                kinds = kinds or BY_NAME.get(fname, 'OBJC')
                if kinds == 'NONE': continue
                rows.append(('WMTR_HF_IN', path, mask(kinds), 'NULL'))
    walk(sname, '')
    body = ['  {offsetof(struct %s, %s), %s, %s, %s},' % (sname, p, fl, mk, sub) for fl, p, mk, sub in rows]
    out.append('static const struct wmtr_hfield %s[] = {' % cname)
    out.extend(body)
    out.append('  {0, WMTR_HF_END, 0, NULL}};')
    emitted[key] = cname
    return cname

def mask(kinds):
    return ' | '.join('WMTR_M_' + k for k in kinds.split('|'))

pointee_n = [0]
def pointee(pt, container):
    if not pt: return 'NULL'
    what, count = pt
    name = 'wp_%d' % pointee_n[0]; pointee_n[0] += 1
    cnt = 'offsetof(struct %s, %s), sizeof(((struct %s *)0)->%s)' % (container, count, container, count) if count else '0, 0'
    if what == 'CMDLIST':
        out.append('static const struct wmtr_hptee %s = {WMTR_HP_CMDLIST, NULL, 0, 0, 0, 0};' % name)
    elif what.startswith('HANDLES_IN:'):
        out.append('static const struct wmtr_hptee %s = {WMTR_HP_HANDLES_IN, NULL, 8, %s, %s};' % (name, cnt, mask(what[11:])))
    elif what == 'HANDLES_OUT0':
        out.append('static const struct wmtr_hptee %s = {WMTR_HP_HANDLES_OUT0, NULL, 8, %s, WMTR_M_OBJC};' % (name, cnt))
    elif what == 'SM50ARGS':
        out.append('static const struct wmtr_hptee %s = {WMTR_HP_SM50ARGS, NULL, 0, 0, 0, 0};' % name)
    elif what.startswith('OUT0:'):
        f = emit_struct(what[5:], as_out=True)
        out.append('static const struct wmtr_hptee %s = {WMTR_HP_STRUCT, %s, sizeof(struct %s), 0, 0, 0};' % (name, f, what[5:]))
    elif what.startswith('OUT1:'):
        out.append('static const struct wmtr_hptee %s = {WMTR_HP_HANDLE_OUT1, NULL, 8, 0, 0, %s};' % (name, mask(what[5:])))
    else:
        f = emit_struct(what)
        out.append('static const struct wmtr_hptee %s = {WMTR_HP_STRUCT, %s, sizeof(struct %s), %s, 0};' % (name, f, what, cnt))
    return '&' + name

call_names.update({i: n for i, n, _ in calls})
call_tab = {}
for idx, name, st in calls:
    if st is None or st == 'void': continue
    if st == 'obj_handle_t':  # retain / release: the handle itself (wmtr_host.m handles these)
        continue
    call_tab[idx] = emit_struct(st, idx, idx in PLUS0, receiver(name))
cmd_tab = []
for kind, enum, st in cmds:
    cmd_tab.append((kind, enum, emit_struct(st)))

out.append('static const struct wmtr_hfield *const wmtr_call_hfields[WMTR_NCALLS] = {')
out.extend('  [%d] = %s,' % (i, c) for i, c in sorted(call_tab.items()))
out.append('};')
out.append('/* command list nodes: kind (0 blit, 1 compute, 2 render), type, size, fields */')
out.append('static const struct wmtr_hcmd wmtr_cmd_hfields[] = {')
kinds = {'BLIT': 0, 'COMPUTE': 1, 'RENDER': 2}
for kind, enum, cname in cmd_tab:
    st = cname[3:]
    out.append('  {%d, %s, sizeof(struct %s), %s},' % (kinds[kind], enum, st, cname))
out.append('};')
open(out_path, 'w').write('\n'.join(out) + '\n')
print(len(call_tab), 'calls,', len(cmd_tab), 'commands,', pointee_n[0], 'pointees')
