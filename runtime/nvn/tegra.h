// Tegra X1 texture layout: formats and block-linear (GOB) swizzling.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Nintendo NVN texture-format IDs. These values are part of the game's API data; keep them named here so format
// tables and format-specific code do not need unexplained hexadecimal IDs.
typedef enum NvnFormat {
    NVN_FORMAT_NONE = 0x00,
    NVN_FORMAT_R8 = 0x01, NVN_FORMAT_R8SN = 0x02, NVN_FORMAT_R8UI = 0x03, NVN_FORMAT_R8I = 0x04,
    NVN_FORMAT_R16F = 0x05, NVN_FORMAT_R16 = 0x06, NVN_FORMAT_R16SN = 0x07, NVN_FORMAT_R16UI = 0x08,
    NVN_FORMAT_R16I = 0x09,
    NVN_FORMAT_R32F = 0x0A, NVN_FORMAT_R32UI = 0x0B, NVN_FORMAT_R32I = 0x0C,
    NVN_FORMAT_RG8 = 0x0D, NVN_FORMAT_RG8SN = 0x0E, NVN_FORMAT_RG8UI = 0x0F, NVN_FORMAT_RG8I = 0x10,
    NVN_FORMAT_RG16F = 0x11, NVN_FORMAT_RG16 = 0x12, NVN_FORMAT_RG16SN = 0x13, NVN_FORMAT_RG16UI = 0x14,
    NVN_FORMAT_RG16I = 0x15,
    NVN_FORMAT_RG32F = 0x16, NVN_FORMAT_RG32UI = 0x17, NVN_FORMAT_RG32I = 0x18,
    NVN_FORMAT_RGB8 = 0x19, NVN_FORMAT_RGB8SN = 0x1A, NVN_FORMAT_RGB8UI = 0x1B, NVN_FORMAT_RGB8I = 0x1C,
    NVN_FORMAT_RGB16F = 0x1D, NVN_FORMAT_RGB16 = 0x1E, NVN_FORMAT_RGB16SN = 0x1F, NVN_FORMAT_RGB16UI = 0x20,
    NVN_FORMAT_RGB16I = 0x21,
    NVN_FORMAT_RGB32F = 0x22, NVN_FORMAT_RGB32UI = 0x23, NVN_FORMAT_RGB32I = 0x24,
    NVN_FORMAT_RGBA8 = 0x25, NVN_FORMAT_RGBA8SN = 0x26, NVN_FORMAT_RGBA8UI = 0x27, NVN_FORMAT_RGBA8I = 0x28,
    NVN_FORMAT_RGBA16F = 0x29, NVN_FORMAT_RGBA16 = 0x2A, NVN_FORMAT_RGBA16SN = 0x2B,
    NVN_FORMAT_RGBA16UI = 0x2C, NVN_FORMAT_RGBA16I = 0x2D,
    NVN_FORMAT_RGBA32F = 0x2E, NVN_FORMAT_RGBA32UI = 0x2F, NVN_FORMAT_RGBA32I = 0x30,
    NVN_FORMAT_STENCIL8 = 0x31, NVN_FORMAT_DEPTH16 = 0x32, NVN_FORMAT_DEPTH24 = 0x33, NVN_FORMAT_DEPTH32F = 0x34,
    NVN_FORMAT_DEPTH24_STENCIL8 = 0x35, NVN_FORMAT_DEPTH32F_STENCIL8 = 0x36,
    NVN_FORMAT_RGBX8_SRGB = 0x37, NVN_FORMAT_RGBA8_SRGB = 0x38, NVN_FORMAT_RGBA4 = 0x39,
    NVN_FORMAT_RGB5 = 0x3A, NVN_FORMAT_RGB5A1 = 0x3B, NVN_FORMAT_RGB565 = 0x3C,
    NVN_FORMAT_RGB10A2 = 0x3D, NVN_FORMAT_RGB10A2UI = 0x3E, NVN_FORMAT_R11G11B10F = 0x3F,
    NVN_FORMAT_RGB9E5F = 0x40,
    NVN_FORMAT_RGB_DXT1 = 0x41, NVN_FORMAT_RGBA_DXT1 = 0x42, NVN_FORMAT_RGBA_DXT3 = 0x43,
    NVN_FORMAT_RGBA_DXT5 = 0x44, NVN_FORMAT_RGB_DXT1_SRGB = 0x45, NVN_FORMAT_RGBA_DXT1_SRGB = 0x46,
    NVN_FORMAT_RGBA_DXT3_SRGB = 0x47, NVN_FORMAT_RGBA_DXT5_SRGB = 0x48,
    NVN_FORMAT_RGTC1_UNORM = 0x49, NVN_FORMAT_RGTC1_SNORM = 0x4A, NVN_FORMAT_RGTC2_UNORM = 0x4B,
    NVN_FORMAT_RGTC2_SNORM = 0x4C, NVN_FORMAT_BPTC_UNORM = 0x4D, NVN_FORMAT_BPTC_UNORM_SRGB = 0x4E,
    NVN_FORMAT_BPTC_SFLOAT = 0x4F, NVN_FORMAT_BPTC_UFLOAT = 0x50,
    NVN_FORMAT_R8_UI2F = 0x51, NVN_FORMAT_R8_I2F = 0x52, NVN_FORMAT_R16_UI2F = 0x53,
    NVN_FORMAT_R16_I2F = 0x54, NVN_FORMAT_R32_UI2F = 0x55, NVN_FORMAT_R32_I2F = 0x56,
    NVN_FORMAT_RG8_UI2F = 0x57, NVN_FORMAT_RG8_I2F = 0x58, NVN_FORMAT_RG16_UI2F = 0x59,
    NVN_FORMAT_RG16_I2F = 0x5A, NVN_FORMAT_RG32_UI2F = 0x5B, NVN_FORMAT_RG32_I2F = 0x5C,
    NVN_FORMAT_RGB8_UI2F = 0x5D, NVN_FORMAT_RGB8_I2F = 0x5E, NVN_FORMAT_RGB16_UI2F = 0x5F,
    NVN_FORMAT_RGB16_I2F = 0x60, NVN_FORMAT_RGB32_UI2F = 0x61, NVN_FORMAT_RGB32_I2F = 0x62,
    NVN_FORMAT_RGBA8_UI2F = 0x63, NVN_FORMAT_RGBA8_I2F = 0x64, NVN_FORMAT_RGBA16_UI2F = 0x65,
    NVN_FORMAT_RGBA16_I2F = 0x66, NVN_FORMAT_RGBA32_UI2F = 0x67, NVN_FORMAT_RGBA32_I2F = 0x68,
    NVN_FORMAT_RGB10A2SN = 0x69, NVN_FORMAT_RGB10A2I = 0x6A, NVN_FORMAT_RGB10A2_UI2F = 0x6B,
    NVN_FORMAT_RGB10A2_I2F = 0x6C,
    NVN_FORMAT_RGBX8 = 0x6D, NVN_FORMAT_RGBX8SN = 0x6E, NVN_FORMAT_RGBX8UI = 0x6F, NVN_FORMAT_RGBX8I = 0x70,
    NVN_FORMAT_RGBX16F = 0x71, NVN_FORMAT_RGBX16 = 0x72, NVN_FORMAT_RGBX16SN = 0x73,
    NVN_FORMAT_RGBX16UI = 0x74, NVN_FORMAT_RGBX16I = 0x75,
    NVN_FORMAT_RGBX32F = 0x76, NVN_FORMAT_RGBX32UI = 0x77, NVN_FORMAT_RGBX32I = 0x78,
    NVN_FORMAT_RGBA_ASTC_4x4 = 0x79, NVN_FORMAT_RGBA_ASTC_5x4 = 0x7A, NVN_FORMAT_RGBA_ASTC_5x5 = 0x7B,
    NVN_FORMAT_RGBA_ASTC_6x5 = 0x7C, NVN_FORMAT_RGBA_ASTC_6x6 = 0x7D, NVN_FORMAT_RGBA_ASTC_8x5 = 0x7E,
    NVN_FORMAT_RGBA_ASTC_8x6 = 0x7F, NVN_FORMAT_RGBA_ASTC_8x8 = 0x80, NVN_FORMAT_RGBA_ASTC_10x5 = 0x81,
    NVN_FORMAT_RGBA_ASTC_10x6 = 0x82, NVN_FORMAT_RGBA_ASTC_10x8 = 0x83, NVN_FORMAT_RGBA_ASTC_10x10 = 0x84,
    NVN_FORMAT_RGBA_ASTC_12x10 = 0x85, NVN_FORMAT_RGBA_ASTC_12x12 = 0x86,
    NVN_FORMAT_RGBA_ASTC_4x4_SRGB = 0x87, NVN_FORMAT_RGBA_ASTC_5x4_SRGB = 0x88,
    NVN_FORMAT_RGBA_ASTC_5x5_SRGB = 0x89, NVN_FORMAT_RGBA_ASTC_6x5_SRGB = 0x8A,
    NVN_FORMAT_RGBA_ASTC_6x6_SRGB = 0x8B, NVN_FORMAT_RGBA_ASTC_8x5_SRGB = 0x8C,
    NVN_FORMAT_RGBA_ASTC_8x6_SRGB = 0x8D, NVN_FORMAT_RGBA_ASTC_8x8_SRGB = 0x8E,
    NVN_FORMAT_RGBA_ASTC_10x5_SRGB = 0x8F, NVN_FORMAT_RGBA_ASTC_10x6_SRGB = 0x90,
    NVN_FORMAT_RGBA_ASTC_10x8_SRGB = 0x91, NVN_FORMAT_RGBA_ASTC_10x10_SRGB = 0x92,
    NVN_FORMAT_RGBA_ASTC_12x10_SRGB = 0x93, NVN_FORMAT_RGBA_ASTC_12x12_SRGB = 0x94,
    NVN_FORMAT_BGR565 = 0x95, NVN_FORMAT_BGR5 = 0x96, NVN_FORMAT_BGR5A1 = 0x97, NVN_FORMAT_A1BGR5 = 0x98,
    NVN_FORMAT_BGRX8 = 0x99, NVN_FORMAT_BGRA8 = 0x9A, NVN_FORMAT_BGRX8_SRGB = 0x9B,
    NVN_FORMAT_BGRA8_SRGB = 0x9C
} NvnFormat;

