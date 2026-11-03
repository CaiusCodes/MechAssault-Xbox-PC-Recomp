SMAA (Enhanced Subpixel Morphological Antialiasing), Jorge Jimenez et al., MIT licence (LICENSE.txt).
From https://github.com/iryoku/smaa, master branch, fetched 2026-11-03: SMAA.hlsl, Textures/AreaTex.h,
Textures/SearchTex.h, unchanged. sdk/runtime/gpu/smaa_hlsl.h embeds SMAA.hlsl as a string (made by
sdk/third_party/smaa/embed.py); the renderer compiles it at run time and uploads the two tables, so a release carries
everything in the executable.
