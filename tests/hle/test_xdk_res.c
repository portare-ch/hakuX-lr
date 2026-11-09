#include "hw/xbox/hle/tex/xdk_res.h"
#include "hle_test.h"

static void test_texture_full_chain(void)
{
    HleResDesc d = { .kind = HLE_RK_TEXTURE, .fmt = HLE_XFMT_A8R8G8B8,
                     .w = 256, .h = 256, .d = 1, .levels = 0 };
    HleResInfo ri;
    HT_CHECK(hle_res_info(&d, &ri));
    HT_EQ_INT(ri.levels, 9);
    HT_EQ_U32(ri.bytes, 349524);
    HT_EQ_U32(ri.common, 0x01040001);
    HT_EQ_U32(ri.format, 0x08890629);
    HT_EQ_U32(ri.size, 0);
    HT_EQ_INT(ri.hdr_bytes, 20);
    HT_CHECK(ri.contiguous);
    uint32_t pitch = 0;
    HT_EQ_U32(hle_res_subresource(&d, 0, 2, &pitch), 327680);
    HT_EQ_U32(pitch, 256);
    HleResDesc back;
    HT_CHECK(hle_res_from_header(ri.common, ri.format, ri.size, &back));
    HT_EQ_INT(back.kind, HLE_RK_TEXTURE);
    HT_EQ_U32(back.w, 256);
    HT_EQ_U32(back.h, 256);
    HT_EQ_U32(back.levels, 9);
}

static void test_dxt1_nonsquare(void)
{
    HleResDesc d = { .kind = HLE_RK_TEXTURE, .fmt = HLE_XFMT_DXT1,
                     .w = 64, .h = 32, .d = 1, .levels = 0 };
    HleResInfo ri;
    HT_CHECK(hle_res_info(&d, &ri));
    HT_EQ_INT(ri.levels, 7);
    HT_EQ_U32(ri.bytes, 1384);
    HT_EQ_U32(ri.format, 0x05670C29);
}

static void test_linear_surface(void)
{
    HleResDesc d = { .kind = HLE_RK_SURFACE, .fmt = HLE_XFMT_LIN_X8R8G8B8,
                     .w = 640, .h = 480, .d = 1 };
    HleResInfo ri;
    HT_CHECK(hle_res_info(&d, &ri));
    HT_EQ_U32(ri.pitch, 2560);
    HT_EQ_U32(ri.bytes, 1228800);
    HT_EQ_U32(ri.size, 0x271DF27F);
    HT_EQ_U32(ri.format, 0x00011E29);   /* PGR2's native back buffer (P2 Task 11 dump) */
    HT_EQ_U32(ri.common, 0x01050001);
    HT_EQ_INT(ri.hdr_bytes, 24);
    HleResDesc back;
    HT_CHECK(hle_res_from_header(ri.common, ri.format, ri.size, &back));
    HT_EQ_INT(back.kind, HLE_RK_SURFACE);
    HT_EQ_U32(back.w, 640);
    HT_EQ_U32(back.h, 480);
    HT_EQ_U32(back.pitch, 2560);
}

/* Linear pitch above the XDK minimum (XGSetTextureHeader's Pitch, tiled
 * frame buffers) must survive a header round trip: Lock and level headers
 * decode it from Size. */
