#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>

#include "pictureEnd.h"
#include "pictureEndBits.h"

// The syntax followed is that of Rec. ITU-T H.264 (08/2021): 7.3 for the parameter sets, the slice header and the
// macroblock layer, 9.2 for CAVLC and 9.3 for CABAC; the neighbours of a macroblock and its blocks as 6.4.9 to 6.4.12.

namespace
{
using picture_end::rangeTabLps;
using picture_end::Rbsp;
using picture_end::Stop;
using picture_end::transIdxLps;
using picture_end::unescape;

struct Vlc4
{
    uint32_t code;
    uint8_t len;
    int8_t a;
    int8_t b;
};
#include "h264CabacInit.inc"
#include "h264Tables.inc"

void scalingList(Rbsp& r, const int size)
{
    int last = 8, next = 8;
    for (int j = 0; j < size; ++j)
    {
        if (next != 0)
            next = (last + r.se() + 256) % 256;
        last = next == 0 ? last : next;
    }
}

struct Sps
{
    int profile = 0;
    bool constraintSet1 = false;
    int chromaFormat = 1;
    bool separatePlanes = false;
    int bitDepthY = 8, bitDepthC = 8;
    int log2MaxFrameNum = 4;
    int pocType = 0;
    int log2MaxPocLsb = 4;
    bool deltaPocAlwaysZero = false;
    int widthMbs = 0, heightMapUnits = 0;
    bool frameMbsOnly = true, mbaff = false, direct8x8 = false;
};

bool parseSps(const std::vector<uint8_t>& nal, Sps& s, int& id)
{
    try
    {
        Rbsp r(nal);
        r.skip(8);
        s.profile = static_cast<int>(r.get(8));
        r.skip(1);
        s.constraintSet1 = r.get(1);
        r.skip(6 + 8);
        id = static_cast<int>(r.ue());
        const int p = s.profile;
        if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128 ||
            p == 138 || p == 139 || p == 134 || p == 135)
        {
            s.chromaFormat = static_cast<int>(r.ue());
            if (s.chromaFormat == 3)
                s.separatePlanes = r.get(1);
            s.bitDepthY = 8 + static_cast<int>(r.ue());
            s.bitDepthC = 8 + static_cast<int>(r.ue());
            r.skip(1);  // qpprime_y_zero_transform_bypass_flag
            if (r.get(1))
                for (int i = 0; i < (s.chromaFormat != 3 ? 8 : 12); ++i)
                    if (r.get(1))
                        scalingList(r, i < 6 ? 16 : 64);
        }
        s.log2MaxFrameNum = 4 + static_cast<int>(r.ue());
        s.pocType = static_cast<int>(r.ue());
        if (s.pocType == 0)
            s.log2MaxPocLsb = 4 + static_cast<int>(r.ue());
        else if (s.pocType == 1)
        {
            s.deltaPocAlwaysZero = r.get(1);
            r.se();
            r.se();
            const uint32_t n = r.ue();
            if (n > 255)
                return false;
            for (uint32_t i = 0; i < n; ++i) r.se();
        }
        r.ue();  // max_num_ref_frames
        r.skip(1);
        s.widthMbs = 1 + static_cast<int>(r.ue());
        s.heightMapUnits = 1 + static_cast<int>(r.ue());
        s.frameMbsOnly = r.get(1);
        if (!s.frameMbsOnly)
            s.mbaff = r.get(1);
        s.direct8x8 = r.get(1);
        return s.chromaFormat <= 3 && s.bitDepthY <= 14 && s.bitDepthC <= 14 && s.log2MaxFrameNum <= 16 &&
               s.log2MaxPocLsb <= 16 && s.pocType <= 2 && s.widthMbs <= 1024 && s.heightMapUnits <= 1024;
    }
    catch (const Stop&)
    {
        return false;
    }
}

struct Pps
{
    int spsId = 0;
    bool cabac = false, bottomFieldPicOrder = false;
    int sliceGroups = 1;
    int numRefIdx[2] = {1, 1};
    bool weightedPred = false;
    int weightedBipred = 0;
    int initQp = 26;
    bool deblockingControl = false, constrainedIntra = false, redundantPicCnt = false, transform8x8 = false;
};

// with its SPS, as the scaling lists it may carry are counted by the chroma format
bool parsePps(const std::vector<uint8_t>& nal, const std::map<int, std::vector<uint8_t>>& spsMap, Pps& p, Sps& s)
{
    try
    {
        Rbsp r(nal);
        r.skip(8);
        r.ue();
        p.spsId = static_cast<int>(r.ue());
        const auto it = spsMap.find(p.spsId);
        int id = 0;
        if (it == spsMap.end() || !parseSps(it->second, s, id))
            return false;
        p.cabac = r.get(1);
        p.bottomFieldPicOrder = r.get(1);
        p.sliceGroups = 1 + static_cast<int>(r.ue());
        if (p.sliceGroups > 1)
            return true;  // slice groups: the caller gives up
        p.numRefIdx[0] = 1 + static_cast<int>(r.ue());
        p.numRefIdx[1] = 1 + static_cast<int>(r.ue());
        p.weightedPred = r.get(1);
        p.weightedBipred = static_cast<int>(r.get(2));
        p.initQp = 26 + r.se();
        r.se();
        r.se();
        p.deblockingControl = r.get(1);
        p.constrainedIntra = r.get(1);
        p.redundantPicCnt = r.get(1);
        if (r.moreRbspData())
        {
            p.transform8x8 = r.get(1);
            if (r.get(1))
                for (int i = 0; i < 6 + (s.chromaFormat != 3 ? 2 : 6) * p.transform8x8; ++i)
                    if (r.get(1))
                        scalingList(r, i < 6 ? 16 : 64);
        }
        return p.numRefIdx[0] <= 32 && p.numRefIdx[1] <= 32;
    }
    catch (const Stop&)
    {
        return false;
    }
}

// The kinds of slice (slice_type % 5)
constexpr int P_SLICE = 0, B_SLICE = 1, I_SLICE = 2, SP_SLICE = 3, SI_SLICE = 4;
// The prediction of a partition: which lists it uses
constexpr uint8_t PRED_L0 = 1, PRED_L1 = 2, PRED_BI = 3;

// Table 7-14: the partitions of the B macroblock types 1 to 21: shape (0 16x16, 1 16x8, 2 8x16) and the prediction of
// each partition
const uint8_t bTypes[22][3] = {{0, 0, 0},
                               {0, PRED_L0, 0},
                               {0, PRED_L1, 0},
                               {0, PRED_BI, 0},
                               {1, PRED_L0, PRED_L0},
                               {2, PRED_L0, PRED_L0},
                               {1, PRED_L1, PRED_L1},
                               {2, PRED_L1, PRED_L1},
                               {1, PRED_L0, PRED_L1},
                               {2, PRED_L0, PRED_L1},
                               {1, PRED_L1, PRED_L0},
                               {2, PRED_L1, PRED_L0},
                               {1, PRED_L0, PRED_BI},
                               {2, PRED_L0, PRED_BI},
                               {1, PRED_L1, PRED_BI},
                               {2, PRED_L1, PRED_BI},
                               {1, PRED_BI, PRED_L0},
                               {2, PRED_BI, PRED_L0},
                               {1, PRED_BI, PRED_L1},
                               {2, PRED_BI, PRED_L1},
                               {1, PRED_BI, PRED_BI},
                               {2, PRED_BI, PRED_BI}};
// Table 7-18: the B sub-macroblock types: number of parts, part width, part height, prediction (0 direct)
const uint8_t bSubTypes[13][4] = {{4, 4, 4, 0},       {1, 8, 8, PRED_L0}, {1, 8, 8, PRED_L1}, {1, 8, 8, PRED_BI},
                                  {2, 8, 4, PRED_L0}, {2, 4, 8, PRED_L0}, {2, 8, 4, PRED_L1}, {2, 4, 8, PRED_L1},
                                  {2, 8, 4, PRED_BI}, {2, 4, 8, PRED_BI}, {4, 4, 4, PRED_L0}, {4, 4, 4, PRED_L1},
                                  {4, 4, 4, PRED_BI}};
// Table 7-17: the P sub-macroblock types
const uint8_t pSubTypes[4][4] = {{1, 8, 8, PRED_L0}, {2, 8, 4, PRED_L0}, {2, 4, 8, PRED_L0}, {4, 4, 4, PRED_L0}};

// What a later macroblock of the slice needs to know of one decoded before it
struct Mb
{
    int slice = -1;  // the slice it was decoded in; -1 not yet
    bool skip = false, field = false, pcm = false, intra = false, i16 = false, inxn = false, direct16 = false;
    bool transform8x8 = false;
    uint8_t cbpLuma = 0, cbpChroma = 0, chromaPredMode = 0;
    bool qpDeltaNonZero = false;
    uint8_t predFlags[4] = {};  // per 8x8 quadrant: PRED_L0 | PRED_L1 as the partition covering it predicts
    int8_t refIdx[2][4] = {};
    uint8_t mvd[2][16][2] = {};  // absolute, capped at 255, per 4x4 block in raster order
    uint8_t totalCoeff[3][16] = {};
    bool cbfDc[3] = {};
    bool cbf4x4[3][16] = {};
    bool cbf8x8[3][4] = {};
};