typedef struct {
    uint8_t bpb;     // Stored bytes per texel, or per compressed block.
    uint8_t bw, bh;  // Number of texels covered by one stored block; 1x1 for uncompressed formats.
    uint8_t kind;    // Format category used by texture and framebuffer handling.
    const char *name; // Optional label for diagnostics and debugging.
} FormatInfo;

enum { FMT_COLOR = 0, FMT_DEPTH = 1, FMT_STENCIL = 2, FMT_DEPTH_STENCIL = 3, FMT_COMPRESSED = 4 };

// Look up the storage metadata for an NVN format ID; unknown IDs use the legacy 4-byte fallback.
const FormatInfo *nvn_format_info(uint32_t nvn_format);

typedef struct {
    uint32_t width, height, depth, layers, levels;  // depth: 3D depth; layers: array layers / cube faces
    const FormatInfo *fmt;
    uint32_t block_height_log2;  // GOBs per block (log2) for level 0
    uint32_t block_depth_log2;
} TexLayout;

typedef struct {
    uint64_t offset;              // from the start of the layer
    uint32_t w, h, d;             // texel dimensions
    uint32_t wb, hb;              // in blocks
    uint32_t block_height_log2, block_depth_log2;
} LevelLayout;

// Choose the default GOB block height used by NVN for a texture of the given block-row count.
uint32_t tegra_default_block_height_log2(uint32_t height_in_blocks);

// Initialize layout defaults, including NVN's block-height choice when the guest did not specify one.
void tegra_layout_init(TexLayout *t);

// Compute one mip level's dimensions, block counts, and byte offset within its array layer.
LevelLayout tegra_level(const TexLayout *t, uint32_t level);

// Return storage size for one array layer, or for all layers in the texture respectively.
uint64_t tegra_layer_size(const TexLayout *t);
uint64_t tegra_total_size(const TexLayout *t);

// Convert between Tegra block-linear storage and row-major texel blocks; row pitch is wb * bpb bytes.
void tegra_deswizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                     uint32_t block_height_log2, uint32_t block_depth_log2);
void tegra_swizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                   uint32_t block_height_log2, uint32_t block_depth_log2);