static void test_linear_custom_pitch(void)
{
    HleResDesc d = { .kind = HLE_RK_TEXTURE, .fmt = HLE_XFMT_LIN_A8R8G8B8,
                     .w = 640, .h = 480, .d = 1, .pitch = 3072 };
    HleResInfo ri, ri2;
    HT_CHECK(hle_res_info(&d, &ri));
    HT_EQ_U32(ri.pitch, 3072);
    HT_EQ_U32(ri.bytes, 3072 * 480);
    HT_EQ_U32(ri.size, 0x2F1DF27F);             /* 639 | 479 << 12 | 47 << 24 */
    HT_EQ_U32(ri.format, 0x00011229);
    HleResDesc back;
    HT_CHECK(hle_res_from_header(ri.common, ri.format, ri.size, &back));
    HT_EQ_U32(back.pitch, 3072);
    uint32_t pitch = 0;
    HT_EQ_U32(hle_res_subresource(&back, 0, 0, &pitch), 0);
    HT_EQ_U32(pitch, 3072);
    HT_CHECK(hle_res_info(&back, &ri2));
    HT_EQ_U32(ri2.size, ri.size);
    HT_EQ_U32(ri2.bytes, ri.bytes);
    /* Header pitch 4096 on a 640-wide LIN_A8R8G8B8 decoded as 2560 before. */
    HT_CHECK(hle_res_from_header(0x01040001, 0x00011221,
                                 639 | (479u << 12) | (63u << 24), &back));
    HT_EQ_U32(back.pitch, 4096);
    hle_res_subresource(&back, 0, 0, &pitch);
    HT_EQ_U32(pitch, 4096);
    /* A header pitch below the row (2496 < 640 * 4) is rejected. */
    HT_CHECK(!hle_res_from_header(0x01040001, 0x00011221,
                                  639 | (479u << 12) | (38u << 24), &back));
    d.pitch = 3000;                             /* not a multiple of 64 */
    HT_CHECK(!hle_res_info(&d, &ri));
    d.pitch = 2496;                             /* below the minimum */
    HT_CHECK(!hle_res_info(&d, &ri));
    d.pitch = 16448;                            /* past Size[31:24] */
    HT_CHECK(!hle_res_info(&d, &ri));
    d.pitch = 16384;
    HT_CHECK(hle_res_info(&d, &ri));
    HT_EQ_U32(ri.size >> HLE_SIZE_PITCH_SHIFT, 255);
    /* Swizzled formats have no Size pitch: the field is ignored. */
    HleResDesc s = { .kind = HLE_RK_TEXTURE, .fmt = HLE_XFMT_A8R8G8B8,
                     .w = 256, .h = 256, .d = 1, .levels = 1, .pitch = 4096 };
    HT_CHECK(hle_res_info(&s, &ri));
    HT_EQ_U32(ri.pitch, 1024);
    HT_EQ_U32(ri.size, 0);
}

static void test_cube_and_volume(void)
{
    HleResDesc c = { .kind = HLE_RK_CUBE, .fmt = HLE_XFMT_A8R8G8B8,
                     .w = 16, .h = 16, .d = 1, .levels = 1 };
    HleResInfo ri;
    HT_CHECK(hle_res_info(&c, &ri));
    HT_EQ_U32(ri.bytes, 6144);
    HT_EQ_U32(ri.format, 0x0441062D);
    uint32_t pitch = 0;
    HT_EQ_U32(hle_res_subresource(&c, 3, 0, &pitch), 3072);
    HleResDesc v = { .kind = HLE_RK_VOLUME, .fmt = HLE_XFMT_L8,
                     .w = 16, .h = 16, .d = 4, .levels = 0 };
    HT_CHECK(hle_res_info(&v, &ri));
    HT_EQ_INT(ri.levels, 5);
    HT_EQ_U32(ri.bytes, 1173);
    HT_EQ_U32(ri.format, 0x24450039);
}

/* Stranger's Wrath's lighting cubes: XGSetCubeTextureHeader(8, 1, 0,
 * X8R8G8B8, ...) with BORDER_COLOR then cleared (0x1b2317), 0x1800 bytes
 * apart, each face filled as 16x16 swizzled with the image at (4, 4). */
static void test_border_texture(void)
{
    HleResDesc back;
    HT_CHECK(hle_res_from_header(0x000c0001, 0x03310725, 0, &back));
    HT_EQ_INT(back.kind, HLE_RK_CUBE);
    HT_CHECK(back.border);
    HT_EQ_U32(back.w, 8);
    HleResInfo ri;
    HT_CHECK(hle_res_info(&back, &ri));
    HT_EQ_U32(ri.bytes, 0x1800);
    HT_EQ_U32(ri.format & 0xFFFFFFFCu, 0x03310724);    /* border kept; DMA A */
    uint32_t pitch = 0, sw, sh, ofs;
    HT_EQ_U32(hle_res_subresource(&back, 2, 0, &pitch), 0x800);
    HT_EQ_U32(pitch, 64);
    hle_res_level(&back, 0, &sw, &sh, &ofs);
    HT_EQ_U32(sw, 16);
    HT_EQ_U32(sh, 16);
    HT_EQ_U32(ofs, 4);
    /* larger than 8: twice the size; the chain halves it */
    HleResDesc t = { .kind = HLE_RK_TEXTURE, .fmt = HLE_XFMT_A8R8G8B8,
                     .w = 64, .h = 32, .d = 1, .levels = 2, .border = true };
    HT_CHECK(hle_res_info(&t, &ri));
    HT_EQ_U32(ri.bytes, 128 * 64 * 4 + 64 * 32 * 4);
    hle_res_level(&t, 1, &sw, &sh, &ofs);
    HT_EQ_U32(sw, 64);
    HT_EQ_U32(sh, 32);
    HT_EQ_U32(ofs, 2);
    /* D3D's own headers (BORDER_COLOR set) and DXT stay unbordered */
    HT_CHECK(hle_res_from_header(0x000c0001, 0x03310729, 0, &back));
    HT_CHECK(!back.border);
    HT_CHECK(hle_res_from_header(0x00040001, 0x05670C21, 0, &back));
    HT_CHECK(!back.border);
}