// 6.4.3: the upper-left luma position of the 4x4 block luma4x4BlkIdx
int blkX(const int i) { return (i / 4 % 2) * 8 + (i % 4 % 2) * 4; }
int blkY(const int i) { return (i / 4 / 2) * 8 + (i % 4 / 2) * 4; }
int blkAt(const int x, const int y) { return 8 * (y / 8) + 4 * (x / 8) + 2 * (y % 8 / 4) + x % 8 / 4; }

class CabacEngine
{
   public:
    void init(Rbsp& r)
    {
        m_range = 510;
        m_offset = r.get(9);
        if (m_offset >= 510)
            throw Stop{false};
    }
    void initContexts(const int sliceType, const int initIdc, const int qp)
    {
        const int8_t(*table)[2] = sliceType == I_SLICE || sliceType == SI_SLICE ? cabacInitI : cabacInitPB[initIdc];
        const int q = std::clamp(qp, 0, 51);
        for (int i = 0; i < 1024; ++i)
        {
            const int pre = std::clamp(((table[i][0] * q) >> 4) + table[i][1], 1, 126);
            m_state[i] = static_cast<uint8_t>(pre <= 63 ? 63 - pre : pre - 64);
            m_mps[i] = pre > 63;
        }
    }
    int decision(Rbsp& r, const int ctx)
    {
        const uint32_t lps = rangeTabLps[m_state[ctx]][(m_range >> 6) & 3];
        m_range -= lps;
        int bin;
        if (m_offset >= m_range)
        {
            bin = !m_mps[ctx];
            m_offset -= m_range;
            m_range = lps;
            if (m_state[ctx] == 0)
                m_mps[ctx] = !m_mps[ctx];
            m_state[ctx] = transIdxLps[m_state[ctx]];
        }
        else
        {
            bin = m_mps[ctx];
            m_state[ctx] = static_cast<uint8_t>(std::min(m_state[ctx] + 1, 62));
        }
        while (m_range < 256)
        {
            m_range <<= 1;
            m_offset = m_offset << 1 | r.bit();
        }
        return bin;
    }
    int bypass(Rbsp& r)
    {
        m_offset = m_offset << 1 | r.bit();
        if (m_offset >= m_range)
        {
            m_offset -= m_range;
            return 1;
        }
        return 0;
    }
    int terminate(Rbsp& r)
    {
        m_range -= 2;
        if (m_offset >= m_range)
            return 1;
        while (m_range < 256)
        {
            m_range <<= 1;
            m_offset = m_offset << 1 | r.bit();
        }
        return 0;
    }

   private:
    uint32_t m_range = 510, m_offset = 0;
    uint8_t m_state[1024] = {};
    bool m_mps[1024] = {};
};

struct SliceResult
{
    enum
    {
        whole,
        outOfData,
        broken
    } state = broken;
    int lastMb = -1;  // the address of the last macroblock it holds, or of the one it ends in
    int stopMb = -1;  // outOfData: the macroblock being read when the data ended (-1 in the slice header)
};

class Slice
{
   public:
    Slice(const Sps& sps, const Pps& pps, int nalType, int nalRefIdc, std::vector<Mb>& mbs, int sliceNum)
        : m_sps(sps), m_pps(pps), m_nalType(nalType), m_nalRefIdc(nalRefIdc), m_mbs(mbs), m_sliceNum(sliceNum)
    {
    }
    SliceResult parse(Rbsp& r);
    int picSizeInMbs() const { return m_picSize; }
    int widthMbs() const { return m_w; }
    bool mbaff() const { return m_mbaff; }
    bool parseHeader(Rbsp& r);
    int redundantPicCnt = 0;
    int firstMb = 0;

   private:
    const Sps& m_sps;
    const Pps& m_pps;
    int m_nalType, m_nalRefIdc;
    std::vector<Mb>& m_mbs;
    int m_sliceNum;

    int m_sliceType = 0;
    bool m_fieldPic = false, m_mbaff = false, m_directSpatial = false;
    int m_numRefIdx[2] = {1, 1};
    int m_initIdc = 0;
    int m_qp = 26;
    int m_w = 0, m_picSize = 0;
    int m_chromaArrayType = 1;
    int m_mbWidthC = 8, m_mbHeightC = 8, m_numC8x8 = 1;
    CabacEngine m_cabac;
    int m_curr = 0;
    int m_prevMb = -1;  // the macroblock before the current one in decoding order, in this slice

    Mb& cur() { return m_mbs[m_curr]; }
    bool available(const int a) const
    {
        return a >= 0 && a < m_picSize && m_mbs[a].slice == m_sliceNum && (a < m_curr || a == m_curr);
    }
    int neighbour(int xN, int yN, int maxW, int maxH, int& xW, int& yW) const;
    void pairNeighbours(int& a, int& b) const;
    bool inferField() const;
    bool fieldCoded() const { return m_fieldPic || m_mbs[m_curr].field; }

    void startMb();
    void markSkipped();
    void macroblockLayer(Rbsp& r, bool cabac);
    // CAVLC
    int residualBlockCavlc(Rbsp& r, int comp, int blk, int kind, int startIdx, int endIdx, int maxNumCoeff);
    int nC(int comp, int blk, bool chroma) const;
    // CABAC
    int ctxSkip() const;
    int ctxTransform() const;
    int ctxField() const;
    int mbTypeCabac(Rbsp& r);
    int intraTypeCabac(Rbsp& r, int offset, bool prefixCtx);
    int subTypeCabac(Rbsp& r);
    int refIdxCabac(Rbsp& r, int list, int x, int y);
    int mvdCabac(Rbsp& r, int list, int comp, int x, int y);
    int cbpCabac(Rbsp& r);
    bool cbfCabac(Rbsp& r, int cat, int comp, int blk);
    void residualBlockCabac(Rbsp& r, int cat, int comp, int blk, int startIdx, int endIdx, int maxNumCoeff);
    // shared
    void mbPred(Rbsp& r, int mbType, bool cabac);
    void subMbPred(Rbsp& r, int mbType, bool cabac, bool& allNoSmallerThan8x8);
    void residual(Rbsp& r, bool cabac);
    void residualLuma(Rbsp& r, bool cabac, int comp);
    int refIdxCavlc(Rbsp& r, int list);
    void setPartition(int list, int x, int y, int w, int h, int ref, bool hasRef);
    void setMvd(int list, int x, int y, int w, int h, int mvx, int mvy);

    int m_mbType = 0;  // of the current macroblock, in the numbering of its slice type
    uint8_t m_partPred[4] = {};
    int m_subType[4] = {};
    int m_cbpBins = 0;  // CABAC: the luma bins of coded_block_pattern decoded so far for the current macroblock
};

bool Slice::parseHeader(Rbsp& r)
{
    r.skip(8);
    firstMb = static_cast<int>(r.ue());
    m_sliceType = static_cast<int>(r.ue() % 5);
    r.ue();  // pic_parameter_set_id
    if (m_sps.separatePlanes)
        return false;
    r.skip(m_sps.log2MaxFrameNum);
    if (!m_sps.frameMbsOnly)
    {
        m_fieldPic = r.get(1);
        if (m_fieldPic)
            r.skip(1);
    }
    if (m_nalType == 5)
        r.ue();
    if (m_sps.pocType == 0)
    {
        r.skip(m_sps.log2MaxPocLsb);
        if (m_pps.bottomFieldPicOrder && !m_fieldPic)
            r.se();
    }
    if (m_sps.pocType == 1 && !m_sps.deltaPocAlwaysZero)
    {
        r.se();
        if (m_pps.bottomFieldPicOrder && !m_fieldPic)
            r.se();
    }
    if (m_pps.redundantPicCnt)
        redundantPicCnt = static_cast<int>(r.ue());
    if (m_sliceType == B_SLICE)
        m_directSpatial = r.get(1);
    m_numRefIdx[0] = m_pps.numRefIdx[0];
    m_numRefIdx[1] = m_pps.numRefIdx[1];
    if (m_sliceType == P_SLICE || m_sliceType == SP_SLICE || m_sliceType == B_SLICE)
        if (r.get(1))
        {
            m_numRefIdx[0] = 1 + static_cast<int>(r.ue());
            if (m_sliceType == B_SLICE)
                m_numRefIdx[1] = 1 + static_cast<int>(r.ue());
        }
    if (m_numRefIdx[0] > 32 || m_numRefIdx[1] > 32)
        return false;
    // ref_pic_list_modification
    for (int list = 0; list < (m_sliceType == B_SLICE ? 2 : 1); ++list)
        if (m_sliceType != I_SLICE && m_sliceType != SI_SLICE && r.get(1))
            for (int n = 0;; ++n)
            {
                const uint32_t idc = r.ue();
                if (idc == 3)
                    break;
                if (idc > 2 || n > 64)
                    return false;
                r.ue();
            }
    // pred_weight_table
    m_chromaArrayType = m_sps.separatePlanes ? 0 : m_sps.chromaFormat;
    if ((m_pps.weightedPred && (m_sliceType == P_SLICE || m_sliceType == SP_SLICE)) ||
        (m_pps.weightedBipred == 1 && m_sliceType == B_SLICE))
    {
        r.ue();
        if (m_chromaArrayType != 0)
            r.ue();
        for (int list = 0; list < (m_sliceType == B_SLICE ? 2 : 1); ++list)
            for (int i = 0; i < m_numRefIdx[list]; ++i)
            {
                if (r.get(1))
                {
                    r.se();
                    r.se();
                }
                if (m_chromaArrayType != 0 && r.get(1))
                    for (int j = 0; j < 4; ++j) r.se();
            }
    }
    // dec_ref_pic_marking
    if (m_nalRefIdc != 0)
    {
        if (m_nalType == 5)
            r.skip(2);
        else if (r.get(1))
            for (int n = 0;; ++n)
            {
                const uint32_t op = r.ue();
                if (op == 0)
                    break;
                if (op > 6 || n > 64)
                    return false;
                if (op == 1 || op == 3)
                    r.ue();
                if (op == 2)
                    r.ue();
                if (op == 3 || op == 6)
                    r.ue();
                if (op == 4)
                    r.ue();
            }
    }
    if (m_pps.cabac && m_sliceType != I_SLICE && m_sliceType != SI_SLICE)
    {
        m_initIdc = static_cast<int>(r.ue());
        if (m_initIdc > 2)
            return false;
    }
    m_qp = m_pps.initQp + r.se();
    if (m_sliceType == SP_SLICE || m_sliceType == SI_SLICE)
    {
        if (m_sliceType == SP_SLICE)
            r.skip(1);
        r.se();
    }
    if (m_pps.deblockingControl && r.ue() != 1)
    {
        r.se();
        r.se();
    }
    m_mbaff = m_sps.mbaff && !m_fieldPic;
    m_w = m_sps.widthMbs;
    const int frameHeight = (2 - m_sps.frameMbsOnly) * m_sps.heightMapUnits;
    m_picSize = m_w * (frameHeight / (1 + m_fieldPic));
    m_mbWidthC = m_chromaArrayType == 3 ? 16 : 8;
    m_mbHeightC = m_chromaArrayType == 1 ? 8 : 16;
    m_numC8x8 = m_chromaArrayType == 2 ? 2 : 1;
    return m_sliceType != SI_SLICE && firstMb * (1 + m_mbaff) < m_picSize;
}

