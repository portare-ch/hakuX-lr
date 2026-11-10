/*
 * hakuX HLE GPU — the NV2A fixed-function vertex stage as GLSL (pure). See
 * ffhw.h. The transform engine's fixed function, as NV_vertex_program's
 * documentation and the hardware tests describe it (LLE's generator read as
 * reference only):
 * - skinning blends the position and normal over model-view matrices 0..n-1
 *   (and their inverses), the last weight generated (1 - the others) in the
 *   "G" modes; without skinning the composite matrix includes model-view;
 * - lighting in eye space, the registers D3D's products: light colours
 *   premultiplied by the material where its source is the material, the
 *   scene ambient (with the emission) and material emission registers
 *   reused for the vertex-colour sources;
 * - the composite matrix includes the viewport scale: x, y, z / w plus the
 *   viewport offset is the screen-space position a program leaves;
 * - the specular and spot terms are the lighting unit's curve, not powers
 *   (NVC below).
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "hw/xbox/hle/shader/ffhw.h"

typedef struct Buf {
    char *p;
    size_t cap, len;
    bool overflow;
} Buf;

static void put(Buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, b->len < b->cap ? b->cap - b->len : 0, fmt, ap);
    va_end(ap);
    if (n < 0 || b->len + (size_t)n >= b->cap) {
        b->overflow = true;
        b->len = b->cap;
        return;
    }
    b->len += (size_t)n;
}

/* A colour source of SET_COLOR_MATERIAL: the material's (the register
 * already holds it: 1), the diffuse or the specular register. */
static const char *src_rgb(unsigned s)
{
    return s == 1 ? "v3.rgb" : s == 2 ? "v4.rgb" : "vec3(1.0)";
}

/* NV2A approximates specular power and spotlight falloff with
 * (x + L) / (M*x + N) (NVIDIA US6417851). D3D's coefficient table is
 * documented in xgu's xgux.h. N = 1 + L - M gives 1 at x = 1.
 * Xbox spotlight captures show saturation at a zero denominator. Use a
 * finite value so combining lighting factors stays within FP32. Negative
 * denominators return zero, following xemu's approximation.
 * Specular uses the first three SET_SPECULAR_PARAMS values. */
#define SAT "1.0e10"
#define NVC_DEFINE "#define NVC(d, k, top) ((d) <= -(k).x ? 0.0 : (k).y * (d) + (k).z == 0.0 ? " SAT " : " \
                   "(k).y * (d) + (k).z < 0.0 ? 0.0 : min(((d) + (k).x) / ((k).y * (d) + (k).z), (top)))\n"

static void skinning(Buf *b, const HleFfhwKey *k)
{
    static const struct { uint8_t n, mix; } k_skin[7] = {
        { 0, 0 }, { 2, 1 }, { 2, 0 }, { 3, 1 }, { 3, 0 }, { 4, 1 }, { 4, 0 },
    };
    unsigned n = k->skin < 7 ? k_skin[k->skin].n : 0, mix = k->skin < 7 ? k_skin[k->skin].mix : 0;
    if (!n) {
        put(b, "    tPos = XF(pos, %d);\n"
               "    tN = XF(n4, %d).xyz;\n", HLE_FFHW_C_MODELVIEW, HLE_FFHW_C_INVMODELVIEW);
        return;
    }
    put(b, "    tPos = vec4(0.0);\n"
           "    tN = vec3(0.0);\n"
           "    float wn = 1.0, wi;\n");
    for (unsigned i = 0; i < n; i++) {
        if (mix && i == n - 1) {
            put(b, "    wi = wn;\n");
        } else {
            put(b, "    wi = v1.%c;\n", "xyzw"[i]);
            if (mix) {
                put(b, "    wn -= wi;\n");
            }
        }
        put(b, "    tPos += XF(pos, %u) * wi;\n"
               "    tN += XF(n4, %u).xyz * wi;\n", HLE_FFHW_C_MODELVIEW + 4 * i,
            HLE_FFHW_C_INVMODELVIEW + 4 * i);
    }
}

