/*
 * ps3recomp - RSX Vertex Program (NV40 ISA) -> HLSL decompiler
 *
 * See rsx_vp_decompiler.h. The 4-word instruction encoding (D0..D3 fields,
 * source operand layout, opcode numbers) follows the public NV40 vertex
 * program layout as documented in the NV_vertex_program3 extension spec and
 * the envytools rnndb; RPCS3 was consulted as a READ-ONLY semantics oracle
 * for the field positions and the dest-write rules (no code copied).
 *
 * Field facts used (verified against the oracle 2026-07-03):
 *   D0: mask_w/z/y/x pairs [2:9] are the CC swizzle, cond [10:12],
 *       cond_test_enable [13], dst_tmp [15:20], src0/1/2_abs [21:23],
 *       addr_reg_sel [24], saturate [26], vec_result [30]
 *   D1: src0h [0:7], input_src [8:11], const_src [12:21],
 *       vec_opcode [22:26], sca_opcode [27:31]
 *   D2: src2h [0:5], src1 [6:22], src0l [23:31]
 *   D3: end [0], index_const [1], dst [2:6], sca_dst_tmp [7:12],
 *       vec_writemask w/z/y/x [13:16], sca_writemask w/z/y/x [17:20],
 *       src2l [21:31]
 *   SRC (17 bits): reg_type [0:1] (1 = temp, 2 = input, 3 = constant),
 *       tmp_src [2:7], swz w/z/y/x [8:15] (2 bits each), neg [16]
 *   Dest rules: the VEC result goes to output o[D3.dst] when D0.vec_result
 *       (dst 0x1F = none) AND to temp r[D0.dst_tmp] (0x3F = none); the SCA
 *       result goes to o[D3.dst] when !D0.vec_result, else to
 *       r[D3.sca_dst_tmp]. The SCA unit reads SRC2.
 *   VEC ADD reads src0 + src2 (not src1).
 *   Output register map: o0 = HPOS, o1 = COL0, o2 = COL1, o3/o4 = back-face
 *       colors, o5.x = FOG, o6 = point size / TEX9, o7..o14 = TEX0..7,
 *       o15 = TEX8. Unwritten outputs default to (0,0,0,1).
 */
#include "rsx_vp_decompiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* LD_VP_CC_EITHER (default on): an instruction whose vector or scalar unit
 * has no destination (no temp, no output) writes its result to the condition
 * register when EITHER update bit (D0 bit 14 or 29) is set (RPCS3
 * VertexProgramDecompiler::SetDST). Only both bits were honoured, and only for
 * the vector unit: God of War: Ascension's level programs carry such writes
 * with bit 14 alone (3 of the 39 condition-only writes in the shader stats),
 * so the branches and predicated writes after them read a stale condition.
 * Text is unchanged for every other instruction. =0: as before. */
static int vp_cc_either_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("LD_VP_CC_EITHER"); on = !(e && e[0] == '0'); }
    return on;
}

/* ---- little-endian word read (RPCS3-capture native order) --------------- */
static u32 rd_le(const u8* p)
{ return (u32)p[0] | ((u32)p[1]<<8) | ((u32)p[2]<<16) | ((u32)p[3]<<24); }

/* ---- opcode names ------------------------------------------------------ */
const char* rsx_vp_vec_name(u32 op)
{
    static const char* n[] = {"NOP","MOV","MUL","ADD","MAD","DP3","DPH","DP4",
        "DST","MIN","MAX","SLT","SGE","ARL","FRC","FLR","SEQ","SFL","SGT","SLE",
        "SNE","STR","SSG","?","?","TXL"};
    return op < 26 ? n[op] : "?";
}
const char* rsx_vp_sca_name(u32 op)
{
    static const char* n[] = {"NOP","MOV","RCP","RCC","RSQ","EXP","LOG","LIT",
        "BRA","BRI","CAL","CLI","RET","LG2","EX2","SIN","COS","BRB","CLB","PSH","POP"};
    return op < 21 ? n[op] : "?";
}

u32 rsx_vp_program_size_instrs(const u8* ucode, u32 max_bytes)
{
    if (!ucode) return 0;
    u32 off = 0, instrs = 0;
    while (off + 16 <= max_bytes) {
        u32 d3 = rd_le(ucode + off + 12);
        instrs++;
        off += 16;
        if (d3 & 1u) return instrs;   /* D3.end */
    }
    return 0;
}

/* ---- output appender --------------------------------------------------- */
typedef struct { char* p; u32 cap; u32 len; int overflow; } Out;
static void emit(Out* o, const char* s)
{
    u32 n = (u32)strlen(s);
    if (o->len + n + 1 > o->cap) { o->overflow = 1; return; }
    memcpy(o->p + o->len, s, n); o->len += n; o->p[o->len] = '\0';
}

/* ---- source operand ---------------------------------------------------- */
static const char SWZ[4] = {'x','y','z','w'};

/* Build the HLSL expression for one 17-bit source field.
 * reg_type: 1 = temp r[], 2 = input v[], 3 = constant vp_c[]. */
static void emit_src(u32 src, int is_abs, u32 input_src, u32 const_src,
                     int index_const, u32 addr_sel, u32 addr_swz,
                     char* buf, u32 bufsz)
{
    u32 reg_type = src & 3;
    u32 tmp_src  = (src >> 2) & 0x3F;
    u32 sw = (src >> 8) & 3, sz = (src >> 10) & 3;
    u32 sy = (src >> 12) & 3, sx = (src >> 14) & 3;
    int neg = (src >> 16) & 1;

    char base[64];
    switch (reg_type) {
    case 2:  snprintf(base, sizeof(base), "v[%u]", input_src & 15); break;
    case 3:
        if (index_const)
            snprintf(base, sizeof(base), "vp_c[(%uu + a%u.%c) & 511u]",
                     const_src, addr_sel & 1, SWZ[addr_swz & 3]);
        else
            snprintf(base, sizeof(base), "vp_c[%u]", const_src & 511);
        break;
    default: snprintf(base, sizeof(base), "r[%u]", tmp_src & 31); break;
    }

    char swz[8];
    swz[0]='.'; swz[1]=SWZ[sx]; swz[2]=SWZ[sy]; swz[3]=SWZ[sz]; swz[4]=SWZ[sw]; swz[5]='\0';

    const char* pre = neg ? "-" : "";
    if (is_abs) snprintf(buf, bufsz, "%sabs(%s)%s", pre, base, swz);
    else        snprintf(buf, bufsz, "%s(%s)%s", pre, base, swz);
}

/* writemask string from 4 bits (x,y,z,w order) -> "xyzw" subset */
static void mask_str(int mx,int my,int mz,int mw, char* m)
{
    int i=0;
    if (mx) m[i++]='x';
    if (my) m[i++]='y';
    if (mz) m[i++]='z';
    if (mw) m[i++]='w';
    m[i]='\0';
}

/* Emit "    dst.mask = _v.mask;" (skips when mask empty). */
static void emit_store(Out* b, const char* dst_fmt, u32 idx, const char* m)
{
    if (!m[0]) return;
    char line[96];
    char dst[32];
    snprintf(dst, sizeof(dst), dst_fmt, idx);
    snprintf(line, sizeof(line), "        %s.%s = _v.%s;\n", dst, m, m);
    emit(b, line);
}

/* ---- flow control -------------------------------------------------------- */
#define RSX_VP_MAX_INSTR 512

static u32 s_vp_branch_base = 0;   /* VP slot the program being decompiled starts at */
void rsx_vp_set_branch_base(u32 start_slot) { s_vp_branch_base = start_slot; }