// 6.4.10: the left and upper macroblock pairs (their top macroblocks) of an MBAFF frame, -1 when not available
void Slice::pairNeighbours(int& a, int& b) const
{
    const int pair = m_curr / 2;
    a = pair % m_w != 0 && available(2 * (pair - 1)) ? 2 * (pair - 1) : -1;
    b = available(2 * (pair - m_w)) ? 2 * (pair - m_w) : -1;
}

// 7.4.4: mb_field_decoding_flag not present for either macroblock of a pair: as the pair to the left, else above, else
// frame
bool Slice::inferField() const
{
    int a, b;
    pairNeighbours(a, b);
    if (a >= 0)
        return m_mbs[a].field;
    if (b >= 0)
        return m_mbs[b].field;
    return false;
}

// 6.4.12: the macroblock holding the location (xN, yN) relative to the upper-left sample of the current macroblock,
// in luma (maxW, maxH 16) or chroma samples, and the location in it; -1 when it is not available
int Slice::neighbour(const int xN, const int yN, const int maxW, const int maxH, int& xW, int& yW) const
{
    int n = -1, yM = yN;
    if (!m_mbaff)
    {
        const int c = m_curr;
        const bool left = c % m_w != 0, right = (c + 1) % m_w != 0;
        if (xN < 0 && yN < 0)
            n = left ? c - m_w - 1 : -1;
        else if (xN < 0 && yN < maxH)
            n = left ? c - 1 : -1;
        else if (xN < 0)
            n = -1;
        else if (xN < maxW && yN < 0)
            n = c - m_w;
        else if (xN < maxW && yN < maxH)
            n = c;
        else if (xN >= maxW && yN < 0)
            n = right ? c - m_w + 1 : -1;
        if (n >= 0 && !available(n))
            n = -1;
    }
    else
    {
        // Table 6-4
        const int pair = m_curr / 2;
        const bool hasLeft = pair % m_w != 0, hasRight = (pair + 1) % m_w != 0;
        const int A = hasLeft ? 2 * (pair - 1) : -1, B = 2 * (pair - m_w), C = hasRight ? 2 * (pair - m_w + 1) : -1;
        const int D = hasLeft ? 2 * (pair - m_w - 1) : -1;
        const bool currFrame = !m_mbs[m_curr].field, top = m_curr % 2 == 0;
        const auto avail = [&](const int x) { return x >= 0 && available(x); };
        const auto frameMb = [&](const int x) { return !m_mbs[x].field; };
        if (yN > maxH - 1)
            return -1;
        if (xN < 0 && yN < 0)
        {
            if (currFrame)
            {
                if (top)
                {
                    if (!avail(D))
                        return -1;
                    n = D + 1;
                }
                else
                {
                    if (!avail(A))
                        return -1;
                    if (frameMb(A))
                        n = A;
                    else
                    {
                        n = A + 1;
                        yM = (yN + maxH) >> 1;
                    }
                }
            }
            else
            {
                if (!avail(D))
                    return -1;
                if (top)
                {
                    if (frameMb(D))
                    {
                        n = D + 1;
                        yM = 2 * yN;
                    }
                    else
                        n = D;
                }
                else
                    n = D + 1;
            }
        }
        else if (xN < 0)
        {
            if (!avail(A))
                return -1;
            const bool aFrame = frameMb(A);
            if (currFrame)
            {
                if (top)
                {
                    if (aFrame)
                        n = A;
                    else
                    {
                        n = yN % 2 == 0 ? A : A + 1;
                        yM = yN >> 1;
                    }
                }
                else
                {
                    if (aFrame)
                        n = A + 1;
                    else
                    {
                        n = yN % 2 == 0 ? A : A + 1;
                        yM = (yN + maxH) >> 1;
                    }
                }
            }
            else
            {
                if (top)
                {
                    if (aFrame)
                    {
                        if (yN < maxH / 2)
                        {
                            n = A;
                            yM = yN << 1;
                        }
                        else
                        {
                            n = A + 1;
                            yM = (yN << 1) - maxH;
                        }
                    }
                    else
                        n = A;
                }
                else
                {
                    if (aFrame)
                    {
                        if (yN < maxH / 2)
                        {
                            n = A;
                            yM = (yN << 1) + 1;
                        }
                        else
                        {
                            n = A + 1;
                            yM = (yN << 1) + 1 - maxH;
                        }
                    }
                    else
                        n = A + 1;
                }
            }
        }
        else if (xN < maxW)
        {
            if (yN >= 0)
                n = m_curr;
            else if (currFrame)
            {
                if (top)
                {
                    if (!avail(B))
                        return -1;
                    n = B + 1;
                }
                else
                    n = m_curr - 1;
            }
            else
            {
                if (!avail(B))
                    return -1;
                if (top)
                {
                    if (frameMb(B))
                    {
                        n = B + 1;
                        yM = 2 * yN;
                    }
                    else
                        n = B;
                }
                else
                    n = B + 1;
            }
        }
        else
        {
            if (yN >= 0)
                return -1;
            if (currFrame)
            {
                if (!top || !avail(C))
                    return -1;
                n = C + 1;
            }
            else
            {
                if (!avail(C))
                    return -1;
                if (top)
                {
                    if (frameMb(C))
                    {
                        n = C + 1;
                        yM = 2 * yN;
                    }
                    else
                        n = C;
                }
                else
                    n = C + 1;
            }
        }
    }
    if (n < 0)
        return -1;
    xW = (xN + maxW) % maxW;
    yW = (yM + maxH) % maxH;
    return n;
}

// 9.2.1: nC of a 4x4 block (comp 0 luma, 1 Cb, 2 Cr; chroma: a chroma AC block of 4:2:0 or 4:2:2)
int Slice::nC(const int comp, const int blk, const bool chroma) const
{
    int n[2], avail = 0, sum = 0;
    for (int k = 0; k < 2; ++k)
    {
        int xW, yW, a;
        if (chroma)
        {
            const int x = blk % 2 * 4, y = blk / 2 * 4;
            a = neighbour(x - (k == 0), y - (k == 1), m_mbWidthC, m_mbHeightC, xW, yW);
            n[k] = a < 0 ? -1 : 2 * (yW / 4) + xW / 4;
        }
        else
        {
            a = neighbour(blkX(blk) - (k == 0), blkY(blk) - (k == 1), 16, 16, xW, yW);
            n[k] = a < 0 ? -1 : blkAt(xW, yW);
        }
        if (a < 0)
            continue;
        const Mb& m = m_mbs[a];
        ++avail;
        sum += m.skip ? 0 : m.pcm ? 16 : m.totalCoeff[comp][n[k]];
    }
    return avail == 2 ? (sum + 1) >> 1 : sum;
}

