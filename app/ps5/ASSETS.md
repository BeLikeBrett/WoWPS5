# Assets

The assets the title ships, written from `ps5/assets.json` by
`ps5/tools/build-assets.py --notices`. Only assets with a clear licence are
shipped: in PS5_VulkanTemplate, the asset pack's other files (the `assets`
submodule) stay out, and the samples that use them get the replacements here.

| Path under `/app0/assets/` | Asset | Author | Licence | Samples |
| --- | --- | --- | --- | --- |
| `Roboto-Medium.ttf` | [Roboto Medium](https://fonts.google.com/specimen/Roboto) | Christian Robertson (Google Fonts) | Apache-2.0 | all |
| `models/ps5/lantern` | [Lantern (glTF sample model)](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Lantern) (parent nodes centre it and scale it to 1.6, 5.5 and 8 tall, one .gltf for each sample) | Microsoft (sbtron) | CC0-1.0 | wowps5probe |
