# Stage 2: the native renderer

Status (2026-11-20): backend chosen, Direct3D version known, function identification not started.

## Choice: option B, Renut's Direct3D-level renderer on deko3d

Option A is the PM4-ring Vulkan renderer (masseffect-nx through banjo-nx, `app/src/me`) on NVK. Option B is Renut's
renderer (`app/src/renut_engine`, the one sa2-nx ships from 1.0.3) on deko3d. B is chosen because:

- **It was measured faster at the same job, on the same base.** With the PM4 ring renderer, sa2-nx's ring thread took
  about 26 ms per frame in gameplay (1,200-1,600 draws, about 20,000 PM4 packets, about 18 us per draw). The game thread
  spent another 12 % in Direct3D packet building (`sa2-nx/docs/api-renderer-assessment.md`). An API-level renderer
  costs about 7.5 us per draw (MarathonRecomp-NX's measurement in the same document). It drops the PM4 parsing and the
  EDRAM model, and most of the game thread's packet building. sa2-nx moved to Renut for exactly this reason, and its
  README states that from 1.0.3 the Switch runs Renut only.
- **No patched Vulkan driver.** Option A needs NVK from mesa-switch built as a static `libvulkan.a`, which is not on this
  PC. sa2-nx links nfsmw-nx's build. deko3d is the Switch's own graphics API and ships with devkitPro.
- **It can still be developed on the PC.** plume's Vulkan and D3D12 back ends run Renut on Windows, with the emulated
  GPU still built in as the reference (`sa2_renderer = "xenos" / "renut"`, `banjo_renut_select.cpp`).
- FNC's target is 30 FPS, not SA2's 60, so A might be enough on CPU. But FNC is a full 3D game with heavy fighter
  models, and the ring renderer's per-draw cost is the part that did not tune down on SA2.

Cost of the choice: Renut hooks about 20 Direct3D functions (device creation, ring/KickOff, fences, InsertCallback,
Swap, the four draws, ClearF, Resolve, shader registration, CreateVertexDeclaration, D3DResource_Destroy). It also
reads the device's private layout, which moves between XDK versions. Option A needs fewer hooks (shader load, draws,
CreateVertex/PixelShader, flip).

## FNC's Direct3D version

`tools/xex_static_libs.py assets/game_root/default.xex` (XEX optional header 0x000200FF): **D3D9 2.0.11626.0**, as are
XGRAPHC and the rest of the XDK. The layouts Renut knows are N&B's (2.0.7645), which is Renut's own, and SA2's
(2.0.21076), which sa2-nx mapped. FNC sits between the two, so its device layout must be read from FNC's own code, every
offset with the function it came from (deko3d-renderer-guide.md, section 2).

## Blocker: no reference binary for tools/d3d_match.py

`d3d_match.py` ranks FNC's functions against a game whose Direct3D functions are named. It needs that game's image
dump and generated code (`--ref-image`, `--ref-gen`, `--ref-names`). None of the reference games' binaries (SA2, N&B,
Mass Effect, NFS Most Wanted) are on this PC, only their port sources with the hook addresses. Options are in the
conversation of 2026-11-20.

## Identifying FNC's Direct3D functions from FNC alone (2026-11-20)

The user chose this over supplying a reference game. Method:
- **Kernel anchors:** only Direct3D calls the `Vd*` video functions.
- **Packet census:** `tools/pm4_writers.py` and its range-limited form. It finds which functions build which PM4 type-3
  headers in the D3D range 0x82CD0000-0x82D00000.
- **Run-time census:** `app/src/fnc/fnc_debug_hooks.cpp` (cvar `fnc_d3d_census`) logs calls per frame, callers and
  r4-r6 for each candidate, every 5 s.

| Function | Static evidence | Run time | Identification |
|---|---|---|---|
| sub_82CEB9A8 | the only caller of `VdSwap`; also calls VdGetSystemCommandBuffer and VdPersistDisplay | 1.0 per swap at 60 FPS (menus and a full fight), called by game code at 0x82A48E94 | **D3DDevice_Swap** |
| sub_82CE5670 | DRAW_INDX[4] (index buffer), calls sub_82CF30C0 first | fight: about 450 per frame, from game code at 0x82A5E294 / 0x8248673C; r4 = 4 (triangle list), r5 = 0, r6 = 0 | **DrawIndexedVertices** |
| sub_82CE5280 | DRAW_INDX[2] (auto index), calls sub_82CF30C0 first | fight: about 54 per frame, from game code at 0x82A5E328; r4 = 13 (quad list), r5 = 0 (start), r6 = 72-1516 (count) | **DrawVertices** |
| sub_82CE4DA0 | DRAW_INDX[2], calls sub_82CF30C0 first; its callers pass type / count / constant stride and no data pointer (sub_82E9E688: `li r6,48`, count capped at 1020; sub_82E74148: `li r4,13`, `li r6,32`) | about 400 per frame in menus and fights | **BeginVertices** (worker; the caller writes the vertices) |
| sub_82CE5238 | `mr r28,r6; mr r6,r7; bl BeginVertices; if r3: bl 0x8229B520(r3, data, count * stride)` (memcpy), then `lwz r11,13652(device); stw r11,48(device)` | 4-14 per frame from game code at 0x82A57640 / 0x82A576E0 | **DrawVerticesUP** (BeginVertices + copy). Device offset 13652 read here: the inline block's end, SA2 13844 |
| sub_82CF30C0 | IM_LOAD x3, SET_CONSTANT, SET_SHADER_CONSTANTS | about 374 per frame, only from inside the draws | **draw-time shader load** |
| sub_82CE3CD0 | VdInitializeRingBuffer, VdEnableRingBufferRPtrWriteBack, writes ME_INIT[18] | once at boot | ring setup of device creation |
| sub_82CF5260 | VdInitializeEngines, VdSetGraphicsInterruptCallback | | engine setup (device creation, hardware path) |
| 0x82CE24A8 | | boot log `SetInterruptCallback(82CE24A8, ...)` | Direct3D's graphics interrupt callback |
| sub_82CE5BC8, sub_82CE5E80 | DRAW_INDX_2[1] (inline data) | 9-19 and 6-11 per frame, Direct3D-internal callers; sub_82CE5E80 gets r6 = 1280 / 640 / 512 | Direct3D's own rectangle draws (clear / resolve?): unconfirmed |

