/* sd_offsets.h -- Sonic Dash / libunity.so 6000.0.72f1 engine offsets
 *
 * EVERY VALUE HERE WAS DERIVED FROM YOUR BINARY. Nothing is inherited from the
 * donor port. See PORTING.md section 2 for the method and section 3 for the
 * per-site evidence.
 *
 * Target:  com.sega.sonicdash, libunity.so BuildID 6618ff361333edc4
 *          Unity 6000.0.72f1, IL2CPP, metadata v31, arm64-v8a
 *          .text @0x43c450, 15,186,928 bytes, 4 PT_LOADs
 *
 * Reference used for derivation (NOT the same build -- see PORTING.md):
 *          libunity.so / libunity_sym.so BuildID aec5f4ea906b4626bc5230f734e2e2ea2c06ec7f
 *          same engine version, unstripped, .text 19,742,288 bytes
 *
 * All values are LINK-TIME RVAs into libunity.so. At runtime:
 *      runtime_addr = libunity_virtbase + RVA
 * and libunity_virtbase is printed by the loader on the first line of debug.log.
 *
 * GUARDS. Each site carries the first one or two instruction words as they
 * actually appear in YOUR binary. sd_patches.c refuses to write any site whose
 * guard does not match and logs the mismatch instead. A stale offset after a
 * game update therefore produces a logged refusal, not a jump into the middle
 * of an unrelated function. Re-derive with tools/derive2.py; do not hand-edit.
 */
#ifndef SD_OFFSETS_H
#define SD_OFFSETS_H

/* AUDIT NOTE. An earlier revision of this header set SD_TM_Update_W1 and
 * SD_VSFR_W1 to the W0 word: the generator emitted word 0 for both slots. The
 * TimeManager guard would therefore have REFUSED the clock detour on every
 * boot, leaving the engine clock unhooked. Found by tools/audit_guards.py,
 * which evaluates every guard in sd_patches.c against the binary. Run it after
 * any re-derivation. */

/* ------------------------------------------------------------------------
 * Entry points -- libunity's JNI surface
 * ------------------------------------------------------------------------ */
#define SD_OFF_JNI_OnLoad                         0x00751a34
#define SD_JNI_OnLoad_W0                          0xd10083ffu
#define SD_OFF_initJni                            0x00750884
#define SD_initJni_W0                             0xa9be57feu
#define SD_OFF_UnityPlayerLoop                    0x0074fbb4
#define SD_UnityPlayerLoop_W0                     0xd10103ffu
#define SD_OFF_nativeUnityPlayerSetRunning        0x00751988
#define SD_nativeSetRunning_W0                    0xa9be57feu

/* initJni and nativeUnityPlayerSetRunning share the same first two words
 * (a9be57fe / a9014ff4). Their guards are therefore NOT individually
 * selective -- they were separated during derivation by whole-body scoring
 * (1.0 vs 0.905), not by the prologue. Treat the guards here as corruption
 * checks only; if you re-derive, trust derive2.py's margin, not these words. */

/* ------------------------------------------------------------------------
 * Engine clock -- TimeManager
 * ------------------------------------------------------------------------ */
#define SD_OFF_TimeManager_Update                 0x00583d74
#define SD_TM_Update_W0                           0xf940b008u
#define SD_TM_Update_W1                           0xb9416809u   /* ldr w9, [x0, #0x168] */

/* Field offsets, read straight off the first three instructions of Update:
 *      ldr  x8,  [x0, #0x160]     frameCount   u64
 *      ldr  w9,  [x0, #0x168]     aux counter  u32
 *      ldrb w10, [x0, #0x1a8]     paused       u8
 * Struct layout is identical between the two builds (same engine version), so
 * these transfer directly and need no fingerprinting. They are also pinned by
 * the W0/W1 guards above, which encode #0x160 and #0x168 in their immediates. */
#define SD_TM_FIELD_FRAMECOUNT                    0x160   /* u64 */
#define SD_TM_FIELD_AUX                           0x168   /* u32 */
#define SD_TM_FIELD_PAUSE                         0x1a8   /* u8  */

