KosmicKrisp
###########

KosmicKrisp is a Vulkan conformant implementation for macOS on Apple Silicon
hardware. It is implemented on top of Metal 4, which requires macOS 26 and up.

No iOS support is present as of now. However, iOS was taken into consideration
during development to support A14 Bionic GPUs and upwards.

Building
********

The following build instructions assume Homebrew as the package manager to
install dependencies. Homebrew homepage https://brew.sh/
Homebrew install command line:

.. code-block:: sh

   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

Terminal restart is recommended after Homebrew installation.

Requirements
============

- Xcode and Xcode command line tools
- Homebrew packages
   - meson (1.9.1+, can also be installed as a Python package)
   - cmake
   - pkg-config
   - libclc
   - llvm (20.1.8+, lower versions may work but have not been tested)
   - spirv-llvm-translator
   - spirv-tools (static library preferred which is not included in the package, Vulkan macOS SDK does)

Users might already have llvm installed, run ``llvm-config --version`` and
check that the command works and prints at least ``20.1.8``. Otherwise install
llvm from homebrew and add it to the ``PATH`` (follow homebrew instructions).

- Python
- Python packages
   - mako
   - packaging
   - pyyaml
   - meson (1.9.1+, if not installed through Homebrew)

Since Homebrew manages the Python environment, it is encouraged to create a
Python virtual environment and install all packages in that environment. To
create a Python virtual environment (e.g. ``$HOME/venv_mesa``):

.. code-block:: sh

   python3 -m venv $HOME/venv_mesa

To enable a Python virtual environment:

.. code-block:: sh

   source $HOME/venv_mesa/bin/activate

Build instructions
==================

Out of tree build directory is recommended.

Once all requirements have been installed, the following command line can be
used to create a debug build:

.. code-block:: sh

   meson setup <path/to/mesa> --buildtype=debug -Dplatforms=macos -Dvulkan-drivers=kosmickrisp -Dgallium-drivers= -Dopengl=false -Dzstd=disabled --prefer-static

Environment variables
*********************

KosmicKrisp specific environment variables:

- ``MESA_KK_DEBUG``: ``msl`` to log all generated Metal Shading Language (MSL) shaders. ``force_robustness`` to force robustness on all shaders.
- ``MESA_KK_GPU_CAPTURE``: Starts Metal capture at device create and ends it at device destroy. Set to ``1`` to activate.
- ``MESA_KK_GPU_CAPTURE_DIRECTORY``: Metal capture will be saved to the specified directory. Defaults to Xcode if no path is provided.
- ``MESA_KK_DISABLE_WORKAROUNDS``: Provide ``all`` to disable all workarounds. Otherwise, provide a comma separated list to disable wanted workarounds e.g. ``1,3,4`` to disable workaround 1, 3 and 4.
- ``MESA_KK_EXPERIMENTAL``: Enables advertising experimental features which may not provide full correctness or may negatively impact performance when enabled. Provide ``all`` to advertise all listed features.
  - ``image_view_min_lod`` to advertise ``VK_EXT_image_view_min_lod``.
  - ``optimize_e5r9g9b9`` to use allowGPUOptimizedContents with this format, which causes some CTS tests for vkCmdCopyImage() to fail

Local exact-indirect / presentation experiment (2026-10-26)
*********************************************************

This local build adapts Mesa merge request 45568 (separate WSI presentation
queue) to athei's queue completion tracking, and adds GPU-generated Metal
indirect render commands. It is experimental and has not completed a Vulkan
conformance run.

``MESA_KK_EXACT_ICB=0`` disables the new indirect-command path;
``MESA_KK_PRESENT_QUEUE=0`` restores the shared application/presentation queue.
Both optimizations default to enabled in this build. These are comparison
switches, not permissions to relax Vulkan correctness.

The follow-up diagnostic build adds ``MESA_KK_DIAGNOSTICS=1`` to enable
release-build Vulkan error logging, full Metal submission error descriptions,
and a first-use message for the ICB path. Queue errors identify whether the
application or private presentation queue failed. Set ``MESA_LOG_LEVEL=info``
to include the diagnostic startup and first-use messages. Logging is opt-in.

``MESA_KK_NARROW_COMPUTE_BARRIERS=0`` restores the original dispatch-and-blit
barriers for comparison. The diagnostic build's ``MESA_KK_EXACT_ICB=0`` also
disables indirect-command support on render pipelines, so the comparison
excludes both ICB execution and its pipeline configuration.

After an Enshrouded application-queue timeout, ordinary render pipelines were
restored to the upstream descriptor defaults. ICB-capable pipeline variants
are now compiled on first eligible ICB draw and cached separately. The original
pipeline is restored immediately after executing the ICB. This avoids changing
pipeline configuration for workloads that never use ICBs; it does not by itself
establish the cause of the reported game timeout.

The ICB path reads the current count on the GPU, executes the minimum of that
count and the application's maximum, and preserves draw IDs and conditional
rendering. GPU generation is dispatched from that exact count. Command storage
is retained for reuse only after its owning submission completes, and released
on command-buffer destruction or a reset requesting resource release.