// kind: 0 a 4x4 block of comp, 1 chroma DC, 2 chroma AC (4:2:0, 4:2:2). Returns TotalCoeff.
int Slice::residualBlockCavlc(Rbsp& r, const int comp, const int blk, const int kind, const int startIdx,
                              const int endIdx, const int maxNumCoeff)
{
    const Vlc4* token;
    if (kind == 1)
        token = m_chromaArrayType == 1 ? &r.vlc(coeffTokenDc420) : &r.vlc(coeffTokenDc422);
    else
    {
        const int c = nC(comp, blk, kind == 2);
        token = c < 2   ? &r.vlc(coeffToken0)
                : c < 4 ? &r.vlc(coeffToken2)
                : c < 8 ? &r.vlc(coeffToken4)
                        : &r.vlc(coeffToken8);
    }
    const int total = token->a, trailing = token->b;
    if (total > maxNumCoeff)
        throw Stop{false};
    if (total == 0)
        return 0;
    int suffixLength = total > 10 && trailing < 3 ? 1 : 0;
    for (int i = 0; i < total; ++i)
    {
        if (i < trailing)
        {
            r.skip(1);
            continue;
        }
        int prefix = 0;
        while (!r.bit())
            if (++prefix > 31)
                throw Stop{false};
        int64_t code = static_cast<int64_t>(std::min(15, prefix)) << suffixLength;
        int suffixSize = suffixLength;
        if (prefix == 14 && suffixLength == 0)
            suffixSize = 4;
        else if (prefix >= 15)
            suffixSize = prefix - 3;
        if (suffixSize > 0)
            code += r.get(suffixSize);
        if (prefix >= 15 && suffixLength == 0)
            code += 15;
        if (prefix >= 16)
            code += (int64_t{1} << (prefix - 3)) - 4096;
        if (i == trailing && trailing < 3)
            code += 2;
        const int64_t level = code % 2 == 0 ? (code + 2) >> 1 : (-code - 1) >> 1;
        if (suffixLength == 0)
            suffixLength = 1;
        if (std::abs(level) > (3 << (suffixLength - 1)) && suffixLength < 6)
            ++suffixLength;
    }
    int zerosLeft = 0;
    if (total < endIdx - startIdx + 1)
    {
        int tz;
        if (maxNumCoeff == 4)
            tz = total == 1   ? r.vlc(totalZerosDc4201).a
                 : total == 2 ? r.vlc(totalZerosDc4202).a
                              : r.vlc(totalZerosDc4203).a;
        else if (maxNumCoeff == 8)
        {
            switch (total)
            {
            case 1:
                tz = r.vlc(totalZerosDc4221).a;
                break;
            case 2:
                tz = r.vlc(totalZerosDc4222).a;
                break;
            case 3:
                tz = r.vlc(totalZerosDc4223).a;
                break;
            case 4:
                tz = r.vlc(totalZerosDc4224).a;
                break;
            case 5:
                tz = r.vlc(totalZerosDc4225).a;
                break;
            case 6:
                tz = r.vlc(totalZerosDc4226).a;
                break;
            default:
                tz = r.vlc(totalZerosDc4227).a;
                break;
            }
        }
        else
        {
            switch (total)
            {
            case 1:
                tz = r.vlc(totalZeros1).a;
                break;
            case 2:
                tz = r.vlc(totalZeros2).a;
                break;
            case 3:
                tz = r.vlc(totalZeros3).a;
                break;
            case 4:
                tz = r.vlc(totalZeros4).a;
                break;
            case 5:
                tz = r.vlc(totalZeros5).a;
                break;
            case 6:
                tz = r.vlc(totalZeros6).a;
                break;
            case 7:
                tz = r.vlc(totalZeros7).a;
                break;
            case 8:
                tz = r.vlc(totalZeros8).a;
                break;
            case 9:
                tz = r.vlc(totalZeros9).a;
                break;
            case 10:
                tz = r.vlc(totalZeros10).a;
                break;
            case 11:
                tz = r.vlc(totalZeros11).a;
                break;
            case 12:
                tz = r.vlc(totalZeros12).a;
                break;
            case 13:
                tz = r.vlc(totalZeros13).a;
                break;
            case 14:
                tz = r.vlc(totalZeros14).a;
                break;
            default:
                tz = r.vlc(totalZeros15).a;
                break;
            }
        }
        if (tz > endIdx - startIdx + 1 - total)
            throw Stop{false};
        zerosLeft = tz;
    }
    for (int i = 0; i < total - 1 && zerosLeft > 0; ++i)
    {
        int run;
        switch (zerosLeft)
        {
        case 1:
            run = r.vlc(runBefore1).a;
            break;
        case 2:
            run = r.vlc(runBefore2).a;
            break;
        case 3:
            run = r.vlc(runBefore3).a;
            break;
        case 4:
            run = r.vlc(runBefore4).a;
            break;
        case 5:
            run = r.vlc(runBefore5).a;
            break;
        case 6:
            run = r.vlc(runBefore6).a;
            break;
        default:
            run = r.vlc(runBefore7).a;
            break;
        }
        if (run > zerosLeft)
            throw Stop{false};
        zerosLeft -= run;
    }
    return total;
}

// The partition data of the current macroblock: the lists each 8x8 quadrant predicts from, its reference index, and
// the motion vector differences of each 4x4 block, for the contexts of the macroblocks and partitions after it
void Slice::setPartition(const int list, const int x, const int y, const int w, const int h, const int ref,
                         const bool hasRef)
{
    for (int qy = y / 8; qy < (y + h + 7) / 8; ++qy)
        for (int qx = x / 8; qx < (x + w + 7) / 8; ++qx)
            if (hasRef)
                cur().refIdx[list][qy * 2 + qx] = static_cast<int8_t>(std::min(ref, 127));
}

void Slice::setMvd(const int list, const int x, const int y, const int w, const int h, const int mvx, const int mvy)
{
    for (int by = y / 4; by < (y + h) / 4; ++by)
        for (int bx = x / 4; bx < (x + w) / 4; ++bx)
        {
            cur().mvd[list][by * 4 + bx][0] = static_cast<uint8_t>(std::min(std::abs(mvx), 255));
            cur().mvd[list][by * 4 + bx][1] = static_cast<uint8_t>(std::min(std::abs(mvy), 255));
        }
}

int Slice::refIdxCavlc(Rbsp& r, const int list)
{
    // te(v): the range doubles for a field macroblock of an MBAFF frame
    const int range = m_mbaff && cur().field ? 2 * m_numRefIdx[list] - 1 : m_numRefIdx[list] - 1;
    if (range == 1)
        return !r.get(1);
    const uint32_t v = r.ue();
    if (static_cast<int>(v) > range)
        throw Stop{false};
    return static_cast<int>(v);
}

// 9.3.3.1.1.1
int Slice::ctxSkip() const
{
    int inc = 0;
    for (int k = 0; k < 2; ++k)
    {
        int xW, yW;
        const int a = neighbour(-(k == 0), -(k == 1), 16, 16, xW, yW);
        inc += a >= 0 && !m_mbs[a].skip;
    }
    return inc;
}

// 9.3.3.1.1.2
int Slice::ctxField() const
{
    int a, b;
    pairNeighbours(a, b);
    return (a >= 0 && m_mbs[a].field) + (b >= 0 && m_mbs[b].field);
}

// mb_type of an I macroblock, Table 9-36: offset 3 in I slices (its first bin by the neighbours), else the suffix
// offset of P (17) or B (32) slices
int Slice::intraTypeCabac(Rbsp& r, const int offset, const bool prefixCtx)
{
    int inc = 0;
    if (prefixCtx)
        for (int k = 0; k < 2; ++k)
        {
            int xW, yW;
            const int a = neighbour(-(k == 0), -(k == 1), 16, 16, xW, yW);
            inc += a >= 0 && !m_mbs[a].inxn;
        }
    if (!m_cabac.decision(r, offset + inc))
        return 0;
    if (m_cabac.terminate(r))
        return 25;
    // the contexts of the later bins: offset 3 uses 3..7, the suffixes 1..3
    const int c2 = prefixCtx ? 3 : 1, c3 = prefixCtx ? 4 : 2;
    const int luma15 = m_cabac.decision(r, offset + c2);
    int chroma = 0, pred;
    if (m_cabac.decision(r, offset + c3))
    {
        chroma = 1 + m_cabac.decision(r, offset + (prefixCtx ? 5 : 2));
        const int p0 = m_cabac.decision(r, offset + (prefixCtx ? 6 : 3));
        pred = p0 << 1 | m_cabac.decision(r, offset + (prefixCtx ? 7 : 3));
    }
    else
    {
        const int p0 = m_cabac.decision(r, offset + (prefixCtx ? 6 : 3));
        pred = p0 << 1 | m_cabac.decision(r, offset + (prefixCtx ? 7 : 3));
    }
    return 1 + pred + 4 * chroma + 12 * luma15;
}