/* The body proper begins after the early-out at +0x24:
 *      +0x1c  cbz  w10, +0x24     paused -> skip
 *      +0x20  ret
 *      +0x24  sub  sp, sp, #0x90  <-- real frame starts here
 * A trampoline that wants the unpaused path must enter at +0x24, and the frame
 * it must restore is 0x90 with d9/d8 at +0x70 and x30/x19 at +0x80. */
#define SD_TM_UPDATE_BODY_ENTRY                   0x24
#define SD_TM_UPDATE_FRAME                        0x90

#define SD_OFF_TimeManager_SetTimeScale           0x00584658
#define SD_TM_SetTimeScale_W0                     0xd10303ffu

/* ------------------------------------------------------------------------
 * Frame pacing -- target frame rate, vsync, Swappy
 * ------------------------------------------------------------------------ */
#define SD_OFF_GetVSyncBasedTargetFrameRate       0x005835a4
#define SD_VSFR_W0                                0xfc1e0fe8u
#define SD_VSFR_W1                                0xa9014ffeu   /* stp x30, x19, [sp, #0x10] */

#define SD_OFF_WaitVSync                          0x0073d57c
#define SD_WaitVSync_W0                           0xf81d0ffeu

#define SD_OFF_Swappy_UpdateSwapInterval          0x00729890
#define SD_SwappyUSI_W0                           0xf81d0ffeu
#define SD_OFF_Swappy_GetTargetFrameRate          0x007297d8
#define SD_SwappyGTFR_W0                          0xfc1e0fe8u

/* Swappy::IsEnabledAndActive() did NOT resolve -- 3 instructions in the
 * reference and no match in the game. Most likely inlined into its callers by
 * this build. It is not needed: androidUseSwappy is off in this title's
 * PlayerSettings, and the two Swappy entry points above are patched to a
 * constant anyway. Do not invent an offset for it. */

/* ------------------------------------------------------------------------
 * Android system probes -- /proc, CPU topology, physical memory
 * ------------------------------------------------------------------------ */
#define SD_OFF_GetBigLittleConfiguration          0x00730394
#define SD_GBL_W0                                 0xfc190fe8u

#define SD_OFF_GetPhysicalMemoryMB                0x00740a18
#define SD_PHYSMEM_W0                             0xf81f0ffeu

#define SD_OFF_GetCachedSystemMemoryInfo          0x007694d8
#define SD_OFF_GetCachedProcessMemoryInfo         0x007696bc
#define SD_PROCFS_READER_W0                       0xa9bf4ffeu

/* The two GetCached*MemoryInfo functions are byte-identical in shape and
 * differ only in which BSS slot they cache into. Engine stripping moved those
 * slots, so strict matching found NEITHER. They were recovered by shape-only
 * matching (tools/derive3.py) which produced exactly two candidates, then
 * separated by the recovered slot offsets:
 *      0x007694d8  slot +0x440   <- lower address, lower slot  = System
 *      0x007696bc  slot +0x540   <- higher address, higher slot = Process
 * The reference has the same ordering on both axes (0xa80164/+0x640 System,
 * 0xa80348/+0x740 Process, also 0x100 apart), which is two independent
 * confirmations of the assignment rather than one. */
#define SD_SYSMEM_SLOT                            0x440
#define SD_PROCMEM_SLOT                           0x540

/* What to report. The Switch gives a homebrew title ~3.2 GB in title-override
 * mode; report a conservative figure the engine's heuristics can live with. */
#define SD_PHYSMEM_MB                             2048
#define SD_SYSMEM_TOTAL_BYTES                     (2048ull << 20)
#define SD_SYSMEM_AVAIL_BYTES                     (1536ull << 20)
#define SD_PROCMEM_RESIDENT_BYTES                 (512ull  << 20)

/* ------------------------------------------------------------------------
 * Choreographer -- Unity 6's frame-pacing path (see PORTING.md s.4)
 * ------------------------------------------------------------------------ */
