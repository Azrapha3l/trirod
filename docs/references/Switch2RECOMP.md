# Switch2RECOMP reference

Repository: <https://github.com/SwitchRECOMP-Dev/Switch2RECOMP>

Local checkout: `[local path omitted]`

Revision inspected: `63e385b` (`main` at the time this reference was added).

## Why it is relevant

Despite its name, the repository describes separate renderer paths for both Switch generations. Its `renderer=vulkan` path processes the game's NVN calls and translates shaders to SPIR-V at runtime. That makes the Switch 1 NVN and Maxwell portions useful for comparison with EoWRecomp. The OpenGL renderer's Ampere/Switch 2 lowering and mesh/task stages are outside our target and are not porting candidates.

The most useful subsystems to inspect are:

- `runtime/nvn/hle_nvn.c`: NVN entry points, resource pools, command recording, and guest-visible handles.
- `runtime/nvn/gfx/`: NVN resource and command state shared with the Vulkan backend.
- `runtime/nvn/vk/vk_replay.c`: Vulkan command replay, descriptor writes, compute dispatch, image views, and GPU ordering.
- `runtime/nvn/vk/vk_tex.c` and `vk_fmt.c`: texture storage, view selection, formats, and storage-image eligibility.
- `runtime/nvn/vk/vk_shader.c`: SPIR-V reflection and pipeline descriptor layout.
- `tools/nxshader/nxs_live.py`, `nxs_refl.py`, and `nxs_spv.py`: live shader translation integration, descriptor reflection, and SPIR-V capability setup.
- `DEVELOPER_GUIDE.md`: trace format and diagnostic workflow.

## Relevance to the current image-write investigation

The developer guide says compute image writes (SUST) select their mip from TIC word 7. Its F5 trace records the image address, mip, and TIC level range before each compute dispatch, along with the translated compute shader and the dispatch result. This gives us a concrete way to compare which image and mip a compute shader writes, and whether the following pass samples that same subresource.

The Vulkan implementation builds a storage-image view for one mip and the selected layer, preserves the image format, and uses identity swizzle. It also checks whether the image was created with storage usage and reports when writes would be lost. After dispatch it records image-level diagnostics. These are useful comparison points for our bindless SUSTB descriptor array and `vk_storage_view` path. They are investigation leads, not evidence that our current mip selection is wrong.

## License and use

The repository is AGPL-3.0 with additional requirements in `NOTICE.txt`, including preserving its startup notice and visible attribution when conveying a derivative. It also contains third-party code under separate licenses. For now, keep this checkout as a read-only reference and compare behavior and design. Do not copy implementation code into EoWRecomp without first reviewing the relevant source license and the project's distribution implications.