static void texgen(Buf *b, const HleFfhwKey *k)
{
    for (unsigned s = 0; s < 4; s++) {
        put(b, "    oT%u = v%u;\n", s, 9 + s);
        for (unsigned j = 0; j < 4; j++) {
            const unsigned g = k->texgen[s][j], plane = HLE_FFHW_C_TEXGEN + 4 * s + j;
            const char c = "xyzw"[j];
            if (g == HLE_FFHW_TG_EYE) {
                put(b, "    oT%u.%c = dot(c[%u], tPos);\n", s, c, plane);
            } else if (g == HLE_FFHW_TG_OBJECT) {
                put(b, "    oT%u.%c = dot(c[%u], pos);\n", s, c, plane);
            } else if (g == HLE_FFHW_TG_SPHERE && j < 2) {
                put(b, "    { vec3 r = reflect(normalize(tPos.xyz), tN);\n"
                       "      oT%u.%c = r.%c / (2.0 * length(r + vec3(0.0, 0.0, 1.0))) + 0.5; }\n",
                    s, c, c);
            } else if (g == HLE_FFHW_TG_REFLECTION && j < 3) {
                put(b, "    oT%u.%c = reflect(normalize(tPos.xyz), tN).%c;\n", s, c, c);
            } else if (g == HLE_FFHW_TG_NORMAL && j < 3) {
                put(b, "    oT%u.%c = tN.%c;\n", s, c, c);
            }
        }
        if (k->texmat & (1u << s)) {
            put(b, "    oT%u = XF(oT%u, %u);\n", s, s, HLE_FFHW_C_TEXMAT + 4 * s);
        }
    }
}

/* The colour lighting starts from, before the lights: src[0] the emission
 * source, src[1] the ambient one (SET_COLOR_MATERIAL), amb the scene ambient
 * register (SET_SCENE_AMBIENT_COLOR or the back's), emi the material
 * emission one (SET_MATERIAL_EMISSION or the back's), diffuse's source the
 * alpha (the material's in amb's w). Where either source is the material,
 * the scene ambient register already holds what D3D folds into it (ambient
 * times material ambient, plus emission) and is used as it is, plus the
 * vertex colour the other source names scaled by the emission register; the
 * Xbox draws FromMaterial and FromMaterial_me0_5 (material emission 0 and
 * 0.5) alike (nxdk_pgraph_tests). With both from vertex colours, the
 * emission colour plus the ambient one scaled by the emission register (as
 * xemu). */
static void base_colour(Buf *b, const char *out, const uint8_t src[4], unsigned amb, unsigned emi)
{
    char alpha[32];
    snprintf(alpha, sizeof(alpha), src[2] == 1 ? "v3.a" : src[2] == 2 ? "v4.a" : "c[%u].w", amb);
    if (src[0] == 0 && src[1] == 0) {
        put(b, "    %s = vec4(c[%u].rgb, %s);\n", out, amb, alpha);
    } else if (src[0] == 0 || src[1] == 0) {
        put(b, "    %s = vec4(c[%u].rgb + %s * c[%u].rgb, %s);\n", out, amb,
            src_rgb(src[0] ? src[0] : src[1]), emi, alpha);
    } else {
        put(b, "    %s = vec4(%s + %s * c[%u].rgb, %s);\n", out, src_rgb(src[0]),
            src_rgb(src[1]), emi, alpha);
    }
}