/* BRA BRI CAL CLI RET BRB CLB (scalar opcodes). */
static int vp_is_flow(u32 sca_op)
{
    return sca_op == 0x08 || sca_op == 0x09 || sca_op == 0x0A || sca_op == 0x0B ||
           sca_op == 0x0C || sca_op == 0x11 || sca_op == 0x12;
}

/* Branch target (NV40 layout, RPCS3 as oracle): D0[23] << 9 | D2[0:5] << 3 |
 * D3[29:31], an absolute VP slot; made relative to the program start. */
static u32 vp_branch_target(u32 d0, u32 d2, u32 d3)
{
    const u32 t = (((d0 >> 23) & 1u) << 9) | ((d2 & 0x3Fu) << 3) | ((d3 >> 29) & 7u);
    return t >= s_vp_branch_base ? t - s_vp_branch_base : t;
}

/* Condition-code state for one instruction (D0). The NV40 VP has two CC
 * registers; a write under cond-test lands only in the components whose
 * CC component (through the D0 CC swizzle) compares true against zero.
 * cond: 0 = never, 1 LT, 2 EQ, 3 LE, 4 GT, 5 NE, 6 GE, 7 = always. */
typedef struct { int test; u32 cond, sel; char swz[4]; } CondCtx;

/* Emit the store, predicated per component when a cond test is active. */
static void emit_store_cc(Out* b, const char* dst_fmt, u32 idx, const char* m, const CondCtx* cc)
{
    if (!cc->test) { emit_store(b, dst_fmt, idx, m); return; }
    if (cc->cond == 0) return;                      /* FL: never writes */
    static const char* cmp[8] = { 0, "<", "==", "<=", ">", "!=", ">=", 0 };
    char dst[32];
    snprintf(dst, sizeof(dst), dst_fmt, idx);
    for (const char* c = m; *c; c++) {
        const int ci = (*c == 'x') ? 0 : (*c == 'y') ? 1 : (*c == 'z') ? 2 : 3;
        char line[160];
        snprintf(line, sizeof(line), "        if (cc[%u].%c %s 0.0) %s.%c = _v.%c;\n",
                 cc->sel, cc->swz[ci], cmp[cc->cond], dst, *c, *c);
        emit(b, line);
    }
}

static u32 vec_source_use(u32 op, int* exact)
{
    switch (op) {
    case 0x00: return 0;       /* NOP */
    case 0x01: return 1u;      /* src0 */
    case 0x02: return 3u;      /* src0, src1 */
    case 0x03: return 5u;      /* src0, src2 */
    case 0x04: return 7u;      /* src0, src1, src2 */
    case 0x05: case 0x06: case 0x07: case 0x08:
    case 0x09: case 0x0A: case 0x0B: case 0x0C:
    case 0x10: case 0x12: case 0x13: case 0x14:
        return 3u;             /* src0, src1 */
    case 0x0D: case 0x0E: case 0x0F: case 0x16: case 0x19:
        return 1u;             /* src0 */
    case 0x11: case 0x15:
        return 0;              /* source-independent constants */
    default:
        *exact = 0;
        return 0;
    }
}

static u32 sca_source_use(u32 op, int* exact)
{
    switch (op) {
    case 0x00:
        return 0;
    case 0x01: case 0x02: case 0x03: case 0x04:
    case 0x05: case 0x06: case 0x07:
    case 0x0D: case 0x0E: case 0x0F: case 0x10:
        return 4u;             /* scalar ALU reads src2 */
    default:
        /* Flow control and currently unsupported scalar opcodes make a
         * narrowed data-flow result unsafe. */
        *exact = 0;
        return 0;
    }
}

int rsx_vp_analyze_inputs(const u8* ucode, u32 max_bytes,
                          rsx_vp_input_analysis* analysis)
{
    if (!ucode || !analysis) return -1;
    analysis->input_mask = 0;
    analysis->exact = 1;
    u32 off = 0;
    int instrs = 0;
    int ended = 0;
    while (off + 16 <= max_bytes) {
        const u32 d1 = rd_le(ucode + off + 4);
        const u32 d2 = rd_le(ucode + off + 8);
        const u32 d3 = rd_le(ucode + off + 12);
        off += 16;
        instrs++;

        const u32 vec_op = (d1 >> 22) & 0x1F;
        const u32 sca_op = (d1 >> 27) & 0x1F;
        const u32 input_src = (d1 >> 8) & 0xF;
        const u32 src0 = ((d2 >> 23) & 0x1FF) | ((d1 & 0xFF) << 9);
        const u32 src1 = (d2 >> 6) & 0x1FFFF;
        const u32 src2 = ((d3 >> 21) & 0x7FF) | ((d2 & 0x3F) << 11);
        const u32 sources[3] = {src0, src1, src2};
        const int vec_writes = ((d3 >> 13) & 0xFu) != 0;
        const int sca_writes = ((d3 >> 17) & 0xFu) != 0;
        u32 use = 0;
        if (vec_writes && vec_op)
            use |= vec_source_use(vec_op, &analysis->exact);
        if (sca_writes && sca_op)
            use |= sca_source_use(sca_op, &analysis->exact);
        for (u32 source = 0; source < 3; source++)
            if ((use & (1u << source)) && (sources[source] & 3u) == 2u)
                analysis->input_mask |= 1u << input_src;

        if (d3 & 1u) {
            ended = 1;
            break;
        }
    }
    if (!ended) {
        analysis->input_mask = 0xFFFFu;
        analysis->exact = 0;
        return -1;
    }
    if (!analysis->exact)
        analysis->input_mask = 0xFFFFu;
    return instrs;
}

static int rsx_vp_decompile_impl(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, int compact_inputs,
    u32 input_mask, const u8* raw_kind, char* out, u32 out_size);
static const signed char* s_vp_pull_slot;   /* GP_PULL: set around one rsx_vp_decompile_impl call */

int rsx_vp_decompile(const u8* ucode, u32 max_bytes, char* out, u32 out_size)
{
    return rsx_vp_decompile_ex(ucode, max_bytes, 0u, out, out_size);
}

int rsx_vp_decompile_ex(const u8* ucode, u32 max_bytes, u32 vtex_mask,
                        char* out, u32 out_size)
{
    return rsx_vp_decompile_impl(
        ucode, max_bytes, vtex_mask, 0, 0xFFFFu, NULL, out, out_size);
}

int rsx_vp_decompile_compact_ex(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, u32 input_mask,
    char* out, u32 out_size)
{
    rsx_vp_input_analysis analysis;
    if (rsx_vp_analyze_inputs(ucode, max_bytes, &analysis) < 0 ||
        !analysis.exact ||
        (analysis.input_mask & ~(input_mask & 0xFFFFu)) != 0)
        input_mask = 0xFFFFu;
    else
        input_mask = analysis.input_mask;
    return rsx_vp_decompile_impl(
        ucode, max_bytes, vtex_mask, 1, input_mask, NULL, out, out_size);
}

int rsx_vp_decompile_compact_raw_ex(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, u32 input_mask,
    const u8 raw_kind[16], char* out, u32 out_size)
{
    if (!raw_kind)
        return -1;
    rsx_vp_input_analysis analysis;
    if (rsx_vp_analyze_inputs(ucode, max_bytes, &analysis) < 0 ||
        !analysis.exact ||
        (analysis.input_mask & ~(input_mask & 0xFFFFu)) != 0)
        input_mask = 0xFFFFu;   /* as the compact variant: all 16, the layout has them */
    else
        input_mask = analysis.input_mask;
    return rsx_vp_decompile_impl(
        ucode, max_bytes, vtex_mask, 1, input_mask, raw_kind, out, out_size);
}