#define SD_OFF_ChoreographerJava_ctor             0x00743248
#define SD_CHOREO_CTOR_W0                         0xd10103ffu
#define SD_OFF_ChoreographerJava_Enable           0x00743508
#define SD_CHOREO_ENABLE_W0                       0xd10083ffu
#define SD_OFF_ChoreographerJava_Disable          0x00743544
#define SD_OFF_ChoreographerJava_HandleMessage    0x00743580
#define SD_CHOREO_HANDLEMSG_W0                    0xd100c3ffu

/* Enable() and Disable() are 15 instructions with identical shape and the same
 * guard word. Separated by whole-body score (each scores 1.0 at its own site
 * and 0.933 at the other) plus address ordering matching the reference
 * (Enable < Disable in both binaries). If you re-derive and the two swap,
 * something is wrong -- stop and read the disassembly. */

/* ------------------------------------------------------------------------
 * Audio -- output selection and the AudioManager property probes
 * ------------------------------------------------------------------------ */
#define SD_OFF_GetAndroidAudioOutputType          0x0074cee8
#define SD_AUDIOOUT_W0                            0xd10203ffu
#define SD_AUDIOOUT_OPENSL                        2   /* force the OpenSL path */

#define SD_OFF_GetNativeOutputSampleRate          0x0074ccb0
#define SD_OFF_GetNativeOutputFramesPerBuffer     0x0074cb74
#define SD_AUDIOPROP_W0                           0xd10103ffu   /* shared by BOTH */

/* The shared W0 above CANNOT tell the two getters apart -- they share their
 * first two words, so if the offsets were ever swapped every guard would
 * still pass and each would silently return the other's value. The word at
 * +0x10 is the BL to the PROPERTY_* accessor, which differs between them, so
 * it is the guard that actually proves which function is which. */
#define SD_AUDIOPROP_RATE_W4                      0x941ec742u   /* bl fPROPERTY_OUTPUT_SAMPLE_RATE       */
#define SD_AUDIOPROP_FRAMES_W4                    0x941ec730u   /* bl fPROPERTY_OUTPUT_FRAMES_PER_BUFFER */

/* These two are the same function calling a different property accessor, and
 * the distinguishing instruction is a BL -- which the fingerprint masks. They
 * tied at 0.974 and were separated by following the BL at +0x10 to its target
 * and reading the string that target constructs:
 *      game 0x0074ccb0 -> 0x00efe9c8 -> "PROPERTY_OUTPUT_SAMPLE_RATE"       (0xe4ca1)
 *      game 0x0074cb74 -> 0x00efe844 -> "PROPERTY_OUTPUT_FRAMES_PER_BUFFER" (0x1613b1)
 * That is a direct read of the binary, not an inference. */

/* NO LONGER PLACEHOLDERS -- both now follow from this project's settings.
 *
 * SAMPLE RATE 48000. The project's AudioManager has m_SampleRate = 0, which
 * means "use whatever the device reports". So the value we report IS the
 * mixer rate; there is no project rate to match. 48000 is the Switch's native
 * output rate, so the SDL device opens with no resampling at all.
 *
 * Do NOT copy Colour Sheep's 24000. That was Colour Sheep's own project
 * setting -- it reported 24000 because its game selected 24000, and reporting
 * anything else made Unity log "Forced to initialize FMOD to the device
 * driver's system output rate". Here the project defers to us, so the right
 * answer is the hardware's.
 *
 * FRAMES 256. FMOD's OpenSL init (0x00f90a6c) fails with error 60 unless:
 *     rate != 0,  frames != 0,  frames <= (numbuffers - 1) * bufferlength
 * This project's m_DSPBufferSize is 1024, so the bound is >= 1024. 256
 * clears it with margin, and would still clear the tightest "Best latency"
 * setting (bufferlength 256, numbuffers 2: 256 <= 256). */
#define SD_AUDIO_NATIVE_SAMPLE_RATE               48000
#define SD_AUDIO_NATIVE_FRAMES_PER_BUFFER         256