static void lighting(Buf *b, const HleFfhwKey *k)
{
    if (!k->lighting) {
        put(b, "    oD0 = v3;\n    oD1 = v4;\n");
        return;
    }
    base_colour(b, "oD0", k->src, HLE_FFHW_C_AMBIENT, HLE_FFHW_C_EMISSION);
    put(b, "    oD1 = vec4(0.0, 0.0, 0.0, v4.a);\n");
    if (k->two_side) {
        /* the back the same from the back registers, by the back's colour
         * sources (SET_COLOR_MATERIAL 15:8), whose vertex colours are the
         * front's (v3, v4): the NV2A tests pass the front specular through
         * on back faces as well (Specular back, ControlFlags_FF), and its
         * NoLight reference shows the back specular alpha uniform where the
         * back specular colours' alphas vary (v4.a) */
        base_colour(b, "oB0", k->bsrc, HLE_FFHW_C_BACK_AMBIENT, HLE_FFHW_C_BACK_EMISSION);
        put(b, "    oB1 = vec4(0.0, 0.0, 0.0, v4.a);\n");
    }
    /* Without LOCALEYE, local lights use the light vector itself as the
     * half vector (nxdk_pgraph_tests' Lighting control, FF_0x000001).
     * Infinite lights use their supplied half vector below. */
    if (k->local_eye) {
        put(b, "    vec3 eyeV = normalize(c[%u].xyz / c[%u].w - tPos.xyz / tPos.w);\n",
            HLE_FFHW_C_EYE, HLE_FFHW_C_EYE);
    } else {
        put(b, "    vec3 eyeV = vec3(0.0);\n");
    }
    for (unsigned i = 0; i < 8; i++) {
        const unsigned t = k->light[i], L = HLE_FFHW_C_LIGHTS + HLE_FFHW_LIGHT_VEC4 * i;
        if (!t) {
            continue;
        }
        put(b, "    { /* light %u */\n"
               "      float att = 1.0, nl, nh;\n"
               "      bool lit = true;\n", i);
        if (t == 1) {
            put(b, "      vec3 Ld = normalize(c[%u].xyz);\n"
                   "      nl = max(dot(tN, Ld), 0.0);\n", L + HLE_FFHW_L_DIRECTION);
            if (k->local_eye) {
                put(b, "      nh = max(dot(tN, normalize(Ld + eyeV)), 0.0);\n");
            } else {
                put(b, "      nh = max(dot(tN, c[%u].xyz), 0.0);\n", L + HLE_FFHW_L_HALF);
            }
        } else {
            put(b, "      vec3 VP = c[%u].xyz - tPos.xyz / tPos.w;\n"
                   "      float d = length(VP);\n"
                   "      lit = d <= c[%u].x;\n"
                   "      VP = normalize(VP);\n"
                   "      vec3 at = c[%u].xyz;\n"
                   "      float ad = at.x + at.y * d + at.z * d * d;\n"
                   "      att = ad == 0.0 ? " SAT " : clamp(1.0 / ad, -" SAT ", " SAT ");\n"
                   "      nl = max(dot(tN, VP), 0.0);\n"
                   "      nh = max(dot(tN, normalize(VP + eyeV)), 0.0);\n",
                L + HLE_FFHW_L_POSITION, L + HLE_FFHW_L_RANGE, L + HLE_FFHW_L_ATTENUATION);
            if (t == 3) {
                /* D3D scales s to 0 at the outer cone and 1 at the inner cone.
                 * Evaluate the curve at 1 throughout the inner cone. */
                put(b, "      float s = dot(c[%u].xyz, VP) + c[%u].w;\n"
                       "      if (s <= 0.0) att = 0.0;\n"
                       "      else att = min(att * NVC(min(s, 1.0), c[%u], 1.0), " SAT ");\n",
                    L + HLE_FFHW_L_SPOT, L + HLE_FFHW_L_SPOT, L + HLE_FFHW_L_FALLOFF);
            }
        }
        put(b, "      if (lit) {\n"
               "        float pf = nl > 0.0 ? NVC(nh, c[%u], 10000.0) : 0.0;\n"
               "        oD0.rgb += c[%u].rgb * att;\n"
               "        oD0.rgb += %s * c[%u].rgb * (att * nl);\n"
               "        oD1.rgb += %s * c[%u].rgb * (att * pf);\n"
               "      }\n",
            HLE_FFHW_C_SPECULAR, L + HLE_FFHW_L_AMBIENT, src_rgb(k->src[2]), L + HLE_FFHW_L_DIFFUSE,
            src_rgb(k->src[3]), L + HLE_FFHW_L_SPECULAR);
        if (k->two_side) {
            /* the back faces: the same light along -N, the back light colours
             * and the back specular curve */
            const unsigned BL = HLE_FFHW_C_BACK_LIGHTS + 3 * i;
            char h[48];
            if (t != 1 || k->local_eye) {
                snprintf(h, sizeof(h), "normalize(%s + eyeV)", t == 1 ? "Ld" : "VP");
            } else {
                snprintf(h, sizeof(h), "c[%u].xyz", L + HLE_FFHW_L_HALF);
            }
            put(b, "      if (lit) {\n"
                   "        float bl = max(dot(-tN, %s), 0.0), bh = max(dot(-tN, %s), 0.0);\n"
                   "        float bf = bl > 0.0 ? NVC(bh, c[%u], 10000.0) : 0.0;\n"
                   "        oB0.rgb += c[%u].rgb * att;\n"
                   "        oB0.rgb += %s * c[%u].rgb * (att * bl);\n"
                   "        oB1.rgb += %s * c[%u].rgb * (att * bf);\n"
                   "      }\n",
                t == 1 ? "Ld" : "VP", h, HLE_FFHW_C_BACK_SPECULAR, BL, src_rgb(k->bsrc[2]), BL + 1,
                src_rgb(k->bsrc[3]), BL + 2);
        }
        put(b, "    }\n");
    }
}