Census caveat: the census logs only the low 16 bits of r5 and r6 (a pointer shows as a small number), so arguments that
looked like pointers were checked in the callers' listings instead.

Done: the fight census (user session, 2026-11-20). Next, the remaining Renut hooks: KickOff /
KickOffSegment, BeginRingBig, the allocators, fences, InsertCallback, SynchronizeToPresentationInterval, ClearF,
Resolve, XGRegister*Shader, CreateVertexDeclaration, D3DResource_Destroy. After that, the device layout of 2.0.11626.

## FNC's device layout (Direct3D 2.0.11626), read from FNC's code

Renut's constants (`app/src/renut_engine/gpu/d3d/d3d.h`) give SA2's offset with N&B's in brackets. Every FNC value
below names the FNC function it was read from. A wrong offset gives a wrong picture, not a crash.

| Field | N&B | SA2 | FNC | Read from (FNC) | Status |
|---|---|---|---|---|---|
| bound pixel shader | 0x3194 (12692) | 13068 | **12868** | sub_82CF30C0 (draw-time shader load): `lwz r29,12868(r31)` at its start | confirmed by the pair |
| bound vertex shader | 0x3198 (12696) | 13072 | **12872** | sub_82CF30C0: `lwz r30,12872(r31)`, the word after the pixel shader | confirmed by the pair |
| index buffer | 0x3094 (12436) | 12812 | **12612** | sub_82CE5670 (DrawIndexedVertices): `lwz r24,12612(r31)` | matches the +176 shift |
| fence completion / next fence | 0x2A90 / 0x2A9C (10896 / 10908) | 11024 / 11036 | **10896 / 10908** | sub_82CE3950 (KickOff family) | N&B's offsets, shift 0 |
| state flags (bytes) | 0x2ABC-0x2ABF (10940-10943) | 11068-11071 | **10940-10943** | sub_82CF30C0, sub_82CE5670, sub_82CE5280 (`lbz rX,10940(r31)` and 10942/10943) | N&B's offsets, shift 0 |
| inline vertices' block end | (N&B -) | 13844 | **13652** | sub_82CE5238 (DrawVerticesUP): `lwz r11,13652(r31); stw r11,48(r31)` | needs the SA2 counterpart checked |
| vertex declaration | 0x2E2C (11820) | 12240 | 11992? | sub_82CF30C0: `lwz r19,11992(r31)` (+172, not +176) | **unconfirmed**: find SetVertexDeclaration |

Working model: the fence/flags block (N&B 0x2A88-0x2ABF) is at N&B's offsets, and the bound-objects block (N&B
0x3094-0x3400) is at N&B + 176. SA2 moved the same two blocks by +128 and +376. Every remaining field still has to be
read from a function that uses it, not inferred from the model.

## More candidates (kernel imports per function in the D3D range; not yet confirmed)

| Function | Kernel imports | Other evidence | Candidate |
|---|---|---|---|
| sub_82CE2DB0 | | the only writer of CP_RB_WPTR (`stw r11,1812(r10)` with r10 = 0x7FC80000); INDIRECT_BUFFER[2]; 14 per frame | the ring submit under KickOff / KickOffSegment (callers sub_82CE3428 11/frame, sub_82CE37C8 and sub_82CE3950 5/frame) |
| sub_82CF5368 | ExGetXConfigSetting, RtlInitializeCriticalSection, KeGetCurrentProcessType | holds the only two boot-time draws (sub_82CE47E0, sub_82CE8048), once | Direct3D_CreateDevice |
| sub_82CF1110 | KeResetEvent, KeWaitForSingleObject, critical section | the only reset-and-wait in Direct3D | BlockOnFence / CBlocker wait |
| sub_82CF0378, sub_82CF03B0, sub_82CF05B0 | KeSetEvent | | completion side of the waits (interrupt) |
| sub_82CF79F8 / sub_82CF7A78 | MmAllocatePhysicalMemoryEx / MmFreePhysicalMemory | | physical alloc / free wrappers (D3DResource_Destroy should reach the free) |
| sub_82CEB200, sub_82CEB320 | KfAcquireSpinLock (and KeQueryPerformanceFrequency in 82CEB320) | write display register 0x6110 (primary surface address) | scanout flip, on the interrupt side |

0x82CD1DF0-0x82CD8110 is XAM messaging/voice (XMsgStartIORequest, XamVoiceSubmitPacket), not Direct3D: the census
range's lower bound was too low.

## The ring and fence functions (identified from their code against Renut's descriptions of SA2's / N&B's)

