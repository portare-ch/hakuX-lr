#include "hw/xbox/hle/shader/ffhw.h"
#include "hle_test.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char buf[65536];

static void dump(const char *name)
{
    const char *dir = getenv("HLE_TEST_DUMP");
    if (dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.vert", dir, name);
        FILE *f = fopen(path, "wb");
        if (f) {
            fputs(buf, f);
            fclose(f);
        }
    }
}

static size_t gen(const HleFfhwKey *k, uint32_t in_mask)
{
    uint32_t tok[HLE_FFHW_SLOTS * 4];
    memcpy(tok, k, sizeof(*k));
    HleVshKey vk = { .in_mask = in_mask };
    return hle_ffhw_glsl(tok, HLE_FFHW_SLOTS, &vk, buf, sizeof(buf));
}

static void test_unlit(void)
{
    HleFfhwKey k = { .magic = HLE_FFHW_MAGIC, .specular = 1, .separate_spec = 1,
                     .alpha_mat_spec = 1 };
    size_t n = gen(&k, 1u << 0 | 1u << 3 | 1u << 9);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("ffhw_unlit");
    /* object space through the composite, over w, plus the viewport offset */
    HT_CHECK(strstr(buf, "vec4 sp = XF(pos, 0);") != NULL);
    HT_CHECK(strstr(buf, "oPos = vec4(sp.xyz / sw + c[68].xyz, sw);") != NULL);
    HT_CHECK(strstr(buf, "oD0 = v3;") != NULL);
    HT_CHECK(strstr(buf, "oT0 = v9;") != NULL);
    HT_CHECK(strstr(buf, "vec4 v1 = vcur[1];") != NULL);   /* the program head's */
    HT_CHECK(strstr(buf, "vFog = 1.0;") != NULL);          /* no fog written */
    HT_CHECK(strstr(buf, "gl_Position") != NULL);
}

static void test_lit_skinned(void)
{
    HleFfhwKey k = { .magic = HLE_FFHW_MAGIC, .skin = 3, .lighting = 1, .normalize = 1,
                     .specular = 1, .fog = 3, .src = { 0, 1, 1, 0 },
                     .light = { 1, 2, 3 } };
    k.texgen[1][0] = HLE_FFHW_TG_SPHERE;
    k.texgen[1][1] = HLE_FFHW_TG_SPHERE;
    k.texgen[2][2] = HLE_FFHW_TG_REFLECTION;
    k.texmat = 1u << 1;
    size_t n = gen(&k, 0x1E1F);
    HT_CHECK(n > 0);
    dump("ffhw_lit");
    /* 3G: two weights given, the third generated */
    HT_CHECK(strstr(buf, "wi = v1.x;\n    wn -= wi;") != NULL);
    HT_CHECK(strstr(buf, "wi = v1.y;\n    wn -= wi;") != NULL);
    HT_CHECK(strstr(buf, "wi = wn;\n    tPos += XF(pos, 12) * wi;") != NULL);
    HT_CHECK(strstr(buf, "tN += XF(n4, 28).xyz * wi;") != NULL);
    HT_CHECK(strstr(buf, "tN = normalize(tN);") != NULL);
    HT_CHECK(strstr(buf, "vec4 sp = XF(tPos, 0);") != NULL);   /* skinned: eye space */
    /* ambient from the diffuse register, scaled by the emission register */
    /* emission from the material, ambient from the diffuse colour: the scene
     * ambient register plus the vertex colour scaled by the emission one */
    HT_CHECK(strstr(buf, "oD0 = vec4(c[71].rgb + v3.rgb * c[72].rgb, v3.a);") != NULL);
    HT_CHECK(strstr(buf, "/* light 0 */") && strstr(buf, "/* light 1 */") &&
             strstr(buf, "/* light 2 */") && !strstr(buf, "/* light 3 */"));
    HT_CHECK(strstr(buf, "nh = max(dot(tN, c[80].xyz), 0.0);") != NULL);  /* infinite: half */
    HT_CHECK(strstr(buf, "lit = d <= c[89].x;") != NULL);               /* local: range */
    /* Evaluate spotlight falloff through the inner cone. */
    HT_CHECK(strstr(buf, "float s = dot(c[103].xyz, VP) + c[103].w;") != NULL);
    HT_CHECK(strstr(buf, "else att = min(att * NVC(min(s, 1.0), c[102], 1.0), 1.0e10);") != NULL);
    HT_CHECK(strstr(buf, "float pf = nl > 0.0 ? NVC(nh, c[182], 10000.0) : 0.0;") != NULL);
    HT_CHECK(strstr(buf, "pow(") == NULL);
    HT_CHECK(strstr(buf, "oD0.rgb += v3.rgb * c[77].rgb * (att * nl);") != NULL);
    HT_CHECK(strstr(buf, "oD0.rgb += oD1.rgb;") != NULL);   /* not separate */
    HT_CHECK(strstr(buf, "oT1.x = r.x / (2.0 * length(r + vec3(0.0, 0.0, 1.0))) + 0.5;"));
    HT_CHECK(strstr(buf, "oT2.z = reflect(normalize(tPos.xyz), tN).z;") != NULL);
    HT_CHECK(strstr(buf, "oT1 = XF(oT1, 40);") != NULL);
    HT_CHECK(strstr(buf, "oFog = vec4(dot(c[69].xyz, tPos.xyz) + c[69].w);") != NULL);
    HT_CHECK(strstr(buf, "vFog = oFog.x;") != NULL);
}