static void test_buffers_and_rejects(void)
{
    HleResDesc vb = { .kind = HLE_RK_VB, .length = 1000 };
    HleResInfo ri;
    HT_CHECK(hle_res_info(&vb, &ri));
    HT_EQ_U32(ri.common, 0x01000001);
    HT_EQ_U32(ri.bytes, 1000);
    HT_EQ_INT(ri.hdr_bytes, 12);
    HleResDesc ib = { .kind = HLE_RK_IB, .length = 64 };
    HT_CHECK(hle_res_info(&ib, &ri));
    HT_CHECK(!ri.contiguous);
    HleResDesc pal = { .kind = HLE_RK_PALETTE, .length = 64 };
    HT_CHECK(hle_res_info(&pal, &ri));
    HT_EQ_U32(ri.common, 0x81030001);          /* size code 2 in Common[31:30] */
    HT_EQ_U32(ri.bytes, 256);
    HleResDesc npot = { .kind = HLE_RK_TEXTURE, .fmt = HLE_XFMT_A8R8G8B8,
                        .w = 100, .h = 64, .d = 1 };
    HT_CHECK(!hle_res_info(&npot, &ri));       /* swizzled needs pow2 */
    HleResDesc unk = { .kind = HLE_RK_TEXTURE, .fmt = 0x7E, .w = 4, .h = 4, .d = 1 };
    HT_CHECK(!hle_res_info(&unk, &ri));
}

static void test_format_helpers(void)
{
    HT_EQ_U32(hle_res_data_va(0x00123000), 0x80123000);
    HT_EQ_U32(hle_res_data_pa(0x80123000), 0x00123000);
}

/* colour sign: the signed 8-bit channels' top bits, and the view
 * channels they reach */
static void test_sign(void)
{
    HT_EQ_U32(hle_fmt_sign_xor(HLE_XFMT_A8R8G8B8, 0xF), 0x80808080u);
    HT_EQ_U32(hle_fmt_sign_rgba(HLE_XFMT_A8R8G8B8, 0xF), 0xF);
    HT_EQ_U32(hle_fmt_sign_xor(HLE_XFMT_LIN_A8R8G8B8, 0x2), 0x00800000u);   /* R */
    HT_EQ_U32(hle_fmt_sign_rgba(HLE_XFMT_LIN_A8R8G8B8, 0x2), 0x1);
    HT_EQ_U32(hle_fmt_sign_xor(HLE_XFMT_X8R8G8B8, 0x1), 0);                 /* no alpha */
    HT_EQ_U32(hle_fmt_sign_rgba(HLE_XFMT_X8R8G8B8, 0x1), 0);
    HT_EQ_U32(hle_fmt_sign_xor(HLE_XFMT_R8G8B8A8, 0x2), 0x80000000u);
    HT_EQ_U32(hle_fmt_sign_xor(HLE_XFMT_G8B8, 0x8), 0x0080u);               /* B: du */
    HT_EQ_U32(hle_fmt_sign_rgba(HLE_XFMT_G8B8, 0x8), 1 | 4);
    HT_EQ_U32(hle_fmt_sign_rgba(HLE_XFMT_G8B8, 0xC), 0xF);
    HT_EQ_U32(hle_fmt_sign_rgba(HLE_XFMT_R8B8, 0x2), 1 | 8);
    HT_EQ_U32(hle_fmt_sign_xor(HLE_XFMT_DXT1, 0xF), 0);                    /* no bytes */
    HT_EQ_U32(hle_fmt_sign_xor(HLE_XFMT_R5G6B5, 0xF), 0);
    /* read as stored: their bytes' channels, nothing flipped */
    HT_EQ_U32(hle_fmt_sign_xor(HLE_XFMT_L8, 0xF), 0);
    HT_EQ_U32(hle_fmt_sign_rgba(HLE_XFMT_L8, 0xF), 0x7);                   /* alpha is 1 */
    HT_EQ_U32(hle_fmt_sign_rgba(HLE_XFMT_LIN_AL8, 0x1), 0x8);
    HT_EQ_U32(hle_fmt_sign_rgba(HLE_XFMT_A8, 0xF), 0x8);                   /* rgb are 1 */
    HT_EQ_U32(hle_fmt_sign_xor(HLE_XFMT_A8L8, 0xF), 0);
    HT_EQ_U32(hle_fmt_sign_rgba(HLE_XFMT_A8L8, 0x3), 0x9);
    /* by the guest's format: those the host expands to 8888 stay unsigned */
    HT_EQ_U32(hle_fmt_sign_rgba(HLE_XFMT_DXT1, 0xF), 0);
    HT_EQ_U32(hle_fmt_sign_rgba(HLE_XFMT_DXT5, 0xF), 0);
    HT_EQ_U32(hle_fmt_sign_rgba(HLE_XFMT_A4R4G4B4, 0xF), 0);
    HT_EQ_U32(hle_fmt_sign_rgba(HLE_XFMT_P8, 0xF), 0);
    HT_EQ_U32(hle_fmt_sign_rgba(HLE_XFMT_R5G6B5, 0xF), 0);
}

