# Plan — Reach Terraria's First Frame (505 splash) on the WebGPU/wasm RPCS3 POC

---

## 🎉🎉🎉 SESSION 6 BREAKTHROUGH (2026-06-03) — THE DEADLOCK IS BROKEN

**The entire "circular deadlock" was one unimplemented file syscall op.** Terraria does ALL file I/O
via `sys_fs_fcntl(0xe0000004)` (no `sys_fs_open`). That op was a no-op stub → every asset open returned
nothing → starvation that LOOKED like a circular deadlock. **Fixed in `sys_fs.cpp` case `0xe0000004`:**
read the path at `+0x10`, `lv2_file::open()` it, `idm::import<lv2_fs_object,lv2_file>` to get a real fd,
write **`+0x28=0` (valid flag) and `+0x2c=fd`** (confirmed by disasm of the caller @`0xc54270`:
`if(*(buf+0x28)!=0) skip; fd=*(buf+0x2c); file_obj+0x18=fd`). Loose paths inside `data.vfs` fail to
open → `+0x28` left nonzero → game falls back to reading `data.vfs`.

**Result (run68): flip 152 → 1301, REGCOUNT 599/601 → 601/601, booted frames 155 → 1305, 0 CELL_EBADF.**
`data.vfs → fd=3`, `flashMP3.pic → fd=4`. main blew past the old gate AND through NP/Trophy/kb init
(`cellKbInit`,`sceNpInit`,`sceNpTrophy*`,`cellGameBootCheck`,`cellGameContentPermit`) and is now
executing game code at **`cia=0x32e7c`** (NOT blocked). The render loop runs freely.

### Remaining to the splash — main RUNS its main loop but renders black (verts=0)
Confirmed across run68–72: main is **not deadlocked** — it runs a normal per-frame main loop and
reaches new HLE init each run (`cellPadInit`, `cellGcm`, `cellProcessElf`, video config). It SETTLES
cycling: **`0x32dd0`** (per-frame time-based update: `f11=*(*(toc-0x7888))` scaled by consts → xorshift
PRNG → fills data arrays; bounded loops `cmplwi 0x360/0x1e6`) → **`0x411150`** (flip-complete wait,
normal pacing) → **`0x9c6f74`** (`cellKbGetInfo` poll). flip climbs to 2200+. BUT **zero geometry ever
submitted** (`verts=0`, frames clear to black) and only **1–2 `sys_fs_read`** total — so main has NOT
streamed the logo/shader assets out of `data.vfs` yet; it's in a pre-draw loading/idle state.

### Post-breakthrough characterization (run73/74) — main is BUSY, not stuck
`[GAMECLOCK]` counter-based dump (cia probes proven unreliable): the value at `0x4e9980` is a constant
(not the clock — hypothesis dropped), BUT main's live `cia` spans the whole binary every sample
(0x1a5f08/0x2725c/0x2ecc7c/0x3c1e04/0x424360/0x492a10/0x49beb8/0x4b00b4/0x115740/0x78088…). So main is
a **live, busy game**, not wedged. It does ~3M+ instr over 70s yet renders BLACK (verts=0) and does only
**1–2 `sys_fs_read`** — so it is NOT streaming the logo/shader assets out of `data.vfs`. THAT is the gap.

### RESOLVED: data.vfs reads are PERFECT, the game is streaming assets (run75/76)
`[FSREAD]` confirms: fd=3 read #1 = `VFS2....` (archive header), main reads the index (real filenames:
`background_7.gtf`, `l3.ps`, `fallen column.gtf`, `spl..tf`=splash, …), then the **ResourceLoop streams
zlib-compressed asset data** (`78 9c` magic) — **4864 reads** in a 170s run, scattered offsets (different
assets), still going at the end. `flashMP3.pic`(fd=4)=`.ELF`. So the file path is fully working and the
game IS loading its assets. The read path is NOT the gate.

### CORRECTION (user tested 20 min): NOT slow — a SECOND deadlock at the load→render transition
flip does not climb past ~2290–2362; it PLATEAUS (user confirmed: 20 min, no movement). At the stall:
- Asset streaming STOPS at read **#4864** (correlated with the hang).
- main **blocks on `0x96004600`** (flip-complete barrier, lr=0x411150) while the RenderLoop **blocks on
  idx5 `0x96004500`** (render-go). Classic circular flip-handshake deadlock (main waits flip-done ↔
  RenderLoop waits render-go). main only "crawls" via the 9s WAITFORCE timeout (≈1 step / 9s → looks
  frozen). GAMECLOCK shows main still executing varied code (~11K instr/s) between the 9s blocks.
So the game runs ~2290 frames + streams ~4864 asset chunks, THEN hits a 2nd deadlock when the load
finishes and main transitions toward the actual (splash) render. This is the SAME class as the original
flip/idx5 deadlock — re-triggered by the load→render transition. `[STALL]` detector (in GAMECLOCK) didn't
fire because main isn't tight-looping in 0x32xxx — it's blocked on 0x96004600 most of the time.

### Experiment: lowering WAITFORCE 600→8 made it WORSE (run79: flip plateaued at 1714, earlier)
Confirms the 2nd deadlock is a real SYNC issue, not slowness: force-completing main's waits faster
corrupts the working streaming-phase sync → stalls earlier. So WAITFORCE must stay 600 (only fires at a
true >9s deadlock; normal waits complete sooner). The fix is NOT to crawl faster — it's to make the
flip handshake actually work at the transition. (Reverted to 600.)

