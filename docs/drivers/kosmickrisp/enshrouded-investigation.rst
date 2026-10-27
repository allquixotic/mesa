Enshrouded on athei Wine: experimental KK investigation
======================================================

Status as of 2026-10-27
----------------------

This branch publishes an experimental patch stack against athei/mesa
``4d597245b76aa01bfda433da5e1c95d0e036f045``. One user-tested profile loaded
Enshrouded with HDR and Performance settings and reportedly delivered high FPS
on an Apple M5 Max with macOS 27.0.1 and athei Wine ``arm64-11.20-3``. Gameplay
was brief, not a long-session stability or FPS benchmark. No full Vulkan CTS
run was performed on this patch stack.

**Serialized Metal compilation is a temporary workaround, not a proven
root-cause fix.** Metal intentionally supports concurrent compilation. The
lock may mask a timing or resource-lifetime defect elsewhere. The original
HDR issue of frames arriving one refresh late remains unresolved.

Contents and attribution
------------------------

* Separate presentation queue adapted from upstream Mesa MR 45568, proposal
  head ``db64cca01164c3fa458dd3ca8e6b28500533ca03``:
  https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/45568
  Queue completion accounting, idle/destruction and atomic-mode transitions
  were adapted for athei's branch. Existing drawable residency is preserved.
* Exact GPU-generated indirect commands execute the current
  ``min(count, maxDrawCount)``. Eligibility thresholds are fallbacks to the
  original implementation, not draw-count caps. Ordinary render pipelines
  retain upstream defaults; ICB variants are compiled lazily. No new shader
  precision relaxation or Enshrouded-specific correctness exception is added.
* Optional narrower compute barriers retain conservative transfer, meta,
  host, indirect and cross-encoder behavior.
* Metal WSI acquire waits for the selected offscreen image's previous
  presentation-copy fence before image reuse. A new drawable does not prove
  the offscreen image is reusable. Timeout/error paths preserve acquire state.
  This addresses a code-level synchronization gap, but did not cure this game
  timeout by itself.
* Optional exact draw-based color clears use existing meta rendering and
  preserve subresources, conditional-rendering semantics and query isolation.
* Opt-in queue/GPU recording retains diagnostic data across command-buffer
  reuse. Shader capture is bounded to 256 MiB per process and stays local.
* Opt-in process-wide serialization of the normal shader-library, compute
  pipeline and render pipeline creation calls in ``mtl_compiler.m``. Compiler
  inputs/options are unchanged. The separate ICB bootstrap-library creation
  is outside this lock; the successful game profile disables ICB.

The additional work was developed with LLM assistance; commits carry
``Generated-by: LLM``. Original athei, Mesa and MR authors retain credit for
inherited work. This publication is in a personal GitHub fork, not an upstream
Mesa merge request.

Successful local game profile
-----------------------------

Use these **only for the game process**, through Wine's compatibility database::

   MESA_KK_EXACT_ICB=0
   MESA_KK_PRESENT_QUEUE=1
   MESA_KK_NARROW_COMPUTE_BARRIERS=0
   MESA_KK_DRAW_COLOR_CLEAR=1
   MESA_KK_SERIALIZE_COMPILATION=1
   MESA_KK_DIAGNOSTICS=1
   MESA_KK_GPU_PROGRESS=0
   MESA_KK_TRACE_QUEUE=0
   MESA_LOG_LEVEL=warning

Do not set a shader-capture directory or enable game breadcrumbs for this
profile. The integration recipe is published in
https://github.com/allquixotic/mach9/tree/enshrouded-kk-integration .
Steam/Zink can retain the bundled driver while the game selects this one.

The branch's original experimental defaults enable exact ICB, the private
queue and narrower compute barriers. **Those defaults are not the successful
Enshrouded profile above.** Compilation serialization, draw clears and the
recorders default off. Explicitly select the documented profile for testing.
The exact ICB optimization has standalone test coverage but is disabled in
the successful game run; no game FPS improvement is attributed to ICB.

Controlled game observations
----------------------------