size_t hle_ffhw_glsl(const uint32_t *tokens, unsigned nslots, const HleVshKey *vk, char *buf,
                     size_t cap)
{
    if (!hle_ffhw_is(tokens, nslots)) {
        return 0;
    }
    HleFfhwKey k;
    memcpy(&k, tokens, sizeof(k));
    char body[24576];
    Buf b = { body, sizeof(body), 0, false };
    put(&b, "#define XF(v, b) vec4(dot((v), c[(b)]), dot((v), c[(b) + 1]), "
            "dot((v), c[(b) + 2]), dot((v), c[(b) + 3]))\n"
            NVC_DEFINE
            "  {\n"
            "    vec4 pos = v0, n4 = vec4(v2.xyz, 0.0), tPos;\n"
            "    vec3 tN;\n");
    skinning(&b, &k);
    if (k.normalize) {
        put(&b, "    tN = normalize(tN);\n");
    }
    texgen(&b, &k);
    lighting(&b, &k);
    /* the back colours: lit by lighting() under two-sided lighting; unlit,
     * (0, 0, 0, 1) whatever the back colour registers hold and whatever
     * SET_LIGHT_CONTROL says (the NV2A tests' Specular back,
     * ControlFlagsLightDisable_FF). Without two-sided lighting the pixel
     * stage doesn't read them. */
    const bool lit2 = k.lighting && k.two_side;
    if (!lit2) {
        put(&b, k.two_side ? "    oB0 = vec4(0.0, 0.0, 0.0, 1.0);\n"
                             "    oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
                           : "    oB0 = v7;\n    oB1 = v8;\n");
    }
    if (!k.specular) {
        /* With specular output disabled, the NV2A adds the highlights to
         * diffuse and exports opaque black as specular (ControlFlags_FF). */
        if (k.lighting) {
            put(&b, "    oD0.rgb += oD1.rgb;\n");
        }
        if (lit2) {
            put(&b, "    oB0.rgb += oB1.rgb;\n");
        }
        put(&b, "    oD1 = vec4(0.0, 0.0, 0.0, 1.0);\n    oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n");
    } else {
        if (!k.separate_spec) {
            if (k.lighting) {
                put(&b, "    oD0.rgb += oD1.rgb;\n");
            }
            put(&b, "    oD1 = v4;\n");
            if (lit2) {
                /* the front vertex specular on back faces too (Specular back,
                 * ControlFlagsNoLight_FF and ControlFlags_FF) */
                put(&b, "    oB0.rgb += oB1.rgb;\n    oB1 = v4;\n");
            }
        }
        if (!k.alpha_mat_spec) {
            put(&b, "    oD1.a = 1.0;\n    oB1.a = 1.0;\n");
        }
    }
    if (k.fog) {
        /* the fog coordinate by SET_FOG_GEN_MODE */
        switch (k.fog - 1) {
        case 0: put(&b, "    oFog = vec4(clamp(v4.a, 0.0, 1.0));\n"); break;
        case 1: put(&b, "    oFog = vec4(length(tPos.xyz));\n"); break;
        case 2: put(&b, "    oFog = vec4(dot(c[%u].xyz, tPos.xyz) + c[%u].w);\n",
                    HLE_FFHW_C_FOGPLANE, HLE_FFHW_C_FOGPLANE); break;
        case 3: put(&b, "    oFog = vec4(abs(dot(c[%u].xyz, tPos.xyz) + c[%u].w));\n",
                    HLE_FFHW_C_FOGPLANE, HLE_FFHW_C_FOGPLANE); break;
        default: put(&b, "    oFog = vec4(v5.x);\n"); break;
        }
    }
    if (k.points) {
        const unsigned p = HLE_FFHW_C_POINT;
        put(&b, "    { float de = length(tPos.xyz);\n"
                "      float pmin = min(c[%u].w, 63.875), pmax = min(c[%u].w + pmin, 63.875);\n"
                "      oPts.x = clamp((inversesqrt(c[%u].x + c[%u].y * de + c[%u].z * de * de) + "
                "c[%u].z) * c[%u].w + c[%u].w, pmin, pmax); }\n",
            p + 1, p, p, p, p, p + 1, p, p + 1);
    } else {
        put(&b, "    oPts.x = max(c[%u].z, 1.0);\n", HLE_FFHW_C_PARAMS);
    }
    /* screen space: the composite (viewport scale included) over w, plus the
     * viewport offset; w kept for the clip space (as RCC, its reciprocal's
     * magnitude clamped) */
    put(&b, "    vec4 sp = XF(%s, %u);\n"
            "    float aw = clamp(abs(sp.w), 5.421011e-20, 1.8446744e19);\n"
            "    float sw = sp.w < 0.0 ? -aw : aw;\n"
            "    oPos = vec4(sp.xyz / sw + c[%u].xyz, sw);\n"
            "  }\n"
            "#undef XF\n"
            "#undef NVC\n",
        k.skin ? "tPos" : "pos", HLE_FFHW_C_COMPOSITE, HLE_FFHW_C_VPOFF);
    if (b.overflow) {
        return 0;
    }
    return hle_vsh_glsl_wrap(vk, body, k.fog != 0, buf, cap);
}