/* two-sided lighting, as the NV2A tests (Lighting Two Sided, Specular back)
 * show it: lit, the back colours from the back registers along -N by the
 * back's own colour sources, and without SEPARATE_SPECULAR the front vertex
 * specular passed through; unlit, (0, 0, 0, 1) whatever the back colour
 * registers hold */
static void test_two_side(void)
{
    HleFfhwKey k = { .magic = HLE_FFHW_MAGIC, .lighting = 1, .two_side = 1, .specular = 1,
                     .src = { 0, 0, 1, 0 }, .bsrc = { 0, 0, 0, 2 }, .light = { 1, 2 } };
    size_t n = gen(&k, 0x1E1F | 3u << 7);               /* back colour arrays too */
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("ffhw_two_side");
    /* the front's diffuse from the vertex, the back's from the material;
     * emission and ambient from the material: the scene ambient as it is,
     * not scaled by the emission register again */
    HT_CHECK(strstr(buf, "oD0 = vec4(c[71].rgb, v3.a);") != NULL);
    HT_CHECK(strstr(buf, "oB0 = vec4(c[156].rgb, c[156].w);") != NULL);
    HT_CHECK(strstr(buf, "oD0.rgb += v3.rgb * c[77].rgb * (att * nl);") != NULL);
    HT_CHECK(strstr(buf, "oB0.rgb += vec3(1.0) * c[162].rgb * (att * bl);") != NULL);
    HT_CHECK(strstr(buf, "oB1.rgb += v4.rgb * c[163].rgb * (att * bf);") != NULL);
    HT_CHECK(strstr(buf, "float bl = max(dot(-tN, Ld), 0.0), "
                         "bh = max(dot(-tN, c[80].xyz), 0.0);") != NULL);
    HT_CHECK(strstr(buf, "float bl = max(dot(-tN, VP), 0.0), "
                         "bh = max(dot(-tN, normalize(VP + eyeV)), 0.0);") != NULL);
    HT_CHECK(strstr(buf, "float bf = bl > 0.0 ? NVC(bh, c[183], 10000.0) : 0.0;") != NULL);
    /* no SEPARATE_SPECULAR: the front vertex specular, back arrays or not */
    HT_CHECK(strstr(buf, "oB0.rgb += oB1.rgb;\n    oB1 = v4;") != NULL);
    HT_CHECK(strstr(buf, "oB1 = v8") == NULL && strstr(buf, "oB0 = v7") == NULL);
    /* the other compositions: ambient from the material, emission from a
     * vertex colour; both from vertex colours */
    k.bsrc[0] = 2;
    HT_CHECK(gen(&k, 0x1E1F) > 0);
    HT_CHECK(strstr(buf, "oB0 = vec4(c[156].rgb + v4.rgb * c[157].rgb, c[156].w);") != NULL);
    k.bsrc[1] = 1;
    HT_CHECK(gen(&k, 0x1E1F) > 0);
    HT_CHECK(strstr(buf, "oB0 = vec4(v4.rgb + v3.rgb * c[157].rgb, c[156].w);") != NULL);
    /* unlit: black, alpha 1, whatever the back colour registers hold */
    k.lighting = 0;
    HT_CHECK(gen(&k, 0x1E1F | 3u << 7) > 0);
    HT_CHECK(strstr(buf, "oB0 = vec4(0.0, 0.0, 0.0, 1.0);\n"
                         "    oB1 = vec4(0.0, 0.0, 0.0, 1.0);") != NULL);
    HT_CHECK(strstr(buf, "oB1 = v4") == NULL);
    /* off: as before (the pixel stage doesn't read them) */
    k.two_side = 0;
    HT_CHECK(gen(&k, 0x1E1F) > 0);
    HT_CHECK(strstr(buf, "oB0 = v7;\n    oB1 = v8;") != NULL);
}