/* ------------------------------------------------------------------------
 * Time / vsync group  (13 sites)
 *
 * Unity 6 RENAMED THIS AREA. If you are carrying offsets forward from a
 * 2020.3-era port, note that GetVSyncTime() and GetFrameTimeNanos() -- both
 * present in battd_nx's table -- DO NOT EXIST in 6000.0.72f1. Their job moved
 * into AndroidVSync::WaitForLastPresentationAndGetTimestamp(). Searching for
 * the old names finds nothing and the temptation is to assume they were
 * inlined; they were not, they were replaced.
 * ------------------------------------------------------------------------ */
#define SD_OFF_TimeManager_SetPause               0x00583a5c   /* 3 insns  */
#define SD_OFF_TimeManager_GetTargetFrameTime     0x00583aa0   /* 39 insns */
#define SD_OFF_TimeManager_EndSyncFrame           0x00583b3c
#define SD_OFF_TimeManager_Sync                   0x00583c88
#define SD_OFF_GetTimeSinceStartup                0x005c68fc
#define SD_OFF_EnableFrameTimeTracker             0x0073d6b8
#define SD_OFF_AndroidVSync_WaitForLastPresentation  0x007213b0

/* EnableFrameTimeTracker is the 2020.3 frame-2 Looper deadlock site that
 * battd_nx patches to a bare `ret`. The same function still exists here and
 * the same reasoning applies: it installs a tracker that wants a Looper.
 * Patch it out if boot stalls around frame 2. */

/* AndroidVSync::UpdateTimeManager() is a single instruction (a tail branch)
 * and therefore has no maskable anchor -- it CANNOT be fingerprinted, and no
 * value is given here. Do not guess one. Reach it through its caller. */

/* ------------------------------------------------------------------------
 * VSync data globals
 *
 * Recovered by positional correspondence rather than by fingerprinting: the
 * game and reference bodies of WaitForLastPresentationAndGetTimestamp agree at
 * 95%, so the adrp/add pairs land at IDENTICAL instruction indices (47, 57, 75,
 * 101) in both. Reading the global off each index maps ref -> game directly,
 * and the reference symbol table then NAMES each one. That is a stronger
 * result than a pattern match: the names are read out of a symbol table, not
 * inferred.
 *
 *   insn[47] read / insn[101] written  -> a read-modify-write, i.e. the counter
 * ------------------------------------------------------------------------ */
#define SD_OFF_AndroidVSync_LastVsyncCounter      0x01397b90   /* .bss,  u64 */
#define SD_OFF_AndroidVSync_LastTimestamp         0x0130cf30   /* .data, u64 */
#define SD_OFF_g_GfxThreadingMode                 0x013cc338   /* .bss,  u32 */

/* CORRECTION -- s_LastVsyncCounter IS NOT THE COUNTER TO TICK.
 * An earlier revision of this header said it was. It is not, and ticking it
 * would make pacing WORSE. Reading WaitForLastPresentationAndGetTimestamp in
 * full shows the loop:
 *     target = s_LastVsyncCounter + interval
 *     current = <live counter>                 (via 0x0073d634)
 *     if (current < target) WaitVSync(target)
 *     s_LastVsyncCounter = target              (str x20,[x19,#0xb90])
 * So s_LastVsyncCounter is "the last target presented". Incrementing it from
 * outside pushes every future target further away. The counter that must
 * advance is the LIVE one below, which WaitVSync actually waits on.
 *
 * g_GfxThreadingMode is a bonus from the same scan. boot.config ships
 * gfx-threading-mode=4; this is where the engine caches it, so it is the place
 * to look (or force) if the threaded renderer misbehaves on the Switch's
 * single GPU queue. */