/* LD_VTX_RAW: the guest's attribute bytes, as little-endian words, back to
 * the float4 rsx_vertex_decode_element makes of them. t is the RSX type
 * (1 s16n, 2 f32, 3 f16, 4 u8n, 5 s16, 6 cmp, 7 u8), n the size; both are
 * literals at every call, so the branches fold. Half: exponent 0 is a signed
 * zero, as rsx_compact_be_f16 has it. */
static const char k_vp_raw_decode[] =
    "uint vb_sw(uint w) { return (w >> 24) | ((w >> 8) & 0xFF00u) | ((w << 8) & 0xFF0000u) | (w << 24); }\n"
    "uint vb_h(uint4 w, uint c) { uint x = w[c >> 1];"
    " return (c & 1u) ? ((((x >> 16) & 0xFFu) << 8) | (x >> 24)) : (((x & 0xFFu) << 8) | ((x >> 8) & 0xFFu)); }\n"
    "uint vb_b(uint4 w, uint c) { return (w[c >> 2] >> ((c & 3u) * 8u)) & 0xFFu; }\n"
    "float vb_f16(uint h) { uint s = (h >> 15) << 31; uint e = (h >> 10) & 31u; uint m = h & 1023u;"
    " return asfloat(e == 0u ? s : (e == 31u ? (s | 0x7F800000u | (m << 13)) : (s | ((e + 112u) << 23) | (m << 13)))); }\n"
    "float vb_s16(uint h) { return (float)(asint(h << 16) >> 16); }\n"
    "float4 vdec(uint4 w, uint t, uint n) {\n"
    "    if (t == 6u) { uint x = vb_sw(w.x);\n"
    "        return float4((float)(asint(x << 21) >> 21) / 1023.0, (float)(asint(x << 10) >> 21) / 1023.0,\n"
    "                      (float)(asint(x) >> 22) / 511.0, 1.0); }\n"
    "    float4 o = float4(0, 0, 0, 1);\n"
    "    [unroll] for (uint c = 0u; c < 4u; c++) {\n"
    "        if (c < n) {\n"
    "            float v = 0.0;\n"
    "            if (t == 2u) v = asfloat(vb_sw(w[c]));\n"
    "            else if (t == 3u) v = vb_f16(vb_h(w, c));\n"
    "            else if (t == 1u) v = vb_s16(vb_h(w, c)) / 32767.0;\n"
    "            else if (t == 5u) v = vb_s16(vb_h(w, c));\n"
    "            else if (t == 4u) v = (float)vb_b(w, c) / 255.0;\n"
    "            else if (t == 7u) v = (float)vb_b(w, c);\n"
    "            o[c] = v;\n"
    "        }\n"
    "    }\n"
    "    return o;\n"
    "}\n";

/* GP_PULL: the raw buffer, the quotient table and the per-slot base / stride. Load = aligned dwords + a funnel
 * shift (byte offsets 1-3 allowed); the decode is vdec's with s16n / u8n / cmp read from the table, whose values
 * are the CPU converter's IEEE quotients (division on the GPU is not correctly rounded). */
static const char k_vp_pull_decl[] =
    "ByteAddressBuffer gp_vb : register(t20);\n"
    "ByteAddressBuffer gp_tab : register(t21);\n"
    "cbuffer GPConst : register(b4) { uint4 gp_c[9]; };\n";
static const char k_vp_pull_fn[] =
    "uint4 gp_ld(uint b, uint st, uint v, uint nw) {\n"
    "    uint a = b + v * st; uint al = a & ~3u; uint sh = (a & 3u) * 8u;\n"
    "    uint4 d = gp_vb.Load4(al);\n"
    "    uint e = nw > 3u ? gp_vb.Load(al + 16u) : 0u;\n"
    "    if (sh == 0u) return d;\n"
    "    uint r = 32u - sh;\n"
    "    return uint4((d.x >> sh) | (d.y << r), (d.y >> sh) | (d.z << r), (d.z >> sh) | (d.w << r), (d.w >> sh) | (e << r));\n"
    "}\n"
    "float gp_t(uint i) { return asfloat(gp_tab.Load(i * 4u)); }\n"
    "float4 gp_vdec(uint4 w, uint t, uint n) {\n"
    "    if (t == 6u) { uint x = vb_sw(w.x);\n"
    "        return float4(gp_t(65792u + (x & 0x7FFu)), gp_t(65792u + ((x >> 11) & 0x7FFu)), gp_t(67840u + (x >> 22)), 1.0); }\n"
    "    float4 o = float4(0, 0, 0, 1);\n"
    "    [unroll] for (uint c = 0u; c < 4u; c++) {\n"
    "        if (c < n) {\n"
    "            float v = 0.0;\n"
    "            if (t == 2u) v = asfloat(vb_sw(w[c]));\n"
    "            else if (t == 3u) v = vb_f16(vb_h(w, c));\n"
    "            else if (t == 1u) v = gp_t(vb_h(w, c));\n"
    "            else if (t == 5u) v = vb_s16(vb_h(w, c));\n"
    "            else if (t == 4u) v = gp_t(65536u + vb_b(w, c));\n"
    "            else if (t == 7u) v = (float)vb_b(w, c);\n"
    "            o[c] = v;\n"
    "        }\n"
    "    }\n"
    "    return o;\n"
    "}\n";
/* 32-bit words an element of kind (t, n) covers from its first byte. */
static u32 vp_pull_words(u32 t, u32 n)
{
    if (t == 2u) return n ? n : 1u;
    if (t == 1u || t == 3u || t == 5u) return (2u * n + 3u) / 4u ? (2u * n + 3u) / 4u : 1u;
    return 1u;
}

int rsx_vp_decompile_pulled(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, u32 input_mask,
    const u8 raw_kind[16], const signed char pull_slot[16], char* out, u32 out_size)
{
    if (!raw_kind || !pull_slot) return -1;
    rsx_vp_input_analysis analysis;
    if (rsx_vp_analyze_inputs(ucode, max_bytes, &analysis) < 0 ||
        !analysis.exact ||
        (analysis.input_mask & ~(input_mask & 0xFFFFu)) != 0)
        input_mask = 0xFFFFu;
    else
        input_mask = analysis.input_mask;
    s_vp_pull_slot = pull_slot;
    const int r = rsx_vp_decompile_impl(ucode, max_bytes, vtex_mask, 1, input_mask, raw_kind, out, out_size);
    s_vp_pull_slot = NULL;
    return r;
}

int rsx_vp_pull_check_source(char* out, u32 out_size)
{
    const int n = snprintf(out, out_size, "%s%s%s%s", k_vp_raw_decode,
        "ByteAddressBuffer gp_vb : register(t0);\n"
        "ByteAddressBuffer gp_tab : register(t1);\n"
        "RWByteAddressBuffer gp_out : register(u0);\n"
        "cbuffer GPConst : register(b0) { uint4 gp_c[10]; };\n",
        k_vp_pull_fn,
        "[numthreads(64, 1, 1)]\n"
        "void main(uint3 id : SV_DispatchThreadID) {\n"
        "    uint n = gp_c[0].x, cnt = gp_c[0].y, ostr = gp_c[0].z;\n"
        "    uint i = id.x; if (i >= n * cnt) return;\n"
        "    uint v = i / cnt, s = i - v * cnt;\n"
        "    uint d = 4u + 2u * s;\n"
        "    uint b = gp_c[d >> 2][d & 3u]; uint st = gp_c[(d + 1u) >> 2][(d + 1u) & 3u];\n"
        "    uint kd = (gp_c[9][s >> 2] >> ((s & 3u) * 8u)) & 0xFFu; uint t = kd >> 3, nn = kd & 7u;\n"
        "    uint nw = t == 2u ? nn : ((t == 1u || t == 3u || t == 5u) ? (2u * nn + 3u) / 4u : 1u);\n"
        "    float4 r = kd ? gp_vdec(gp_ld(b, st, v, nw), t, nn) : asfloat(gp_ld(b, st, v, 4u));\n"
        "    gp_out.Store4(v * ostr + s * 16u, asuint(r));\n"
        "}\n");
    return n > 0 && (u32)n < out_size ? n : -1;
}