static void test_lit_specular_disabled(void)
{
    /* ControlFlags_FF on Xbox: highlights remain in diffuse, and a combiner
     * reading specular alone gets opaque black regardless of LIGHT_CONTROL. */
    HleFfhwKey k = { .magic = HLE_FFHW_MAGIC, .lighting = 1, .separate_spec = 1,
                     .alpha_mat_spec = 1, .light = { 1 } };
    HleVshKey vk = { .in_mask = 0x1F, .spec_off = 1 };
    uint32_t tok[HLE_FFHW_SLOTS * 4];
    for (unsigned two_side = 0; two_side < 2; two_side++) {
        k.two_side = two_side;
        memcpy(tok, &k, sizeof(k));
        HT_CHECK(hle_ffhw_glsl(tok, HLE_FFHW_SLOTS, &vk, buf, sizeof(buf)) > 0);
        HT_CHECK(strstr(buf, "oD0.rgb += oD1.rgb;") != NULL);
        HT_CHECK((strstr(buf, "oB0.rgb += oB1.rgb;") != NULL) == (two_side != 0));
        HT_CHECK(strstr(buf, "vSpecular = vec4(0.0, 0.0, 0.0, 1.0);") != NULL);
        HT_CHECK(strstr(buf, "vBack1 = vec4(0.0, 0.0, 0.0, 1.0);") != NULL);
        dump(two_side ? "ffhw_specular_disabled_two_side" : "ffhw_specular_disabled");
    }
}

static void test_not_a_key(void)
{
    uint32_t tok[12] = { 0 };
    HleVshKey vk = { 0 };
    HT_CHECK(hle_ffhw_glsl(tok, 3, &vk, buf, sizeof(buf)) == 0);
    HT_CHECK(!hle_ffhw_is(tok, 3));
    tok[0] = HLE_FFHW_MAGIC;
    HT_CHECK(!hle_ffhw_is(tok, 2));
    HT_CHECK(hle_ffhw_is(tok, 3));
}

/* ffhw.c's NVC, as the shader evaluates it */
static float nvc(float x, const float k[3], float top)
{
    const float q = k[1] * x + k[2];
    return x <= -k[0] ? 0.0f : q == 0.0f ? 1.0e10f : q < 0.0f ? 0.0f : fminf((x + k[0]) / q, top);
}

/* Saturated attenuation, falloff and specular can combine in one light.
 * Its contributions must stay finite and preserve zero colour channels. */