// mb_type in the numbering of the slice type (Tables 9-36, 9-37)
int Slice::mbTypeCabac(Rbsp& r)
{
    if (m_sliceType == I_SLICE)
        return intraTypeCabac(r, 3, true);
    if (m_sliceType == P_SLICE || m_sliceType == SP_SLICE)
    {
        if (m_cabac.decision(r, 14))
            return 5 + intraTypeCabac(r, 17, false);
        if (!m_cabac.decision(r, 15))
            return m_cabac.decision(r, 16) ? 3 : 0;
        return m_cabac.decision(r, 17) ? 1 : 2;
    }
    int inc = 0;
    for (int k = 0; k < 2; ++k)
    {
        int xW, yW;
        const int a = neighbour(-(k == 0), -(k == 1), 16, 16, xW, yW);
        inc += a >= 0 && !m_mbs[a].skip && !m_mbs[a].direct16;
    }
    if (!m_cabac.decision(r, 27 + inc))
        return 0;
    if (!m_cabac.decision(r, 27 + 3))
        return 1 + m_cabac.decision(r, 27 + 5);
    const int b2 = m_cabac.decision(r, 27 + 4);
    const auto bin = [&] { return m_cabac.decision(r, 27 + 5); };
    if (!b2)
    {
        const int b3 = bin(), b4 = bin(), b5 = bin();
        return 3 + (b3 << 2 | b4 << 1 | b5);
    }
    if (!bin())
    {
        const int b4 = bin(), b5 = bin(), b6 = bin();
        return 12 + (b4 << 2 | b5 << 1 | b6);
    }
    if (bin())
        return bin() ? 22 : 11;
    if (bin())
        return 23 + intraTypeCabac(r, 32, false);
    return 20 + bin();
}

// Table 9-38
int Slice::subTypeCabac(Rbsp& r)
{
    if (m_sliceType != B_SLICE)
    {
        if (m_cabac.decision(r, 21))
            return 0;
        if (!m_cabac.decision(r, 22))
            return 1;
        return m_cabac.decision(r, 23) ? 2 : 3;
    }
    if (!m_cabac.decision(r, 36))
        return 0;
    if (!m_cabac.decision(r, 37))
        return 1 + m_cabac.decision(r, 39);
    if (!m_cabac.decision(r, 38))
    {
        const int b3 = m_cabac.decision(r, 39);
        return 3 + (b3 << 1 | m_cabac.decision(r, 39));
    }
    if (m_cabac.decision(r, 39))
        return 11 + m_cabac.decision(r, 39);
    const int b4 = m_cabac.decision(r, 39);
    return 7 + (b4 << 1 | m_cabac.decision(r, 39));
}

// 9.3.3.1.1.6; (x, y) the upper-left luma sample of the partition
int Slice::refIdxCabac(Rbsp& r, const int list, const int x, const int y)
{
    int inc = 0;
    for (int k = 0; k < 2; ++k)
    {
        int xW, yW;
        const int a = neighbour(x - (k == 0), y - (k == 1), 16, 16, xW, yW);
        if (a < 0)
            continue;
        const Mb& m = m_mbs[a];
        const int q = yW / 8 * 2 + xW / 8;
        if (m.skip || m.intra || !(m.predFlags[q] & (1 << list)))
            continue;
        const bool zero = m_mbaff && !cur().field && m.field ? m.refIdx[list][q] <= 1 : m.refIdx[list][q] <= 0;
        if (!zero)
            inc += k == 0 ? 1 : 2;
    }
    int v = 0;
    for (int ctx = 54 + inc; m_cabac.decision(r, ctx); ctx = 54 + (v == 1 ? 4 : 5))
        if (++v > 63)
            throw Stop{false};
    return v;
}

// 9.3.3.1.1.7: UEG3 with uCoff 9, the sign after it; returns the absolute value
int Slice::mvdCabac(Rbsp& r, const int list, const int comp, const int x, const int y)
{
    const int offset = comp == 0 ? 40 : 47;
    int absN[2] = {};
    for (int k = 0; k < 2; ++k)
    {
        int xW, yW;
        const int a = neighbour(x - (k == 0), y - (k == 1), 16, 16, xW, yW);
        if (a < 0)
            continue;
        const Mb& m = m_mbs[a];
        if (m.skip || m.intra || !(m.predFlags[yW / 8 * 2 + xW / 8] & (1 << list)))
            continue;
        int v = m.mvd[list][yW / 4 * 4 + xW / 4][comp];
        if (comp == 1 && m_mbaff && !cur().field && m.field)
            v *= 2;
        else if (comp == 1 && m_mbaff && cur().field && !m.field)
            v /= 2;
        absN[k] = v;
    }
    const int sum = absN[0] + absN[1];
    const int inc = absN[0] > 32 || absN[1] > 32 || sum > 32 ? 2 : sum > 2 ? 1 : 0;
    int prefix = 0;
    static const int later[9] = {0, 3, 4, 5, 6, 6, 6, 6, 6};
    while (prefix < 9 && m_cabac.decision(r, offset + (prefix == 0 ? inc : later[prefix]))) ++prefix;
    int value = prefix;
    if (prefix == 9)
    {
        int k = 3;
        while (m_cabac.bypass(r))
        {
            value += 1 << k;
            if (++k > 30)
                throw Stop{false};
        }
        while (k-- > 0) value += m_cabac.bypass(r) << k;
    }
    if (value != 0)
        m_cabac.bypass(r);
    return value;
}

// 9.3.3.1.1.4
int Slice::cbpCabac(Rbsp& r)
{
    m_cbpBins = 0;
    for (int b8 = 0; b8 < 4; ++b8)
    {
        int inc = 0;
        for (int k = 0; k < 2; ++k)
        {
            int xW, yW;
            const int a = neighbour(b8 % 2 * 8 - (k == 0), b8 / 2 * 8 - (k == 1), 16, 16, xW, yW);
            bool cond;
            if (a < 0)
                cond = false;
            else
            {
                const Mb& m = m_mbs[a];
                const int bN = yW / 8 * 2 + xW / 8;
                if (m.pcm)
                    cond = false;
                else if (a != m_curr)
                    cond = m.skip || !(m.cbpLuma >> bN & 1);
                else
                    cond = !(m_cbpBins >> bN & 1);
            }
            inc += cond ? (k == 0 ? 1 : 2) : 0;
        }
        m_cbpBins |= m_cabac.decision(r, 73 + inc) << b8;
    }
    int chroma = 0;
    if (m_chromaArrayType == 1 || m_chromaArrayType == 2)
    {
        for (int bin = 0; bin < 2; ++bin)
        {
            int inc = bin == 1 ? 4 : 0;
            for (int k = 0; k < 2; ++k)
            {
                int xW, yW;
                const int a = neighbour(-(k == 0), -(k == 1), 16, 16, xW, yW);
                bool cond;
                if (a >= 0 && m_mbs[a].pcm)
                    cond = true;
                else if (a < 0 || m_mbs[a].skip)
                    cond = false;
                else
                    cond = bin == 0 ? m_mbs[a].cbpChroma != 0 : m_mbs[a].cbpChroma == 2;
                inc += cond ? (k == 0 ? 1 : 2) : 0;
            }
            if (!m_cabac.decision(r, 77 + inc))
                break;
            chroma = bin + 1;
        }
    }
    return chroma << 4 | m_cbpBins;
}

// 9.3.3.1.1.9: blk is the 4x4 block (cats 1, 2, 4, 7, 8, 11, 12) or the 8x8 block (cats 5, 9, 13)
bool Slice::cbfCabac(Rbsp& r, const int cat, const int comp, const int blk)
{
    static const int catOffset[14] = {0, 4, 8, 12, 16, 0, 0, 4, 8, 4, 0, 4, 8, 8};
    const int base = cat < 5 ? 85 : cat == 5 || cat == 9 || cat == 13 ? 1012 : cat < 9 ? 460 : 472;
    int inc = 0;
    for (int k = 0; k < 2; ++k)
    {
        int xW = 0, yW = 0, a;
        int bN = 0;
        const bool dc = cat == 0 || cat == 6 || cat == 10 || cat == 3;
        if (dc)
            a = neighbour(-(k == 0), -(k == 1), 16, 16, xW, yW);
        else if (cat == 4)
        {
            a = neighbour(blk % 2 * 4 - (k == 0), blk / 2 * 4 - (k == 1), m_mbWidthC, m_mbHeightC, xW, yW);
            bN = 2 * (yW / 4) + xW / 4;
        }
        else if (cat == 5 || cat == 9 || cat == 13)
        {
            a = neighbour(blk % 2 * 8 - (k == 0), blk / 2 * 8 - (k == 1), 16, 16, xW, yW);
            bN = 2 * (yW / 8) + xW / 8;
        }
        else
        {
            a = neighbour(blkX(blk) - (k == 0), blkY(blk) - (k == 1), 16, 16, xW, yW);
            bN = blkAt(xW, yW);
        }
        int cond;
        if (a < 0)
            cond = cur().intra;
        else
        {
            const Mb& m = m_mbs[a];
            if (m.pcm)
                cond = 1;
            else
            {
                int v = -1;  // -1: the block is not there (transBlockN not available)
                if (cat == 0 || cat == 6 || cat == 10)
                {
                    if (m.i16)
                        v = m.cbfDc[comp];
                }
                else if (cat == 3)
                {
                    if (!m.skip && m.cbpChroma != 0)
                        v = m.cbfDc[comp];
                }
                else if (cat == 4)
                {
                    if (!m.skip && m.cbpChroma == 2)
                        v = m.cbf4x4[comp][bN];
                }
                else if (cat == 5 || cat == 9 || cat == 13)
                {
                    // the bit of the neighbouring 8x8 block. The text of the Recommendation names luma8x8BlkIdx, the
                    // current block, here; encoded 4:4:4 streams follow the neighbouring one, as for cats 1 and 2
                    if (!m.skip && (m.cbpLuma >> bN & 1) && m.transform8x8)
                        v = m.cbf8x8[comp][bN];
                }
                else if (!m.skip && (m.cbpLuma >> (bN >> 2) & 1))
                    v = m.transform8x8 ? m.cbf8x8[comp][bN >> 2] : m.cbf4x4[comp][bN];
                cond = v < 0 ? 0 : v;
            }
        }
        inc += cond ? (k == 0 ? 1 : 2) : 0;
    }
    return m_cabac.decision(r, base + catOffset[cat] + inc);
}