static int rsx_vp_decompile_impl(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, int compact_inputs,
    u32 input_mask, const u8* raw_kind, char* out, u32 out_size)
{
    if (!ucode || !out || out_size < 256) return -1;

    /* Body is built first, preamble/epilogue wrap it. */
    static char body[192 * 1024];
    Out b = { body, sizeof(body), 0, 0 };
    body[0] = '\0';

    int n_cond_skipped = 0, n_flow_skipped = 0;

    /* Flow control (BRI/CAL/CLI/RET...). It carries no write mask, so it was
     * dropped and every block ran straight through: a lit shader that
     * branches around its lighting when lighting is off (@20 -> 134 on
     * cc.y == 0 in Tornado Outbreak's UI/particle shader) zeroed its colour
     * and summed no lights -- black text, black smoke, black leaves.
     *
     * A branching program is emitted as a block state machine: leaders are
     * the entry, every branch target and the instruction after every branch;
     * each block is "if (_pc == N) { ... _pc = next-or-target; }" inside a
     * bounded loop, with a small call stack for CAL/RET. Branch-free programs
     * are emitted exactly as before. Targets are VP slot addresses; the slot
     * the program starts at comes from rsx_vp_set_branch_base(). */
    static u8 leader[RSX_VP_MAX_INSTR + 1];
    memset(leader, 0, sizeof leader);
    int has_flow = 0;
    {
        u32 o2 = 0; int k = 0;
        while (o2 + 16 <= max_bytes && k < RSX_VP_MAX_INSTR) {
            const u32 e0 = rd_le(ucode + o2), e1 = rd_le(ucode + o2 + 4);
            const u32 e2 = rd_le(ucode + o2 + 8), e3 = rd_le(ucode + o2 + 12);
            const u32 so = (e1 >> 27) & 0x1F;
            if (vp_is_flow(so)) {
                has_flow = 1;
                const u32 t = vp_branch_target(e0, e2, e3);
                if (t < RSX_VP_MAX_INSTR) leader[t] = 1;
                leader[k + 1] = 1;
            }
            k++; o2 += 16;
            if (e3 & 1u) break;
        }
        leader[0] = 1;
    }
    int block_open = 0;

    u32 off = 0; int instrs = 0;
    while (off + 16 <= max_bytes) {
        u32 d0 = rd_le(ucode+off+0), d1 = rd_le(ucode+off+4);
        u32 d2 = rd_le(ucode+off+8), d3 = rd_le(ucode+off+12);
        if (has_flow && instrs < RSX_VP_MAX_INSTR && leader[instrs]) {
            char line[64];
            if (block_open) {
                snprintf(line, sizeof line, "        _pc = %d;\n    }\n", instrs);
                emit(&b, line);
            }
            snprintf(line, sizeof line, "    if (_pc == %d) {\n", instrs);
            emit(&b, line);
            block_open = 1;
        }
        off += 16; instrs++;

        u32 vec_op = (d1 >> 22) & 0x1F;
        u32 sca_op = (d1 >> 27) & 0x1F;
        u32 input_src = (d1 >> 8) & 0xF;
        u32 const_src = (d1 >> 12) & 0x3FF;
        int index_const = (d3 >> 1) & 1;

        /* assemble the three 17-bit source fields */
        u32 src0 = ((d2 >> 23) & 0x1FF) | ((d1 & 0xFF) << 9);
        u32 src1 = (d2 >> 6) & 0x1FFFF;
        u32 src2 = ((d3 >> 21) & 0x7FF) | ((d2 & 0x3F) << 11);
        int a0 = (d0 >> 21) & 1, a1 = (d0 >> 22) & 1, a2 = (d0 >> 23) & 1;
        u32 addr_sel = (d0 >> 24) & 1;
        u32 addr_swz = d0 & 3;
        int saturate = (d0 >> 26) & 1;
        int cond_test = (d0 >> 13) & 1;
        u32 cond = (d0 >> 10) & 7;

        char A[96], B[96], C[96];
        emit_src(src0, a0, input_src, const_src, index_const, addr_sel, addr_swz, A, sizeof(A));
        emit_src(src1, a1, input_src, const_src, index_const, addr_sel, addr_swz, B, sizeof(B));
        emit_src(src2, a2, input_src, const_src, index_const, addr_sel, addr_swz, C, sizeof(C));

        u32 dst_tmp     = (d0 >> 15) & 0x3F;
        u32 dst_out     = (d3 >> 2) & 0x1F;
        u32 sca_dst_tmp = (d3 >> 7) & 0x3F;
        int vec_result  = (d0 >> 30) & 1;

        int vmx=(d3>>16)&1, vmy=(d3>>15)&1, vmz=(d3>>14)&1, vmw=(d3>>13)&1;
        int smx=(d3>>20)&1, smy=(d3>>19)&1, smz=(d3>>18)&1, smw=(d3>>17)&1;

        /* Condition codes (D0: CC swizzle [2:9], cond [10:12], test [13],
         * update-enable [14] and [29], CC register select [25]). They were
         * not modeled: every predicated write ran unconditionally and every
         * CC-only write was dropped. Tornado Outbreak's UI shader replaces
         * the vertex colour with a material constant under a CC test, so its
         * text took that constant's alpha of 0 and the alpha test discarded
         * every glyph. */
        CondCtx cc = { cond_test && cond != 7, cond, (d0 >> 25) & 1, {0} };
        cc.swz[0] = SWZ[(d0 >> 8) & 3]; cc.swz[1] = SWZ[(d0 >> 6) & 3];
        cc.swz[2] = SWZ[(d0 >> 4) & 3]; cc.swz[3] = SWZ[(d0 >> 2) & 3];
        const int cc_update = ((d0 >> 14) & 1) && ((d0 >> 29) & 1);
        const int cc_either = vp_cc_either_on() && (((d0 >> 14) & 1) || ((d0 >> 29) & 1));
        if (cc.test) n_cond_skipped++;

        /* ---- vector ALU ---- */
        if ((vmx|vmy|vmz|vmw) && vec_op != 0x00) {
            char rhs[512];
            int handled = 1;
            switch (vec_op) {
            case 0x01: snprintf(rhs,sizeof rhs,"%s",A); break;                 /* MOV */
            case 0x02: snprintf(rhs,sizeof rhs,"vp_mul((%s),(%s))",A,B); break;  /* MUL */
            case 0x03: snprintf(rhs,sizeof rhs,"(%s)+(%s)",A,C); break;        /* ADD: src0 + src2 */
            case 0x04: snprintf(rhs,sizeof rhs,"vp_mul((%s),(%s))+(%s)",A,B,C); break; /* MAD */
            case 0x05: snprintf(rhs,sizeof rhs,"vp_dot3((%s),(%s))",A,B); break; /* DP3 */
            case 0x06: snprintf(rhs,sizeof rhs,"(vp_dot3((%s),(%s))+(%s).w)",A,B,B); break; /* DPH */
            case 0x07: snprintf(rhs,sizeof rhs,"vp_dot4((%s),(%s))",A,B); break; /* DP4 */
            case 0x08: snprintf(rhs,sizeof rhs,"float4(1,(%s).y*(%s).y,(%s).z,(%s).w)",A,B,A,B); break; /* DST */
            case 0x09: snprintf(rhs,sizeof rhs,"min((%s),(%s))",A,B); break;   /* MIN */
            case 0x0A: snprintf(rhs,sizeof rhs,"max((%s),(%s))",A,B); break;   /* MAX */
            case 0x0B: snprintf(rhs,sizeof rhs,"(float4)((%s)<(%s))",A,B); break;  /* SLT */
            case 0x0C: snprintf(rhs,sizeof rhs,"(float4)((%s)>=(%s))",A,B); break; /* SGE */
            case 0x0E: snprintf(rhs,sizeof rhs,"frac(%s)",A); break;           /* FRC */
            case 0x0F: snprintf(rhs,sizeof rhs,"floor(%s)",A); break;          /* FLR */
            case 0x10: snprintf(rhs,sizeof rhs,"(float4)((%s)==(%s))",A,B); break; /* SEQ */
            case 0x11: snprintf(rhs,sizeof rhs,"(float4)0"); break;            /* SFL */
            case 0x12: snprintf(rhs,sizeof rhs,"(float4)((%s)>(%s))",A,B); break;  /* SGT */
            case 0x13: snprintf(rhs,sizeof rhs,"(float4)((%s)<=(%s))",A,B); break; /* SLE */
            case 0x14: snprintf(rhs,sizeof rhs,"(float4)((%s)!=(%s))",A,B); break; /* SNE */
            case 0x15: snprintf(rhs,sizeof rhs,"float4(1,1,1,1)"); break;      /* STR */
            case 0x16: snprintf(rhs,sizeof rhs,"sign(%s)",A); break;           /* SSG */
            case 0x19: {
                const u32 tex_unit = (d2 >> 8) & 3u;
                if ((vtex_mask >> tex_unit) & 1u)
                    snprintf(rhs, sizeof rhs,
                        "rsx_vtex%u.SampleLevel(rsx_vsamp%u, (%s).xy, 0.0)",
                        tex_unit, tex_unit, A);
                else
                    snprintf(rhs, sizeof rhs, "float4(0,0,0,0)");
                break;
            }
            case 0x0D: /* ARL: address register load, rounds down */
                if (vmx|vmy|vmz|vmw) {
                    char m[6]; mask_str(vmx,vmy,vmz,vmw,m);
                    char line[256];
                    snprintf(line,sizeof line,
                        "    { int4 _a = (int4)floor(%s); a%u.%s = _a.%s; }\n",
                        A, addr_sel, m, m);
                    emit(&b, line);
                }
                handled = 0;
                break;
            default:
                n_flow_skipped++;
                emit(&b, "    /* TODO: unhandled VP vec op */\n");
                handled = 0;
                break;
            }

            if (handled) {
                char m[6]; mask_str(vmx,vmy,vmz,vmw,m);
                char line[600];
                snprintf(line,sizeof line,"    { float4 _v = (float4)(%s);%s\n",
                         rhs, saturate ? " _v = saturate(_v);" : "");
                emit(&b, line);
                if (vec_result && dst_out != 0x1F)
                    emit_store_cc(&b, "o[%u]", dst_out & 15, m, &cc);
                if (dst_tmp != 0x3F)
                    emit_store_cc(&b, "r[%u]", dst_tmp & 31, m, &cc);
                if (cc_update || (cc_either && !(vec_result && dst_out != 0x1F) && dst_tmp == 0x3F))
                    emit_store_cc(&b, "cc[%u]", cc.sel, m, &cc);
                emit(&b, "    }\n");
            }
        }

        /* Flow control has no write mask, so the ALU path below skips it
         * silently. Mark it, with the decoded target, so a dump shows where a
         * program branches. */
        char flow_term[256] = "";
        if (vp_is_flow(sca_op)) {
            const u32 tgt = vp_branch_target(d0, d2, d3);
            char line[160];
            snprintf(line, sizeof line, "    /* FLOW %s @%d -> %u cond=%u test=%d swz=%c%c%c%c */\n",
                     rsx_vp_sca_name(sca_op), instrs - 1, tgt, cond, cond_test,
                     SWZ[(d0 >> 8) & 3], SWZ[(d0 >> 6) & 3], SWZ[(d0 >> 4) & 3], SWZ[(d0 >> 2) & 3]);
            emit(&b, line);
            /* The condition applies whether or not cond_test is set: every
             * branch in these programs has test=0 with a real cond. */
            static const char* cmp[8] = { 0, "<", "==", "<=", ">", "!=", ">=", 0 };
            char c[48];
            if (cond == 7)      snprintf(c, sizeof c, "true");
            else if (cond == 0) snprintf(c, sizeof c, "false");
            else snprintf(c, sizeof c, "(cc[%u].%c %s 0.0)", (d0 >> 25) & 1,
                          SWZ[(d0 >> 8) & 3], cmp[cond]);
            const int next = instrs;
            if (sca_op == 0x09 || sca_op == 0x08)          /* BRI, BRA */
                snprintf(flow_term, sizeof flow_term, "        _pc = %s ? %u : %d;\n", c, tgt, next);
            else if (sca_op == 0x0A || sca_op == 0x0B)     /* CAL, CLI */
                snprintf(flow_term, sizeof flow_term,
                         "        if (%s) { _cs[_csp] = %d; _csp = min(_csp + 1, 7); _pc = %u; } else _pc = %d;\n",
                         c, next, tgt, next);
            else if (sca_op == 0x0C)                       /* RET */
                snprintf(flow_term, sizeof flow_term,
                         "        if (%s) { if (_csp > 0) { _csp--; _pc = _cs[_csp]; } else _pc = -1; } else _pc = %d;\n",
                         c, next);
            else {                                         /* BRB/CLB: boolean-register branches */
                n_flow_skipped++;
                snprintf(flow_term, sizeof flow_term,
                         "        _pc = %d; /* TODO: %s not modelled */\n", next, rsx_vp_sca_name(sca_op));
            }
        }
        /* ---- scalar ALU (reads SRC2) ---- */
        if ((smx|smy|smz|smw) && sca_op != 0x00) {
            char rhs[256];
            switch (sca_op) {
            case 0x01: snprintf(rhs,sizeof rhs,"%s",C); break;                /* MOV */
            case 0x02: snprintf(rhs,sizeof rhs,"(1.0/(%s).x)",C); break;      /* RCP */
            case 0x03: snprintf(rhs,sizeof rhs,"clamp(1.0/(%s).x, 5.42101e-20, 1.884467e19)",C); break; /* RCC */
            case 0x04: snprintf(rhs,sizeof rhs,"rsqrt(max(abs((%s).x), 1e-10))",C); break; /* RSQ */
            case 0x05: snprintf(rhs,sizeof rhs,"exp2((%s).x)",C); break;      /* EXP */
            case 0x06: snprintf(rhs,sizeof rhs,"log2(max(abs((%s).x), 1e-10))",C); break; /* LOG */
            case 0x0D: snprintf(rhs,sizeof rhs,"log2(max(abs((%s).x), 1e-10))",C); break; /* LG2 */
            case 0x0E: snprintf(rhs,sizeof rhs,"exp2((%s).x)",C); break;      /* EX2 */
            case 0x0F: snprintf(rhs,sizeof rhs,"sin((%s).x)",C); break;       /* SIN */
            case 0x10: snprintf(rhs,sizeof rhs,"cos((%s).x)",C); break;       /* COS */
            case 0x07: /* LIT: (1, max(s.x,0), s.x>0 ? 2^(s.w*log2(max(s.y,eps))) : 0, 1) */
                snprintf(rhs,sizeof rhs,
                    "float4(1.0, max((%s).x, 0.0), ((%s).x > 0.0) ? "
                    "exp2(clamp((%s).w, -127.996, 127.996) * log2(max((%s).y, 1e-10))) : 0.0, 1.0)",
                    C, C, C, C);
                break;
            default:
                n_flow_skipped++;
                emit(&b, "    /* TODO: VP flow-control op skipped */\n");
                rhs[0]='\0';
                break;
            }
            if (rhs[0]) {
                char m[6]; mask_str(smx,smy,smz,smw,m);
                char line[600];
                snprintf(line,sizeof line,"    { float4 _v = (float4)(%s);%s\n",
                         rhs, saturate ? " _v = saturate(_v);" : "");
                emit(&b, line);
                /* SCA writes the output register when the VEC unit does not
                 * own it; otherwise it targets its own temp. */
                if (!vec_result && dst_out != 0x1F)
                    emit_store_cc(&b, "o[%u]", dst_out & 15, m, &cc);
                if (sca_dst_tmp != 0x3F)
                    emit_store_cc(&b, "r[%u]", sca_dst_tmp & 31, m, &cc);
                /* LD_VP_CC_EITHER: a scalar result with nowhere else to go. */
                if (cc_either && !(!vec_result && dst_out != 0x1F) && sca_dst_tmp == 0x3F)
                    emit_store_cc(&b, "cc[%u]", cc.sel, m, &cc);
                emit(&b, "    }\n");
            }
        }

        if (has_flow && flow_term[0]) {
            emit(&b, flow_term);
            emit(&b, "    }\n");
            block_open = 0;
        }
        if (d3 & 1u) break; /* end */
    }
    if (block_open) emit(&b, "        _pc = -1;\n    }\n");

    if (b.overflow) return -1;

    /* ---- assemble full shader ---- */
    Out o = { out, out_size, 0, 0 };
    if (!compact_inputs) {
        emit(&o,
            /* All 16 RSX vertex attributes arrive as float4 (the harness
             * fetches and converts them on the CPU). */
            "struct VSInput {\n"
            "    float4 a0:ATTR0;  float4 a1:ATTR1;  float4 a2:ATTR2;  float4 a3:ATTR3;\n"
            "    float4 a4:ATTR4;  float4 a5:ATTR5;  float4 a6:ATTR6;  float4 a7:ATTR7;\n"
            "    float4 a8:ATTR8;  float4 a9:ATTR9;  float4 a10:ATTR10; float4 a11:ATTR11;\n"
            "    float4 a12:ATTR12; float4 a13:ATTR13; float4 a14:ATTR14; float4 a15:ATTR15;\n"
            "};\n");
    } else if (s_vp_pull_slot) {   /* GP_PULL */
        emit(&o, "struct VSInput {\n    uint gp_vid:SV_VertexID;\n};\n");
    } else {
        emit(&o, "struct VSInput {\n");
        if (!(input_mask & 0xFFFFu))
            emit(&o, "    uint vertex_id:SV_VertexID;\n");
        for (u32 attr = 0; attr < 16; attr++) {
            if (!(input_mask & (1u << attr))) continue;
            char declaration[64];
            snprintf(declaration, sizeof(declaration),
                     raw_kind && raw_kind[attr] ? "    uint4 a%u:ATTR%u;\n" : "    float4 a%u:ATTR%u;\n",
                     attr, attr);
            emit(&o, declaration);
        }
        emit(&o, "};\n");
    }
    emit(&o,
        "struct VSOutput {\n"
        "    float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1;\n"
        "    float4 fog:FOG;\n"
        "    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
        "    float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7;\n"
        "    float4 t8:TEXCOORD8; float4 t9:TEXCOORD9;\n"
        "};\n"
        /* b0: 512 vec4 transform constants + the RSX viewport transform,
         * pre-mapped by the harness to D3D clip space:
         *   ndc.xyz = clip.xyz * vp_posscale.xyz + clip.w * vp_posoffset.xyz */
        "cbuffer VPConst : register(b0) {\n"
        "    float4 vp_c[512];\n"
        "    float4 vp_posscale;\n"
        "    float4 vp_posoffset;\n"
        "};\n"
        /* NV vertex programs multiply by the legacy rule: 0 times anything,
         * INF and NaN included, is 0. IEEE gives NaN, and one RCP of a zero
         * distance then poisons a whole lighting sum -- Tornado Outbreak's UI
         * text came out NaN, which the FP's NaN guard turned into black. */
        /* Scalar logic per component: glslang's HLSL front end (the Metal
         * path) does not take || on vectors. */
        "float vp_mul1(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }\n"
        "float4 vp_mul(float4 a, float4 b) { return float4(vp_mul1(a.x, b.x), vp_mul1(a.y, b.y),"
        " vp_mul1(a.z, b.z), vp_mul1(a.w, b.w)); }\n"
        "float vp_dot3(float4 a, float4 b) { float4 m = vp_mul(a, b); return m.x + m.y + m.z; }\n"
        "float vp_dot4(float4 a, float4 b) { float4 m = vp_mul(a, b); return m.x + m.y + m.z + m.w; }\n");

    if (raw_kind)
        emit(&o, k_vp_raw_decode);
    if (s_vp_pull_slot) { emit(&o, k_vp_pull_decl); emit(&o, k_vp_pull_fn); }   /* GP_PULL */
    for (u32 vtu = 0; vtu < 4; vtu++) {
        if (!((vtex_mask >> vtu) & 1u)) continue;
        char decl[128];
        snprintf(decl, sizeof(decl),
            "Texture2D rsx_vtex%u : register(t%u);\n"
            "SamplerState rsx_vsamp%u : register(s%u);\n",
            vtu, 16u + vtu, vtu, vtu);
        emit(&o, decl);
    }

    emit(&o, "VSOutput main(VSInput input) {\n");
    if (!compact_inputs) {
        emit(&o,
            "    float4 v[16];\n"
            "    v[0]=input.a0;  v[1]=input.a1;  v[2]=input.a2;  v[3]=input.a3;\n"
            "    v[4]=input.a4;  v[5]=input.a5;  v[6]=input.a6;  v[7]=input.a7;\n"
            "    v[8]=input.a8;  v[9]=input.a9;  v[10]=input.a10; v[11]=input.a11;\n"
            "    v[12]=input.a12; v[13]=input.a13; v[14]=input.a14; v[15]=input.a15;\n");
    } else if (s_vp_pull_slot) {   /* GP_PULL: each input loaded and decoded at the clamped vertex index */
        emit(&o,
            "    float4 v[16];\n"
            "    [unroll] for (int _k=0;_k<16;_k++) v[_k]=float4(0,0,0,1);\n"
            "    uint gp_v = min(input.gp_vid - gp_c[0].y, gp_c[0].x - 1u);\n");
        static const char xyzw[4] = { 'x', 'y', 'z', 'w' };
        for (u32 attr = 0; attr < 16; attr++) {
            if (!(input_mask & (1u << attr))) continue;
            const int sl = s_vp_pull_slot[attr];
            if (sl < 0 || sl > 15) return -1;
            const u32 db = 4u + 2u * (u32)sl, ds = db + 1u;
            char assignment[160];
            const u32 k = raw_kind ? raw_kind[attr] : 0u;
            if (k)
                snprintf(assignment, sizeof(assignment), "    v[%u]=gp_vdec(gp_ld(gp_c[%u].%c, gp_c[%u].%c, gp_v, %uu), %uu, %uu);\n",
                         attr, db >> 2, xyzw[db & 3u], ds >> 2, xyzw[ds & 3u], vp_pull_words(k >> 3, k & 7u), k >> 3, k & 7u);
            else   /* a constant: a float4 at base, stride 0 */
                snprintf(assignment, sizeof(assignment), "    v[%u]=asfloat(gp_ld(gp_c[%u].%c, gp_c[%u].%c, gp_v, 4u));\n",
                         attr, db >> 2, xyzw[db & 3u], ds >> 2, xyzw[ds & 3u]);
            emit(&o, assignment);
        }
    } else {
        emit(&o,
            "    float4 v[16];\n"
            "    [unroll] for (int _k=0;_k<16;_k++) v[_k]=float4(0,0,0,1);\n");
        for (u32 attr = 0; attr < 16; attr++) {
            if (!(input_mask & (1u << attr))) continue;
            char assignment[96];
            if (raw_kind && raw_kind[attr])
                snprintf(assignment, sizeof(assignment), "    v[%u]=vdec(input.a%u, %uu, %uu);\n",
                         attr, attr, (u32)raw_kind[attr] >> 3, (u32)raw_kind[attr] & 7u);
            else
                snprintf(assignment, sizeof(assignment),
                         "    v[%u]=input.a%u;\n", attr, attr);
            emit(&o, assignment);
        }
    }
    emit(&o,
        "    float4 r[32]; float4 o[16];\n"
        "    [unroll] for (int _i=0;_i<32;_i++) r[_i]=(float4)0;\n"
        "    [unroll] for (int _j=0;_j<16;_j++) o[_j]=float4(0,0,0,1);\n"
        "    int4 a0 = (int4)0; int4 a1 = (int4)0;\n"
        "    float4 cc[2]; cc[0] = (float4)0; cc[1] = (float4)0;\n");

    if (has_flow)
        emit(&o, "    int _pc = 0; int _cs[8]; int _csp = 0;\n"
                 "    [loop] for (int _it = 0; _it < 16384 && _pc >= 0; _it++) {\n");
    emit(&o, body);
    if (has_flow) emit(&o, "    }\n");

    /* Output register map (see header comment): o0 = HPOS through the RSX
     * viewport transform, o1/o2 = colors, o5.x = fog, o7..o14 = TEX0..7. */
    emit(&o,
        "    VSOutput Out;\n"
        "    float4 _p = o[0];\n"
        "    Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);\n"
        "    Out.col0 = o[1]; Out.col1 = o[2]; Out.fog = o[5].xxxx;\n"
        "    Out.t0=o[7];  Out.t1=o[8];  Out.t2=o[9];  Out.t3=o[10];\n"
        "    Out.t4=o[11]; Out.t5=o[12]; Out.t6=o[13]; Out.t7=o[14];\n"
        /* TEX8 is output 15, TEX9 output 6 (beside the point size). God of
         * War: Ascension's amber and terrain materials read TEX8 (a normalised
         * light vector): unplumbed it was zero and their N.L terms with it. */
        "    Out.t8=o[15]; Out.t9=o[6];\n"
        "    return Out;\n"
        "}\n");

    (void)n_cond_skipped;
    (void)n_flow_skipped;
    if (o.overflow) return -1;
    return instrs;
}

