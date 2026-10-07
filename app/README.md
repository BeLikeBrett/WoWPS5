# WoWPS5 Probe

A PS5 homebrew title (`PPSA99220`) on RADV, made from the
[PS5 Vulkan Template](https://github.com/mihawk-99/PS5_VulkanTemplate) foundation at
`fdd41492`: Sascha Willems' Vulkan example base class with its PS5 hooks, the PS5 layer
(the launch, the pad, klog, test runs, the build and the console tools), and one
program, `examples/wowps5probe/wowps5probe.cpp`, grown from the starter sample.

## Building and running

It builds against my PS5 stack, checked out beside it: PS5_Vulkan (the RADV release
archive, the link recipe, the native tool and `libc.prx`) and the payload SDK fork
(`ps5/tools/setup-sdk.sh` installs it at the pinned revision).

```bash
ps5/tools/build.sh              # dist/PPSA99220/: eboot.bin, sce_sys, shaders, assets
ps5/tools/deploy.sh             # upload what changed, over the console's FTP server
ps5/tools/run.sh                # a test run: 300 frames, the last one saved and checked
ps5/tools/run.sh --menu         # no test: the program, until it ends itself
ps5/tools/host-reference.sh     # the same frames on this PC's Vulkan driver
```

A launch from the home screen starts the program at once. The sticks move the camera,
OPTIONS or the touch pad shows and hides the settings window, the D-pad and CROSS
change them, and its Quit ends the title.

## Growing it

- **The program** is one class on the base class, `examples/wowps5probe/wowps5probe.cpp`, with its
  shaders in `shaders/glsl/wowps5probe/` (GLSL beside the SPIR-V it loads: after changing one,
  `ps5/tools/compile-shaders.sh wowps5probe`).
- **A technique from the template's samples** (shadows, deferred lighting, bloom, MSAA,
  instancing, indirect draws, compute, bindless textures, mesh shaders, ray queries)
  copies across as it stands: every sample is a class on the same base.
- **Assets** go in `ps5/assets.json`, each with its origin and licence; only assets
  with a clear licence are shipped (`ps5/ASSETS.md` is written from it).
- **More programs**: another `SAMPLE(...)` line in `ps5/src/samples.cpp` turns the
  title into a menu of them, as Vulkan Template is.
- How the foundation works, its test runs and its tools: PS5_VulkanTemplate's
  `ps5/README.md`; the agent skills for this stack are in its `skills/`.

## Licences

The foundation is MIT (`LICENSE.md`, Sascha Willems'; the PS5 layer is mine, under the
same licence). The assets keep their own (`ps5/ASSETS.md`). The built title links the
PS5 platform layer of my payload SDK fork, which is GPL-3.0, so the title as
distributed is under GPL-3.0.
