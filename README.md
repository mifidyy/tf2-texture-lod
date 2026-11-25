# TF2 Texture LOD — LOD tweaks for Team Fortress 2 on Linux

Get the blurry, low-detail texture look from TF2 **LOD tweaks (lodtweaks)** on Linux. TF2 Texture LOD is a Vulkan layer for the native Linux version of Team Fortress 2 on Steam.

[![Download for Linux (64-bit)](https://img.shields.io/badge/Download-Linux%20%2864--bit%29-2ea44f?style=for-the-badge)](https://github.com/mifidyy/tf2-texture-lod/releases/latest/download/tf2-texture-lod-linux-x86_64.zip)

## Install on Linux

You need the native Linux version of Team Fortress 2 installed through Steam and a working Vulkan graphics driver. The download contains the two files you need. There is no installer to run.

1. Click **Download for Linux (64-bit)** above and extract the ZIP with your file manager. Keep `libtf2_texture_lod.so` and `VK_LAYER_LOCAL_tf2_texture_lod.json` together in the extracted folder.
2. Open that folder and copy its full path from your file manager's address bar. The example below uses `/home/alex/Downloads/tf2-texture-lod`.
3. In **Steam → Library**, right-click **Team Fortress 2** and open **Properties → General → Launch Options**. If you previously forced Proton for TF2, clear that override in **Compatibility** so Steam uses the native Linux version.
4. Paste the following line into **Launch Options**. Replace the example path with your folder's path and keep the quotes. If you already have game arguments, keep them after `%command%`.

   ```text
   VK_ADD_LAYER_PATH="/home/alex/Downloads/tf2-texture-lod" VK_INSTANCE_LAYERS=VK_LAYER_LOCAL_tf2_texture_lod %command% -vulkan
   ```

5. Launch TF2 and load a map to see the texture change. To undo the tweak, remove the two `VK_…` settings from **Launch Options** and restart the game. Remove `-vulkan` too if you added it solely for this tweak.

### Using Gamescope or MangoHud

With Gamescope, put the layer variables after Gamescope's `--`. This applies the layer to the game instead of the compositor.

```sh
gamescope -- env VK_ADD_LAYER_PATH="/path/to/tf2-texture-lod/build" VK_INSTANCE_LAYERS=VK_LAYER_LOCAL_tf2_texture_lod %command% -vulkan
```

Disable MangoHud's `picmip` override when using this layer so only one layer controls bias.

## NVIDIA Profile Inspector for Linux and TF2

If you're looking for NVIDIA Profile Inspector for Linux to get the TF2 LOD tweak, this project provides a similar texture effect through Vulkan. It only adjusts texture sampling in the game you launch with the layer. It does not edit driver profiles or import NVIDIA Profile Inspector `.nip` files.

LOD means **level of detail**. The tweak makes the game choose smaller, blurrier versions of textures, called mip levels. By default, it adds **+15** to the game's texture LOD bias, subject to the GPU's limit, and caps eligible samplers at **mip 5**. For a 512×512 texture viewed from mip 0, mip 5 is 16×16. The effect approximates the high-LOD-bias Windows profile look. In-game visual parity is unverified.

## Settings

| Environment variable | Default | Effect |
| --- | --- | --- |
| `TF2_LOD_BIAS` | `15` | Bias added to each eligible sampler, subject to the GPU limit. |
| `TF2_LOD_MAX_MIP` | `5` | Coarsest mip allowed by the layer. Lower values retain more detail. |
| `TF2_LOD_TRACE` | Unset | Set to `1` to print the original and adjusted bias and maximum mip to stderr. Unset it to turn tracing off. |

Add these variables alongside `VK_INSTANCE_LAYERS` in your launch options. The two numeric settings accept non-negative finite values and fall back to their defaults if invalid. Restart the game after changing them so it recreates its samplers.

## Build from source

Requires Linux, a C++17 compiler, CMake 3.21 or newer, and the Vulkan loader development package with headers. Use a 64-bit build for TF2.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Follow the Steam steps above and use the full path to your `build` folder, which contains both files.

## Implementation

- `layer.cpp` negotiates with the Vulkan loader, stores downstream dispatch functions per instance and device, and intercepts `vkCreateSampler`. It reads the GPU's bias limit when the device is created. The sampler hook copies the create info and adjusts bias and maximum LOD before forwarding the call.
- `layer.json` names the explicit layer and locates the shared library. `CMakeLists.txt` builds the library and stages the manifest alongside it.
- The layer preserves the original minification, magnification, and mip filtering modes. It passes through fixed-LOD, comparison, unnormalized-coordinate, and subsampled samplers. It also preserves the application's minimum LOD and any tighter maximum.

## Troubleshooting

- Check that TF2 is started with `-vulkan` and that `VK_ADD_LAYER_PATH` points to the directory containing both build outputs.
- Add `TF2_LOD_TRACE=1` and check stderr for lines beginning with `TF2 texture LOD:`. These show changes for eligible samplers. The reported bias can be lower than requested because of the GPU's limit.
- Use `VK_LOADER_DEBUG=layer` to diagnose layer discovery and loading.