/* ---- LD_SHADER_STATS: which opcodes and modifiers the title's programs use --
 * Called by the live draw once per pipeline it builds (only when
 * LD_SHADER_STATS is set); counted once per distinct program, as
 * "occurrences/programs". Returns the output registers o0..o15 the program
 * writes (bit n = o[n]), for the fragment-side checks. Nothing here touches
 * the generated text. */
enum {
    VPS_SAT, VPS_PRED, VPS_COND0_NOTEST, VPS_CC_UPD14, VPS_CC_UPD29, VPS_CC_UPD_BOTH,
    VPS_CC_UPD_WITH_DEST, VPS_CC_UPD_NO_DEST, VPS_CC_SEL1, VPS_INDEX_CONST, VPS_ADDR_SWZ_NOT_X,
    VPS_ADDR_SEL1, VPS_ARL_DST_NE_SEL, VPS_ARL_TO_OUTPUT, VPS_ABS, VPS_NEG, VPS_SRC_TYPE0,
    VPS_CONST_GE468, VPS_VEC_DOUBLE_WRITE, VPS_SCA_TO_OUTPUT, VPS_DUAL_ISSUE, VPS_DST_GE16,
    VPS_FLOW, VPS_FLOW_NOTEST_COND, VPS_NO_DEST,
    VPS_NFLAGS
};
static const char* const vps_flag_names[VPS_NFLAGS] = {
    "sat", "predicated", "cond0_without_test", "cc_update_bit14_only", "cc_update_bit29_only",
    "cc_update_both", "cc_update_with_dest", "cc_update_no_dest", "cc_sel_cc1", "index_const",
    "addr_swz_not_x", "addr_sel_a1", "arl_dst_ne_addr_sel", "arl_to_output", "abs", "neg",
    "src_type0", "const_ge_468", "vec_double_write", "sca_to_output", "vec_and_sca",
    "dst_ge_16", "flow", "flow_cond_untested", "no_dest"
};
#define VPS_SEEN 4096
static struct {
    u32 programs, psos, instructions, seen_full;
    unsigned long long vec_occ[32], sca_occ[32], fl_occ[VPS_NFLAGS], in_occ[16], out_occ[16];
    u32 vec_prog[32], sca_prog[32], fl_prog[VPS_NFLAGS], in_prog[16], out_prog[16];
    u64 seen[VPS_SEEN];
} s_vps;