Renut's hook comments (`app/src/renut_engine/gpu/d3d/hooks/device.cpp`) give each function's signature and behaviour.
FNC's listing matches each one:

| Function | FNC | Evidence (FNC listing; run-time census) |
|---|---|---|
| CDevice::SetFence | **sub_82CE3338** | two EVENT_WRITE_SHD packets with the fence value to the completion word, then `fence tag (10908) += 2`: Renut's KickFence copies this |
| CDevice::KickOffSegment | **sub_82CE37C8** | reads ring (48) and segment start (15140), calls SetFence, submits via sub_82CE3428 (-> sub_82CE2DB0, the CP_RB_WPTR writer), stores the new ring / segment start; no return value |
| CDevice::KickOff | **sub_82CE3950** | checks completion (10896), the in-KickOff word (13408) and flags, submits, calls KickOffSegment, returns `lwz r3,48(r31)` (PRING). About 80 static callers (the inline BeginRing of every draw/setter) but 5 per frame at run time (full segments, plus once from Swap at 0x82CEBF58) |
| CDevice::BeginRingBig(count) | **sub_82CE3B88** | `if (ring(48) + count * 4 > limit(52)) KickOff`, then sub_82CE2C78 if still short; returns the ring |
| D3DDevice_IsFencePending(fence) | **sub_82CE3A90** | device from the VdGlobalDevice import slot (title process: 0x82000864, else 0x82000860; SA2 0x82000710); KickOff if fence == current tag; returns the completion test |
| CDevice::BlockOnFence(fence, type, nokick) | **sub_82CE28D0** | returns if done; KickOff when fence == tag, nokick (r7) clear and in-KickOff (13408) zero; then polls with a CBlocker (sub_82CE4098 set up, **sub_82CE41A8 CBlocker::Check**, sub_82CE40C8 close). Once per frame from Swap (0x82CEBF90) plus the lock/resource paths |
| D3DDevice_InsertFence | sub_82CE3A58 | copies the fence tag (10908) to 10928, KickOff, returns the fence; once per frame from game code. Not hooked by Renut |
| InsertCallback? | sub_82CE3C78 | three arguments, makes ring room and calls sub_82CE3588 (writes WAIT_REG_MEM). Once per frame from Direct3D (0x82CDB220): **unconfirmed** |

More device fields from these listings: ring pointer **48** and ring limit **52** (public part, as N&B); in-KickOff
word **13408** (N&B 13232, +176); segment start **15140**; 10928 (written by InsertFence).

## Clear, Resolve and predicated tiling

| Function | FNC | Evidence |
|---|---|---|
| D3DDevice_Clear(count, rects, flags, color, Z, stencil) | **sub_82CE6E38** | 5 per frame from game code (0x82A4D990, 0x824953C0); unpacks the D3DCOLOR in r7 to a float4 (`vupkd3d128` + `vmaddfp` scale/bias), Z in f1, then calls sub_82CE6D10 with a pointer to the float color |
| D3DDevice_ClearF | **sub_82CE6D10** | the float-color clear Renut hooks: 6 per frame (from Clear, and once from BeginTiling); chain sub_82CE6660 -> sub_82CE60E8 -> sub_82CE5E80 -> sub_82CE5BC8 (the DRAW_INDX_2 rectangle) |
| D3DDevice_Resolve | **sub_82CDD200** | 14 per frame: 4 game call sites (0x82A48E70, 0x82A4904C, 0x82A4FEEC, 0x82A4FF34) and twice from EndTiling; calls sub_82CDC130 |
| Resolve worker | sub_82CDC130 | writes the EDRAM copy registers (`li r9,8984` = RB_COPY_CONTROL 0x2318, `li r7,8987` = 0x231B), a DRAW_INDX_2 rectangle and EVENT_WRITEs |
| D3DDevice_BeginTiling? | sub_82CDA978 | once per frame from game code (0x82A4DBB8): stores a rect count at 12924 (r5), walks a rect array (r24), clears through ClearF |
| D3DDevice_EndTiling? | sub_82CDAFE0 | once per frame from game code (0x82A4DC58): resolves (sub_82CDD200 x2), KickOff x2, sub_82CE3C78 |