void Slice::residualBlockCabac(Rbsp& r, const int cat, const int comp, const int blk, const int startIdx,
                               const int endIdx, const int maxNumCoeff)
{
    const bool cbf = maxNumCoeff != 64 || m_chromaArrayType == 3 ? cbfCabac(r, cat, comp, blk) : true;
    if (cat == 0 || cat == 6 || cat == 10 || cat == 3)
        cur().cbfDc[comp] = cbf;
    else if (cat == 5 || cat == 9 || cat == 13)
        cur().cbf8x8[comp][blk] = cbf;
    else
        cur().cbf4x4[comp][blk] = cbf;
    if (!cbf)
        return;
    static const int sigCat[14] = {0, 15, 29, 44, 47, 0, 0, 15, 29, 0, 0, 15, 29, 0};
    static const int absCat[14] = {0, 10, 20, 30, 39, 0, 0, 10, 20, 0, 0, 10, 20, 0};
    const bool field = fieldCoded();
    int sigBase, lastBase, absBase;
    if (cat < 5)
    {
        sigBase = field ? 277 : 105;
        lastBase = field ? 338 : 166;
        absBase = 227;
    }
    else if (cat == 5)
    {
        sigBase = field ? 436 : 402;
        lastBase = field ? 451 : 417;
        absBase = 426;
    }
    else if (cat < 9)
    {
        sigBase = field ? 776 : 484;
        lastBase = field ? 864 : 572;
        absBase = 952;
    }
    else if (cat == 9)
    {
        sigBase = field ? 675 : 660;
        lastBase = field ? 699 : 690;
        absBase = 708;
    }
    else if (cat < 13)
    {
        sigBase = field ? 820 : 528;
        lastBase = field ? 908 : 616;
        absBase = 982;
    }
    else
    {
        sigBase = field ? 733 : 718;
        lastBase = field ? 757 : 748;
        absBase = 766;
    }
    sigBase += sigCat[cat];
    lastBase += sigCat[cat];
    absBase += absCat[cat];
    const bool block8x8 = cat == 5 || cat == 9 || cat == 13;
    int numCoeff = endIdx + 1, levels = 0;
    bool lastSeen = false;
    for (int i = startIdx; i < numCoeff - 1; ++i)
    {
        const int sigInc = cat == 3 ? std::min(i / m_numC8x8, 2) : block8x8 ? sig8x8[i][field] : i;
        if (m_cabac.decision(r, sigBase + sigInc))
        {
            ++levels;
            const int lastInc = cat == 3 ? std::min(i / m_numC8x8, 2) : block8x8 ? sig8x8[i][2] : i;
            if (m_cabac.decision(r, lastBase + lastInc))
            {
                numCoeff = i + 1;
                lastSeen = true;
            }
        }
    }
    if (!lastSeen)
        ++levels;  // the coefficient at the end of the block, significant without a flag
    int eq1 = 0, gt1 = 0;
    for (int k = 0; k < levels; ++k)
    {
        int prefix = 0;
        if (m_cabac.decision(r, absBase + (gt1 != 0 ? 0 : std::min(4, 1 + eq1))))
        {
            prefix = 1;
            const int ctx = absBase + 5 + std::min(4 - (cat == 3), gt1);
            while (prefix < 14 && m_cabac.decision(r, ctx)) ++prefix;
        }
        int value = prefix;
        if (prefix == 14)
        {
            int e = 0;
            while (m_cabac.bypass(r))
            {
                value += 1 << e;
                if (++e > 30)
                    throw Stop{false};
            }
            while (e-- > 0) value += m_cabac.bypass(r) << e;
        }
        m_cabac.bypass(r);  // coeff_sign_flag
        if (value == 0)
            ++eq1;
        else
            ++gt1;
    }
}

void Slice::residualLuma(Rbsp& r, const bool cabac, const int comp)
{
    Mb& m = cur();
    const int catDc = comp * 4 + (comp ? 2 : 0), catAc = catDc + 1, cat4x4 = catDc + 2;
    const int cat8x8 = comp == 0 ? 5 : catDc + 3;
    if (m.i16)
    {
        if (cabac)
            residualBlockCabac(r, catDc, comp, 0, 0, 15, 16);
        else
            residualBlockCavlc(r, comp, 0, 0, 0, 15, 16);
    }
    for (int i8x8 = 0; i8x8 < 4; ++i8x8)
    {
        if (!(m.cbpLuma >> i8x8 & 1))
            continue;
        if (m.transform8x8 && cabac)
        {
            residualBlockCabac(r, cat8x8, comp, i8x8, 0, 63, 64);
            continue;
        }
        for (int i4x4 = 0; i4x4 < 4; ++i4x4)
        {
            const int blk = i8x8 * 4 + i4x4;
            if (cabac)
                residualBlockCabac(r, m.i16 ? catAc : cat4x4, comp, blk, 0, m.i16 ? 14 : 15, m.i16 ? 15 : 16);
            else
                m.totalCoeff[comp][blk] =
                    static_cast<uint8_t>(residualBlockCavlc(r, comp, blk, 0, 0, m.i16 ? 14 : 15, m.i16 ? 15 : 16));
        }
    }
}

void Slice::residual(Rbsp& r, const bool cabac)
{
    Mb& m = cur();
    residualLuma(r, cabac, 0);
    if (m_chromaArrayType == 1 || m_chromaArrayType == 2)
    {
        const int dcCoeff = 4 * m_numC8x8;
        for (int c = 1; c <= 2; ++c)
            if (m.cbpChroma & 3)
            {
                if (cabac)
                    residualBlockCabac(r, 3, c, 0, 0, dcCoeff - 1, dcCoeff);
                else
                    residualBlockCavlc(r, c, 0, 1, 0, dcCoeff - 1, dcCoeff);
            }
        for (int c = 1; c <= 2; ++c)
            for (int b = 0; b < 4 * m_numC8x8; ++b)
                if (m.cbpChroma & 2)
                {
                    if (cabac)
                        residualBlockCabac(r, 4, c, b, 0, 14, 15);
                    else
                        m.totalCoeff[c][b] = static_cast<uint8_t>(residualBlockCavlc(r, c, b, 2, 0, 14, 15));
                }
    }
    else if (m_chromaArrayType == 3)
    {
        residualLuma(r, cabac, 1);
        residualLuma(r, cabac, 2);
    }
}

void Slice::mbPred(Rbsp& r, const int mbType, const bool cabac)
{
    Mb& m = cur();
    if (m.intra)
    {
        if (m.inxn)
            for (int i = 0; i < (m.transform8x8 ? 4 : 16); ++i)
            {
                if (cabac)
                {
                    if (!m_cabac.decision(r, 68))
                        for (int k = 0; k < 3; ++k) m_cabac.decision(r, 69);
                }
                else if (!r.get(1))
                    r.skip(3);
            }
        if (m_chromaArrayType == 1 || m_chromaArrayType == 2)
        {
            int mode;
            if (cabac)
            {
                int inc = 0;
                for (int k = 0; k < 2; ++k)
                {
                    int xW, yW;
                    const int a = neighbour(-(k == 0), -(k == 1), 16, 16, xW, yW);
                    inc += a >= 0 && m_mbs[a].intra && !m_mbs[a].pcm && m_mbs[a].chromaPredMode != 0;
                }
                mode = 0;
                if (m_cabac.decision(r, 64 + inc))
                {
                    mode = 1;
                    while (mode < 3 && m_cabac.decision(r, 67)) ++mode;
                }
            }
            else
            {
                mode = static_cast<int>(r.ue());
                if (mode > 3)
                    throw Stop{false};
            }
            m.chromaPredMode = static_cast<uint8_t>(mode);
        }
        return;
    }
    // the partitions: (x, y, w, h) and their prediction
    int parts = 1, shape = 0;
    uint8_t pred[2] = {};
    if (m_sliceType == B_SLICE)
    {
        shape = bTypes[mbType][0];
        pred[0] = bTypes[mbType][1];
        pred[1] = bTypes[mbType][2];
        parts = shape ? 2 : 1;
    }
    else
    {
        shape = mbType;  // 0 16x16, 1 16x8, 2 8x16
        pred[0] = pred[1] = PRED_L0;
        parts = mbType ? 2 : 1;
    }
    int px[2], py[2], pw = shape == 2 ? 8 : 16, ph = shape == 1 ? 8 : 16;
    for (int p = 0; p < parts; ++p)
    {
        px[p] = shape == 2 ? 8 * p : 0;
        py[p] = shape == 1 ? 8 * p : 0;
        for (int q = 0; q < 4; ++q)
            if (q % 2 * 8 >= px[p] && q % 2 * 8 < px[p] + pw && q / 2 * 8 >= py[p] && q / 2 * 8 < py[p] + ph)
                m.predFlags[q] = pred[p];
    }
    const bool fieldDiffers = m.field != m_fieldPic;
    for (int list = 0; list < 2; ++list)
        for (int p = 0; p < parts; ++p)
            if (pred[p] & (1 << list) && (m_numRefIdx[list] > 1 || fieldDiffers))
            {
                const int ref = cabac ? refIdxCabac(r, list, px[p], py[p]) : refIdxCavlc(r, list);
                setPartition(list, px[p], py[p], pw, ph, ref, true);
            }
    for (int list = 0; list < 2; ++list)
        for (int p = 0; p < parts; ++p)
            if (pred[p] & (1 << list))
            {
                const int mx = cabac ? mvdCabac(r, list, 0, px[p], py[p]) : r.se();
                const int my = cabac ? mvdCabac(r, list, 1, px[p], py[p]) : r.se();
                setMvd(list, px[p], py[p], pw, ph, mx, my);
            }
}