/* ------------------------------------------------------------------------
 * The LIVE vsync counter -- the one the clock thread ticks at 60 Hz
 *
 * WaitVSync(target) at 0x0073d57c:
 *     lock   (mutex @ 0x0139cac0)
 *   loop:
 *     ldr    x21, [0x0139cb18]          <- live counter
 *     cmp    x21, target ; b.ge done
 *     cond_wait(mutex+0x28, mutex)
 *     b      loop
 * This is exactly the shape clayjam_nx validated against laytonbmr_nx's
 * independently documented OFF_ANDROID_VSYNC_COUNTER. The counter lands in
 * .bss, which is what a counter should be.
 *
 * On Android something ticks this from the display. Here nothing does, so
 * without the clock thread WaitVSync spins on its cond_wait forever -- the
 * classic vsync hang.
 *
 * WHY THE CLOCK THREAD ALSO BROADCASTS. The shim caps even untimed cond_wait
 * at 16 ms (imports.c, COND_WAIT_CAP_MS) and reports it as a spurious wakeup,
 * so a bare increment does eventually get noticed. But "eventually" is up to
 * 16 ms late on every frame, which on a 16.7 ms budget is enough to halve the
 * frame rate in the worst case. Broadcasting the condvar after each tick ends
 * the wait at the tick. It broadcasts only once Unity has initialised that
 * condvar itself: two threads racing through ensure_cond() could each create a
 * cond and orphan a waiter on the leaked one.
 * ------------------------------------------------------------------------ */
#define SD_OFF_ANDROID_VSYNC_COUNTER              0x0139cb18   /* .bss, u64 -- TICK THIS */
#define SD_OFF_VSYNC_MUTEX                        0x0139cac0   /* bionic pthread_mutex_t slot */
#define SD_OFF_VSYNC_COND                         0x0139cae8   /* = mutex + 0x28 */
/* Guards: WaitVSync's own load of the counter, so a stale offset is refused
 * rather than ticked. */
#define SD_OFF_WaitVSync_ldr                      0x0073d5a0
#define SD_WAITVSYNC_ADRP                         0xf00062f6u   /* adrp x22, #0x139c000     */
#define SD_WAITVSYNC_LDR                          0xf9458ed5u   /* ldr  x21, [x22, #0xb18]  */
#define SD_VSYNC_PERIOD_NS                        16666667ull

/* ------------------------------------------------------------------------
 * Choreographer FREE-RUN -- the primary pacing mechanism
 *
 * ChoreographerBase::Get() is the factory that returns either the NDK or the
 * Java choreographer. Patched to return NULL, Unity uses NEITHER and free-runs
 * against the vsync counter above. This is the path colorsheep_nx proved on
 * hardware, and the guard words below are byte-identical to theirs
 * (d10303ff / a90a57fe), with 100% mnemonic agreement to the reference.
 *
 * Two layers, both pointing the same way. This patch keeps Unity from creating
 * either choreographer. If its guard were ever refused, the inherited
 * AChoreographer_getInstance (imports.c) returns NULL, which
 * ChoreographerNDK::Enable null-checks and falls back to loop cadence; the
 * Java choreographer is likewise answered with NULL by jni_fake.c.
 * ------------------------------------------------------------------------ */
#define SD_OFF_ChoreographerBase_Get              0x0073d758
#define SD_CHOREO_GET_W0                          0xd10303ffu   /* sub sp, sp, #0xc0      */
#define SD_CHOREO_GET_W1                          0xa90a57feu

/* TimeManager::Update's real frame starts here; the detour calls it directly. */
#define SD_TM_BODY_W0                             0xd10243ffu   /* sub sp, sp, #0x90 */

/* GetBigLittleConfiguration returns a 16-BYTE STRUCT in x0:x1, not a flag:
 *     x0 = { u32 big_count,  u32 little_count }
 *     x1 = { u32 big_mask,   u32 little_mask  }   (its epilogue builds x1 with
 *                                                   `orr x1, x10, x21, lsl #32`)
 * systeminfo::GetProcessorCount() sums the two counts. An earlier revision of
 * this port stubbed it to `return 0`, which reports ZERO cores -- Unity's job
 * scheduler then sizes parallel-for batches to zero and runs batch 0 over
 * uninitialised descriptors. colorsheep_nx hit exactly that crash. The value
 * below is theirs: 3 uniform cores on 0-2, matching homebrew's core grant. */