**FNC uses predicated tiling** (BeginTiling/EndTiling once per frame), the Xbox 360's way of drawing a target larger than
the EDRAM in tiles, with the draws predicated per tile. SA2 did not (sa2-nx's notes: "flag byte +11068 is always 0 at a
draw, i.e. no predicated tiling"), but N&B (Renut's own game) does: `d3d_native.h` carries TilingSurfaces, TileCount, TileRects, TileOrigins and the
predication words, so Renut handles tiling as N&B's hooks did. SA2's port never exercised that path, so it must be
checked against FNC Reborn frame by frame. Confirmation of BeginTiling: N&B's TileCount is 0x31CC (12748), and
12748 + 176 = 12924, the field sub_82CDA978 stores its rect count into.

## Presentation

| Function | FNC | Evidence |
|---|---|---|
| D3DDevice_SynchronizeToPresentationInterval | **sub_82CEB578** | writes WAIT_REG_MEM[5] x2; once per frame from game code at 0x82A48E14 (also statically from sub_82CEB740 and sub_82CEB7C0, which Swap calls) |

Cross-check: the game's present function sub_82A48DD0 calls, in order, SynchronizeToPresentationInterval
(0x82A48E14), Resolve (0x82A48E70), Swap (0x82A48E94) and InsertFence (0x82A48E9C), which is the XDK's present
sequence. Each of the four was identified independently above.

Still to find: XGRegisterVertexShader / XGRegisterPixelShader, CreateVertexDeclaration, D3DResource_Destroy,
Direct3D_CreateDevice (candidate sub_82CF5368), RingBufferDeviceAllocate, AllocateInlineData,
SetRingBufferParameters, InsertCallback (candidate sub_82CE3C78), EndTiling/BeginTiling (candidates above).

## Shaders (found with the census build's shader-container check)

The census build checks, on each function's first 256 calls, whether r3..r6 point at a word of the container magic
family 0x102A11xx (`fnc_d3d_census.cpp`, `VirtualQuery` before each read). Run: boot, title, autosave notice, intro.

| Function | FNC | Evidence |
|---|---|---|
| D3DDevice_CreateVertexShader(pFunction, ppShader) | **sub_82CE8360** | container in r3 (189 calls), from game code at 0x82C2B9F0; allocates the object, clears 872 bytes, copies the container to **object + 0x368**, allocates physical memory for the microcode and copies it there, then calls sub_82CE8268(object, microcode address) and sets bit 0x00100000 in the object header |
| D3DDevice_CreatePixelShader(pFunction, ppShader) | **sub_82CE8178** | container in r3 (187 calls), from game code at 0x82C2BA40; the same, with the container at **object + 0x28** and sub_82CE78F8 |
| XGRegisterVertexShader(pShader, Address) | **sub_82CE8268** | called by CreateVertexShader with (object, physical microcode address) |
| XGRegisterPixelShader(pShader, Address) | **sub_82CE78F8** | called by CreatePixelShader with (object, physical microcode address) |
| microcode upload at draw time | sub_82CF2E38 | container in r4, 2 per frame, from inside the draw-time shader load (sub_82CF30C0 at 0x82CF3294 / 0x82CF34E0) |
| (helper) | sub_82CE8000 | container in r3 for every shader (376 = 187 + 189); ORs two 64-bit masks from the container (usage masks), no allocation |

The shader objects have N&B's (and SA2's) layout: containers at +0x368 (vertex) and +0x28 (pixel), as Renut expects.
Both create functions start with an optional override through a function pointer (0x83047EF8 for pixel, 0x83047EFC
for vertex: `lis r11,-31996; lwz r10,32504/32508(r11); bctrl`).

## Vertex declarations

| Function | FNC | Evidence |
|---|---|---|
| D3DDevice_SetVertexDeclaration(device, decl) | **sub_82CE7D50** | `stw r4,11992(r3)`, about 166 per frame from game code (0x82A4DA2C, 0x82A4D9F0, 0x82A51488). **Vertex declaration field = 11992** (N&B 0x2E2C + 172, not + 176) |
| D3DDevice_SetFVF(device, fvf) | sub_82CE8450 | stores the FVF at **12608** (N&B 0x3090 + 176), builds a declaration inline at device + 12368 (sub_82CE6EF8, no calls), stores it at 11992, sets a dirty bit in device+16 |
| D3DVertexDeclaration *D3DDevice_CreateVertexDeclaration(pElements) | **sub_82CE7E98** | element array in r3 (census check: Xbox 360 D3DVERTEXELEMENT9, 12 bytes); counts the elements in 12-byte steps up to Stream 255 (`lhzu r9,12(r10)`), allocates 56 + 12 * count, fills it (sub_82CE7DA8) and **returns it in r3**: SA2's signature. From game code at 0x82A4B24C. (An earlier label here, `(pElements, ppDecl)`, was wrong.) |
| (worker) | sub_82CE7DA8 | the same array in r3, from 0x82CE7EFC inside CreateVertexDeclaration |

Only one CreateVertexDeclaration call in boot, title, autosave notice and intro, against 166 SetVertexDeclaration calls
per frame: FNC's declarations mostly come pre-built from its data or from SetFVF's inline one. Renut must find them by
object at the draw (as it does), not only through the create hook. To be checked in a fight.

The census's first element check assumed the PC's 8-byte element and found nothing. The 360's element is 12 bytes.

## Device creation and destruction

| Function | FNC | Evidence |
|---|---|---|
| Direct3D_CreateDevice(adapter, type, hwnd, behaviorFlags, pParams, ppDevice) | **sub_82CE4958** | clears *ppDevice (r8); allocates the device with sub_82CE4790(**24320 = 0x5F00**, 128); device type 2 (no hardware, r4): flag 0x80 in byte 10940, then sub_82CE2540 / sub_82CF55D0; otherwise behaviorFlags (r6) \|= 0x0C000000 stored at **24200**, then the hardware initialization; stores *ppDevice, returns 0. Called twice from game code at 0x82459A70 |
| hardware initialization | sub_82CF5368 | critical sections at device + **15156** and + **15184**; flag 4 in byte 10942; sub_82CF5260 (VdInitializeEngines); stores the device into the VdGlobalDevice slot (0x82000864 for the title process); allocates 96 bytes and stores their address at **10896** (the fence completion pointer) |
| D3DResource_Destroy(resource) | **sub_82CDE0F8** | one argument; device from the VdGlobalDevice slot; type = `resource->Common & 0xF` (`clrlwi r10,r11,28`), dispatched per type; per-type paths wait on the resource fence (BlockOnFence x3) and release its memory. One call from 0x82CDE6C8 (D3DResource_Release dropping the last reference, to be confirmed) |

