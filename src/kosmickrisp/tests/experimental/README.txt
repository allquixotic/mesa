Experimental standalone regression probes (MIT licensed)

Set VK_DRIVER_FILES to the ICD of the built driver. Supply Vulkan-Headers and
Volk from their upstream projects or a Vulkan SDK; neither is vendored here.
Example, from this directory:

  clang -O2 -I"$VULKAN_HEADERS/include" -I"$VOLK" clear_probe.c "$VOLK/volk.c" -o /tmp/kk-clear-probe
  /tmp/kk-clear-probe /absolute/path/to/libvulkan.1.dylib

Use the same compile command for kk_exact_probe.c or kk_mixed_pipeline_probe.c.
Add -pthread for concurrent_clear_probe.c. For Windows/Wine use a MinGW compiler
with -static; the executable loads vulkan-1.dll and takes no loader argument.
Use a scratch Wine prefix, not a game prefix. The probes do not launch a game.

clear_probe covers six formats, multiple mips/layers, untouched regions,
conditional-rendering behavior and statistics isolation (13 cases total).
concurrent_clear_probe records from six threads with independent command pools
and externally synchronized queue submission.
kk_exact_probe and kk_mixed_pipeline_probe include changing indirect counts and
compute dependencies. See their PROBE_* environment variables for variants.
The .vert/.frag/.comp files are our small test shaders; corresponding .h files
embed their SPIR-V, not any game shader. Regenerate with glslangValidator -V
and an equivalent uint32_t array conversion if editing them.

Run the acquire regression with:
  python3 test_acquire.py /absolute/path/to/this/mesa/checkout
It compiles the actual old and current acquire functions with a controlled
backend. The old source must fail and the new source must pass all six cases.
It requires xcrun/clang and the pinned athei base commit in local Git history.

The successful game profile has ICB=0, PRESENT_QUEUE=1,
NARROW_COMPUTE_BARRIERS=0, DRAW_COLOR_CLEAR=1, SERIALIZE_COMPILATION=1 and
GPU_PROGRESS=TRACE_QUEUE=0 (all variable names start MESA_KK_).
Test exact ICB separately with MESA_KK_EXACT_ICB=1. Probe success is not a
Vulkan conformance certification or proof of an Enshrouded/HDR fix.