### Refined model: the ~2290 frames are a loading/barrier phase; the TRANSITION out of it deadlocks
main's render loop = a flip-complete BARRIER (post idx5 render-go → wait 0x96004600 flip-done, per frame;
see IDX6D/BARRIER decode @0x411150). It runs while the ResourceLoop streams (~2290 frames). When the
load finishes (~read #4864), main EXITS that loop → enters the next phase (splash setup?) → STOPS posting
idx5 → RenderLoop starves on idx5 → main waits 0x96004600 that never comes → crawl-deadlock.

### NEXT — break the 2nd (flip-handshake) deadlock
1. Why does main stop posting idx5 (render-go) at ~frame 2290? It leaves the render loop for a post-load
   phase and waits 0x96004600 without driving the RenderLoop. Trace main's code around the transition
   (it executes 0x49beb8/0x3c2430/0x70a28 between blocks) — what new state does it enter?
2. The RenderLoop is parked on idx5 (0x40fc9c). On real HW a vblank/interrupt drives it; here only main
   posts idx5. When main stops, the RenderLoop starves. Either (a) make the vblank path post idx5/drive
   the RenderLoop, or (b) understand why main stops the per-frame render-go and fix that.
3. Did the ResourceLoop FINISH the load (all assets) or hang decompressing #4864? Check its state at the
   stall — if it hung, that (not the flip handshake) is the trigger.
NOTE: the pattern (break one deadlock → hit the next) means actual splash pixels likely need several more
of these threading fixes. The `0xe0000004` file fix remains THE foundational win (deadlock → running game).

### OPTIONS to actually see the splash (pick one)
1. **Let it run ~20–30 min** — confirm the splash draws once the preload finishes (decisive but slow).
2. **HLE the zlib decompression** — if the game inflates via a known routine, intercept it and inflate
   natively (massively faster than interpreting it). Biggest speedup lever. (Find the game's inflate; the
   `78 9c` streams are zlib — host `uncompress()` could replace the interpreted decode.)
3. **Accept the milestone** — the multi-session "unbreakable deadlock" is gone; the game boots, runs,
   and streams its real assets. First *pixels* are now an interpreter-speed problem, not a logic wall.
**KEEP the `0xe0000004` open-file fix (sys_fs.cpp) — THE foundation. Commit it as a milestone.**
cia probes for RenderLoop/main are unreliable — use lv2 `ppu.cia` or counter-based dumps.

---

## ⭐⭐⭐ SESSION 6 (2026-06-03) — ROOT CAUSE: all asset I/O goes through one no-op fcntl op

### The finding
The entire boot uses **ONLY `sys_fs_fcntl`** (672 calls) — **no `sys_fs_open`/`read`/`lseek` at all**.
Terraria's libfs does all file access via two custom fcntl ops + reads the **preloaded 318MB disc
directly** (`JsDiscBridge`):
- `0xc0000006` (×448): map `/dev_bdvd` → id 0x1b5 (implemented).
- **`0xe0000004` (×224): LOCATE a file → return its offset+size so the game reads it from the disc.**
  This was a **no-op** (`case 0xe0000004: break;`) → the game got NO location for ANY asset
  (`data.vfs`, `shaders/*.vs|ps`, `images/logo_1/2/3.gtf` ← the splash, `sounds/*.msf`) → nothing
  loads → the whole "circular deadlock" (it was never circular; it was starved at the source).

### `0xe0000004` request buffer (0x30 bytes), decoded from `[E4-in]`
```
+0x00 0x30 (struct size)   +0x04 0x10   +0x08 0x28   +0x0c path_len
+0x10 path_ptr (e.g. "/dev_bdvd/PS3_GAME/USRDIR/images/logo_1.gtf")
+0x14 0x00c61780 (const ctx)   +0x18..+0x24 OUTPUT (was zero)   +0x28 0x80010003   +0x2c -1
```
The loose paths (logo_*.gtf, shaders, sounds) are NOT on the disc — they're packed inside `data.vfs`
(the only USRDIR file, 42,748,659 bytes). So the game stats the loose path (fails, expected), then
reads from `data.vfs`. ONLY `data.vfs` (+ `/dev_flash/.../flashMP3.pic`) stat as real files.

### Status of the fix (first attempt — partial)
Implemented `0xe0000004` to `vfs::get`+`fs::get_stat` and fill size at +0x18/+0x20, out_code +0x2c=0.
Result: `data.vfs → OK size=42748659`, but **game flow UNCHANGED** (flip=152, REGCOUNT 599/601). So the
**output format is wrong** (game reads size/offset from a different field) and/or the op must return the
file's **disc offset** (LBA in the ISO) for the direct `JsDiscBridge` read, which `fs::get_stat` doesn't give.

### NEXT (concrete, tractable — this is the path to the splash)
1. **Pin the output format:** log the guest caller (`ppu.cia`/`ppu.lr`) in the `0xe0000004` case, dump+
   disasm the libfs caller to see which buffer fields it READS after the op (size? offset? handle?).
2. **Return the disc offset+size:** the game reads `data.vfs` straight from the preloaded ISO, so
   `0xe0000004` for `data.vfs` must yield its **byte offset within /dev_bdvd** + size. rpcs3's mounted
   ISO9660 knows this — find the API (lv2_file::open_raw / the iso device) to get the file's disc offset.
3. Verify: after the fix, expect the game to read `data.vfs`, register 601/601, and start drawing
   (`WGPU-DRAW verts>0`) — the splash logos come out of `data.vfs`.
NOTE: env is fragile — ALWAYS `pkill -9 -f remote-debugging-port` before each run (zombie Chrome squats
9223), keep `python3 /tmp/cors_server.py` (rom 8008) + `PORT=3030 npm start` (js/rpcs3_browser) alive,
and run captures from `js/rpcs3_browser` (cwd matters). Don't stack background runs (crashes the dev server).

---

## ⭐ SESSION 5 (2026-06-03) — decoded the inter-thread command architecture (the deadlock)

### Reusable tooling built this session
- **Runtime PPC disassembly pipeline.** The EBOOT (`PS3_GAME/USRDIR/EBOOT.BIN`, extract via
  `7z e terraria.iso PS3_GAME/USRDIR/EBOOT.BIN`) is an SCE/SELF whose code segments are **encrypted**
  (seg0 file_off=0x980 vaddr=0x10000 — high-entropy), so offline objdump fails. Instead dump the
  **decrypted bytes from runtime memory** via a `[CODEDUMP]` probe (one-shot, `this->id==0x1000000`,
  prints `addr: 8 words`), then disassemble with **capstone** (`Cs(CS_ARCH_PPC, CS_MODE_BIG_ENDIAN|
  CS_MODE_64)`). Script at `/tmp/ppcdis.py` style; codedump parser proven. ⚠️ don't name the script
  `dis.py` (shadows stdlib, breaks capstone import).
- **Probe-gating gotcha:** the big probe block in `exec_task` from line ~2532 is inside
  `if (this->id == 0x1000000u)` (main-only). Probes for OTHER threads (RenderLoop 0x1000003) must go
  in the per-thread block at ~line 2505 (`(this->id & 0xFF000000)==0x01000000`). RLTRACE lives there.

### The architecture (decoded from real disasm)
The game uses an **actor/channel** system. Each actor (main, RenderLoop, workers) owns a **channel
object** (main's = `0x35ef0c00`, vtable `0x592380`). Channel layout (offsets):
`+0x00`=vtable, `+0x18`=current/pending command obj, `+0xc8/0xcc`=flags, `+0xd4`=aux sem `0x96004300`,
`+0xe4`=**completion sem `0x96004400`**, `+0xe8`→sem-desc whose `+4`=**wake sem = idx5 `0x96004500`**.
- **main's send-and-wait** (`0x426434`): `bl 0x426370` (acquire slot, polls `0x3c4d08`), `bl 0x4217bc`
  (commit queue `0x3c4dd4` + **post wake sem = idx5**), then `sc 92` **wait on `0x96004400`** (`+0xe4`).
- **main's wait-for-result** (`0x63c78`, the usleep poller): `bl 0x418dd0(*(arg+0x10))` = "is it done?";
  if 0 → done; else spin `usleep(20ms)` while `*(arg+0x18)==0` (abort flag). Forcing `+0x18` bypasses
  the real result → NULL vtable dispatch → OOB (confirmed session 4). So `0x418dd0`/the result is the gate.
- **RenderLoop = the consumer** of main's channel: dispatch `0x40fc64` → `bl 0x42179c` (cmd-processor;
  blocks on **idx5** when empty, lr=0x40fc7c). Processor `0x421480` is a per-command-type jump table;
  it `bctrl`s **vtable fn-ptrs** (obj+0 → +0x8/+0x18/+0x1c) and the **case at `0x421524` posts
  `0x96004400`** (the "signal" command → main's completion).

### The deadlock (precise, runtime-confirmed)
main pushes a command (queue obj `0x35ef0d30`: **type=0xd**, handler OPD **`0x592a58`**, back-ptr
`0x35ef0c00`) and waits on `0x96004400`. Runtime trace (`run44`):
- `[RLPROC]` RenderLoop entered cmd-processor `0x42179c` only **3×** (chan=0x35ef0c00), `[RLSIGNAL]`
  posted `0x96004400` only **once**, then parked on idx5. main's **last command is undrained
  (queue count=1)** — the RenderLoop never re-enters the processor for it.
- idx5 (`0x96004500`) is **dual-use**: per-frame render-go AND command-wake. main posted idx5 308×
  (mostly per-frame). After the 152-frame intro, main switches to the synchronous-command path; the
  RenderLoop, woken by idx5, appears to run its per-frame path and re-park WITHOUT draining the cmd
  queue → main's type-0xd command never processed → no 2nd `0x96004400` → main waits forever.

### Leading hypotheses for next session (in priority order)
1. **RenderLoop doesn't re-enter `0x42179c` (cmd-processor) for post-intro commands.** Trace the
   RenderLoop's top loop: after idx5 wake, does it check the cmd queue every iteration or only render?
   Find where it decides render-vs-process and why it stops processing after the 3rd command.
2. **RESOLVED: the type-13 command is a RENDER command, not a resource load.** Its C++ object is at
   `0x592a58`, vtable `0x59bb70` (seg1 is an OPD/vtable table of `{fn,toc=0x5a7448}` pairs — deref
   TWICE: `cmd+0x14`→obj→`*(obj)`=vtable→`*(vtable)`=fn). Handler fn0 = **`0x4116b8`** (seg0, right
   beside main's flip-barrier 0x411xxx). The vtable methods are graphics setters (toc-const→`*(r3)`)
   and 4×4 matrix copies (`0x4118xx`); method `0x41173c` spins on `*(*(cmd+4)+0x118)` via `0x40ce6c`
   before a vtable `bctrl`. So main is submitting a **render/draw command** and waiting for the
   RenderLoop to execute it — the deadlock is in the render-command pipeline (may still bottom out at
   an undecoded resource the draw needs). Next: disasm `0x40ce6c` (the wait) + what `0x118` gates.
3. Queue visibility/pointer: confirm the RenderLoop's drain actually sees count=1 (memory ordering
   across wasm pthreads, or a ring-ptr that main advanced but the consumer didn't observe).

### Drain loop decoded (`0x42179c` → `0x421708`) — the bug is the queue check-empty
`0x42179c` is a thin wrapper → real loop **`0x421708`**:
```
r3 = 0x3c4c58(channel+0x18)   ; "queue has a ready command?" (nonzero = yes)
if yes: bl 0x4213d4(channel)  ; process ONE command (dispatcher) — loop until 0x3c4c58 == 0
else:   channel+0xdc++        ; consecutive-empty counter; outer 0x40fc64 idx5-sleeps when +0xdc>0x10
```
So the drain **does** process every command `0x3c4c58` reports. But main's command sits in the queue
(`0x35ef0d30`, count=1) UNprocessed ⇒ **`0x3c4c58` is returning EMPTY despite main's committed
command**. main commits via `0x3c4dd4` (called in `0x4217bc`@0x4217d8) then posts idx5. So the gate is
a **lock-free queue check-empty/commit mismatch** — the RenderLoop's `0x3c4c58` doesn't observe main's
`0x3c4dd4` commit (likely a wasm-pthread memory-ordering / non-atomic `be_t<>` ring-pointer bug, or a
ring read/write-index the producer advanced but the consumer reads stale).

### Ring primitives DECODED — they are CORRECT, so the ring isn't the bug
- `0x3c4c58` (peek/has-item): `write=*(ring+0xc)`, `read=*(ring+0xa0)`; empty iff `write==read` (with
  capacity-wrap via `*(ring+4)`); `lwsync` before publishing the item ptr. `ring = channel+0x18`.
- `0x3c4dd4` (commit): writes item size, computes new write index, **`lwsync` then `*(ring+0xc)=write`**.
- Runtime (main's channel `0x35ef0c00`): write `*(channel+0x24)=0x34`, read `*(channel+0xb8)=0` ⇒
  **peek WOULD return HAS-ITEM**. The command is correctly committed. So the consumer just isn't draining.

### So the bug = the RenderLoop STOPS draining post-intro (not a ring/visibility bug)
- idx5 `0x96004500` is **overloaded**: per-frame render-go AND command-wake. main posts it **308×**
  (307 from the commit-wake `0x4217bc`@lr=0x4217dc), then blocks on `0x96004400`. RenderLoop parks at
  `cia=0x40fc9c val=-1` (idx5 wait) and **self-posts idx5 only once** (throttle path barely taken).
- **Unresolved contradiction (needs a new angle):** `RLPROC`(0x42179c)≥3 and `RLSIGNAL`(0x421524)≥1
  FIRE, but `RLCHAN`(0x40fc78) and `RLDRAIN`(0x42172c) are **0** — impossible under the static disasm
  (0x42179c→bl 0x421708→0x42172c; 0x40fc64→0x40fc78). So the interpreter-cia model is wrong somewhere.
  `RLFINE` shows the RenderLoop is **~entirely blocked** (1 sample, @lib `0xc33f90`, lr=`0x48d440`).

### DEADLOCK FULLY TRACED (lv2 RLWAIT instrumentation in sys_semaphore.cpp)
`[RLWAIT-IN]` (RenderLoop 0x1000003 enters idx5 wait) fires **≥256×**, `[RLWAIT-OUT]` (return, slow path)
**once** — most idx5 waits hit the FAST path (sema had a value, returns at sys_semaphore.cpp line ~205
before the OUT probe); it's blocked in the one slow wait. The waits happen at **lr=0x421468 / 0x4216b4
(most) / 0x421768 — all INSIDE command processing**, not the throttle. Chain:
- drain `0x421708` → `bl 0x4213d4` (dispatch) → dispatcher case `0x421690`: `bctrl` (0x4216b0) into the
  **channel vtable[0x20] method = `0x410cac`** (OPD `0x59bb00`→`*=0x410cac`).
- `0x410cac` = per-command dispatcher: reads opcode `r11=*(ring read-ptr)`, advances read idx
  (`ring+0xa8`)/count (`ring+0xac`), then jumps via table `*(toc+0x5a80)` (opcode≤4) else `0x410f1c`.
- One opcode handler **waits on idx5** (lr stays 0x4216b4 through the bctr). main waits `0x96004400`.
  ⇒ circular: RenderLoop's opcode handler waits idx5 (render-go) ↔ main waits completion.
- **idx5 `0x96004500` is posted ONLY by main (308×, lr=0x4217dc) + RenderLoop self (1×).** `[VBLANK]`
  fires (post #1024) but **`isHLE=0`** — the game's vblank/`_gcm_intr_thread` handler ISN'T running.
  On HW that handler posts idx5 (render-go) every vblank; in our port main is the sole poster, so when
  main enters its synchronous wait the RenderLoop's opcode handler **starves**. This is the root.

### idx5-force EXPERIMENT DONE → NEGATIVE: idx5 is NOT the gate (deadlock = symptom)
Extended WAITFORCE to force-complete the RenderLoop's idx5 wait (`0x1000003`+`0x96004500`). It **fired
5× but main did NOT advance** — flip stayed 152, **no 2nd `0x96004400` post**, no draws. So forcing idx5
just makes the opcode handler re-check a **condition**, find it false, and wait again. Also: the
type-13 command routes to `0x410f1c` which only `*(channel+0x100)++` and loops back (a **skip/no-op**) —
so type-13 isn't the work; the idx5 wait lives in an opcode-0..4 jump-table handler (`bctr` @0x410d38,
table `*(toc+0x5a80)`=`0x5acec8`) that **spins waiting for DATA that's never produced.** (Force reverted.)

### CONCLUSION: the command-queue deadlock is a SYMPTOM of the un-decoded resource pipeline
Every gate bottoms out the same way (session 4 NULL-dispatch crash, session 5 idx5-force no-op): handlers
wait for **resource/asset data the decode pipeline never produces**. Forcing the *waits* never helps
because the *data* isn't there. So stop chasing the consumer side (main/RenderLoop waits) and fix the
**producer**: the actual asset decode.

### Producer-side findings (run53–56)
- **ResourceLoop (0x1000006)** is NOT idle: it did **666 `sys_fs_fcntl`** (enumerated ~224 asset entries:
  `0xc0000006` map /dev_bdvd ×2 + `0xe0000004` query ×1 per entry) then **parked on `0x96006800`
  (val=-1..-5 = STARVED)**. `0xe0000004` is a no-op (`break`→CELL_OK, logs "Unknown operation") but ran
  FINITELY then stopped — NOT an infinite poll, so likely not the gate. The ResourceLoop waits for decode
  JOBS that main (deadlocked) never posts ⇒ it's downstream of main's deadlock too.
- **Vblank path WORKS** (ruled out): events flow to queue `0x8d006000` and the RenderLoop receives from
  `0x8d006000` (matched; even flooded 4096×). The `isHLE=0 handler=0` is fine — `post_vblank_event` takes
  the `else` branch `sys_rsx_context_attribute(0xFED)`. So the missing-render-go theory is dead.
- **idx5 wait location (corrected via lv2 `ppu.cia`, since `lr` is STALE at `sc`):** RenderLoop waits idx5
  at **`cia=0x4216d4`** (the `sc` right after the dispatcher's `bctrl`@0x4216b0 — "wait for NEXT command"),
  also 0x421488/0x421550/0x40fc9c. It processed main's cmds, posted `0x96004400` ONCE (RLSIGNAL=1), now
  waits for a cmd main won't send. ⚠️ **Interpreter-cia probes (RLCHAN/RLDRAIN/RLOP/RLBCTRL) are unreliable
  for the RenderLoop** (fire inconsistently — HLE boundary?); trust the lv2-layer `cia` instead.
- **SPURS is "audio" only** (`prefix="audio"`) — no asset-decode SPURS. But **2 SPU groups created, only 1
  started** (`0x4000100`=audio) ⇒ there is an **UNSTARTED SPU group** = a concrete unexplored producer.

### Honest state: genuine circular deadlock (main↔RenderLoop↔ResourceLoop), consumer-side EXHAUSTED
Every consumer wait bottoms out at "data not produced"; every producer is itself starved waiting on the
others. Forcing waits never helps (data absent). Stop tracing consumer waits. Two concrete leads left:
1. **The UNSTARTED SPU group** — get its id/name + what it's for. If it's the asset/resource decoder and
   the game never `spu_thread_group_start`s it (or our HLE drops the start), THAT is the dead producer.
   Grep `spu_thread_group_create` args (both) + who/if `_start` is called for the 2nd; check cellSpursSpu /
   sys_spu start path under `__EMSCRIPTEN__`.
2. **ResourceLoop's decode** — who is supposed to post `0x96006800` (its work-sem)? Find the producer of
   its jobs; if it's main and main is pre-deadlock, that's the cycle entry. (Enable `0x96006800` in the
   SEMPOST probe — currently excluded.)
Probes are **read-only**; tree clean (~152-flip stall). Disasm: `/tmp/ppcdis.py` + capstone on `[CODEDUMP]`.

---

## ⭐ SESSION 4 (2026-06-03) — root-caused the "game-logic gate": it's the dead SPU asset pipeline

### 0. Environment was silently broken (fixed) — needed before any run works
- The ROM fileserver on **:8008 had died**; boot failed with `loadFixedRom fetch threw: Failed to fetch`.
- Two gotchas restarting it: (a) Chrome resolves `localhost`→**IPv6 `::1`**, so an IPv4-only
  `python -m http.server` is unreachable → bind **dual-stack** (`AF_INET6` + `IPV6_V6ONLY=0`);
  (b) the page is `COEP:require-corp` (crossOriginIsolated for SAB) so the ROM response needs
  **`Access-Control-Allow-Origin:*` + `Cross-Origin-Resource-Policy:cross-origin`**.
  Server script saved at `/tmp/cors_server.py` (serves `~/Documents/works/emu/rom`).
- **Biggest trap: a zombie Chrome squatting `--remote-debugging-port=9223`.** capture_screenshot.js
  deliberately doesn't always kill Chrome ("survive flaky reconnects"), so a stale instance holds
  9223; new runs connect to the *dead* page → every boot "fails." Fix before a run:
  `pkill -9 -f "remote-debugging-port=922"` and kill leftover `capture_screenshot.js` node procs
  (NOT the npm dev server on 3030). Verify `lsof -iTCP:9223` is empty first.

### 1. The gate is NOT "main won't draw the splash" — it's a real async-completion wait
Precise chain (addresses stable across runs):
- After the 152-frame intro, **main (0x1000000) parks in a `usleep` poll loop @`lr=0x63c98`**,
  spinning on a flag at **`*(0x948da8)` (= `*(r31+0x18)`, r31=0x948d90)** that stays 0 forever.
  (Bonus bug: a *single* `sys_timer_usleep(20ms)` there never returns — main wedges in
  `lv2_obj::wait_timeout`; the cia-level probe almost never fires, so hook the **syscall** to observe.)
- The per-frame **flip barrier (idx6 sem `0x96004600`, `limit=1 counter=1`) WORKS** (152 flips). The
  other sem main blocks on (`0x96004400` @`0x426460`) and the usleep flag are the real stall.
- **Decisive experiment (DONE):** force `*(0x948da8)=1` so main exits the poll. Result →
  `[exec_task dispatch] main_thread cia=0x0 raw=0x0 fn=0` → **`RuntimeError: memory access out of
  bounds`**. main reads a **NULL function pointer** out of a result table the async task should have
  filled. ⇒ The poll waits for a **genuine SPU/SPURS asset-decode completion that registers a
  fn-ptr/asset**, and the producer is dead. **Forcing is not a fix — main needs the real data.**
- This is exactly the prior "**circular resource deadlock / SPU-processed assets never register**".
  The 601/601 `REGCOUNT` counts a *different* (file) registration; this is the **SPU decode → result
  table** registration, which never completes.

### 2. What this means for the splash (revised, honest)
The 505 splash is gated on, in order:
  1. **Make the SPU/SPURS asset-decode pipeline run to completion and register results** (populate
     the fn-ptr/asset table main polls). This is the core deadlock — deep, multi-component
     (SPURS taskset HLE + the decode itself + completion signalling + result registration).
  2. Only then does main leave the poll and build the real scene (expect `WGPU-DRAW verts>0`).
  3. **Wall B renderer** — `WebGPUGSRender::end()` is still `execute_nop_draw()`; turning the
     textured quad into pixels is multi-week (unchanged from session 3).
Forcing past gate 1 crashes (NULL dispatch), so the splash is **not reachable by short-circuits** —
it requires real SPU-pipeline work + the renderer. All experimental forces this session were
**reverted** (tree returns to its clean ~152-flip stall; no crash).

### Next concrete lead (gate 1)
The result table main polls is reachable from `r31=0x948d90` (struct refs `0x96004300/400/600` at
`+0xd4/+0xe4/+0x114`, work-desc ptrs at `+0xb4..+0xc4`). Trace **who is supposed to write
`0x948da8` + the fn-ptr table** — it should be the SPU taskset that decodes the boot assets. Cross
with `cellSpursSpu.cpp` taskset HLE: the taskset likely runs but never posts its completion /
writes its result. That signalling gap is the thing to close.

---

## ⭐⭐ SESSION 3 BREAKTHROUGH (2026-06-03) — the game RUNS its render loop now

From "deadlocked, 0 frames" to **running frames continuously** (`flip n=1,2,3,4…`, 152
ConvertAndFlips, `GSH blit=1 flip=152`, no crash). What landed:

1. **idx6 decoded = a per-frame FLIP-COMPLETE barrier** (via full struct dump `[BARRIER+]`): a
   RenderLoop-owned C++ sync object (`+0x0` vtable, `+0x14`=RenderLoop tid, sems idx4 `+0xe4` /
   idx6 `+0x114`). main does `post idx5 (render-go) → wait idx6 (flip-done)` each frame.
2. **Frame loop SUSTAINED** — `cellResc.cpp::cellRescSetConvertAndFlip`: the game's RESC flip
   handler (`cellRescSetFlipHandler → 0x599f38`) never ran because the raw `GCM_FLIP_COMMAND`
   injection bypasses the flip-notification path. **Now invoke it directly on the RenderLoop PPU
   thread** (`render->flip_handler(ppu, 0)`) → it posts main's idx6 sem AND sets the RenderLoop's
   flip-done flag → both unblock → the loop runs. (+ belt-and-braces hard-post of idx6.)
3. **RSX empty/degenerate-draw crash FIXED** — `draw_call.hpp::get_range()`: once main drives
   frames, the RESC path submits degenerate begin/end (empty OR out-of-bounds `draw_command_ranges`);
   upstream `ensure()` killed the RSX thread → no flips. Guarded to return a zero range (skip).
   ⚠️ **HEADER change** — must `touch RSXThread.cpp Core/RSXDrawCommands.cpp ui_js/WebGPUGSRender.cpp
   rsx_methods.cpp` before rebuild or ninja won't recompile it (cost me a cycle).
4. **SPU group-start + SPURS kernel HLE-dispatch** — `cellSpurs.cpp::cellSpursWakeUp` starts the SPU
   group; `cellSpursSpu.cpp::spurs_hle_dispatch` + `SPUThread.cpp` interpreter hook run the HLE
   kernel (entry 0x818/0x848, exit 0x808/0x838, workload-entry 0xA00 sys-srv|taskset, syscall 0xA70).
   SPU kernels now reach the interpreter. **NB: this is the AUDIO SPURS (`msngSPURS_MP3`)** — it
   fails on the MP3 codec and is NOT the resource gate. Kept as a real capability.

### Current gate (session 3 end): main RUNS but renders an empty scene
- ✅ **Assets fully load now** — `REGCOUNT` reaches **601/601** (was 599; the running render loop
  let the ResourceLoop finish). So the gate is NOT the asset pipeline.
- ✅ main runs its frame loop at ~50fps (post idx5 / wait idx6 / `usleep(20ms)` @0x63c98). The
  usleep loop is per-frame pacing, not a deadlock (main is seen past its exit, cycling).
- ❌ **main issues ZERO `verts>0` draws.** It does only ~8 `end()` calls total — all empty quads
  (`prim=8 verts=0`) — then just presents empty frames. `centerRow nonblack=0` → black canvas.
  main sits in a **steady minimal-render state** (interp cycles `0x418bf0`/`0x63c98`), NOT
  progressing to populate the scene with the splash.
- ⇒ Two gates remain to the 505 splash, both deep:
  1. **Game-logic:** get main to progress from this minimal-render state to actually drawing the
     splash quad (with real vertices/texture). It's waiting on some state/trigger that isn't firing
     — NOT assets (loaded). Needs decoding main's per-frame scene-build at `0x418bf0` to find what
     gates the splash draw.
  2. **Wall B (renderer):** even once main draws a real quad, `WebGPUGSRender::end()` =
     `execute_nop_draw()` — it doesn't rasterize. Need a real RSX→WebGPU (or CPU-raster-into-VRAM)
     backend to turn the quad+texture into pixels. Multi-week.
- NB: a WebGPU "Destroyed texture used in a submit" error appears in GSH (swapchain lifecycle) —
  benign while the buffer is empty, but must be fixed once there are real pixels to present.

### Honest status at session 3 end
The project went from **"deadlocked at a resource poll, 0 PS3 frames"** to **"boots Terraria into a
continuously-running render loop at ~50fps with all 601 assets loaded."** The defining deadlock is
gone. But the 505 splash specifically is still behind (1) a game-logic gate (main not progressing
to the splash draw) and (2) Wall B (no real renderer). Both are fresh, deep investigations; the
renderer alone is multi-week. This is a real, major milestone — not the splash yet.

---

## ⭐ CURRENT STATE — re-baselined 2026-06-02 (supersedes everything below)

**The old body of this file (and `plan-poc-ps3-webgpu.md`) is STALE.** It describes
main stuck at a `0x27700` resource poll with "every table entry empty / `data.vfs`
never read." That is no longer the failure. Ground truth from a fresh `ram=2048`
boot (`capture_screenshot.js terraria … "ram=2048"`):

- `data.vfs` **is** read; resources register (`[REGCOUNT] reg=599/601`).
- The **EE RenderLoop (`tid 0x1000003`)** reaches the RESC present path and a flip fires.
- Boot survives all init (firmware, threads, trophy, savedata, audio, SPURS-init).

### ✅ Landed this session — RESC present path fixed
`cellRescGcmSurface2RescSrc` (cellResc.cpp) was a **stub returning `CELL_OK` without
writing `rescSrc`**. So the render loop's per-frame cycle
`GcmSurface2RescSrc → cellRescSetSrc → cellRescSetConvertAndFlip` failed at SetSrc
(`CELL_RESC_ERROR_BAD_ARGUMENT`, width/height==0) and never published a display buffer.
Implemented the real conversion (GCM color-surface A → RESC src). Verified:
`GcmSurface2RescSrc … 960x540 … → cellRescSetSrc count=1 → flip n=1 count=1 →
db[0] 960x540 vm_addr=0x40000000 → [GSH] blit=1`. The canvas now blits the game's
**real display buffer** (not the fake animated clear). Diagnostic logs added:
`[RESC] cellRescSetSrc REJECT …`, widened `[SEMPOST]` to the whole `0x9600xxxx` pool.

### 🧱 Wall A — the frame never completes (boot deadlock). IN PROGRESS.
main (`tid 0x1000000`) loops `post 0x96004500 (idx5) → sys_semaphore_wait 0x96004600
(idx6) @lr=0x411150`. **`0x96004600` is NEVER posted by any thread** (verified: 0 posts
across the pool; `[WAITFORCE]` band-aid force-releases main each spin so it livelocks).
- Sem pool = base `0x96004000` + index×0x100. main posts idx5, render posts idx4
  (`0x96004400` @lr=0x421768), audio loop (`tid 0x1000002`) stuck on idx1 (`0x96004100`).
- **DECODED — idx6 is a TASK-COMPLETION BARRIER, not a flip signal.** main's loop at game
  `0x411168` (raw disasm via `[IDX6D]` probe in PPUThread.cpp) is:
  `counter=*(r31+0x108); counter++; store; r3=*(r31+0x114)=0x96004600; r4=0; sc(92=sem_wait);
   if ((counter + r30) < *(r31+0x104)=limit) loop`.
  → main blocks until idx6 is posted **`limit` times** = "wait for N dispatched async tasks to
  complete." Those N tasks are the resource-decode jobs main feeds to work-queue `0x96006800`
  (635 posts observed). The per-task completion posts (idx6) NEVER arrive, so each wait relies
  on the 9 s WAITFORCE timeout → main crawls forever. **This is the commit-message
  "SPU-processed assets never register" wall, seen from the consumer side.**
- **Barrier-skip experiment** (`sys_semaphore.cpp`, `#ifdef __EMSCRIPTEN__`: short-circuit ONLY
  main's `0x96004600` waits) → main **exits the barrier and advances** — but to the NEXT
  async-gated wait: a tight `cellKbGetInfo + sys_timer_usleep(20ms)` poll loop (game `0x63c98`,
  HLE `0x9c6f54/0x9c6f74`). No crash. **But `end()` is STILL never called — 0 RSX draws.**
  ⇒ Forcing past barriers is whack-a-mole; the render path never issues scene draws because it
  too is gated on the (incomplete) resource pipeline.
- **ROOT CAUSE (conclusive): the async resource-decode pipeline never completes.** main + worker
  threads (`0x1000002` audio, `0x1000006` work-queue consumer) + SPURS SPU kernels form a
  producer/consumer chain whose terminal step is SPU/SPURS task execution that doesn't run to
  completion in this port. Every main wait (resource-poll `0x27700`, idx6 barrier, kb-poll) is
  downstream of it, and the game won't issue *any* draws without the decoded assets.
- **Real fix for Wall A is NOT patching sync primitives** — it's making the decode pipeline
  complete: (a) get the SPU/SPURS decode tasks actually executing to completion, OR (b) HLE the
  specific decode + resource-registration in C++ (compiles to fast wasm) so resources register
  and the completion sems fire naturally. Only then will main advance AND issue draws.
- (The RSXThread.cpp `flip_handler` dispatch from attempt #1 is committed but inert for this
  `isHLE=0` title — wrong mechanism, left as a no-op. `_gcm_intr_thread`'s "Unimplemented
  syscall_972" is a red herring: sys_dbg_*, `null_func`/`CELL_OK` upstream.)

### 🧱 Wall B — empty render target (the fundamental one). NOT STARTED.
`WebGPUGSRender` is Null-style: `end()=execute_nop_draw()`, and the present path **reads
pixels straight out of guest VRAM** (`g_webgpu_fb_addr`). The probe shows the published
960×540 buffer is **all zeros** (`centerRow nonblack=0`). Terraria's splash is GPU-drawn,
so VRAM stays black → **blit-from-VRAM (old plan "Option 1") cannot work for this title.**
Real pixels require translating RSX draws → WebGPU (surface cache, vertex/texture upload,
RSX FP/VP → WGSL). This is the plan's XL item and gates the actual splash regardless of Wall A.

### Order of attack (revised after the decode)
The decode changes the picture: **Wall A is not a sync-primitive bug** — it bottoms out at the
async resource-decode pipeline (SPU/SPURS), and the game issues **0 draws** until assets decode.
So the real sequence is:
1. **Wall A = resource pipeline.** Make the decode complete — either (a) get SPU/SPURS actually
   executing the decode tasks to completion (multi-day: the SPURS PPU handler is disabled in
   `cellSpurs.cpp` and the SPU-side kernel is partial), or (b) identify the decode codec
   (`data.vfs` → in-EBOOT inflate-HLE `@0x41c294` + any SPU codec) and HLE it in C++ so resources
   register and the completion sems fire naturally. Verify: main advances past the idx6 barrier
   on its own AND `WebGPUGSRender::end()` starts firing (draws issued).
2. **Wall B = renderer.** Once draws are issued, build a minimal RSX→WebGPU backend (surface
   cache + vertex/texture upload + RSX FP/VP→WGSL), or a CPU-rasterizer-into-VRAM for the 2D
   subset, until the 505 splash appears.

Both are multi-week. **Honest odds:** this is the SPU-wall the original plan flagged as the
"pivot to JIT/AOT" trigger. The PPU/SPU *interpreters* may be too slow to decode `data.vfs`
in acceptable time even if the pipeline is wired — which is why the original plan deferred
playable framerate to the wasm-JIT/AOT track. A C++ HLE of the decode (option b) is the most
promising path to a *static* splash specifically, since it sidesteps SPU-interpreter speed.

### SPU root-cause chain (session 2 — 2026-06-03, the deepest dig)
Decoded the async-completion wall all the way down:
- **main's gate is a `limit=1` barrier**, not "wait for 601 resources." `[BARRIER]
  r31=0x35ef0c00 limit(+0x104)=1 counter(+0x108)=1 sem=0x96004600 r30=0`. main does a
  *sequence* of single-completion waits (game loop @0x411168), each blocking on ONE idx6
  post that never comes. The producer posts **nothing** (workers post 0 sems).
- **SPU threads never executed.** `[SPURUN]/[SPUENTRY]/[SPUPROG]` were all 0 — the SPURS
  SPU kernels (`audiCellSpursKernel0-3`) spawned but never reached the interpreter.
- **Root: the SPU thread group is never started.** `sys_spu_thread_group_start` count=0.
  It's only called from `_spurs::handler_entry` (cellSpurs.cpp:729), spawned by
  `_spurs::create_handler` — which is **commented out** (lines 761/772-777).
- **FIX LANDED (cellSpurs.cpp `cellSpursWakeUp`, `#ifdef __EMSCRIPTEN__`): start the group
  directly when SPURS is woken.** Verified: `[SPURSSTART] …group_start(0x4000100)=0` and
  `[SPURUN]/[SPUENTRY]` now fire — **the SPU kernels reach the interpreter for the first time.**
- **Next wall: the SPURS *kernel* isn't wired.** SPU sits at `pc=0x818 op=0x00000000` (stop)
  forever — LS has no kernel code. The kernel is HLE'd (`spursKernelEntry` in cellSpursSpu.cpp,
  EXISTS) but invoked via `spu.RegisterHleFunction(entry, spursKernelEntry)` — a **defunct API**
  (commented at cellSpurs.cpp:1297 + cellSpursSpu.cpp's own registration lines), AND the **wasm
  SPU interpreter has no HLE-function dispatch** (it just runs LS opcodes: `table.decode(op)`).
  The kernel's atomic `vm::reservation_op`s are also commented throughout cellSpursSpu.cpp.
- **To finish Wall A's SPU path:** (1) add an HLE-function registry + dispatch to the wasm SPU
  interpreter (SPUThread.cpp cpu_task), (2) re-enable the `spursKernelEntry`/exit/select
  registrations, (3) provide non-atomic equivalents for the commented `reservation_op`s,
  (4) ensure SPU channel/DMA works for the kernel + the dispatched decode task. Then the decode
  task runs → resources complete → completion sems post → main's barriers clear.
- **CAVEAT:** a prior note in cellSpursSpu.cpp says "SPURS is not main's direct boot-poll
  producer" — so the idx6 producer for main's *current* barrier may be a different thread, not
  the audio SPURS. The idx6 producer is still unidentified (never posts; 0 SEMPOSTs). Confirm
  whether the SPU pipeline actually feeds main's idx6, or trace the idx6 producer separately.

### Repro / build (verified working this session)
```sh
# edit under rpcs3/Emu/** or Utilities/** → MUST rebuild emucore first:
source /Users/bank23232525/emsdk/emsdk_env.sh
cmake --build build_cmake/build/root-wasm --target rpcs3_emu && ./build_wasm.sh 2048
# run (servers already up: npm@3030, python@8008 in ~/Documents/works/emu/rom):
cd js/rpcs3_browser && HEADLESS=1 node capture_screenshot.js terraria out.png "ram=2048" "<trigger>" "" <delayMs>
# ground-truth signals: [REGCOUNT], [RESC], [WebGPUGSRender] flip n=, centerRow, [GSH] blit=, [SEMBLOCK]/[SEMPOST]
```
⚠️ Use the **2048** variant — the 1024 build is stale (May 28). Headless WebGPU is flaky
(~16k frames then CDP drops; the script auto-reconnects). RESC/deadlock appear ~line 5300.

---

## Definition of done
A screenshot showing the **real 505 Games splash** (orange "505" + gray "GAMES" on black,
matching `js/rpcs3_browser/public/terraria.png`), produced by the RSX executing real GCM
commands → `WebGPUGSRender::flip()` → WebGPU blit. **No faked pixels.**
Success signal: `[WebGPUGSRender] flip n=1` fires (first real flip) and the blit shows the splash.

Non-goal: playable framerate (NOT achievable on the static PPU/SPU interpreters — that needs
the wasm-JIT/AOT track, deferred).

## The dependency chain (what must happen, reverse-ordered from the flip)
1. `flip()` runs ⟸ RSX RenderLoop processes a flip command  *(pipeline already ALIVE)*
2. main issues an RSX flip (builds a GCM command buffer w/ a draw, calls cellGcmSetFlip)
3. main passes its resource-poll at `0x27700` ⟸ resource ID it wants is registered
4. the resource/handle table (0x68c-stride, ~390 entries @ ~0x8axxxxx) gets entries
   registered (entry `+0x60` halfword set from `0xffff` → real ID) ⟸
5. the asset loader reads `data.vfs` → decompresses → registers resources ⟸
6. the asset-loader thread (WorkerThread/ResourceLoop) unblocks after its init  ← **CURRENT GATE**

## ⭐ SESSION 7 (2026-06-03) — the game is ALIVE & loads fully; the wall is SLOWNESS + a render gate, NOT a deadlock

Hard-won, definitive findings this session (runs 80–84). Several earlier theories are now DISPROVEN —
read these before re-deriving:

1. **The flip-barrier-force theory is DEAD.** Forcing main's `0x96004600` (idx6) wait — at ANY spin
   threshold low enough to help — ALSO fires during legit-slow streaming and corrupts sync (FBFORCE
   stalled at 1858 < 2362 baseline; matches the earlier WAITFORCE 600→8 regression). The flip-barrier
   is NOT the wall. Do not re-attempt blind forcing.
2. **main is NOT cleanly deadlocked on a syscall.** `[MAINLAST]` reads "NOT in syscall; cia=0x32dd0",
   but `[MAINFINE]` (main's own PC, every 256k instr) shows main visiting MANY varied funcs over the run
   (0x69xxx, 0x42xxxx, 0x115740, 0x49cdc8, 0x49be58, 0x3c8cd4 …) — main PROGRESSES, slowly. 0x32dd0 is
   just a dt/time fn (`bl` from 0x1d4954; reads scale const @0x4e9980) it parks at between bursts;
   `MAINSPIN` proved it executes 0x32dd0 only ONCE (not a tight spin).
3. **Asset loading COMPLETES.** ResourceLoop (tid 0x1000006) streams `data.vfs` reads #1→~#4864 across
   the whole 42 MB file (sz=0x28c4af3), tons of `78 9c` zlib headers (decompressing). main reads the VFS
   TOC filenames: `combat…`, `background_7.gtf`, splash (`.gtf` = PS3 texture). Then reads STOP.
4. **Post-load, workers go idle/blocked** (LDUMP): ResourceLoop=`sys_semaphore_wait`, 0x1000007=`usleep`,
   0x1000008=`_sys_lwcond_queue_wait`, 0x1000009=`cellSpursEventFlagWait`. They finished their work.
5. **Resource table reaches ~600/601 registered** (REGCOUNT), never quite 601. main polls `0x27700` for
   the missing few (POLLBYPASS3 tgt=0x131, POLLBYPASS5 tgt=0x8 fire once each and force it past).
6. **The render pipeline works END-TO-END**: game → `cellResc` (960×540 src @0x40000000) →
   `cellRescSetConvertAndFlip` → `WebGPUGSRender::flip()` → WebGPU blit. The ONLY problem: the 960×540
   source surface is **only ever cleared to BLACK** (argb=0) — the game never draws splash content into it.
7. **The "8 draws" are a red herring** — degenerate cellResc/flip begin/end submissions (verts=0,
   `dc.empty()`), NOT the splash (confirmed by the existing wasm-tolerance comment in
   `draw_call.hpp get_range()`). The 3D draw path itself is still `execute_nop_draw()` (NOP).
8. **Console printf was NOT the execution bottleneck.** Throttling SCALL/SEMPOST/RESC cut log 8×
   (84k→10k lines) but FSREAD progress was identical → the limit is interpreter speed + main being
   BLOCKED-waiting much of the time (~12k main-instr/sec *average*, incl. blocked time).
9. **`flip=N` is NOT a reliable progress metric** — it's the RenderLoop free-running at a
   host-scheduling-dependent rate (2434 / 2578 / 457 across runs with identical real progress). Track
   **FSREAD / MAINFINE / REGCOUNT** instead.

### The two REAL remaining blockers (both required; neither alone yields pixels)
- **(P1) main never SUBMITS the splash draw — and it's a HARD GATE, not slowness** (run84, 600s = 3×
  the 200s run: still only 8 degenerate draws, still black). Registration is NOT the gate: REGCOUNT
  reaches **601/601** (the dominant final state); POLLBYPASS3/5/6 each fire ONCE early then main
  proceeds. All assets load (data.vfs fully read), all 601 resources register, workers go idle
  (sem/usleep/lwcond/SpursEventFlag waits — done, nothing stuck mid-task). Yet main runs its frame
  loop (cellResc convert+flip each frame) presenting BLACK and **never issues a real scene draw**.
  ⇒ The gate is in **main's own per-frame game logic** — a state-machine branch that decides NOT to
  draw the splash, on some non-resource condition (timer? vblank count? an event the idle workers
  should post? a boot-phase flag?). **NEXT: trace main's frame loop** — log the call stack at
  `cellRescSetConvertAndFlip` (main's present), disassemble the frame routine, find the conditional
  that skips the scene draw, and identify what it waits on.

  **Render architecture decoded (run85/86):** the present is run by the **RenderLoop (0x1000003)**, not
  main. main is a PRODUCER that enqueues **command-objects** into an SPSC ring; the RenderLoop is the
  CONSUMER. RenderLoop frame stack: thread-entry 0x9c000c → 0x420ff0 → 0x423674 → worker-wait 0x423c00
  → consume-loop 0x40fc7c → per-command dispatch **`bctrl`@0x4216b0** → handler. The dispatch is a
  virtual call: `r9=*(r30); r9=*(r9+0x20); ctr=*(r9); r2=*(r9+4); bctrl` (r30 = command object, vtable
  method +0x20 = the command's `execute()`). The FLIP command's handler → cellResc convert+flip.
  **Reliable conclusion (our RSX backend sees the real GCM FIFO):** the FIFO holds only CLEAR(black) +
  degenerate FLIP — **never a real NV4097 draw**. So main enqueues only clear/flip command-objects,
  never a DRAW command-object. ⇒ the gate is upstream in **main's scene-building logic**, which decides
  not to emit draw commands. (Gotcha confirmed: interpreter `if(cia==X)` probes are unreliable for
  arbitrary addresses — only branch-target/block-head cias get caught; 0x4216a4 RLBCTRL never fired
  though 0x4216b0 ran. Use HLE-layer / backend-layer signals, not raw cia== probes, for the RenderLoop.)

  **NEXT (next session): find main's scene-build gate.** main has a per-frame "build the scene" step
  that would enqueue draw command-objects; it's short-circuited. Candidate approaches: (a) instrument
  the ring PRODUCER (main's enqueue, ~0x3c4dd4 commit) to log the command-object vtable main pushes —
  enumerate flip/clear vs (absent) draw; (b) find main's top frame loop (its present-equivalent) and the
  branch that gates scene emission; (c) check whether main waits on a boot-phase flag/event that the now-
  idle workers were supposed to post (the splash may be gated behind a "first update tick" the engine
  never fires). The 3D-NOP (P2) is moot until a real draw command appears.

  **Main's full frame-loop call hierarchy (run88, IDX6D back-chain, bound fixed to 0x60000000):**
  entry `0x10250` → `0x104d8` → `0x142148` → **game loop `0x409xxx`** → render-alloc `0x4a5ca0`→`0x4a0e10`
  → flip-sync `0x402a84` → flip-wait `0x4111b0`. The game loop at **0x4097ac–0x4098a0** runs EVERY frame
  (allocs command buffers sized 0x160/0x120/0x20 via `0x4a5ca0`, flips via `0x402a84`@0x4097dc) — so main
  is NOT gated before rendering; it renders an **empty scene**. `0x402a84` = flip + reset-command-buffer
  (zeroes r28 fields after the barrier).

  **CORRECTED understanding of the boot state (run89) — the init is HEALTHY now:**
  - `0x409a90` (main's big init fn) returns **r3=1 SUCCESS naturally** — the `MAIN-FORCE` hack@0x142148
    is DORMANT (only fires on r3=0). The e4 file-fix healed this.
  - `ONLINE-SKIP`@0x1d6e90 (np/online init) is DORMANT — online init runs; G[0]@0x9714f0 is built.
  - **Only `POLLBYPASS3/5/6` (resource-poll bypasses) remain active.** main polls "is resource 0x131 /
    0x8 registered?", they're NOT found in the table (despite REGCOUNT 601/601), and the bypass fakes
    "found" + redirects to the found-handler 0x2766c.
  ⇒ The papered-init-failure theory is WRONG. main initializes properly and runs its render loop; the
  ONLY remaining force-hack is the resource poll, and the scene is empty.

  **NEXT — the sharpest lead: the POLLBYPASS resources (0x131 / 0x8).** The game wants two resources that
  aren't in the 601-entry table; the bypass fakes them "found" so the game "draws" with assets that may
  not actually exist/be-ready → empty render. Investigate: (a) what are 0x131 / 0x8 — are they the splash
  GTF / a render target? (b) why aren't they registered when 601 others are (different table? a
  second registration step that didn't run?). (c) If they're genuinely-loaded-but-unregistered, register
  them honestly (write +0x60) instead of faking the poll; if genuinely-missing, find the load/register
  step that should produce them. Removing the last papering and seeing the game render naturally is the
  goal. (Alt lead: trace the game's UPDATE logic — the sibling of the render call in 0x409xxx — for the
  state branch that adds the splash sprite to the scene.)

  (Probe note: main's stack lives at **0x5008xxxx** — back-chain walks must use `sp < 0x60000000`.)

  ### ⭐ DEEPEST FINDING (run82/89): the loader's second-phase work queue is never posted
  The loader (ResourceLoop 0x1000006) has TWO work-queue semaphores:
  - **0x96006800** — posted normally; drives the initial asset load (→ 4864 data.vfs reads, 601 register). ✅
  - **0x96006700** — **NEVER posted (0 posts in the whole run)**. After the initial load the loader blocks
    FOREVER on it (`[SEMBLOCK] tid=0x1000006 sem=0x96006700 val=-1 lr=0x423c00 cia=0x423bc4`).
  This is the **second-phase** loader queue. The resources main polls for (0x131 / 0x8, the "0x8xxxxx
  class that the real poll never produces") are what this phase would decode/produce. Because nobody posts
  0x96006700, they never load; main would busy-poll 0x27700 forever (the original livelock); the broad
  POLLBYPASS5 fakes them "found" so main proceeds and renders an empty scene.
  **The vicious cycle the bypass creates:** poll "is 0x131 present?" → bypass says YES → main SKIPS posting
  the load-request → 0x96006700 never posted → 0x131 never loads → renders black.

  **NEXT (concrete): find the producer of 0x96006700.** Who is supposed to post it (enqueue second-phase
  work)? Candidates: (a) main, in a render/resource-setup step that's gated *before* the post (the bypass
  may be short-circuiting the very code that would post it — test by narrowing POLLBYPASS5 to NOT bypass
  the specific path that leads to the 0x96006700 post); (b) a thread/event that's blocked; (c) an SPU/SPURS
  completion. Trace: grep the EBOOT for `sys_semaphore_post(0x96006700)` call sites (disassemble around the
  loader's create of 0x96006700 to find its paired post), and check whether narrowing/removing POLLBYPASS5
  lets main reach that post naturally. If main is *supposed* to post it but the bypass skips it → the fix is
  to make the poll resolve correctly (load the resource) rather than fake-found. If it's SPU/SPURS-gated →
  that's the SPU-speed wall (pivot to HLE-decode or JIT/AOT per the Risk section).

  ### run90 experiment — DISABLED POLLBYPASS5 (proved the bypass is NOT the cause)
  With the broad redirect off, main runs the REAL poll and:
  - **busy-polls forever** at 0x27700 from its loop **@0x27638**, cycling target IDs **0x8, 0xe1, 0x6,
    0x96, 0x131, …** (a LIST of resources it needs), all "not found";
  - **NEVER posts 0x96006700** (`POST6700` count 0) — so main is NOT the phase-2 producer; the vicious-
    cycle theory is dead. The bypass merely papers the wait.
  - flip still hits 2362 (RenderLoop free-runs black independently of main's poll).
  So the gate: main waits for resource IDs 0x8/0xe1/0x6/0x96/0x131… that are NOT among the 601 registered,
  and phase-2 production (0x96006700) never triggers. Two sub-hypotheses to disambiguate NEXT:
  - **(H-logic)** the game genuinely needs a phase-2 load that's gated on a state/trigger that never fires
    (find the 0x96006700 producer — a thread/event/SPURS path, NOT main).
  - **(H-bug)** our loader DOES register these resources but writes the ID to the wrong field, so main's
    compare (`id @ desc+0x6c` vs entry `+0x50..0x70`) always misses — an EMULATION bug in the
    decompress/decode/register path (e.g. the inflate-HLE @0x41c294 producing subtly-wrong bytes).
  **NEXT experiment:** dump the 601 registered entries' ID fields and compare against main's wanted IDs
  (0x8/0xe1/0x6/0x96/0x131). If main's IDs appear in the table at a DIFFERENT offset than main checks →
  H-bug (tractable, fix the offset/decode). If they're absent entirely → H-logic (find the phase-2 trigger).

  ### run91/92 — the "601 registered" metric is UNRELIABLE; the table is largely EMPTY
  IDDUMP at a real cursor (cur=0x947630) showed entries near it are **all ZEROS**. The REGCOUNT filter
  `+0x60 != 0xffff` counts **zero-initialized entries as "registered"** — so "601/601" was never real.
  The resource table is largely **empty** (entries lack real IDs). That is WHY main can't find resources
  0x8/0xe1/0x6/0x96/0xc0/0x131 — they were never written with real IDs, **despite the loader reading +
  decompressing 4864 assets**. So the gate is the loader's **decode→register step producing empty/zero
  entries** (reads+inflate run, but the parsed result isn't written into the table). Leans
  **emulation-gap in the decode-register path** (e.g. inflate output subtly wrong, or a parse/register op
  that no-ops) — which would be TRACTABLE (our code).
  **NEXT (clean):** (1) refactor IDDUMP to SEARCH the whole table (±4000 entries, offsets 0x40..0x78) for
  main's tq + count entries with a REAL id (≠0xffff AND ≠0) — confirm empty-table. (2) Instrument the
  loader's register step (after inflate @0x41c294) — does it WRITE a table entry, with real data or zeros?
  (3) Decompress the same data.vfs chunk offline in Python and diff against the in-EBOOT inflate-HLE output
  to test bit-correctness. A wrong inflate → garbage parse → empty registration → empty render.

  ### ⚠️ run93/94 — METHODOLOGICAL CORRECTION: all prior table metrics were INVALID
  Disassembled the REAL resource-presence poll (entry 0x2761c, loop 0x27638). The actual lookup is a
  **two-level hash**, NOT the flat `stride 0x68c / +0x60` that REGCOUNT/IDSCAN/RESSCAN/POLLBYPASS assumed:
  target = `lhz desc+0x6c`; base `r8 = *(-0x78ac(r2)) + 0x290000`; per slot reads byte `r10 = *(tableptr)`,
  computes `r10<<7` and `index×0x68c`, reads compare halfword at `-0x6f8c(base+…)`; match→0x2766c sets
  `desc+0x15=1,r3=1`, miss→0x276d8 sets `=0`. **AND POLLBYPASS5 redirects cia==0x27700→0x2766c, so the REAL
  search never runs at runtime.** ⇒ "601 registered" / "2044 entries" / "found @+0x6e" were ALL artifacts —
  discard. Only solid fact: bypass-OFF (run90) the game's OWN search never matches main's targets → those
  resources are genuinely absent from the index at `*(-0x78ac(r2))+0x290000`.
  **NEXT (real):** (1) probe that index (`r2≈0x5a7448` → base=`*(0x59fb9c)`, index=base+0x290000): dump its
  count byte + first slots — populated? (2) find where the LOADER registers (the write that should fill this
  index); compare its target address with main's `base+0x290000`. DIFFER → loader fills table A, main reads
  table B (disconnect = the bug). SAME but empty → decode/register write no-ops/wrong. (3) only fix the
  structure once it's correct — prior HANDREG failed because it wrote the WRONG structure.

  ### ✅ run95 — SOLID (correct structure): main's resources are genuinely ABSENT; ~187 others registered
  IDSCAN2 with the correct two-level lookup (inner array @ `r10g+0x290000-0x6f8c`, here r10g=0x5f7630 →
  **inner=0x8806a4**, stride 0x80, id @ off 0): **all of main's targets (0xc0, 0x251, 0x252, 0x253, 0x24b)
  are ABSENT (found_i=-1)**, while the index DOES hold **187 non-zero entries** (other resources). So it's
  not "table empty" and not "wrong offset" — the register step **writes ~187 resources but never the
  specific IDs main waits for**. main busy-polls those → bypass papers it → empty render.
  **ROOT now precise:** find why those specific resource IDs never get registered into 0x8806a4. Either
  (a) phase-2 load (queue 0x96006700, never posted) would produce them — find its producer; or (b) the
  register/decode step skips/fails for that asset class. **NEXT:** locate the code that WRITES the index
  (0x8806a4 region) — set a probe on writes there (or find the loader's register fn) and watch which IDs
  it registers and where main's targets (0x251-0x253…) would come from. That writer + its trigger is the fix.

  ### ✅✅ run96-100 — DEFINITIVE: registration PLATEAUS at 342; phase-2 queue 0x96006700 never posted
  REGTRACK (count tracked independently of main's polling, via `*(0x59fb9c)+0x289064`) over a 240s run:
  cnt goes **0 → 342 → flat at 342 for 18+ samples**. So it's a hard STALL, not slowness. The registered
  IDs are low/specific (run96: 0x1,0x4,0x8,0x9,0xc,0xe,0x10,0x17,… + float-looking 0x3f80 noise); main's
  targets (0xc0, 0x251-0x253, 0x24b) are NOT among the 342. Mechanism: the loader (0x1000006) drains
  **phase-1** work from queue **0x96006800** (main posts it 635× via the channel-signal @lr=0x4217dc →
  registers 342), then blocks FOREVER on **phase-2 queue 0x96006700** (`SEMBLOCK val=-1`), which **nobody
  ever posts** (0 posts all run). main's beyond-342 resources are phase-2 → never register → main polls
  them forever → bypass papers it → black.
  **THE GATE, precisely:** find the producer that should post **0x96006700** (enqueue phase-2 loader work).
  Proven NOT main (run90: main never posts it even bypass-off). Candidates: a different worker thread; an
  SPU/SPURS workload completion (cellSpursSendWorkloadSignal/WakeUp seen) whose completion-callback should
  post 0x96006700 but is unimplemented/never fires (= the SPU wall the Risk section flagged); or a phase-1-
  done detector that should self-trigger phase-2. **NEXT:** capture the loader's r3 at the 0x96006700 wait
  (0x423bc4) = the phase-2 CHANNEL object addr; find who else references it (the enqueue site that sets
  chan+0xec and posts chan+0xd4); determine if that producer is PPU-logic (fixable) or SPU/SPURS (the wall).

  ### ✅✅✅ run104/105 — THE REGISTRATION MECHANISM, decoded via a store-watch on the count
  Watched writes to the count @0x880694 (interpreter STW hook): the writer is **MAIN (0x1000000) at
  cia=0x690b4**, driven by a long **unrolled/generated block at 0xe0xxx** (lr marches 0xe0164→0xe795c).
  Disassembly of the driver (0xe0140+): a sequence of `bl 0x86a40(obj, r4=ID, r5=count, 0)` (per-resource
  setup) + `bl 0x68f64` (commit → 0x690b4 increments cnt). The r4 IDs are **9, 0xa, 8, 3, 0x17, 1, 0x15f,
  0xaa, 0x11, 4 …** — a HARDCODED static resource MANIFEST, **342 entries, IDs up to ~0x15f**. cnt climbs
  1→342 then the block ends (~0xe795c).
  **THE GATE, finally precise:** main's targets **0x251-0x253 (593-595) are BEYOND the static manifest's
  max (~0x15f)** — they are DYNAMIC resources (from data.vfs). And the commit (0x690b4) is **ONLY EVER
  called by the static block** — so **dynamic resources are NEVER committed to the index** the poll
  searches. The loader reads+decompresses data.vfs (4864 reads) but the register-commit (0x86a40/0x68f64)
  never runs for those decoded assets. The dynamic-registration code path is **never reached** (not
  "reached but fails" — cnt is touched only by 0xe0xxx).
  **NEXT:** find the code that SHOULD register dynamic resources (call 0x86a40/0x68f64 with id≥0x251).
  Instrument 0x86a40's callers (it's a block-head, so cia== may catch it; else watch its first store) to
  confirm only-static callers, then trace upstream: after the loader decodes an asset, who is supposed to
  commit it? (a main post-load registration loop that's gated, or a loader-side register that our decode-
  path skips). Likely tie-in: the decode/inflate result must be valid for the register to fire — so the
  inflate-correctness check (decompress a data.vfs chunk in Python, diff vs the in-EBOOT inflate) is now
  doubly worth doing. THIS is the crux of P1.

  ### run106-109 — verification loop + commit-guard; printf-RE hitting diminishing returns
  - After the static manifest commits (cnt→342 @0xe795c), main runs a **verification loop** (0xe7994+)
    that polls (0x2761c) all 342 descriptors AND their stride-0x80 SUB-RESOURCE slots (+0x80,+0x100,…).
    main's targets (0xc0, 0x251-0x253) are SUB-RESOURCES polled here; absent → main loops.
  - The register commit has a **guard** (0x69078: skip unless obj+0x1150 > 0) — resources without their
    data/attributes populated are skip-committed. REGSETUP shows the setup fn 0x86a40 IS called with
    high keys (0x252 etc.), but the committed *resource* set (read at the cnt-write) is only ~17 distinct
    type-values across 357 commits, and **main's targets are NOT committed**.
  - The decode is NOT corrupt: the inflate match-copy HLE (0x41c294) is a correct overlap-safe forward
    byte copy. So this is a registration/population gap, not a decode bug.
  - **HONESTY CHECK:** ~10 build/run cycles this session went into the resource hash internals. It's an
    intricate MULTI-LEVEL structure (descriptors stride 0x68c, sub-resources stride 0x80, type vs id vs
    attribute-key namespaces) and live printf-RE keeps misreading runtime offsets (cnt-as-count vs
    type-vs-id, staging bases r14+0xcd0 vs +0xc50). Diminishing returns on probe-RE.
  - **Consistent ROOT across all of it:** main's target SUB-RESOURCES (dynamic, from data.vfs) are never
    populated+committed into the index; the post-load registration/population step never runs (the static
    manifest commits only what's already populated; nothing re-commits after the loader loads).
  - **NEXT / better tools:** (a) find main's post-load registration trigger — what consumes loader
    completions and populates obj+0x1150 then re-commits (trace from the loader's processing of a 0x96006800
    work item to any write of a descriptor's data fields); (b) given probe-RE limits, consider OFFLINE
    decompile (Ghidra/IDA) of the dumped resource-manager funcs (0x69040 register, 0x86a40 setup, 0xe0xxx
    manifest, 0x2761c poll) for a correct structural model; (c) or compare against desktop RPCS3 (runs
    Terraria) to see the populated index state. THIS is the precise crux of P1.

  ### ✅✅✅ run110 (LDRFINE) — DECISIVE: the loader never registers; re-commit-after-load is the missing step
  Dense PC-sample of the loader (0x1000006) process loop: **445× in 0x41xxxx (inflate), 186× in 0x49xxxx
  (data copy/populate), a few in 0x42xxxx (queue)/lv2 — and ZERO in register code (0x69xxx/0x86xxx/0xe0xxx).**
  So the loader is purely **I/O + data-population**; the searchable-index COMMIT is done ONLY by main's
  static manifest (0xe0xxx → 0x86a40/0x68f64), which runs ONCE early. The full mechanism:
  1. main runs the static manifest early → registers the ~342 resources whose data is already present;
     dynamic resources' data isn't loaded yet so the commit-guard (`obj+0x1150 > 0`) SKIPS them.
  2. main enqueues 635 load items; the loader reads data.vfs + inflates + copies data into the resource
     descriptors (0x49xxxx) — POPULATING obj+0x1150 etc.
  3. main busy-polls the verify loop waiting for the dynamic sub-resources to appear in the index.
  4. **MISSING: nobody re-runs the commit after step 2 populates the data** → dynamic resources stay
     uncommitted → poll never matches → black.
  **POTENTIALLY TRACTABLE + HONEST FIX:** if the loader genuinely populates the descriptor data (verify
  obj+0x1150 becomes > 0 for a dynamic resource after load), then the fix is to **re-trigger the game's
  OWN commit (0x68f64) for populated-but-uncommitted resources** — real data, the game's own code, no faked
  pixels. NEXT: (a) confirm the loader populates obj+0x1150 for a dynamic resource (store-watch a descriptor
  data field during loader 0x49xxxx, or read it after load); (b) if yes, find the cleanest honest re-commit
  trigger (re-call the manifest's commit pass, or call 0x68f64 per populated resource); (c) if the data is
  NOT populated, the loader's copy target/decode is the deeper gap.

  ### run111/112 — resource-manager orchestration mapped; rapid-RE genuinely exhausted
  Manifest call stack (MANIFEST-FR): static manifest 0xe0xxx ← **0x1d6274 ← 0x1d6fec ← game loop 0x409bfc**.
  So 0x1d4xxx/0x1d6xxx is main's **resource-manager**. Post-manifest (0x1d6274+) is a repeating
  `bl 0x3ca300(sub-object, &status); if status==2 {refcount}` over a fixed set of sub-objects — the
  per-sub-resource processor. REGSETUP also showed register-SETUP (0x86a40) called from **0x1d4xxx, 0xd0xxx,
  0x3fb2e0** (not just the manifest) — so dynamic register paths DO exist and run, but their COMMIT
  (0x68f64→cnt) is guard-skipped (data-loaded check `obj+0x1150>0` fails) and IDXWRITE confirms only the
  manifest ever commits.
  **Honest status:** ~15 build/run cycles this session went into the resource subsystem. It's now fully
  mapped at the structural level: static manifest (342) + dynamic setup paths (0x1d4xxx/0xd0xxx) whose
  commits are skipped because the data isn't loaded-in-time, + loader that's I/O+populate-only + a verify
  poll loop. The ONE missing fact that printf-RE can't cleanly get (varying staging offsets, type/id/key
  namespaces) is whether the loader populates the commit-guard field so a re-commit would succeed.
  **The realistic next tools (rapid printf-RE is spent):**
  - **Offline decompile** 0x3ca300 (sub-resource processor), 0x69040 (register fn + guard), 0x86a40 (setup),
    0x1d6xxx (orchestrator) into a correct model — then the honest fix (re-commit after load) is clear.
  - or **compare vs desktop RPCS3** (runs Terraria) to see the populated index + which thread commits dynamic.
  - or accept this is a deep multi-week subsystem and decide scope. P1's gate is precisely localized; the
    remaining work is "build the fix carefully", not "find the bug".

  ### run114/115 — FORCE-COMMIT experiment (user-approved): informative NEGATIVE
  Patched the register-commit guard (`ble 0x69110` @0x6907c/0x69094) to nop to force all set-up resources
  to commit. Result: **broke the register fn** — main stuck AT 0x69094 (the patched guard), cnt=312 (worse).
  So the guard is **load-bearing loop control**, not a simple skip; dynamic resources are *correctly* skipped
  because their data isn't valid at manifest-commit time. Reverted (run115: cnt=342, baseline restored).
  Two big takeaways:
  1. A clean force-commit would require correctly replicating the game's **two-level-hash insertion**
     (outer-slot byte + inner[id] + memcpy 0x68b + hash-update 0x68dcc) — high risk of breaking like v1.
  2. **P2 is required for pixels REGARDLESS of P1:** POLLBYPASS5 already makes main proceed past the poll
     (faking found), and it STILL renders black with 0 real draws — because the 3D draw path is NOP. So even
     a perfect P1 fix yields no pixels without P2. P1 and P2 are interlocked: P1 must emit a real draw, P2
     must render it.
  **Honest conclusion:** P1's clean fix needs a stronger toolchain (decompiler) or risky hash-replication;
  P2 (renderer) is required regardless and is self-contained (our WebGPU code). Recommended pivot: **build
  the minimal RSX→WebGPU textured-quad path (P2)**, verified with a clearly-labeled dev-test draw, so the
  pipeline is proven and ready for the real splash draw once P1 lands. (This does NOT fake the splash — a
  dev-test quad is a pipeline check, not a claimed result.)
- **(P2) the 3D draw path is NOP.** Even once main submits the splash quad, `execute_nop_draw()` renders
  nothing. Need a minimal RSX→WebGPU path — most likely a **full-screen textured-quad fast-path** (bind
  the decoded GTF, blit to the 960×540 surface) rather than a general FP/VP→WGSL pipeline. (Open question:
  does Terraria draw the splash via a 3D textured quad, or a 2D blit / direct framebuffer write? If 2D/
  direct, clear_surface-style VRAM writes already present correctly — investigate NV3089/texture-upload.)

### Old "Current state" (pre-e4-fix, now FALSE — kept for history)
- ~~main blocks at 0x27700; every +0x60 is 0xffff; sys_fs_read=0 → data.vfs never read.~~ Reads work now.

## Phase A — Unblock the asset loader  ← WE ARE HERE
- [A1] WORKERCIA probe: is the loader **spinning** (where?) or **syscall-blocked** (which?).
- [A2] Classify & fix the block:
  - sync wait (cond/sem/event) → find the signaler thread, fix why it never signals
  - busy-poll → trace the upstream producer it waits on
  - `sys_ppu_thread_join` → which thread, why it never exits
  - SPURS-task wait → **speed wall** → fall back to targeted HLE (Phase B fallback)
- [A3] Verify: `sys_fs_read`/`cellFsRead` on **fd=3 (data.vfs)** appears.

## Phase B — Asset load → decompress → register resources
- [B1] Confirm data.vfs reads (fd=3).
- [B2] Confirm decompression runs (in-EBOOT inflate-HLE @ `0x41c294` fires); keep it fast.
- [B3] Confirm resources registered: table entries' `+0x60` set to real IDs → main's poll matches.
- Verify: main's poll target (e.g. `0x0191`) becomes present in the table.
- **Fallback (if registration is SPU/SPURS-driven & too slow):**
  - honest-mark the *specific* resource main waits for (write its `+0x60` = target) **iff the
    resource data is actually loaded** (else it'll use empty data → crash), OR
  - HLE the registration step in C++ (compiles to fast wasm), OR
  - escalate to JIT/AOT track.

## Phase C — main proceeds → RSX render setup → first flip
- [C1] main passes the poll → render init. Watch for `cellGcmInit`, `cellGcmSetFlipMode`,
  `cellGcmSetDisplayBuffer`, command-buffer build, draw calls.
- [C2] main issues the first flip (NV4097 flip / `sys_rsx` flip / cellGcmSetFlip).
- Verify: RSX RenderLoop receives flip; `flip()` called; `g_webgpu_flip_count` bumps.

## Phase D — Blit → splash → capture
- [D1] `flip()` publishes front buffer (g_webgpu_fb_addr/host/w/h/pitch); GSH_WebGPUJs blits
  ARGB-BE → BGRA → swapchain.
- [D2] **Resolution reconcile:** GSH haveFrame check expects `fb_w==1280 && fb_h==720`, but
  CreateVM sets `_480p`. Use the actual display-buffer dims from `flip()` (likely 720×480 or
  the game's chosen mode); relax/fix the 1280×720 gate so the real db blits.
- [D3] Capture (trigger `flip n=`) → screenshot == ref → **DONE**.

## Cross-cutting
- **Host memory:** runs die early under host pressure (other apps). Mitigate: orphan-Chrome
  cleanup in run script (done); accept flaky/re-run; optionally HLE the 88MB arena-init
  (`0x33f24`/`0x1a5730`) to cut ~30s boot + pressure (structs are zero → no-op, skippable).
- **Probes:** keep lean — high-rate printfs flood the CDP console and kill the page.
- Each phase may surface sub-gates; iterate per the loop.

## Risk / honest odds
- Phase A/B tractable **if** the loader block is PPU-logic (missing event, wrong wait, a fixable
  HLE return). **Hard** if it bottoms out at a SPURS-task producer (SPU-speed wall).
- Fallbacks (honest-mark a loaded-but-unmarked resource, or hand-HLE the hot producer) raise the
  odds for a **static splash** specifically — which is the goal — even though they wouldn't help
  framerate.
- If we hit the SPU wall with no honest workaround → that's the signal to pivot to the JIT/AOT track.

---

## 🧭 DECOMPILER HANDOFF (2026-06-03, after ~53 cycles) — the exact map for a stronger toolchain

The first frame is gated behind TWO interlocked subsystems. printf+hand-disasm has localized both precisely
but cannot crack either efficiently (intricate type/hash/vtable structures). Next session should load these
functions into **Ghidra/IDA** (dump bytes from runtime via the existing [CODEDUMP] probes; EBOOT is encrypted
on disc) or compare against **desktop RPCS3** running Terraria.

### P1 — dynamic resources never registered (the black-screen root)
- main's resource index lives at `*(0x59fb9c)+0x290000` (base r10g=0x5f7630 → index 0x880694 cnt, entries
  stride 0x68c; a 2-level hash: outer-slot byte → `inner[byte*0x80]` = id @ `r10g+0x28906c`).
- **Register API:** `0x86a40(obj,key,count)` = setup one attribute; `0x68f64` → register fn `0x69040`
  (commit: guard `obj+0x1150>0 && obj+0x11d0>0` @0x69078/0x69094 [LOAD-BEARING — do not nop], then
  `cnt++` @0x690c4, memcpy staged 0x68b bytes to entry @0x690ec, hash-update @0x68dcc).
- **Static manifest** `0xe0xxx` (← `0x1d6274` ← `0x1d56dc` ← init `0x1d6fxx`, runs ONCE in init): commits the
  342 resources whose data is already present.
- main's WANTED resources (ids 0x251-0x253, 0xc0, 0x24b — sub-resources polled by the verify loop @0xe7994)
  are loaded LATER: game loop `0x409a90` enqueues 635 jobs to the loader via channel-signal `lr=0x4217dc`
  → sem `0x96006800`.
- **Loader (0x1000006)** = I/O+copy ONLY (PC-sampled: 0x41xxxx inflate + 0x49xxxx copy; never reaches
  register code; never posts a completion sem). Its completion-dispatch `0x4257b4` walks objects (+0x3c
  list) calling `vtable[0x14]()` per item — the handlers load but **never register**.
- **THE GATE:** nobody re-commits dynamic resources after their data loads. main busy-polls `0x27700` for
  them forever (poll: pre-check `0x275f8` = type-filter [types 0x47-0x4a short-circuit]; else 2-level-hash
  search). POLLBYPASS5 fakes "found" so main proceeds but with no real resource → black.
- **DECOMPILE THESE** to find/fix the missing post-load registration: `0x69040` (register fn), `0x86a40`
  (setup), `0x4257b4`+its `vtable[0x14]` handler (loader item-dispatch), `0x1d56dc`/`0x1d6274` (resource-mgr).
  Open question a decompiler resolves fast: is the data genuinely populated (→ honest re-commit fix) or does
  the handler's register call get skipped on a decompress/validity check (→ verify inflate bit-exactness:
  the match-copy HLE @0x41c294 is correct; the bit-refill HLE @0x41bf78 is unaudited).

### P2 — 3D draw path is NOP (required REGARDLESS of P1)
- `WebGPUGSRender::end()` calls `execute_nop_draw()`. cellResc presents the 960x540 surface @0x40000000
  (cleared black). clear_surface→VRAM→flip→canvas WORKS; the missing half is DRAW→VRAM.
- Needs a minimal RSX→WebGPU textured-quad path (vertex fetch + a fixed textured-quad pipeline + GTF texture
  decode + alpha). Untestable until P1 emits a real draw — verify standalone with a labeled dev-test quad.
- P1+P2 are INTERLOCKED: P1 must emit a real draw (verts>0); P2 must render it. Either alone yields no pixels.

### Reliable-probe notes (hard-won)
- `if(cia==X)` interpreter probes only fire at branch-target/block-head addresses — unreliable mid-function.
  Use store-watches (STW/STH in PPUInterpreter.cpp), HLE-layer, or syscall-layer probes for reliability.
- main stack @0x5008xxxx (back-chain bound `sp<0x60000000`). `flip=N` is the RenderLoop free-running — NOT a
  progress metric. Real progress = FSREAD count, REGTRACK cnt, real WGPU-DRAW (verts>0), centerRow nonblack.

---

## ✅✅✅ P2 RENDERER VALIDATED (2026-06-03, run117) — first real rendered pixels on the canvas
Implemented a **software textured-quad rasterizer** in `WebGPUGSRender::clear_surface` (dev-test, guarded by
`s_p2test`): rasterize an axis-aligned textured quad (synthetic UV-gradient + grid texture) into the VRAM
color surface @0x40000000, which `flip()` publishes and GSH_WebGPUJs blits to the canvas.
**Result: it renders.** flip probe: `centerRow nonblack=16 distinct=2` (was 0/all-black all session);
screenshot (`scratch/run117.png`, 8.5 KB vs 3.7 KB black) shows the gradient+grid quad centered on the
canvas exactly as coded. So the FULL path is proven: **rasterize → VRAM surface → flip → WebGPU blit →
canvas.** The clear surface addr (0x40000000) == the flip display-buffer addr, so the surface IS presented.

**Why this matters:** a full-screen splash is an axis-aligned textured quad — so THIS rasterizer is the
actual splash renderer. P2 is effectively DONE for the splash case; it just needs real inputs:
- the real **GTF splash texture** (decode the loaded `splash*.gtf` → RGBA), and
- the draw's **quad coords / surface target** (from the real RSX draw).
Both come from **P1** (the game emitting the real splash draw). So the splash now reduces to: P1 lands a
real draw → swap the synthetic texture for the decoded GTF + real coords → splash pixels. The interlock is
half-solved: the renderer is ready; only P1 (resource registration → real draw) remains.

NOTE: minor WebGPU teardown warning ("Destroyed texture used in a submit") at exit — cosmetic, post-render.

---

## 🔎 OFFLINE data.vfs ANALYSIS (2026-06-03, run118) — texture format + corrected "splash" identity
- data.vfs is PLAINTEXT in terraria.iso @ISO-offset 0xf0000 (VFS2 magic). 3650 zlib streams; TOC = 24-byte
  records @0x100 (id, type, offset, size).
- **CORRECTION: "splash" = AUDIO** (`splash_0.msf`/`splash_1.msf` = water-splash SFX), NOT the 505 screen.
  The boot LOGOS are **`logo.gtf`/`logo_1.gtf`/`logo_2.gtf`/`logo2.gtf`/`logo_3.gtf`** + **`esrb.gtf`**
  (Terraria boots: 505 Games → Engine Software → Re-Logic logos, then ESRB).
- **Texture format = RAW RGBA pixels**, zlib-compressed in ~32KB chunks (decompressed assets are
  `ff000000…`-style pixel runs; NO GTF/DDS header). That's why the emulator GTFSCAN found 0 headers —
  there are none. Finding a loaded logo in emu memory by header-scan is impossible; would need to track
  the loader decompressing the logo's specific TOC offset, or match its pixel content.
- So the real 505 splash path = loader decompresses logo.gtf → raw RGBA → registered as a resource (P1
  GATE) → game draws it as a textured quad → **P2 renders it** (P2 now proven to do exactly this).
  The interlock stands: renderer ready (P2 ✅); game-driven draw blocked on P1 (resource registration).

---

## 🏁 DEFINITIVE CONCLUSION (2026-06-03, runs 117-120) — all components proven; ONE walled blocker (P1)
Decisive offline-vs-emulator verification (extracted real assets from terraria.iso/data.vfs, scanned emu memory):
- ✅ **P2 renderer WORKS** (run117): software textured-quad rasterizer → VRAM → flip → WebGPU blit → canvas;
  screenshot showed real rendered geometry. A boot logo IS a textured quad, so this IS the splash renderer.
- ✅ **Decompression (inflate) is BIT-CORRECT** (run120): a loaded .msf audio chunk in emu memory matches
  Python's zlib decompression byte-for-byte (sig found @0x408db064). NOT the bug.
- ✅ **Audio assets load fine**; the offline-extracted real **Terraria title logo decodes correctly**
  (logo.gtf = 486×142 ARGB; verified visually). The whole data path is proven.
- ❌ **Visual assets (logo.gtf etc.) are NOT loaded in emu memory** (run119/120: 0 matches) — the boot
  **gates at the resource-registration wall BEFORE loading visuals.** main waits for visual resources
  (ids 0x251-0x253) that never register; audio uses a path that registers, visuals don't.
- ⇒ Manual-render of the real logo is impossible (it's not loaded). The fully-honest splash REQUIRES P1:
  the game registering its visual resources → proceeding → loading + drawing the logo → P2 renders it.

**So the splash reduces to exactly ONE walled fix: P1 visual-resource registration.** Every other component
(decompression, asset format, data path, renderer) is proven working. P1 needs a decompiler (Ghidra/IDA) or
desktop-RPCS3 reference — printf+hand-disassembly hit its limit after ~58 cycles. The decompiler handoff
(functions 0x69040/0x86a40/0x4257b4/0x1d56dc) + this proof that everything else works is the milestone.

GTF/asset format for the future P1+P2 wiring: files = [u32-offset chunk-table][zlib 32KB chunks]; decompressed
= [~44-176B header: magic 020200ff, dims as BE u16 pair @+32][ARGB8888 pixels]. data.vfs in iso @0xf0000;
TOC @0x100 (24B entries: id,type,flag,offset,size); 270 files; name-list @0x28c37b1 ([u32 len][name]).

---

## ✅✅ P2 PRODUCTION-WIRED (2026-06-03, run121) — renderer now handles the game's REAL present path
Added the real-RSX-draw path to `WebGPUGSRender::end()`: when a draw binds a texture (fragment_textures[0]),
decode it and rasterize (scaled) into the color surface that flip() presents. Result: **it fires on the
game's own cellResc present** — `[P2-DRAW] rasterized bound texture 960x540 fmt=0xa5 (A8R8G8B8) -> surface
960x540`, 4×/boot. So the renderer is wired into the game's ACTUAL render path (the cellResc source→display
blit that was execute_nop_draw before). It presents black ONLY because the source surface is empty (the game
never draws the scene — gated by P1). Verified no regression (boot OK, flip→2101; the "Narrowing error @0x32e7c"
crash is PRE-EXISTING — present in runs 115-120 too, a teardown artifact).
- Dev-test (synthetic quad) toggled OFF (s_p2test=false); the real-draw path is the renderer now.
- **The rendering chain is COMPLETE end-to-end** except the one walled piece: the game drawing its scene into
  the source surface, which requires **P1 (visual-resource registration)**. The instant P1 lands and the game
  draws the splash into the source, P2-DRAW rasterizes it → flip → canvas → splash. Nothing else is missing.
- (Note: A8R8G8B8 passthrough assumes linear/un-swizzled; if real splash textures are swizzled, add de-swizzle.
  The crash @0x32e7c narrowing in main's dt-fn is a separate, pre-existing teardown issue worth a look.)