void Slice::subMbPred(Rbsp& r, const int mbType, const bool cabac, bool& noSmall)
{
    Mb& m = cur();
    const bool b = m_sliceType == B_SLICE;
    for (int q = 0; q < 4; ++q)
    {
        m_subType[q] = cabac ? subTypeCabac(r) : static_cast<int>(r.ue());
        if (m_subType[q] > (b ? 12 : 3))
            throw Stop{false};
        const uint8_t* t = b ? bSubTypes[m_subType[q]] : pSubTypes[m_subType[q]];
        m.predFlags[q] = t[3];
        if (t[3] == 0)
        {
            if (!m_sps.direct8x8)
                noSmall = false;
        }
        else if (t[0] > 1)
            noSmall = false;
    }
    const bool fieldDiffers = m.field != m_fieldPic;
    for (int list = 0; list < 2; ++list)
        for (int q = 0; q < 4; ++q)
            if (m.predFlags[q] & (1 << list) && (m_numRefIdx[list] > 1 || fieldDiffers) && !(!b && mbType == 4))
            {
                const int ref = cabac ? refIdxCabac(r, list, q % 2 * 8, q / 2 * 8) : refIdxCavlc(r, list);
                setPartition(list, q % 2 * 8, q / 2 * 8, 8, 8, ref, true);
            }
    for (int list = 0; list < 2; ++list)
        for (int q = 0; q < 4; ++q)
        {
            if (!(m.predFlags[q] & (1 << list)))
                continue;
            const uint8_t* t = b ? bSubTypes[m_subType[q]] : pSubTypes[m_subType[q]];
            const int w = t[1], h = t[2];
            for (int s = 0; s < t[0]; ++s)
            {
                const int x = q % 2 * 8 + s % (8 / w) * w, y = q / 2 * 8 + s / (8 / w) * h;
                const int mx = cabac ? mvdCabac(r, list, 0, x, y) : r.se();
                const int my = cabac ? mvdCabac(r, list, 1, x, y) : r.se();
                setMvd(list, x, y, w, h, mx, my);
            }
        }
}

int64_t pcmBits(const Sps& s, const int chromaArrayType)
{
    const int c = chromaArrayType == 0 ? 0 : chromaArrayType == 3 ? 256 : chromaArrayType == 2 ? 128 : 64;
    return 256 * static_cast<int64_t>(s.bitDepthY) + 2 * static_cast<int64_t>(c) * s.bitDepthC;
}

// 9.3.3.1.1.10
int Slice::ctxTransform() const
{
    int inc = 0;
    for (int k = 0; k < 2; ++k)
    {
        int xW, yW;
        const int a = neighbour(-(k == 0), -(k == 1), 16, 16, xW, yW);
        inc += a >= 0 && m_mbs[a].transform8x8;
    }
    return inc;
}

// A macroblock begins: nothing of it is known yet but its field mode, which a macroblock of an MBAFF frame takes from
// its pair, or before the pair has said it, as 7.4.4 infers it
void Slice::startMb()
{
    Mb& m = m_mbs[m_curr];
    m = Mb{};
    m.slice = m_sliceNum;
    if (!m_mbaff)
        m.field = m_fieldPic;
    else
        m.field = m_curr % 2 == 0 ? inferField() : m_mbs[m_curr - 1].field;
}

void Slice::markSkipped()
{
    Mb& m = cur();
    m.skip = true;
    if (m_sliceType == B_SLICE)
        m.direct16 = true;
    else
        for (uint8_t& f : m.predFlags) f = PRED_L0;
}

// 7.3.5
void Slice::macroblockLayer(Rbsp& r, const bool cabac)
{
    Mb& m = cur();
    int mbType;
    if (cabac)
        mbType = mbTypeCabac(r);
    else
    {
        mbType = static_cast<int>(r.ue());
        if (mbType > (m_sliceType == I_SLICE ? 25 : m_sliceType == B_SLICE ? 48 : 30))
            throw Stop{false};
    }
    int intraType = -1;
    if (m_sliceType == I_SLICE)
        intraType = mbType;
    else if (m_sliceType == B_SLICE)
        intraType = mbType >= 23 ? mbType - 23 : -1;
    else
        intraType = mbType >= 5 ? mbType - 5 : -1;
    bool noSmall = true;
    if (intraType >= 0)
    {
        m.intra = true;
        if (intraType == 25)
        {
            m.pcm = true;
            while (!r.aligned()) r.skip(1);
            r.skip(pcmBits(m_sps, m_chromaArrayType));
            if (cabac)
                m_cabac.init(r);
            return;
        }
        if (intraType == 0)
        {
            m.inxn = true;
            if (m_pps.transform8x8)
                m.transform8x8 = cabac ? m_cabac.decision(r, 399 + ctxTransform()) : r.get(1);
        }
        else
        {
            m.i16 = true;
            m.cbpLuma = intraType >= 13 ? 15 : 0;
            m.cbpChroma = static_cast<uint8_t>((intraType - 1) / 4 % 3);
        }
        mbPred(r, mbType, cabac);
    }
    else
    {
        const bool b = m_sliceType == B_SLICE;
        if ((!b && mbType >= 3) || (b && mbType == 22))
            subMbPred(r, mbType, cabac, noSmall);
        else if (b && mbType == 0)
            m.direct16 = true;
        else
            mbPred(r, mbType, cabac);
    }
    if (!m.i16)
    {
        int cbp;
        if (cabac)
            cbp = cbpCabac(r);
        else
        {
            const uint32_t code = r.ue();
            const bool chroma = m_chromaArrayType == 1 || m_chromaArrayType == 2;
            if (code >= (chroma ? 48u : 16u))
                throw Stop{false};
            cbp = chroma ? cbpChroma[code][m.intra ? 0 : 1] : cbpNoChroma[code][m.intra ? 0 : 1];
        }
        m.cbpLuma = static_cast<uint8_t>(cbp & 15);
        m.cbpChroma = static_cast<uint8_t>(cbp >> 4);
        if (m.cbpLuma > 0 && m_pps.transform8x8 && !m.inxn && noSmall && (!m.direct16 || m_sps.direct8x8))
            m.transform8x8 = cabac ? m_cabac.decision(r, 399 + ctxTransform()) : r.get(1);
    }
    if (m.cbpLuma > 0 || m.cbpChroma > 0 || m.i16)
    {
        if (cabac)
        {
            int inc = 0;
            if (m_prevMb >= 0)
            {
                const Mb& p = m_mbs[m_prevMb];
                inc = !(p.skip || p.pcm || (!p.i16 && p.cbpLuma == 0 && p.cbpChroma == 0) || !p.qpDeltaNonZero);
            }
            int k = 0;
            for (int ctx = 60 + inc; m_cabac.decision(r, ctx); ctx = 60 + (k == 1 ? 2 : 3))
                if (++k > 128)
                    throw Stop{false};
            m.qpDeltaNonZero = k != 0;
        }
        else
            r.se();
        residual(r, cabac);
    }
}