Layout fields from these: device size **0x5F00** (N&B 0x5E00, SA2 0x6080: Renut's kDeviceAllocSize); behavior flags
**24200** (SA2 24392); critical sections **15156 / 15184** (N&B 14944 / 14972: **+212**, another shift for that block).
Shifts against N&B so far: fence/flags block 0, vertex declaration +172, bound objects +176, the critical sections
block +212.

### Renut hook list: status

Identified: Direct3D_CreateDevice, KickOff, KickOffSegment, BeginRingBig, the ring segment allocator,
RingBufferDeviceAllocate, AllocateInlineData, BlockOnFence, CBlocker::Check, IsFencePending,
SynchronizeToPresentationInterval, Swap, DrawVertices, DrawIndexedVertices, DrawVerticesUP, BeginVertices, ClearF,
Resolve, XGRegisterVertexShader, XGRegisterPixelShader, CreateVertexDeclaration, D3DResource_Destroy.
Candidates: InsertCallback (sub_82CE3C78). Not present: SetRingBufferParameters (below). FNC-specific: BeginTiling /
EndTiling (candidates sub_82CDA978 / sub_82CDAFE0).

### The ring's allocators

| Function | FNC | Evidence |
|---|---|---|
| AllocateInlineData(device, dwords, alignment) | **sub_82CE27E8** | KickOff calls it with `li r4,4; li r5,4`; returns 0 when byte 10941 has 0x20; size = dwords * 4 on the stack, then RingBufferDeviceAllocate(device, 1, &size, alignment), or the recorded command buffer's allocator (sub_82CEC010 / sub_82CEC280) when field **13728** is set; advances field 15144. About 493 per frame, mostly from BeginVertices |
| RingBufferDeviceAllocate(device, flags, pSize, alignment) | **sub_82CE25B8** | called as (device, 1, &size, alignment) from AllocateInlineData and (device, 2, &size, 32) from the segment allocator: Renut's signature. About 495 per frame |
| ring segment allocator(device, count) | **sub_82CE2C78** | size = (count + 14) * 4, from RingBufferDeviceAllocate(device, 2, &size, 32); called by BeginRingBig and KickOffSegment (SA2's 8254DA90 sits next to BeginRingBig the same way) |
| SetRingBufferParameters | not present | the ring setup sub_82CE3CD0 (VdInitializeRingBuffer) is reached only from the hardware initialization (sub_82CF5368) and the shutdown path (sub_82CF56A0 <- sub_82CE43E8, VdShutdownEngines): no function re-creates the ring, so Renut's "do nothing" hook has nothing to replace |

More fields: command buffer being recorded **13728**; inline data cursor / end **15144 / 15148**.

## FNC's layout for Renut's constants (2026-11-20)

"verified" = read in FNC's own code (function named). "model" = predicted from the verified shift of its block, still
to be read before it is trusted. Blocks: fence/flags (N&B 0x2A88-0x2ABF) shift 0; vertex declaration +172; bound
objects and tiling (N&B 0x3090-0x33FF) +176; critical sections (N&B 0x3A60-0x3A9C) +212; the public part (ring,
constants, packets) N&B's.

| Renut constant | N&B | SA2 | FNC | Status |
|---|---|---|---|---|
| kDeviceAllocSize | 0x5E00 | 0x6080 | **0x5F00** | verified: Direct3D_CreateDevice sub_82CE4958 |
| Ring / RingLimit (native::off) | 0x30 / 0x34 | same | **0x30 / 0x34** | verified: BeginRingBig sub_82CE3B88 |
| FenceCompletion | 0x2A90 | 11024 | **10896** | verified: hardware init sub_82CF5368, KickOff, BlockOnFence |
| FenceTag | 0x2A9C | 11036 | **10908** | verified: SetFence sub_82CE3338 (`+= 2`) |
| SwapFenceTag | 0x2AB0 | 11056 | **10928** | verified: Swap sub_82CEB9A8 (`lwz r30,10908; stw r30,10928`) and InsertFence |
| Flags / Flags2 / SwapFlags | 0x2ABC / 0x2ABD | 11068 / 11069 | **10940 / 10941** | verified: draws, KickOff, CreateDevice (`ori 0x80` on 10940) |
| CommandBufferTagMask / TilePredication / RunPredication | 0x2AA0 / 0x2AB4 / 0x2AB8 | +128 | 10912 / 10932 / 10936 | model (shift 0) |
| VertexDeclaration | 0x2E2C | 12240 | **11992** | verified: SetVertexDeclaration sub_82CE7D50 |
| FVF | 0x3090 | | **12608** | verified: SetFVF sub_82CE8450 |
| IndexBuffer | 0x3094 | 12812 | **12612** | verified: DrawIndexedVertices sub_82CE5670 |
| RenderTargets[4] / DepthStencil | 0x3098 / 0x30A8 | 12816 / 12832 | **12616 / 12632** | verified: ClearF sub_82CE6D10 (`lwz r7,12616(r3)`, then `lwz r11,12632(r3)` without a target) |
| StreamSources[16] / StreamStrides | 0x30AC / 0x30F0 | 12836 / 12904 | 12636 / 12704 | model |
| Textures[26] | 0x3100 | 12920 | 12720 | model |
| ScissorRect | 0x3184 | 13052 | 12852 | model |
| PixelShader / VertexShader | 0x3194 / 0x3198 | 13068 / 13072 | **12868 / 12872** | verified: draw-time shader load sub_82CF30C0 |
| Predication / RunPredicationState / PredicationSet | 0x31A4 / 0x31A8 / 0x31AC | 13084 / 13088 / 13092 | 12884 / 12888 / **12892** | PredicationSet verified: InsertCallback's worker sub_82CE3588 ORs `lwz 12892(r30)` into its packet headers; the other two model |
| TilingSurfaces / TileCount / TileRects / TileOrigins | 0x31B8 / 0x31CC / 0x31D0 / 0x32C0 | +376 | 12904 / **12924** / 12928 / 13168 | TileCount verified (BeginTiling sub_82CDA978); rest model |
| InKickOff | 13232 | 13600 | **13408** | verified: KickOff, BlockOnFence |
| VerticesRingEnd | | 13844 | **13652** | verified: DrawVerticesUP sub_82CE5238 |
| CommandBuffer (being recorded) | | 13920 | **13728** | verified: AllocateInlineData sub_82CE27E8 |
| CriticalSection0 / 1 | 14944 / 14972 | 15348 / 15376 | **15156 / 15184** | verified: hardware init sub_82CF5368 |
| BehaviorFlags | | 24392 | **24200** | verified: Direct3D_CreateDevice |
| VdGlobalDevice import slot | | 0x82000710 | **0x82000864** (title) | verified: IsFencePending sub_82CE3A90, hardware init |
| PresentFenceTag | 0x3A5C (14940) | 15344 | **15152** | verified: Swap loads it and stores the fence into it (+212, the critical sections' block) |
| GammaRamp (kSa2DeviceOffsetGammaRamp) | 0x3A9C (15004) | 15408 | **15216** | verified address: Swap `addi r26,r31,15216` (+212) |
| SwapCount | 0x40B0 (16560) | 16968 | **16776**? | likely: loaded and stored back at Swap's start (+216); the increment to be checked |
| the -1 words (kDeviceOffsetsMinusOne, first two) | 21560 / 21564 | 21968 / 21972 | **21776 / 21780** | verified store: hardware init sub_82CF5368 (+216, as SwapCount) |
| second block after the fence completion | | | **10900** | verified: hardware init allocates it (`stw r3,10900(r31)`) |
| SideBuffer | 16728 | 17136 | 16944? | model (+216); sub_82CE2A48 reads `lwz r31,16944(r11)` |
| GlobalQword, PerfFrequency, third -1 word, TitleMinusOne, CreationThreadId, IsTitleProcess | | | ? | **to read**: the hardware init's callees (sub_82CF5260, sub_82CF1548, sub_82CF4E60, sub_82CF4BF8, sub_82CE42D0, sub_82CE84A0) against Renut's CreateDevice hook |
| TextureFetch 0x480, VertexFetch 0x6F0, SurfaceInfo 0x2880, window/values/control packets 0x28C0-0x2948 | N&B's | N&B's | N&B's? | to confirm with one reader each (the draw-time shader load reads 10372 = 0x2884, 10536-10580) |

## What Renut's CreateDevice needs from FNC (sa2-nx's approach)

sa2-nx's Sa2CreateDevice (`hooks/device.cpp`) runs the game's own Direct3D_CreateDevice with device type 2 (the
no-hardware path: the game's code fills the device in its own layout). For the title device it adds what the hardware
path gives apart from the GPU, then the game's own InitializePresentationParameters and InitializeApiState. FNC's
counterparts:

| Piece | SA2 | FNC | Evidence |
|---|---|---|---|
| Direct3D_CreateDevice | sub_8254FC10 | **sub_82CE4958** | above |
| InitializePresentationParameters(device, params) | sub_8255CB20 | **sub_82CF4E60** | in the hardware init: VdQueryVideoMode, two surfaces created (sub_82CDA108 / sub_82CDA228: the automatic back and depth buffers), the parameters copied, gamma (sub_82CF4110 / sub_82CF4158): the steps of Renut's native copy |
| InitializeApiState(device) | sub_8255C150 | **sub_82CF4BF8** | called from both creation paths, after InitializePresentationParameters in the hardware one; calls the state tables (sub_82CDA4A8), **sub_82CE8950(device, 2)** (SetScreenExtentQueryMode, D3DSEQM_PRECLIP) and **SetFVF(device, 2)**: N&B's order |
| XenonGpu_Initialize | (type-2 path) | sub_82CE84A0 | the other function both paths call, before InitializePresentationParameters |
| swap scratch word | 11028 [10900] | **10900** | the hardware init allocates its block there (shift 0) |
| critical sections | 15348 / 15376 | **15156 / 15184** | hardware init |
| VdGlobalDevice import slot | 0x82000710 | **0x82000864** | hardware init, IsFencePending |
| behavior flags, `\|= 0xC000000` default | 24392 | **24200** | Direct3D_CreateDevice (same default) |
| the two -1 words | 21968 / 21972 | **21776 / 21780** | hardware init |
| PerfFrequency (+4: period) | 21996 [21588] | **none** | FNC's device keeps no timer frequency: the only KeQueryPerformanceFrequency caller in Direct3D (sub_82CEB320, flip side) uses it at once for a percentage and stores nothing, and no function uses 21804. FNC's CreateDevice must leave SA2's two PerfFrequency writes out |

## Renut's hook list, SA2 -> FNC (the four hook files that are built)

| File | Renut hook | SA2 | FNC | Notes |
|---|---|---|---|---|
| device.cpp | Direct3D_CreateDevice (raw, Sa2CreateDevice) | sub_8254FC10 | **sub_82CE4958** | FNC's own InitializePresentationParameters sub_82CF4E60 and InitializeApiState sub_82CF4BF8; swap scratch word 10900 |
| device.cpp | D3DDevice_InsertCallback | sub_8254EEE8 | **sub_82CE3C78** | confirmed: (device, type, callback, context) passed on as r5 / r6 / r7 to sub_82CE3588, which writes the callback and context into the ring (`stwu r28`, `stwu r27`) around the interrupt packets |
| device.cpp | SynchronizeToPresentationInterval | sub_825589D8 | **sub_82CEB578** | |
| device.cpp | KickOff | sub_8254EBB8 | **sub_82CE3950** | |
| device.cpp | KickOffSegment | sub_8254E830 | **sub_82CE37C8** | |
| device.cpp | BeginRingBig | sub_8254EDF8 | **sub_82CE3B88** | |
| device.cpp | RingBufferDeviceAllocate | sub_8254D4B8 | **sub_82CE25B8** | |
| device.cpp | AllocateInlineData (SHARED in SA2) | sub_8254D9A8 | **sub_82CE27E8** | plain hook in FNC (no src/me hooks share the address) |
| device.cpp | ring segment allocator (Sa2NewSegment_hook) | sub_8254DA90 | **sub_82CE2C78** | |
| device.cpp | SetRingBufferParameters | sub_8254EF40 | none | not present in FNC (see above): no hook |
| device.cpp | D3DDevice_Swap | sub_82558E98 | **sub_82CEB9A8** | |
| device.cpp | BlockOnFence | sub_8254D6E8 | **sub_82CE28D0** | |
| device.cpp | CBlocker::Check (SHARED in SA2) | sub_8254F400 | **sub_82CE41A8** | plain hook in FNC |
| device.cpp | IsFencePending | sub_8254ECF8 | **sub_82CE3A90** | |
| draw.cpp | DrawVertices (SHARED in SA2) | sub_82565C60 | **sub_82CE5280** | plain hook in FNC |
| draw.cpp | DrawIndexedVertices (SHARED) | sub_82566078 | **sub_82CE5670** | plain hook |
| draw.cpp | DrawVerticesUP (SHARED) | sub_82565C18 | **sub_82CE5238** | plain hook |
| draw.cpp | BeginVertices (SHARED) | sub_82565758 | **sub_82CE4DA0** | plain hook |
| draw.cpp | ClearF (raw) | sub_82563978 | **sub_82CE6D10** | Z in f1, stencil in r8: to check in FNC's ClearF |
| draw.cpp | Resolve (raw) | sub_825656F8 | **sub_82CDD200** | checked: FNC's frame is 160 bytes and it loads 252(r1) / 260(r1), the caller's words at +92 / +100, as SA2; pClearColor in r10, ClearZ in f1; it reorders the arguments for the worker sub_82CDC130 |
| shader.cpp | XGRegisterVertexShader (raw) | sub_8255E658 | **sub_82CE8268** | |
| shader.cpp | XGRegisterPixelShader (raw) | sub_8255DBF8 | **sub_82CE78F8** | |
| shader.cpp | CreateVertexDeclaration (raw) | sub_8255E198 | **sub_82CE7E98** | |
| resource.cpp | D3DResource_Destroy (raw) | sub_8254A200 | **sub_82CDE0F8** | |

Plan: a game switch (RENUT_GAME_FNC) in each of the four files keeps SA2's block for SA2 and adds FNC's block with
these addresses; d3d.h / d3d_native.h / device.cpp take FNC's constants from the layout table above, each "model"
value read before it is used.

### Model offsets checked against their users (tools/offset_users.py, Direct3D range 0x82CD8000-0x82D10000)

| Offset (model) | Users | Reading |
|---|---|---|
| 12884 / 12888 / 12904 Predication / RunPredicationState / TilingSurfaces | written by BeginTiling sub_82CDA978 and EndTiling sub_82CDAFE0, read by the resolve worker and the clear chain | consistent: the tiling functions are their writers |
| 12928 TileRects / 13168 TileOrigins | EndTiling (array base); the resolve worker and the clear path | consistent |
| 10932 / 10936 TilePredication / RunPredication | zeroed by InitializeApiState sub_82CF4BF8 (Renut's copy of N&B's zeroes them too), set by BeginTiling / EndTiling | consistent |
| 12704 StreamStrides | **byte** loads/stores in BeginVertices (Renut keeps u8[16]) | consistent |
| 12852 ScissorRect | 9 functions, stored by a few (SetScissorRect candidates) | consistent |
| 16944 SideBuffer | both creation paths (sub_82CE2540, sub_82CF52F8), the segment code | consistent |
| 12636 StreamSources / 12720 Textures | array bases in sub_82CDD7C0 only | consistent but weak: read in a draw before trusting |
| 10912 CommandBufferTagMask | 7 functions incl. the per-frame shader setters | consistent |
| 21804 PerfFrequency | none | **wrong**: no such field (above) |

Also: 16780 is incremented on the flip side (sub_82CEB320), next to the swap count 16776: a vblank / interrupt count.

### Verified while porting the native offsets (d3d_native.h)

| Offset | Field | Evidence |
|---|---|---|
| 13356 | TilingExtent (N&B 0x337C + 176) | stored by BeginTiling sub_82CDA978, read by ClearF sub_82CE6D10 |
| 15036 / 15040 / 15044 | AutoDepthStencil / AutoFrontBuffer / AutoBackBuffer (N&B 0x39E8 / 0x39EC / 0x39F0 + 212) | all three stored by FNC's InitializePresentationParameters sub_82CF4E60, the function that creates them |

## First picture through Renut (PC, D3D12 back end, 2026-11-20)

Runs muted and minimized (`sa2_renderer=renut`, lockstep capture: `-VideoEvery 1`).

1. **The game crawled at about 11 fps.** The PC CPU profile (`sa2_cpu_profile_from_s`, `tools/cpu_profile_report.py`)
   had the render thread FGGameRender at 1.00 core: 50.5 % in sub_82CE3C00, 49.5 % in the condition-register compare
   of its game caller sub_827EFCC8. sub_82CE3C00 is **D3DDevice_BlockUntilIdle**: a WAIT_UNTIL (0x05C8, 3D idle)
   packet, BlockOnFence on the current fence, then a spin while device word 11012 is non-zero, a word FNC's KickOff
   path sets and only the GPU clears. Renut's N&B hook for it (state.cpp, not built) waits for the current fence
   without the spin; FNC now hooks it (hooks/device.cpp). SA2 never hit it.
2. **The pacing's presentation interval** was read at SA2's 14000. FNC's InitializePresentationParameters copies
   D3DPRESENT_PARAMETERS (124 bytes) to +13756 (`addi r3,r29,13756; li r5,124; bl memcpy`, N&B 0x34E8 + 212), so the
   interval is at 13808 (sa2_renut_pacing.cpp). Fixing it alone did not fix the speed (frame 229 at 20 s, and 233 with
   the pacing off): the spin above was the cause.

After both, the game reaches frame 801 at 20 s, and the lockstep capture records 300 frames at 59.2 fps. Renut draws
FNC's title screen. Against FNC Reborn's reference recording: **0.7** mean abs difference at the two title-screen
points (the same as the emulated GPU's own comparison); 7.9 / 8.1 at two points where the reference shows the
autosave notice and this unscripted run is still on the title (a different screen, checked by eye).

Shader library: Renut captured 360 containers (181 vs, 179 ps) its cache lacked in the first run (FNC loads most of
its shaders as objects from its data, so the CreateVertex/PixelShader dump saw only 12); after the rebuild, 0 missing
on the title screen. Still to cover: menus, fights, Legacy, Create a Fighter, cinematics, movies.

## Renut on the intro cinematic (PC, 2026-11-20)

After the shader loop's second pass (the intro's 507 captured containers translated), the scripted intro run reports 0
shaders missing. Against FNC Reborn's reference recording (lockstep, every frame, 10 s from 33 s): 0.7, 1.1, 1.2, 1.0,
0.8, 1.4, 3.3, 1.2 at eight points through the cinematic (3.3 not checked by eye).

One unexplained exit: a Renut run of the intro ended after 14 s with 0xC0000409 (fail-fast) and nothing in the log or
crash report. It did not reproduce in the next two runs (a plain boot of 45 s and the same intro run). It coincided
with MSYS2's core update closing processes during the devkitPro install, which may or may not be related. Watch for it.

## Switch toolchain (2026-11-20)

devkitPro through pacman, the msys2 route of https://devkitpro.org/wiki/devkitPro_pacman (approved by the user):
a fresh MSYS2 at C:\devkitPro\msys2, /opt/devkitpro mapped to C:\devkitPro, the dkp-libs and dkp-windows
repositories, then `switch-dev deko3d switch-libexpat switch-libzstd switch-zlib`. Installed: devkitA64 (GCC 16.1.0),
libnx, deko3d, the portlibs. The keyring package is downloaded by pacman itself (`pacman -U <url>`), as documented.

## The Switch shader chain (2026-11-20)

deko3d-renderer-guide.md section 4 / renut-renderer.md "Shaders for the Switch", on FNC's containers:

1. PC tools (`tools/deko3d_build_tools.sh`): SPIRV-Cross (Khronos, Apache-2.0, cloned to C:\Games\nx-ref\_tools) and
   uam 1.1.0 (app/thirdparty/uam), with MSYS2's clang64 toolchain in the devkitPro MSYS2.
2. Input: 879 containers (442 vs, 437 ps) = the CreateVertex/PixelShader dump (12) + Renut's captures over boot, title,
   autosave notice and intro, in out/fnc_all_shaders (game data, not committed).
3. XenosRecomp's SPIR-V pass (BANJO_DEKO3D_SPIRV_DIR): 879 SPIR-V; 200 bool specialization constants in 153 shaders.
   Run without BANJO_HLSL_OVERRIDE (the optional early-alpha copies: the folder was empty).
4. `deko3d_shaders.py --no-uam`: 879 GLSL. `deko3d_glsl_opt.py --check`: all 27 of banjo-nx's passes, every output
   compiled with the PC uam (0 kept as they were).
5. `deko3d_shader_package.py`: 879 shaders, 7 MB of GLSL -> 639 KB (app/out/renut_switch/deko3d_glsl.bin).
   `renut_switch_shader_cache.py --pc out/pc-renut`: the SPIR-V cache for the Switch (281 KB).

Coverage is only what has been played: fights, Legacy/Champion, Create a Fighter and the other cinematics will add
containers (Renut captures them on the PC; the console compiles a missing variant on first use).

Open for packaging: the NRO's name, title and icon are still SA2's (app/CMakeLists.txt, app/icon.jpg).