static uint32_t conv1(uint32_t code, uint32_t texel)
{
    uint8_t in[4] = { texel & 0xFF, texel >> 8 & 0xFF, texel >> 16 & 0xFF, texel >> 24 };
    uint32_t out = 0xDEADBEEF;
    HT_CHECK(hle_tex_convert(code, in, 1, &out));
    return out;
}

static float depth1(uint32_t code, uint32_t texel)
{
    uint32_t bits = conv1(code, texel);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

/* texels the back end can't sample as stored: A8R8G8B8 (bytes B, G, R, A)
 * or depth as a float scaled to 0..1 */
static void test_convert(void)
{
    HT_EQ_U32(conv1(HLE_XFMT_A4R4G4B4, 0xF8A1), 0xFF88AA11u);       /* n * 17 */
    HT_EQ_U32(conv1(HLE_XFMT_LIN_A4R4G4B4, 0x0000), 0);
    /* R4G4B4A4: R 15-12, G 11-8, B 7-4, A 3-0 */
    HT_EQ_U32(conv1(HLE_XFMT_R4G4B4A4, 0xF8A1), 0x11FF88AAu);
    HT_EQ_U32(conv1(HLE_XFMT_LIN_R4G4B4A4, 0x000F), 0xFF000000u);
    HT_EQ_U32(conv1(HLE_XFMT_R4G4B4A4, 0x00F0), 0x000000FFu);
    /* R5G5B5A1: R 15-11, G 10-6, B 5-1, A 0 */
    HT_EQ_U32(conv1(HLE_XFMT_R5G5B5A1, 0xFFFF), 0xFFFFFFFFu);
    HT_EQ_U32(conv1(HLE_XFMT_R5G5B5A1, 0xF800), 0x00FF0000u);
    HT_EQ_U32(conv1(HLE_XFMT_LIN_R5G5B5A1, 0x07C1), 0xFF00FF00u);
    HT_EQ_U32(conv1(HLE_XFMT_R5G5B5A1, 0x0020), 0x00000084u);       /* B 16: 0x84 */
    /* R6G5B5 (L6V5U5): R 15-10, G 9-5, B 4-0, alpha 1 */
    HT_EQ_U32(conv1(HLE_XFMT_R6G5B5, 0xFFFF), 0xFFFFFFFFu);
    HT_EQ_U32(conv1(HLE_XFMT_R6G5B5, 0x8000), 0xFF820000u);         /* L 32: 0x82 */
    HT_EQ_U32(conv1(HLE_XFMT_LIN_R6G5B5, 0x03E0), 0xFF00FF00u);
    HT_EQ_U32(conv1(HLE_XFMT_R6G5B5, 0x0010), 0xFF000084u);         /* U -16: 0x84 */
    HT_CHECK(!hle_tex_convert(HLE_XFMT_L16, "\0\0", 1, &(uint32_t){ 0 }));
    /* R16B16 uses the high bytes of V and U in RGBA order (V, U, U, V). */
    HT_EQ_U32(conv1(HLE_XFMT_V16U16, 0x80FF7F00u), 0x80807F7Fu);
    HT_EQ_U32(conv1(HLE_XFMT_LIN_V16U16, 0x0001FFFFu), 0x0000FFFFu);
    /* depth over the shadow compare's range: 2^24 - 1, 65535, 511.9375;
     * F24 as its float (nxdk_pgraph_tests compares one at 0.5e30) */
    HT_CHECK(depth1(HLE_XFMT_D24S8, 0xFFFFFF12u) == 1.0f);
    HT_CHECK(depth1(HLE_XFMT_LIN_D24S8, 0x000000FFu) == 0.0f);         /* stencil only */
    HT_CHECK(depth1(HLE_XFMT_D24S8, 0x00010012u) == 256 / 16777215.0f);
    HT_CHECK(depth1(HLE_XFMT_D16, 0xFFFF) == 1.0f);
    HT_CHECK(depth1(HLE_XFMT_LIN_D16, 0x8000) == 32768 / 65535.0f);
    HT_CHECK(depth1(HLE_XFMT_F16, 0xFFFF) == 1.0f);                   /* 511.9375 */
    HT_CHECK(depth1(HLE_XFMT_F16, 0x0000) == 0.0f);
    HT_CHECK(depth1(HLE_XFMT_LIN_F16, 0x7000) == 1.0f / 511.9375f);    /* e 7, m 0 */
    HT_CHECK(depth1(HLE_XFMT_F16, 0x8800) == 3.0f / 511.9375f);        /* 2 * 1.5 */
    HT_CHECK(depth1(HLE_XFMT_F24S8, 0x7F000055u) == 1.0f);             /* 1.0f's bits >> 7 */
    HT_EQ_U32(conv1(HLE_XFMT_LIN_F24S8, 0xE193E512u), 0x70C9F280u);    /* the test's 0.5e30 */
    HT_EQ_U32(conv1(HLE_XFMT_F24S8, 0xFF000000u), 0x7F800000u);        /* infinity */
    /* several texels, unaligned */
    uint8_t in[7] = { 0, 0x00, 0xFC, 0xFF, 0xFF, 0x1F, 0x00 };
    uint32_t out[3];
    HT_CHECK(hle_tex_convert(HLE_XFMT_R6G5B5, in + 1, 3, out));
    HT_EQ_U32(out[0], 0xFFFF0000u);
    HT_EQ_U32(out[1], 0xFFFFFFFFu);
    HT_EQ_U32(out[2], 0xFF0000FFu);
    HT_CHECK(!hle_tex_convert(HLE_XFMT_A8R8G8B8, in, 1, out));
    HT_CHECK(!hle_tex_convert(HLE_XFMT_DXT1, in, 1, out));
    /* the code they are sent as keeps a linear texture linear */
    HT_EQ_U32(hle_tex_conv_xfmt(HLE_XFMT_R6G5B5), HLE_XFMT_A8R8G8B8);
    HT_EQ_U32(hle_tex_conv_xfmt(HLE_XFMT_LIN_R6G5B5), HLE_XFMT_LIN_A8R8G8B8);
    HT_EQ_U32(hle_tex_conv_xfmt(HLE_XFMT_LIN_A4R4G4B4), HLE_XFMT_LIN_A8R8G8B8);
    HT_EQ_U32(hle_tex_conv_xfmt(HLE_XFMT_D24S8), HLE_XFMT_F24S8);
    HT_EQ_U32(hle_tex_conv_xfmt(HLE_XFMT_LIN_D16), HLE_XFMT_LIN_F24S8);
    HT_EQ_U32(hle_tex_conv_xfmt(HLE_XFMT_F16), HLE_XFMT_F24S8);
}

int main(void)
{
    HT_RUN(test_texture_full_chain);
    HT_RUN(test_dxt1_nonsquare);
    HT_RUN(test_linear_surface);
    HT_RUN(test_linear_custom_pitch);
    HT_RUN(test_cube_and_volume);
    HT_RUN(test_border_texture);
    HT_RUN(test_buffers_and_rejects);
    HT_RUN(test_format_helpers);
    HT_RUN(test_sign);
    HT_RUN(test_convert);
    HT_DONE();
}