u32 rsx_vp_stats_note(const u8* ucode, u32 max_bytes)
{
    if (!ucode) return 0;
    s_vps.psos++;
    u64 h = 1469598103934665603ull;
    {
        const u32 n = rsx_vp_program_size_instrs(ucode, max_bytes);
        for (u32 i = 0; i < n * 16u; i++) { h ^= ucode[i]; h *= 1099511628211ull; }
        if (!h) h = 1;
    }
    int fresh = 0;
    {
        u32 slot = (u32)(h ^ (h >> 31)) & (VPS_SEEN - 1u), n = 0;
        while (s_vps.seen[slot] && s_vps.seen[slot] != h && n < VPS_SEEN) {
            slot = (slot + 1u) & (VPS_SEEN - 1u); n++;
        }
        if (n >= VPS_SEEN) s_vps.seen_full++;
        else if (!s_vps.seen[slot]) { s_vps.seen[slot] = h; fresh = 1; }
    }
    u32 vec_seen = 0, sca_seen = 0, fl_seen = 0, in_seen = 0, out_written = 0;
#define VPS_FLAG(f) do { if (fresh) { s_vps.fl_occ[f]++; fl_seen |= 1u << (f); } } while (0)
    u32 off = 0;
    while (off + 16 <= max_bytes) {
        const u32 d0 = rd_le(ucode + off + 0), d1 = rd_le(ucode + off + 4);
        const u32 d2 = rd_le(ucode + off + 8), d3 = rd_le(ucode + off + 12);
        off += 16;
        if (fresh) s_vps.instructions++;
        const u32 vec_op = (d1 >> 22) & 0x1Fu, sca_op = (d1 >> 27) & 0x1Fu;
        const u32 vmask = (d3 >> 13) & 0xFu, smask = (d3 >> 17) & 0xFu;
        const int vec_on = vmask && vec_op, sca_on = smask && sca_op;
        const int flow = vp_is_flow(sca_op);
        const u32 dst_out = (d3 >> 2) & 0x1Fu, dst_tmp = (d0 >> 15) & 0x3Fu;
        const u32 sca_tmp = (d3 >> 7) & 0x3Fu;
        const int vec_result = (d0 >> 30) & 1u, cond_test = (d0 >> 13) & 1u;
        const u32 cond = (d0 >> 10) & 7u;
        if (vec_on && fresh) { s_vps.vec_occ[vec_op]++; vec_seen |= 1u << vec_op; }
        if ((sca_on || flow) && fresh) { s_vps.sca_occ[sca_op]++; sca_seen |= 1u << sca_op; }
        if (!vec_on && !sca_on && !flow) { if (d3 & 1u) break; continue; }
        if ((d0 >> 26) & 1u) VPS_FLAG(VPS_SAT);
        if (cond_test && cond != 7) VPS_FLAG(VPS_PRED);
        if (!cond_test && cond == 0) VPS_FLAG(VPS_COND0_NOTEST);
        { const int u14 = (d0 >> 14) & 1u, u29 = (d0 >> 29) & 1u;
          if (u14 && u29) VPS_FLAG(VPS_CC_UPD_BOTH);
          else if (u14) VPS_FLAG(VPS_CC_UPD14);
          else if (u29) VPS_FLAG(VPS_CC_UPD29);
          if (u14 || u29) {
              const int dest = (vec_on && ((vec_result && dst_out != 0x1Fu) || dst_tmp != 0x3Fu)) ||
                               (sca_on && ((!vec_result && dst_out != 0x1Fu) || sca_tmp != 0x3Fu));
              VPS_FLAG(dest ? VPS_CC_UPD_WITH_DEST : VPS_CC_UPD_NO_DEST);
              if ((d0 >> 25) & 1u) VPS_FLAG(VPS_CC_SEL1);
          } else if (cond_test && cond != 7 && ((d0 >> 25) & 1u)) VPS_FLAG(VPS_CC_SEL1); }
        if (flow) {
            VPS_FLAG(VPS_FLOW);
            if (!cond_test && cond != 7) VPS_FLAG(VPS_FLOW_NOTEST_COND);
        }
        if (vec_on && sca_on) VPS_FLAG(VPS_DUAL_ISSUE);
        if (vec_on && vec_result && dst_out != 0x1Fu && dst_tmp != 0x3Fu && vec_op != 0x0D)
            VPS_FLAG(VPS_VEC_DOUBLE_WRITE);
        if (sca_on && !vec_result && dst_out != 0x1Fu) VPS_FLAG(VPS_SCA_TO_OUTPUT);
        if (vec_on && vec_op == 0x0D) {
            if (dst_tmp != ((d0 >> 24) & 1u)) VPS_FLAG(VPS_ARL_DST_NE_SEL);
            if (vec_result && dst_out != 0x1Fu) VPS_FLAG(VPS_ARL_TO_OUTPUT);
        }
        if ((vec_on && vec_op != 0x0D && !(vec_result && dst_out != 0x1Fu) && dst_tmp == 0x3Fu) ||
            (sca_on && !(!vec_result && dst_out != 0x1Fu) && sca_tmp == 0x3Fu))
            VPS_FLAG(VPS_NO_DEST);
        if (dst_out != 0x1Fu && dst_out > 15u &&
            ((vec_on && vec_result) || (sca_on && !vec_result))) VPS_FLAG(VPS_DST_GE16);
        if (dst_out != 0x1Fu && dst_out <= 15u &&
            ((vec_on && vec_result) || (sca_on && !vec_result)))
            out_written |= 1u << dst_out;
        int exact = 1;
        u32 use = 0;
        if (vec_on) use |= vec_source_use(vec_op, &exact);
        if (sca_on) use |= sca_source_use(sca_op, &exact);
        const u32 src[3] = { ((d2 >> 23) & 0x1FFu) | ((d1 & 0xFFu) << 9), (d2 >> 6) & 0x1FFFFu,
                             ((d3 >> 21) & 0x7FFu) | ((d2 & 0x3Fu) << 11) };
        int uses_const = 0;
        for (u32 si = 0; si < 3; si++) {
            if (!(use & (1u << si))) continue;
            const u32 type = src[si] & 3u;
            if (type == 0) VPS_FLAG(VPS_SRC_TYPE0);
            if (type == 2) {
                const u32 in = (d1 >> 8) & 0xFu;
                if (fresh) s_vps.in_occ[in]++;
                in_seen |= 1u << in;
            }
            if (type == 3) uses_const = 1;
            if ((d0 >> (21 + si)) & 1u) VPS_FLAG(VPS_ABS);
            if ((src[si] >> 16) & 1u) VPS_FLAG(VPS_NEG);
        }
        if (uses_const) {
            if ((d3 >> 1) & 1u) {
                VPS_FLAG(VPS_INDEX_CONST);
                if (d0 & 3u) VPS_FLAG(VPS_ADDR_SWZ_NOT_X);
                if ((d0 >> 24) & 1u) VPS_FLAG(VPS_ADDR_SEL1);
            } else if (((d1 >> 12) & 0x3FFu) >= 468u) VPS_FLAG(VPS_CONST_GE468);
        }
        if (d3 & 1u) break;
    }
#undef VPS_FLAG
    if (fresh) {
        s_vps.programs++;
        for (u32 i = 0; i < 32; i++) {
            if ((vec_seen >> i) & 1u) s_vps.vec_prog[i]++;
            if ((sca_seen >> i) & 1u) s_vps.sca_prog[i]++;
            if (i < VPS_NFLAGS && ((fl_seen >> i) & 1u)) s_vps.fl_prog[i]++;
        }
        for (u32 i = 0; i < 16; i++) {
            if ((in_seen >> i) & 1u) s_vps.in_prog[i]++;
            if ((out_written >> i) & 1u) { s_vps.out_prog[i]++; s_vps.out_occ[i]++; }
        }
    }
    return out_written;
}