// 7.3.4
SliceResult Slice::parse(Rbsp& r)
{
    SliceResult res;
    try
    {
        if (!parseHeader(r))
            return res;
        const bool cabac = m_pps.cabac;
        const bool intraSlice = m_sliceType == I_SLICE;
        if (cabac)
        {
            while (!r.aligned()) r.skip(1);
            m_cabac.initContexts(m_sliceType, m_initIdc, m_qp);
            m_cabac.init(r);
        }
        m_curr = firstMb * (1 + m_mbaff);
        bool prevMbSkipped = false, moreData = true;
        for (;;)
        {
            if (!intraSlice && !cabac)
            {
                const uint32_t run = r.ue();
                if (run > static_cast<uint32_t>(m_picSize))
                    throw Stop{false};
                prevMbSkipped = run > 0;
                for (uint32_t i = 0; i < run; ++i)
                {
                    if (m_curr >= m_picSize)
                        throw Stop{false};
                    res.stopMb = m_curr;
                    startMb();
                    markSkipped();
                    res.lastMb = m_prevMb = m_curr++;
                }
                if (run > 0)
                    moreData = r.moreRbspData();
            }
            bool skipped = false;
            if (moreData)
            {
                if (m_curr >= m_picSize)
                    throw Stop{false};
                res.stopMb = m_curr;
                startMb();
                if (!intraSlice && cabac)
                {
                    skipped = m_cabac.decision(r, (m_sliceType == B_SLICE ? 24 : 11) + ctxSkip());
                    if (skipped)
                        markSkipped();
                }
                if (!skipped)
                {
                    if (m_mbaff && (m_curr % 2 == 0 || prevMbSkipped))
                    {
                        const bool field = cabac ? m_cabac.decision(r, 70 + ctxField()) : r.get(1);
                        cur().field = field;
                        if (m_curr % 2 == 1)
                            m_mbs[m_curr - 1].field = field;  // the skipped top macroblock of the pair
                    }
                    macroblockLayer(r, cabac);
                }
                res.lastMb = m_curr;
            }
            if (!cabac)
                moreData = r.moreRbspData();
            else
            {
                if (!intraSlice)
                    prevMbSkipped = skipped;
                moreData = m_mbaff && m_curr % 2 == 0 ? true : !m_cabac.terminate(r);
            }
            m_prevMb = m_curr++;
            if (!moreData)
                break;
        }
        // a whole slice ends at its stop bit: CAVLC right in front of it, CABAC with it as the last bit read
        if (cabac ? r.restZeroAfterByte() : r.pos() == r.lastOne())
            res.state = SliceResult::whole;
        else
            res.state = cabac ? SliceResult::broken : SliceResult::outOfData;
    }
    catch (const Stop& s)
    {
        res.state = s.outOfData ? SliceResult::outOfData : SliceResult::broken;
    }
    return res;
}

int ppsIdOf(const std::vector<uint8_t>& nal)
{
    try
    {
        Rbsp r(nal);
        r.skip(8);
        return static_cast<int>(r.ue());
    }
    catch (const Stop&)
    {
        return -1;
    }
}

// pic_parameter_set_id of a slice (after first_mb_in_slice and slice_type); -2 when the data ends before it
int slicePpsId(const std::vector<uint8_t>& nal)
{
    try
    {
        Rbsp r(nal);
        r.skip(8);
        r.ue();
        r.ue();
        return static_cast<int>(r.ue());
    }
    catch (const Stop& s)
    {
        return s.outOfData ? -2 : -1;
    }
}
}  // namespace

PictureEnd h264PictureEnd(const uint8_t* data, const uint8_t* end, const std::vector<std::vector<uint8_t>>& paramSets)
{
    PictureEnd result;
    std::map<int, std::vector<uint8_t>> spsMap, ppsMap;
    std::map<int, bool> spsFromData, ppsFromData;
    for (const auto& ps : paramSets)
    {
        if (ps.empty())
            continue;
        Sps s;
        int id;
        if ((ps[0] & 0x1f) == 7 && parseSps(ps, s, id))
            spsMap[id] = ps;
        else if ((ps[0] & 0x1f) == 8 && (id = ppsIdOf(ps)) >= 0)
            ppsMap[id] = ps;
    }
    // the NAL units of the data, each from after its start code to the next start code
    std::vector<std::pair<const uint8_t*, const uint8_t*>> nals;
    const uint8_t* start = nullptr;
    for (const uint8_t* p = data; p + 3 <= end; ++p)
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
        {
            if (start)
                nals.emplace_back(start, p);
            start = p + 3;
            p += 2;
        }
    if (start && start < end)
        nals.emplace_back(start, end);
    // the zero bytes in front of a start code (trailing_zero_8bits, the first byte of a four byte start code) belong
    // to no NAL unit, whose last byte is never 0
    for (auto& [b, e] : nals)
        while (e > b && e[-1] == 0) --e;
    // the last slice of a primary coded picture, the parameter sets in force for it, and what follows it
    std::vector<uint8_t> lastNal, lastSps, lastPps;
    int lastSpsId = -1, lastPpsId = -1;
    bool lastSpsOutside = false, lastPpsOutside = false, uncertain = false, endOfStream = false, headerCut = false;
    for (size_t i = 0; i < nals.size(); ++i)
    {
        const auto [b, e] = nals[i];
        if (b >= e)
            continue;
        const int type = b[0] & 0x1f;
        if (type == 7 || type == 8)
        {
            std::vector<uint8_t> nal = unescape(b, e);
            Sps s;
            int id = -1;
            if (type == 7 && parseSps(nal, s, id))
            {
                // a parameter set the last slice took from outside the data, given again after it: the one outside
                // may be this later one, not the one in force for the slice
                if (!lastNal.empty() && id == lastSpsId && lastSpsOutside && nal != lastSps)
                    uncertain = true;
                spsMap[id] = nal;
                spsFromData[id] = true;
            }
            else if (type == 8 && (id = ppsIdOf(nal)) >= 0)
            {
                if (!lastNal.empty() && id == lastPpsId && lastPpsOutside && nal != lastPps)
                    uncertain = true;
                ppsMap[id] = nal;
                ppsFromData[id] = true;
            }
            continue;
        }
        if (type >= 2 && type <= 4)
        {
            if (!lastNal.empty())
                uncertain = true;  // slice data partitioning: not followed
            continue;
        }
        if (type == 10 || type == 11)
        {
            if (!lastNal.empty())
                endOfStream = true;
            continue;
        }
        if (type != 1 && type != 5)
            continue;
        std::vector<uint8_t> nal = unescape(b, e);
        const int ppsId = slicePpsId(nal);
        headerCut = ppsId == -2 && i + 1 == nals.size();  // the data ends inside the first fields of its header
        if (headerCut)
            continue;
        const auto pit = ppsMap.find(ppsId);
        Pps pps;
        Sps sps;
        bool primary = i + 1 == nals.size();  // the last NAL unit of the data: possibly cut inside its header
        if (pit != ppsMap.end() && parsePps(pit->second, spsMap, pps, sps) && pps.sliceGroups == 1)
        {
            std::vector<Mb> none;
            Slice header(sps, pps, type, b[0] >> 5 & 3, none, 0);
            Rbsp r(nal);
            try
            {
                if (header.parseHeader(r))
                    primary = header.redundantPicCnt == 0;
            }
            catch (const Stop&)
            {
            }
        }
        if (!primary)
            continue;
        lastNal = std::move(nal);
        lastPpsId = ppsId;
        lastPps = pit != ppsMap.end() ? pit->second : std::vector<uint8_t>();
        lastPpsOutside = !ppsFromData[ppsId];
        lastSpsId = pit != ppsMap.end() ? pps.spsId : -1;
        lastSps = spsMap.count(lastSpsId) ? spsMap[lastSpsId] : std::vector<uint8_t>();
        lastSpsOutside = !spsFromData[lastSpsId];
        uncertain = endOfStream = false;
    }
    if (headerCut)
    {
        result.state = PictureEnd::State::cut;
        return result;
    }
    if (lastNal.empty() || lastPps.empty() || lastSps.empty() || uncertain)
        return result;
    if (endOfStream)
    {
        result.state = PictureEnd::State::whole;  // an end of sequence or stream follows the picture: it is whole
        return result;
    }
    std::map<int, std::vector<uint8_t>> oneSps{{lastSpsId, lastSps}};
    Pps pps;
    Sps sps;
    if (!parsePps(lastPps, oneSps, pps, sps) || pps.sliceGroups != 1 || sps.separatePlanes)
        return result;
    // arbitrary slice order (Baseline without constraint_set1_flag, Extended): the slices need not end the picture
    const bool anyOrder = (sps.profile == 66 && !sps.constraintSet1) || sps.profile == 88;
    std::vector<Mb> mbs(static_cast<size_t>(sps.widthMbs) * sps.heightMapUnits * (2 - sps.frameMbsOnly));
    Slice slice(sps, pps, lastNal[0] & 0x1f, lastNal[0] >> 5 & 3, mbs, 0);
    Rbsp r(lastNal);
    const SliceResult s = slice.parse(r);
    const int w = std::max(slice.widthMbs(), 1);
    const auto rowOf = [&](const int a) { return slice.mbaff() ? a / 2 / w * 2 + a % 2 : a / w; };
    result.rows = slice.picSizeInMbs() / w;
    if (s.state == SliceResult::outOfData)
    {
        result.state = PictureEnd::State::cut;
        result.row = s.stopMb < 0 ? 0 : rowOf(s.stopMb) + 1;
    }
    else if (s.state == SliceResult::whole && !anyOrder)
    {
        if (s.lastMb + 1 >= slice.picSizeInMbs())
            result.state = PictureEnd::State::whole;
        else
        {
            // the slices of the rest of the picture are not there
            result.state = PictureEnd::State::cut;
            result.betweenSlices = true;
            result.row = rowOf(s.lastMb + 1) + 1;
        }
    }
    return result;
}