#define SD_GBL_X0                                 0x0000000000000003ull  /* big=3, little=0 */
#define SD_GBL_X1                                 0x0000000000000007ull  /* mask 0x7, 0     */

/* ------------------------------------------------------------------------
 * FMOD -- forcing the OpenSL output at the call site FMOD actually reads
 *
 * GetAndroidAudioOutputType -> 2 (above) is NECESSARY BUT NOT SUFFICIENT.
 * Colour Sheep patched only that and FMOD never loaded OpenSL at all -- no
 * dlopen("libOpenSLES.so") in its log -- because the Unity-level enum
 * override does not reliably reach FMOD's own setOutput. The value FMOD
 * consumes is the argument at the call site inside AudioManager::InitNormal:
 *
 *   0x0088d7d0  bl   GetPlatformOutputOverride    (0x008a1224)
 *   0x0088d7d4  tbz  w0, #0, ...
 *   0x0088d7d8  ldr  x0, [x19, #0x178]            ; FMOD::System*
 *   0x0088d7dc  ldr  w1, [sp, #0x2c]              ; <-- PATCH SITE
 *   0x0088d7e0  bl   FMOD::System::setOutput      (0x00f844c4)
 *
 * Rewritten to `mov w1, #0x16` so Unity asks FMOD for OpenSL directly.
 *
 * HOW IT WAS FOUND. setOutput itself cannot be fingerprinted: it is a
 * 13-instruction public wrapper shaped exactly like every other
 * FMOD::System:: method, and it tied FOUR ways (0xf84944, 0xf84680,
 * 0xf84618, 0xf844c4). The scorer ranked 0xf84944 first. That was WRONG.
 * Anchoring on the already-located GetPlatformOutputOverride call and taking
 * the next BL resolves it to 0xf844c4 -- the fourth candidate. The call site
 * sits at instruction index 30 of InitNormal in BOTH builds, and the patched
 * word is byte-identical to the one in the reference.
 *
 * WHY 0x16. Read from THIS binary's GetPlatformOutputOverride (0x008a1224),
 * not inherited:
 *     cmp w0,#1 -> 0x15 AudioTrack     cmp w0,#2 -> 0x16 OpenSL
 *     cmp w0,#4 -> 0x17                default   -> 0x18 AAudio
 * ------------------------------------------------------------------------ */
#define SD_OFF_FMOD_SETOUTPUT_SITE                0x0088d7dc
#define SD_FMOD_SETOUTPUT_FROM                    0xb9402fe1u   /* ldr w1, [sp, #0x2c]  */
#define SD_FMOD_SETOUTPUT_NEXT                    0x941bdb39u   /* bl  setOutput        */
#define SD_FMOD_SETOUTPUT_TO                      0x528002c1u   /* mov w1, #0x16        */
#define SD_FMOD_OUTPUTTYPE_OPENSL                 0x16          /* 22 */

/* Diagnostic anchors -- not patched, but where to look when audio fails. */
#define SD_OFF_AudioManager_InitNormal            0x0088d758
#define SD_OFF_AudioManager_InitFMOD              0x0088cffc
#define SD_OFF_GetPlatformOutputOverride          0x008a1224
#define SD_OFF_FMOD_System_setOutput              0x00f844c4
#define SD_OFF_FMOD_OutputOpenSL_init             0x00f90938
#define SD_OFF_FMOD_OutputOpenSL_registerLib      0x00f90844
#define SD_OFF_FMOD_OpenSL_DriverConfigCheck      0x00f90a6c   /* the error-60 path */

/* If the log says "FMOD failed to initialize the output device (60)":
 *   - no dlopen("libOpenSLES.so") logged  -> registerLib failed, or the
 *     setOutput patch above did not apply
 *   - dlopen logged, slCreateEngine logged -> it is the driver-config check
 *     at 0x00f90a6c: one of the two AudioManager getters returned 0
 * Error 33 (0x21) instead means an OpenSL object call failed -- look in
 * opensles.c, not here. */

#endif /* SD_OFFSETS_H */