static void test_saturated(void)
{
    const float zero[3] = { 0.0f, 0.0f, 0.0f };
    const float ad = 0.0f, s = 1.2f, nl = 0.7f, nh = 0.9f;
    float att = ad == 0.0f ? 1.0e10f : fmaxf(fminf(1.0f / ad, 1.0e10f), -1.0e10f);
    att = fminf(att * nvc(fminf(s, 1.0f), zero, 1.0f), 1.0e10f);
    const float pf = nvc(nh, zero, 10000.0f);
    const float col[3] = { 1.0f, 0.0f, 0.5f };
    for (unsigned c = 0; c < 3; c++) {
        const float d = col[c] * att + col[c] * (att * nl), sp = col[c] * (att * pf);
        HT_CHECK(isfinite(d) && isfinite(sp));
        HT_CHECK(col[c] != 0.0f || (d == 0.0f && sp == 0.0f));
        HT_CHECK(col[c] == 0.0f || (fminf(d, 1.0f) == 1.0f && fminf(sp, 1.0f) == 1.0f));
    }
    /* A tiny negative denominator clamps to the negative limit. */
    HT_CHECK(fmaxf(fminf(1.0f / -1.0e-39f, 1.0e10f), -1.0e10f) == -1.0e10f);
    HleFfhwKey k = { .magic = HLE_FFHW_MAGIC, .lighting = 1, .light = { 2 } };
    HT_CHECK(gen(&k, 0x1E1F) > 0);
    HT_CHECK(strstr(buf, "att = ad == 0.0 ? 1.0e10 : clamp(1.0 / ad, -1.0e10, 1.0e10);") != NULL);
}

/* The lighting unit's curve over the coefficients D3D writes, as the NV2A
 * tests captured them (nxdk_pgraph_tests' Specular and Lighting spotlight):
 * close to the powers they stand for, 1 at x = 1 */
static void test_curve(void)
{
    static const struct { float power, k[3]; } k_sets[] = {
        { 2.0f, { -0.170208f, -0.855843f, 1.68563f } },
        { 4.0f, { -0.421539f, -1.70592f, 2.28438f } },
        { 8.0f, { -0.64766f, -2.36199f, 2.71433f } },
        { 16.0f, { -0.803673f, -2.7813f, 2.97762f } },
        { 24.0f, { -0.863702f, -2.92701f, 3.06331f } },
        { 50.0f, { -0.932112f, -3.097628f, 3.165516f } },  /* Ninja Gaiden Black's */
    };
    for (unsigned i = 0; i < sizeof(k_sets) / sizeof(k_sets[0]); i++) {
        HT_CHECK(fabsf(nvc(1.0f, k_sets[i].k, 10000.0f) - 1.0f) < 1e-4f);
        for (float x = 0.5f; x < 1.0f; x += 0.01f) {
            HT_CHECK(fabsf(nvc(x, k_sets[i].k, 10000.0f) - powf(x, k_sets[i].power)) < 0.045f);
        }
        HT_CHECK(nvc(-k_sets[i].k[0], k_sets[i].k, 10000.0f) == 0.0f);   /* its zero */
    }
    /* falloff 1 (D3D's default, the power-on value) isn't linear */
    const float pow1[3] = { 0.0f, -0.494592f, 1.494592f };
    HT_CHECK(fabsf(nvc(0.5f, pow1, 1.0f) - 0.40087f) < 1e-4f);
    /* falloff 0: 1 throughout */
    const float pow0[3] = { -0.0f, 1.0f, 0.0f };
    HT_CHECK(nvc(0.25f, pow0, 1.0f) == 1.0f);
    /* A zero denominator uses the saturation value. */
    const float none[3] = { 0.0f, 0.0f, 0.0f };
    HT_CHECK(nvc(0.5f, none, 1.0f) == 1.0e10f);
    /* the shader's macro is the same expression */
    HleFfhwKey k = { .magic = HLE_FFHW_MAGIC, .lighting = 1, .specular = 1, .light = { 1 } };
    HT_CHECK(gen(&k, 0x1E1F) > 0);
    HT_CHECK(strstr(buf, "#define NVC(d, k, top) ((d) <= -(k).x ? 0.0 : (k).y * (d) + (k).z == 0.0 ? 1.0e10 : "
                         "(k).y * (d) + (k).z < 0.0 ? 0.0 : min(((d) + (k).x) / ((k).y * (d) + (k).z), (top)))\n")
             != NULL);
    HT_CHECK(strstr(buf, "#undef NVC") != NULL);
}

int main(void)
{
    HT_RUN(test_unlit);
    HT_RUN(test_lit_skinned);
    HT_RUN(test_two_side);
    HT_RUN(test_lit_specular_disabled);
    HT_RUN(test_not_a_key);
    HT_RUN(test_curve);
    HT_RUN(test_saturated);
    HT_DONE();
}