void rsx_vp_stats_print(void)
{
    FILE* f = stderr;
    fprintf(f, "[shader-stats] vp programs=%u instructions=%u pipelines=%u seen_full=%u"
               " (counts: occurrences/programs)\n",
            s_vps.programs, s_vps.instructions, s_vps.psos, s_vps.seen_full);
    char line[1024]; int n = 0;
    line[0] = '\0';
    for (u32 i = 0; i < 32; i++)
        if (s_vps.vec_occ[i])
            n += snprintf(line + n, sizeof line - (size_t)n, " %s=%llu/%u",
                          rsx_vp_vec_name(i), s_vps.vec_occ[i], s_vps.vec_prog[i]);
    fprintf(f, "[shader-stats] vp vec ops:%s\n", line);
    n = 0; line[0] = '\0';
    for (u32 i = 0; i < 32; i++)
        if (s_vps.sca_occ[i])
            n += snprintf(line + n, sizeof line - (size_t)n, " %s=%llu/%u",
                          rsx_vp_sca_name(i), s_vps.sca_occ[i], s_vps.sca_prog[i]);
    fprintf(f, "[shader-stats] vp sca ops:%s\n", line);
    n = 0; line[0] = '\0';
    for (u32 i = 0; i < VPS_NFLAGS; i++)
        if (s_vps.fl_occ[i])
            n += snprintf(line + n, sizeof line - (size_t)n, " %s=%llu/%u",
                          vps_flag_names[i], s_vps.fl_occ[i], s_vps.fl_prog[i]);
    fprintf(f, "[shader-stats] vp flags:%s\n", line);
    n = 0; line[0] = '\0';
    for (u32 i = 0; i < 16; i++)
        if (s_vps.in_occ[i])
            n += snprintf(line + n, sizeof line - (size_t)n, " v%u=%llu/%u", i, s_vps.in_occ[i], s_vps.in_prog[i]);
    fprintf(f, "[shader-stats] vp inputs:%s\n", line);
    n = 0; line[0] = '\0';
    for (u32 i = 0; i < 16; i++)
        if (s_vps.out_prog[i])
            n += snprintf(line + n, sizeof line - (size_t)n, " o%u=%u", i, s_vps.out_prog[i]);
    fprintf(f, "[shader-stats] vp outputs written (programs):%s\n", line);
}