All comparisons used the same world, HDR and Performance settings. Most failing
profiles were tried once; the fully instrumented clear profile succeeded twice.

* Private queue + ICB + narrow barriers: world-load timeout.
* ICB disabled: timeout. Original compute barriers restored: timeout.
* Original shared presentation queue, ICB off, original barriers: world loads.
* Private queue with acquire/reuse synchronization correction: timeout.
* Queue recorder, game breadcrumbs and GPU timestamp diagnostics: still timeout.
* Draw-clear candidate plus all diagnostics and shader capture: loads twice,
  but reported gameplay performance is poor.
* Same draw-clear driver with diagnostics removed: timeout.
* Game breadcrumbs restored alone, then GPU recorder restored: timeout.
* Full successful diagnostic profile with **only shader capture removed**:
  timeout. This isolates capture-related timing as an important factor in the
  observed runs, not proof that concurrent Metal compilation is defective.
* Serialized compilation with expensive diagnostics off: one successful world
  entry and high FPS reported, followed by normal shutdown. Startup was slow.

The last failing recordings show approximately four-second gaps entering a
1512x982 RGBA16Float render pass, even when the six-vertex clear draw is active.
One example has 68 nonzero samples out of 73, a 4000.663 ms gap and 4017.941 ms
commit duration. Game breadcrumbs sometimes identify ClearTexture, sometimes
CompositeFrame, and sometimes contain no entries. Timestamps emitted during
GPU recovery do not prove that all earlier operations succeeded. Queue traces
have not demonstrated a circular dependency. Precise GPU timestamps can change
execution and are unsuitable for comparing gameplay performance.

The source-capture mutex runs *after* synchronous library compilation, around
file operations. The workaround instead serializes Metal compiler calls.
These are different interventions: success supports a concurrency/timing
hypothesis but does not identify the precise missing dependency. The next
investigation should narrow the affected operation and restore parallelism,
not treat serialization as a general Metal requirement.

Additional startup problem
--------------------------

The game rejects a 1,373,028,924-byte Vulkan pipeline cache because its limit
is 1,073,741,824 bytes. It then rebuilds and saves another oversized cache.
This can force repeated compilation on subsequent launches. The successful
run reached world entry about 23 minutes after process startup; that total
cannot be attributed entirely to compiler serialization. No cache truncation,
game executable patch or cache-limit override is included here.

Validation and limits
---------------------

* Exact indirect probes: changing GPU-written counts, indexed/non-indexed,
  conditional rendering, robust access fallbacks, overlapping submissions,
  DrawID/base values, zero draws, and maxima larger than the current count.
  Targeted raster readback, compute dependencies and Wine/FEX runs passed.
* Six-case acquire regression: the actual unpatched acquire function fails
  the pending-image reuse case; the corrected source passes finite/zero/infinite
  timeout, device loss, successful reuse and first-use cases.
* Thirteen exact-clear cases: six formats, mips/layers, partial/remaining
  ranges, conditional rendering and statistics-query isolation; native and
  Wine/FEX passed. Six concurrent recording threads also passed with Metal
  API validation under the serialization option.
* Twelve mixed direct/indirect draw cases and compute/deferred-copy checks
  passed with the successful game settings (ICB disabled).
* A 600-frame clear/blit probe used the observed HDR dimensions/heap offset
  and verified 1,484,784 final pixels. It did not reproduce the game hang.
  Its window was behind others and yielded no presentation timing samples;
  this does not validate sustained visible HDR presentation or game pacing.

Portable source probes are in ``src/kosmickrisp/tests/experimental``. They
are targeted regression tools, not substitutes for Vulkan CTS. No game
binaries, generated game shaders, saves, crash dumps, raw personal logs or
credentials are included.

Source/build provenance
-----------------------

The successful local signed dylib has SHA-256
``8697abe9ea22f5377413b098164ebf7d81d461bd50c54fd610f0186c7aee07b0``.
It was built from this runtime-source snapshot while changes were uncommitted
on the athei base. Publishing commits changes Git-derived build identity;
rebuilding these commits need not produce the same binary hash or cache UUID.
The publication does not replace or rebuild the user's installed runtime.