The original path remains for capacities outside 1024 through 65536, robust
indexed draws, tessellation, geometry, multiview, transform feedback, active
queries/statistics, per-draw roots, and topology/provoking-vertex/index cases
requiring emulation. These eligibility bounds never truncate a Vulkan draw.
No new fast-math, tessellation-budget, or game-specific correctness exceptions
are introduced. Compute-only intra-encoder barriers are narrowed to dispatch;
cross-encoder barriers and resource-alias visibility remain conservative.

The independent presentation queue removes drawable waits from the application
queue. Completion accounting, public queue-idle waits, atomic-mode transitions,
and destruction include the private queue. This does not establish a fix for
HDR frames arriving a refresh late under game load.

The Metal WSI acquire path now waits for the selected offscreen image's prior
presentation fence before rebinding its drawable or signaling acquisition to
the application. A drawable becoming available does not establish that this
particular offscreen image is no longer being read by a private blit queue.
The existing present-time throttle is too late to protect application writes
between acquisition and presentation. The wait shares the acquire timeout;
a timeout leaves the image index and fetched drawable unchanged.

The image-reuse correction alone did not resolve Enshrouded's timeout.
A later profile with the private queue, draw clears and serialized Metal
compilation loaded with good reported gameplay performance. This is a temporary
per-game workaround; the root cause remains under investigation. See
:doc:`kosmickrisp/enshrouded-investigation` for the exact profile, controlled
comparisons, validation limits and unresolved pipeline-cache problem.

``MESA_KK_TRACE_QUEUE=1`` enables a bounded flight recorder of the last 1024
Metal commit, event-wait/signal, and drawable-wait/signal operations. On the
first failed commit it dumps copied labels, queue identities, event values,
CPU timestamps, and GPU completion feedback. Completion markers describe
callback delivery, not GPU synchronization. No resources are retained by the
recorder and the option is disabled by default. This is diagnostic information,
not a rendering or synchronization change.

``MESA_KK_GPU_PROGRESS=1`` adds diagnostic timestamp samples after bridged
compute, copy, and draw operations, with a separate 4096-entry counter heap for
each command-buffer recording. Its completion callback retains the counter
heap and copied operation descriptions independently of command-buffer reuse.
On failure, it reports the entire bounded recording, the device timestamp
frequency, and deltas between adjacent nonzero samples. Metal may coalesce
render-stage samples even with precise granularity, so a zero entry does not
prove that the corresponding operation failed to run. GPU recovery may allow
later timestamps to be written after earlier work has failed: a nonzero sample
does not prove that all preceding work succeeded. This mode may split render
passes and change timing; it is not suitable for performance measurements.
``MESA_KK_GPU_PROGRESS_DUMP=1`` additionally reports successful completions for
standalone validation; ``MESA_KK_GPU_PROGRESS_FULL=1`` includes their entire
bounded recordings as well. All options are disabled by default.

While GPU progress is enabled, pipeline labels include generated MSL source
BLAKE3 hashes and entry-point names. Setting ``MESA_KK_PROGRESS_SHADER_DIR`` to
an existing directory also captures generated sources there, deduplicated by
hash and limited to 256 MiB per process. It never overwrites an existing file.
This optional capture does not change shader source or compiler options.

``MESA_KK_SERIALIZE_COMPILATION=1`` serializes synchronous Metal library,
compute-pipeline and render-pipeline creation with a process-wide mutex.
This is an opt-in diagnostic/workaround candidate for a workload whose GPU
timeout depends on shader-source capture. It does not establish that concurrent
Metal compilation is invalid or defective. Shader source, compiler options,
and draw/dispatch behavior stay unchanged. Shader compilation may take longer;
there is no per-frame GPU instrumentation or submission wait added by this option.
The default remains concurrent compilation. For Enshrouded, scope this setting
to its executable through Wine's compatibility database rather than exporting
it globally.

``MESA_KK_DRAW_COLOR_CLEAR=1`` is an experimental alternative for
``vkCmdClearColorImage`` on single-sample 2D renderable color images, including
array layers and mip levels. It replaces an empty attachment-load clear with the existing meta
attachment-clear draw. The requested values and complete subresource ranges
are preserved, and conditional rendering cannot suppress the clear. Other
image types, multisample images, non-renderable formats, and depth/stencil clears
keep their existing paths. This option
is disabled by default and can be enabled only for a game's Wine compatibility
rule. It is a candidate workaround for an observed Enshrouded ClearTexture
GPU timeout, not a verified fix or a correctness relaxation. GPU progress
reports also include copied render attachment metadata for diagnosis.

Metal workarounds
*****************

Different workarounds are applied throughout the project to avoid issues such
as:

- Metal API and Vulkan API discrepancies
- Metal bugs
- MSL compiler bugs
- MSL compiler crashes

These workarounds can be found in:

.. toctree::
   :maxdepth: 1

   kosmickrisp/workarounds
