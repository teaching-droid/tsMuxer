#include <algorithm>
#include <map>
#include <memory>

#include "pictureEnd.h"
#include "pictureEndBits.h"

// The syntax followed is that of Rec. ITU-T H.265 (09/2023): 7.3 for the parameter sets, the slice segment header and
// the slice segment data, 9.3 for CABAC, 6.4.1 for the availability of a neighbour, 6.5 for the scans, 8.4.2 and 8.4.3
// for the intra prediction modes, which choose the scan of a block. The profiles followed are Main, Main 10, Main Still
// Picture and those of the format range extensions with any chroma format, as long as the stream uses none of the
// range extension tools that change the syntax of the slice data (sps_range_extension( )); the screen content and
// multi-layer extensions are not followed.

namespace
{
using picture_end::rangeTabLps;
using picture_end::Rbsp;
using picture_end::Stop;
using picture_end::transIdxLps;
using picture_end::unescape;

#include "hevcCabacInit.inc"

struct Ctx
{
    uint8_t state = 0;
    uint8_t mps = 0;
    void init(const int initValue, const int qp)
    {
        const int m = (initValue >> 4) * 5 - 45, n = ((initValue & 15) << 3) - 16;
        const int pre = std::clamp(((m * std::clamp(qp, 0, 51)) >> 4) + n, 1, 126);
        mps = pre > 63;
        state = static_cast<uint8_t>(mps ? pre - 64 : 63 - pre);
    }
};

// The context variables of a slice: one array, so that WPP and dependent slice segments can copy them as a whole
struct Contexts
{
    Ctx saoMerge, saoType, splitCu[3], transquantBypass, cuSkip[3], predMode, partMode[4], prevIntraLuma, intraChroma;
    Ctx rqtRootCbf, mergeFlag, mergeIdx, interPredIdc[5], refIdx[2], mvp, splitTransform[3], cbfLuma[2], cbfChroma[5];
    Ctx mvdGreater0, mvdGreater1, cuQpDeltaAbs[2], transformSkip[2], lastX[18], lastY[18], csbf[4], sig[44],
        greater1[24], greater2[6];
    Ctx chromaQpOffsetFlag, chromaQpOffsetIdx, resScaleAbs[8], resScaleSign[2];

    // 9.3.2.2: initType 0 for I slices, 1 and 2 for P and B by cabac_init_flag
    void init(const int t, const int qp)
    {
        const auto one = [&](Ctx* c, const uint8_t* table, const int count, const int first)
        {
            for (int i = 0; i < count; ++i) c[i].init(table[first + i], qp);
        };
        one(&saoMerge, init9_5, 1, t);
        one(&saoType, init9_6, 1, t);
        one(splitCu, init9_7, 3, 3 * t);
        one(&transquantBypass, init9_8, 1, t);
        one(partMode, init9_11, t == 0 ? 1 : 4, t == 0 ? 0 : 1 + 4 * (t - 1));
        one(&prevIntraLuma, init9_12, 1, t);
        one(&intraChroma, init9_13, 1, t);
        one(splitTransform, init9_20, 3, 3 * t);
        one(cbfLuma, init9_21, 2, 2 * t);
        one(cbfChroma, init9_22, 4, 4 * t);
        one(cbfChroma + 4, init9_22, 1, 12 + t);
        one(cuQpDeltaAbs, init9_24, 2, 2 * t);
        one(transformSkip, init9_25, 1, t);
        one(transformSkip + 1, init9_25, 1, 3 + t);
        one(lastX, init9_26, 18, 18 * t);
        one(lastY, init9_27, 18, 18 * t);
        one(csbf, init9_28, 4, 4 * t);
        one(sig, init9_29, 42, 42 * t);
        one(sig + 42, init9_29, 2, 126 + 2 * t);
        one(greater1, init9_30, 24, 24 * t);
        one(greater2, init9_31, 6, 6 * t);
        one(&chromaQpOffsetFlag, init9_34, 1, t);
        one(&chromaQpOffsetIdx, init9_35, 1, t);
        one(resScaleAbs, init9_36, 8, 8 * t);
        one(resScaleSign, init9_37, 2, 2 * t);
        if (t > 0)
        {
            one(cuSkip, init9_9, 3, 3 * (t - 1));
            one(&predMode, init9_10, 1, t - 1);
            one(&rqtRootCbf, init9_14, 1, t - 1);
            one(&mergeFlag, init9_15, 1, t - 1);
            one(&mergeIdx, init9_16, 1, t - 1);
            one(interPredIdc, init9_17, 5, 5 * (t - 1));
            one(refIdx, init9_18, 2, 2 * (t - 1));
            one(&mvp, init9_19, 1, t - 1);
            one(&mvdGreater0, init9_23, 1, 2 * (t - 1));
            one(&mvdGreater1, init9_23, 1, 2 * (t - 1) + 1);
        }
    }
};

class Cabac
{
   public:
    void init(Rbsp& r)
    {
        m_range = 510;
        m_offset = r.get(9);
        if (m_offset >= 510)
            throw Stop{false};
    }
    int decision(Rbsp& r, Ctx& c)
    {
        const uint32_t lps = rangeTabLps[c.state][(m_range >> 6) & 3];
        m_range -= lps;
        int bin;
        if (m_offset >= m_range)
        {
            bin = !c.mps;
            m_offset -= m_range;
            m_range = lps;
            if (c.state == 0)
                c.mps = !c.mps;
            c.state = transIdxLps[c.state];
        }
        else
        {
            bin = c.mps;
            c.state = static_cast<uint8_t>(std::min(c.state + 1, 62));
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
    uint32_t bypassBits(Rbsp& r, int n)
    {
        uint32_t v = 0;
        while (n-- > 0) v = v << 1 | bypass(r);
        return v;
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
    // k-th order Exp-Golomb in bypass bins (9.3.3.3)
    uint32_t expGolomb(Rbsp& r, int k)
    {
        uint32_t v = 0;
        while (bypass(r))
        {
            v += 1u << k;
            if (++k > 31)
                throw Stop{false};
        }
        return v + bypassBits(r, k);
    }

   private:
    uint32_t m_range = 510, m_offset = 0;
};

// A short-term reference picture set: the delta of each picture and whether the current picture uses it (7.4.8)
struct Rps
{
    std::vector<std::pair<int, bool>> s0, s1;
    [[nodiscard]] int numDelta() const { return static_cast<int>(s0.size() + s1.size()); }
};

// 7.3.7 and 7.4.8; idx: the index of this set, sets: those before it (of the SPS), numSps: the sets of the SPS
void parseRps(Rbsp& r, const int idx, const std::vector<Rps>& sets, const int numSps, Rps& out)
{
    if (idx != 0 && r.get(1))
    {
        int refIdx = idx - 1;
        if (idx == numSps)
            refIdx = idx - (static_cast<int>(r.ue()) + 1);
        if (refIdx < 0 || refIdx >= static_cast<int>(sets.size()))
            throw Stop{false};
        const int sign = static_cast<int>(r.get(1));
        const int deltaRps = (1 - 2 * sign) * (static_cast<int>(r.ue()) + 1);
        const Rps& ref = sets[refIdx];
        const int n = ref.numDelta();
        std::vector<bool> used(n + 1), useDelta(n + 1);
        for (int j = 0; j <= n; ++j)
        {
            used[j] = r.get(1);
            useDelta[j] = used[j] ? true : r.get(1);
        }
        const int neg = static_cast<int>(ref.s0.size()), pos = static_cast<int>(ref.s1.size());
        for (int j = pos - 1; j >= 0; --j)
            if (const int d = ref.s1[j].first + deltaRps; d < 0 && useDelta[neg + j])
                out.s0.emplace_back(d, used[neg + j]);
        if (deltaRps < 0 && useDelta[n])
            out.s0.emplace_back(deltaRps, used[n]);
        for (int j = 0; j < neg; ++j)
            if (const int d = ref.s0[j].first + deltaRps; d < 0 && useDelta[j])
                out.s0.emplace_back(d, used[j]);
        for (int j = neg - 1; j >= 0; --j)
            if (const int d = ref.s0[j].first + deltaRps; d > 0 && useDelta[j])
                out.s1.emplace_back(d, used[j]);
        if (deltaRps > 0 && useDelta[n])
            out.s1.emplace_back(deltaRps, used[n]);
        for (int j = 0; j < pos; ++j)
            if (const int d = ref.s1[j].first + deltaRps; d > 0 && useDelta[neg + j])
                out.s1.emplace_back(d, used[neg + j]);
    }
    else
    {
        const uint32_t neg = r.ue(), pos = r.ue();
        if (neg > 16 || pos > 16)
            throw Stop{false};
        int poc = 0;
        for (uint32_t i = 0; i < neg; ++i)
        {
            poc -= static_cast<int>(r.ue()) + 1;
            out.s0.emplace_back(poc, r.get(1));
        }
        poc = 0;
        for (uint32_t i = 0; i < pos; ++i)
        {
            poc += static_cast<int>(r.ue()) + 1;
            out.s1.emplace_back(poc, r.get(1));
        }
    }
}

int ceilLog2(const int v)
{
    int n = 0;
    while ((1 << n) < v) ++n;
    return n;
}

void skipScalingListData(Rbsp& r)
{
    for (int sizeId = 0; sizeId < 4; ++sizeId)
        for (int matrixId = 0; matrixId < 6; matrixId += sizeId == 3 ? 3 : 1)
            if (!r.get(1))
                r.ue();
            else
            {
                const int coefNum = std::min(64, 1 << (4 + (sizeId << 1)));
                if (sizeId > 1)
                    r.se();
                for (int i = 0; i < coefNum; ++i) r.se();
            }
}

// E.2.2, E.2.3
void skipHrd(Rbsp& r, const bool commonInf, const int maxSubLayers)
{
    bool nal = false, vcl = false, subPic = false;
    if (commonInf)
    {
        nal = r.get(1);
        vcl = r.get(1);
        if (nal || vcl)
        {
            subPic = r.get(1);
            if (subPic)
                r.skip(8 + 5 + 1 + 5);
            r.skip(4 + 4);
            if (subPic)
                r.skip(4);
            r.skip(5 + 5 + 5);
        }
    }
    for (int i = 0; i <= maxSubLayers; ++i)
    {
        bool withinCvs = true, lowDelay = false;
        if (!r.get(1))  // fixed_pic_rate_general_flag
            withinCvs = r.get(1);
        if (withinCvs)
            r.ue();
        else
            lowDelay = r.get(1);
        uint32_t cpbCnt = 0;
        if (!lowDelay)
            cpbCnt = r.ue();
        if (cpbCnt > 31)
            throw Stop{false};
        for (int k = 0; k < nal + vcl; ++k)
            for (uint32_t j = 0; j <= cpbCnt; ++j)
            {
                r.ue();
                r.ue();
                if (subPic)
                {
                    r.ue();
                    r.ue();
                }
                r.skip(1);
            }
    }
}

// E.2.1
void skipVui(Rbsp& r, const int maxSubLayers)
{
    if (r.get(1) && r.get(8) == 255)
        r.skip(32);
    if (r.get(1))
        r.skip(1);
    if (r.get(1))
    {
        r.skip(4);
        if (r.get(1))
            r.skip(24);
    }
    if (r.get(1))
    {
        r.ue();
        r.ue();
    }
    r.skip(3);
    if (r.get(1))
        for (int i = 0; i < 4; ++i) r.ue();
    if (r.get(1))
    {
        r.skip(64);
        if (r.get(1))
            r.ue();
        if (r.get(1))
            skipHrd(r, true, maxSubLayers);
    }
    if (r.get(1))
    {
        r.skip(3);
        for (int i = 0; i < 5; ++i) r.ue();
    }
}

struct Sps
{
    int chromaFormat = 1;
    int width = 0, height = 0;
    int bitDepthY = 8, bitDepthC = 8;
    int log2MaxPocLsb = 4;
    int minCbLog2 = 3, ctbLog2 = 4, minTbLog2 = 2, maxTbLog2 = 5, maxTrDepthInter = 0, maxTrDepthIntra = 0;
    bool amp = false, sao = false, pcm = false;
    int pcmBitDepthY = 8, pcmBitDepthC = 8, pcmMinLog2 = 3, pcmMaxLog2 = 3;
    std::vector<Rps> rps;
    bool longTerm = false;
    std::vector<bool> ltUsedSps;
    bool temporalMvp = false;
};

bool parseSps(const std::vector<uint8_t>& nal, Sps& s, int& id)
{
    try
    {
        Rbsp r(nal);
        r.skip(16 + 4);
        const int maxSubLayers = static_cast<int>(r.get(3));
        r.skip(1);
        // profile_tier_level( 1, sps_max_sub_layers_minus1 )
        r.skip(3);
        const int profile = static_cast<int>(r.get(5));
        const uint32_t compatible = r.get(32);
        r.skip(4 + 43 + 1 + 8);
        bool subProfile[8] = {}, subLevel[8] = {};
        for (int i = 0; i < maxSubLayers; ++i)
        {
            subProfile[i] = r.get(1);
            subLevel[i] = r.get(1);
        }
        if (maxSubLayers > 0)
            for (int i = maxSubLayers; i < 8; ++i) r.skip(2);
        for (int i = 0; i < maxSubLayers; ++i)
        {
            if (subProfile[i])
                r.skip(88);
            if (subLevel[i])
                r.skip(8);
        }
        // Main, Main 10 and Main Still Picture, or compatible with them; the format range extensions profiles when they
        // use none of the tools of sps_range_extension( ) that change the syntax (below)
        const bool mainFamily = (profile >= 1 && profile <= 3) || (compatible & 0x70000000u);
        const bool rangeExtensions = profile == 4 || (compatible & 0x08000000u);
        id = static_cast<int>(r.ue());
        s.chromaFormat = static_cast<int>(r.ue());
        if (s.chromaFormat == 3 && r.get(1))
            return false;  // separate_colour_plane_flag
        s.width = static_cast<int>(r.ue());
        s.height = static_cast<int>(r.ue());
        if (r.get(1))
            for (int i = 0; i < 4; ++i) r.ue();
        s.bitDepthY = 8 + static_cast<int>(r.ue());
        s.bitDepthC = 8 + static_cast<int>(r.ue());
        s.log2MaxPocLsb = 4 + static_cast<int>(r.ue());
        const bool orderingAll = r.get(1);
        for (int i = orderingAll ? 0 : maxSubLayers; i <= maxSubLayers; ++i)
            for (int k = 0; k < 3; ++k) r.ue();
        s.minCbLog2 = 3 + static_cast<int>(r.ue());
        s.ctbLog2 = s.minCbLog2 + static_cast<int>(r.ue());
        s.minTbLog2 = 2 + static_cast<int>(r.ue());
        s.maxTbLog2 = s.minTbLog2 + static_cast<int>(r.ue());
        s.maxTrDepthInter = static_cast<int>(r.ue());
        s.maxTrDepthIntra = static_cast<int>(r.ue());
        if (r.get(1) && r.get(1))
            skipScalingListData(r);
        s.amp = r.get(1);
        s.sao = r.get(1);
        s.pcm = r.get(1);
        if (s.pcm)
        {
            s.pcmBitDepthY = 1 + static_cast<int>(r.get(4));
            s.pcmBitDepthC = 1 + static_cast<int>(r.get(4));
            s.pcmMinLog2 = 3 + static_cast<int>(r.ue());
            s.pcmMaxLog2 = s.pcmMinLog2 + static_cast<int>(r.ue());
        }
        if (s.pcm)
            r.skip(1);
        const uint32_t numRps = r.ue();
        if (numRps > 64)
            return false;
        for (uint32_t i = 0; i < numRps; ++i)
        {
            Rps set;
            parseRps(r, static_cast<int>(i), s.rps, static_cast<int>(numRps), set);
            s.rps.push_back(std::move(set));
        }
        s.longTerm = r.get(1);
        if (s.longTerm)
        {
            const uint32_t n = r.ue();
            if (n > 32)
                return false;
            for (uint32_t i = 0; i < n; ++i)
            {
                r.skip(s.log2MaxPocLsb);
                s.ltUsedSps.push_back(r.get(1));
            }
        }
        s.temporalMvp = r.get(1);
        r.skip(1);  // strong_intra_smoothing_enabled_flag
        if (r.get(1))
            skipVui(r, maxSubLayers);
        if (r.get(1))  // sps_extension_present_flag
        {
            const bool range = r.get(1);
            if (r.get(1) || r.get(1) || r.get(1))
                return false;  // the multilayer, 3D and screen content extensions
            r.skip(4);
            if (range)
            {
                const uint32_t flags = r.get(9);
                // transform_skip_rotation, transform_skip_context, implicit_rdpcm, explicit_rdpcm,
                // extended_precision_processing, (intra_smoothing_disabled), (high_precision_offsets),
                // persistent_rice_adaptation, cabac_bypass_alignment: those not in brackets change the syntax
                if (flags & 0x1F3)
                    return false;
            }
        }
        return (mainFamily || rangeExtensions) && s.width > 0 && s.height > 0 && s.ctbLog2 >= 4 && s.ctbLog2 <= 6 &&
               s.maxTbLog2 <= 5 && s.bitDepthY <= 16 && s.log2MaxPocLsb <= 16;
    }
    catch (const Stop&)
    {
        return false;
    }
}

struct Pps
{
    int spsId = 0;
    bool dependentSlices = false, outputFlag = false;
    int extraSliceHeaderBits = 0;
    bool signHiding = false, cabacInitPresent = false;
    int numRefIdx[2] = {1, 1};
    int initQp = 26;
    bool transformSkip = false, cuQpDelta = false;
    int diffCuQpDeltaDepth = 0;
    bool sliceChromaQpOffsets = false, weightedPred = false, weightedBipred = false, transquantBypass = false;
    bool tiles = false, wpp = false;
    std::vector<int> colWidth, rowHeight;  // in CTBs; empty: uniform with numCols, numRows
    int numCols = 1, numRows = 1;
    bool uniform = true;
    bool loopFilterAcrossSlices = false, deblockingOverride = false, deblockingDisabled = false;
    bool listsModification = false, sliceHeaderExtension = false;
    // pps_range_extension( )
    int log2MaxTransformSkipSize = 2;
    bool crossComponentPrediction = false, chromaQpOffsetList = false;
    int diffCuChromaQpOffsetDepth = 0, chromaQpOffsetListLen = 0;
};

bool parsePps(const std::vector<uint8_t>& nal, Pps& p, int& id)
{
    try
    {
        Rbsp r(nal);
        r.skip(16);
        id = static_cast<int>(r.ue());
        p.spsId = static_cast<int>(r.ue());
        p.dependentSlices = r.get(1);
        p.outputFlag = r.get(1);
        p.extraSliceHeaderBits = static_cast<int>(r.get(3));
        p.signHiding = r.get(1);
        p.cabacInitPresent = r.get(1);
        p.numRefIdx[0] = 1 + static_cast<int>(r.ue());
        p.numRefIdx[1] = 1 + static_cast<int>(r.ue());
        p.initQp = 26 + r.se();
        r.skip(1);  // constrained_intra_pred_flag
        p.transformSkip = r.get(1);
        p.cuQpDelta = r.get(1);
        if (p.cuQpDelta)
            p.diffCuQpDeltaDepth = static_cast<int>(r.ue());
        r.se();
        r.se();
        p.sliceChromaQpOffsets = r.get(1);
        p.weightedPred = r.get(1);
        p.weightedBipred = r.get(1);
        p.transquantBypass = r.get(1);
        p.tiles = r.get(1);
        p.wpp = r.get(1);
        if (p.tiles)
        {
            p.numCols = 1 + static_cast<int>(r.ue());
            p.numRows = 1 + static_cast<int>(r.ue());
            if (p.numCols > 64 || p.numRows > 64)
                return false;
            p.uniform = r.get(1);
            if (!p.uniform)
            {
                for (int i = 0; i < p.numCols - 1; ++i) p.colWidth.push_back(1 + static_cast<int>(r.ue()));
                for (int i = 0; i < p.numRows - 1; ++i) p.rowHeight.push_back(1 + static_cast<int>(r.ue()));
            }
            r.skip(1);
        }
        p.loopFilterAcrossSlices = r.get(1);
        if (r.get(1))
        {
            p.deblockingOverride = r.get(1);
            p.deblockingDisabled = r.get(1);
            if (!p.deblockingDisabled)
            {
                r.se();
                r.se();
            }
        }
        if (r.get(1))
            skipScalingListData(r);
        p.listsModification = r.get(1);
        r.ue();
        p.sliceHeaderExtension = r.get(1);
        if (r.get(1))  // pps_extension_present_flag
        {
            const bool range = r.get(1);
            if (r.get(1) || r.get(1) || r.get(1))
                return false;  // the multilayer, 3D and screen content extensions
            r.skip(4);
            if (range)
            {
                if (p.transformSkip)
                    p.log2MaxTransformSkipSize = 2 + static_cast<int>(r.ue());
                p.crossComponentPrediction = r.get(1);
                p.chromaQpOffsetList = r.get(1);
                if (p.chromaQpOffsetList)
                {
                    p.diffCuChromaQpOffsetDepth = static_cast<int>(r.ue());
                    p.chromaQpOffsetListLen = 1 + static_cast<int>(r.ue());
                    if (p.chromaQpOffsetListLen > 6)
                        return false;
                    for (int i = 0; i < p.chromaQpOffsetListLen; ++i)
                    {
                        r.se();
                        r.se();
                    }
                }
                r.ue();
                r.ue();
            }
        }
        return p.numRefIdx[0] <= 16 && p.numRefIdx[1] <= 16 && p.log2MaxTransformSkipSize <= 5;
    }
    catch (const Stop&)
    {
        return false;
    }
}

constexpr int B_SLICE = 0, P_SLICE = 1, I_SLICE = 2;
constexpr int INTRA_DC = 1;

// The picture: the tiles, and per 4x4 luma block what the neighbours of later blocks need
struct Picture
{
    const Sps* sps = nullptr;
    int ctbSize = 16, wCtbs = 0, hCtbs = 0, sizeCtbs = 0;
    std::vector<int> rsToTs, tsToRs, tileId;  // tileId by TS address
    std::vector<int> ctbSlice;                // by RS address: SliceAddrRs of the slice that holds it, -1 not read
    int w4 = 0, h4 = 0;
    std::vector<uint8_t> depth, skip, intra, mode;  // per 4x4: CtDepth, cu_skip_flag, intra (pcm: 2), IntraPredModeY

    void setup(const Sps& s, const Pps& p)
    {
        sps = &s;
        ctbSize = 1 << s.ctbLog2;
        wCtbs = (s.width + ctbSize - 1) / ctbSize;
        hCtbs = (s.height + ctbSize - 1) / ctbSize;
        sizeCtbs = wCtbs * hCtbs;
        // 6.5.1
        std::vector<int> colBd(1, 0), rowBd(1, 0);
        for (int i = 0; i < p.numCols; ++i)
            colBd.push_back(colBd.back() + (p.uniform ? ((i + 1) * wCtbs) / p.numCols - (i * wCtbs) / p.numCols
                                            : i < p.numCols - 1 ? p.colWidth[i]
                                                                : wCtbs - colBd.back()));
        for (int j = 0; j < p.numRows; ++j)
            rowBd.push_back(rowBd.back() + (p.uniform ? ((j + 1) * hCtbs) / p.numRows - (j * hCtbs) / p.numRows
                                            : j < p.numRows - 1 ? p.rowHeight[j]
                                                                : hCtbs - rowBd.back()));
        rsToTs.assign(sizeCtbs, 0);
        tsToRs.assign(sizeCtbs, 0);
        tileId.assign(sizeCtbs, 0);
        for (int rs = 0; rs < sizeCtbs; ++rs)
        {
            const int tbX = rs % wCtbs, tbY = rs / wCtbs;
            int tileX = 0, tileY = 0;
            for (int i = 0; i < p.numCols; ++i)
                if (tbX >= colBd[i])
                    tileX = i;
            for (int j = 0; j < p.numRows; ++j)
                if (tbY >= rowBd[j])
                    tileY = j;
            int v = 0;
            for (int i = 0; i < tileX; ++i) v += (rowBd[tileY + 1] - rowBd[tileY]) * (colBd[i + 1] - colBd[i]);
            for (int j = 0; j < tileY; ++j) v += wCtbs * (rowBd[j + 1] - rowBd[j]);
            v += (tbY - rowBd[tileY]) * (colBd[tileX + 1] - colBd[tileX]) + tbX - colBd[tileX];
            rsToTs[rs] = v;
            tsToRs[v] = rs;
        }
        for (int j = 0, id = 0; j < p.numRows; ++j)
            for (int i = 0; i < p.numCols; ++i, ++id)
                for (int y = rowBd[j]; y < rowBd[j + 1]; ++y)
                    for (int x = colBd[i]; x < colBd[i + 1]; ++x) tileId[rsToTs[y * wCtbs + x]] = id;
        ctbSlice.assign(sizeCtbs, -1);
        w4 = (s.width + 3) / 4;
        h4 = (s.height + 3) / 4;
        depth.assign(static_cast<size_t>(w4) * h4, 0);
        skip.assign(depth.size(), 0);
        intra.assign(depth.size(), 0);
        mode.assign(depth.size(), INTRA_DC);
    }
    [[nodiscard]] int ctbOf(const int x, const int y) const
    {
        return (y >> sps->ctbLog2) * wCtbs + (x >> sps->ctbLog2);
    }
    // 6.4.1, for a neighbour left of or above the current block (or the above-right CTB for WPP): earlier in the
    // decoding order wherever it lies inside the picture, so it is available when read, in the same slice and tile
    [[nodiscard]] bool available(const int xCurr, const int yCurr, const int xN, const int yN) const
    {
        if (xN < 0 || yN < 0 || xN >= sps->width || yN >= sps->height)
            return false;
        const int c = ctbOf(xCurr, yCurr), n = ctbOf(xN, yN);
        return ctbSlice[n] >= 0 && ctbSlice[n] == ctbSlice[c] && tileId[rsToTs[n]] == tileId[rsToTs[c]];
    }
    [[nodiscard]] size_t at(const int x, const int y) const { return static_cast<size_t>(y >> 2) * w4 + (x >> 2); }
    void fill(std::vector<uint8_t>& map, const int x0, const int y0, const int size, const uint8_t v)
    {
        for (int y = y0; y < std::min(y0 + size, sps->height); y += 4)
            for (int x = x0; x < std::min(x0 + size, sps->width); x += 4) map[at(x, y)] = v;
    }
};

// 6.5.3 to 6.5.5: the scans of a block of 1, 2, 4 or 8 sub-blocks (index log2 size), diagonal, horizontal, vertical
struct Scans
{
    uint8_t pos[4][3][64][2];
    Scans()
    {
        for (int l = 0; l < 4; ++l)
        {
            const int n = 1 << l;
            int i = 0;
            for (int x = 0, y = 0; i < n * n;)
            {
                while (y >= 0)
                {
                    if (x < n && y < n)
                    {
                        pos[l][0][i][0] = static_cast<uint8_t>(x);
                        pos[l][0][i][1] = static_cast<uint8_t>(y);
                        ++i;
                    }
                    --y;
                    ++x;
                }
                y = x;
                x = 0;
            }
            for (int k = 0; k < n * n; ++k)
            {
                pos[l][1][k][0] = static_cast<uint8_t>(k % n);
                pos[l][1][k][1] = static_cast<uint8_t>(k / n);
                pos[l][2][k][0] = static_cast<uint8_t>(k / n);
                pos[l][2][k][1] = static_cast<uint8_t>(k % n);
            }
        }
    }
};
const Scans scans;

const uint8_t ctxIdxMap[16] = {0, 1, 4, 5, 2, 3, 4, 5, 6, 6, 8, 8, 7, 7, 8, 8};

struct SegmentResult
{
    enum
    {
        whole,
        outOfData,
        broken
    } state = broken;
    int lastTs = -1;   // the TS address of the last CTB read
    int stopCtb = -1;  // outOfData: the RS address of the CTB being read when the data ended
};

class Segment
{
   public:
    Segment(const Sps& sps, const Pps& pps, Picture& picture, const int nalType)
        : s(sps), p(pps), pic(picture), m_nalType(nalType)
    {
    }
    // the header; prevIndependent: the header values of the independent slice segment a dependent one belongs to
    bool parseHeader(Rbsp& r, const Segment* prevIndependent);
    SegmentResult parseData(Rbsp& r, Contexts& wpp, Contexts& ds, bool& wppStored);
    bool dependent = false;
    int address = 0, sliceAddrRs = 0;

   private:
    const Sps& s;
    const Pps& p;
    Picture& pic;
    int m_nalType;
    int sliceType = I_SLICE;
    bool saoLuma = false, saoChroma = false, cabacInitFlag = false, mvdL1Zero = false;
    int numRefIdx[2] = {1, 1};
    int maxNumMergeCand = 5;
    int qp = 26;
    Cabac cabac;
    Contexts ctx;
    // the coding unit being read
    bool transquantBypass = false, intraCu = false;
    int partMode = 0, ctDepth = 0;
    int chromaMode[4] = {};  // IntraPredModeC of each part (one for 4:2:0 and 4:2:2, one a part for 4:4:4)
    bool chromaDm[4] = {};   // intra_chroma_pred_mode of the part is 4
    bool isCuQpDeltaCoded = false;
    bool chromaQpOffsetEnabled = false, isCuChromaQpOffsetCoded = false;
    int log2MinCuChromaQpOffsetSize = 0;
    int x0Cu = 0, y0Cu = 0, sizeCu = 0;
    int log2MinCuQpDeltaSize = 0;

    int initType() const
    {
        if (sliceType == I_SLICE)
            return 0;
        return sliceType == P_SLICE ? (cabacInitFlag ? 2 : 1) : (cabacInitFlag ? 1 : 2);
    }
    void sao(Rbsp& r, int rx, int ry, int ctbRs);
    void codingQuadtree(Rbsp& r, int x0, int y0, int log2CbSize, int depth);
    void codingUnit(Rbsp& r, int x0, int y0, int log2CbSize);
    void predictionUnit(Rbsp& r, int w, int h, bool& mergeFlag);
    void mvdCoding(Rbsp& r);
    // cbf_cb and cbf_cr of a transform block: two of each for 4:2:2, the upper and the lower chroma block
    struct ChromaCbf
    {
        bool cb[2] = {};
        bool cr[2] = {};
        [[nodiscard]] bool any() const { return cb[0] || cb[1] || cr[0] || cr[1]; }
    };
    void transformTree(Rbsp& r, int x0, int y0, int xBase, int yBase, int log2Size, int depth, int blkIdx, int maxDepth,
                       bool intraSplit, const ChromaCbf& parent);
    void transformUnit(Rbsp& r, int x0, int y0, int xBase, int yBase, int log2Size, int blkIdx, bool cbfLuma,
                       const ChromaCbf& own, const ChromaCbf& parent);
    [[nodiscard]] int partOf(int x, int y) const;
    void residualCoding(Rbsp& r, int x0, int y0, int log2Size, int cIdx);
};

bool Segment::parseHeader(Rbsp& r, const Segment* prevIndependent)
{
    r.skip(16);
    const bool first = r.get(1);
    if (m_nalType >= 16 && m_nalType <= 23)
        r.skip(1);
    r.ue();  // slice_pic_parameter_set_id
    if (!first)
    {
        if (p.dependentSlices)
            dependent = r.get(1);
        address = static_cast<int>(r.get(ceilLog2(pic.sizeCtbs)));
        if (address >= pic.sizeCtbs)
            return false;
    }
    if (dependent)
    {
        if (!prevIndependent)
            return false;
        sliceType = prevIndependent->sliceType;
        saoLuma = prevIndependent->saoLuma;
        saoChroma = prevIndependent->saoChroma;
        cabacInitFlag = prevIndependent->cabacInitFlag;
        mvdL1Zero = prevIndependent->mvdL1Zero;
        numRefIdx[0] = prevIndependent->numRefIdx[0];
        numRefIdx[1] = prevIndependent->numRefIdx[1];
        maxNumMergeCand = prevIndependent->maxNumMergeCand;
        qp = prevIndependent->qp;
        sliceAddrRs = prevIndependent->sliceAddrRs;
        chromaQpOffsetEnabled = prevIndependent->chromaQpOffsetEnabled;
    }
    else
    {
        sliceAddrRs = address;
        r.skip(p.extraSliceHeaderBits);
        sliceType = static_cast<int>(r.ue());
        if (sliceType > 2)
            return false;
        if (p.outputFlag)
            r.skip(1);
        int numPicTotalCurr = 0;
        bool temporalMvp = false;
        if (m_nalType != 19 && m_nalType != 20)
        {
            r.skip(s.log2MaxPocLsb);
            Rps rps;
            const int numSets = static_cast<int>(s.rps.size());
            if (!r.get(1))
                parseRps(r, numSets, s.rps, numSets, rps);
            else
            {
                const int idx = numSets > 1 ? static_cast<int>(r.get(ceilLog2(numSets))) : 0;
                if (idx >= numSets)
                    return false;
                rps = s.rps[idx];
            }
            for (const auto& [d, used] : rps.s0) numPicTotalCurr += used;
            for (const auto& [d, used] : rps.s1) numPicTotalCurr += used;
            if (s.longTerm)
            {
                const int numLtSps = static_cast<int>(s.ltUsedSps.size());
                const uint32_t sps = numLtSps > 0 ? r.ue() : 0;
                const uint32_t pics = r.ue();
                if (sps > static_cast<uint32_t>(numLtSps) || pics > 32)
                    return false;
                for (uint32_t i = 0; i < sps + pics; ++i)
                {
                    if (i < sps)
                    {
                        const int k = numLtSps > 1 ? static_cast<int>(r.get(ceilLog2(numLtSps))) : 0;
                        if (k >= numLtSps)
                            return false;
                        numPicTotalCurr += s.ltUsedSps[k];
                    }
                    else
                    {
                        r.skip(s.log2MaxPocLsb);
                        numPicTotalCurr += static_cast<int>(r.get(1));
                    }
                    if (r.get(1))
                        r.ue();
                }
            }
            if (s.temporalMvp)
                temporalMvp = r.get(1);
        }
        if (s.sao)
        {
            saoLuma = r.get(1);
            if (s.chromaFormat != 0)
                saoChroma = r.get(1);
        }
        if (sliceType != I_SLICE)
        {
            numRefIdx[0] = p.numRefIdx[0];
            numRefIdx[1] = p.numRefIdx[1];
            if (r.get(1))
            {
                numRefIdx[0] = 1 + static_cast<int>(r.ue());
                if (sliceType == B_SLICE)
                    numRefIdx[1] = 1 + static_cast<int>(r.ue());
            }
            if (numRefIdx[0] > 16 || numRefIdx[1] > 16)
                return false;
            if (p.listsModification && numPicTotalCurr > 1)
            {
                const int bits = ceilLog2(numPicTotalCurr);
                for (int list = 0; list < (sliceType == B_SLICE ? 2 : 1); ++list)
                    if (r.get(1))
                        r.skip(static_cast<int64_t>(bits) * numRefIdx[list]);
            }
            if (sliceType == B_SLICE)
                mvdL1Zero = r.get(1);
            if (p.cabacInitPresent)
                cabacInitFlag = r.get(1);
            if (temporalMvp)
            {
                bool fromL0 = true;
                if (sliceType == B_SLICE)
                    fromL0 = r.get(1);
                if ((fromL0 && numRefIdx[0] > 1) || (!fromL0 && numRefIdx[1] > 1))
                    r.ue();
            }
            if ((p.weightedPred && sliceType == P_SLICE) || (p.weightedBipred && sliceType == B_SLICE))
            {
                r.ue();
                if (s.chromaFormat != 0)
                    r.se();
                for (int list = 0; list < (sliceType == B_SLICE ? 2 : 1); ++list)
                {
                    std::vector<bool> luma(numRefIdx[list]), chroma(numRefIdx[list]);
                    for (int i = 0; i < numRefIdx[list]; ++i) luma[i] = r.get(1);
                    if (s.chromaFormat != 0)
                        for (int i = 0; i < numRefIdx[list]; ++i) chroma[i] = r.get(1);
                    for (int i = 0; i < numRefIdx[list]; ++i)
                    {
                        if (luma[i])
                        {
                            r.se();
                            r.se();
                        }
                        if (chroma[i])
                            for (int j = 0; j < 4; ++j) r.se();
                    }
                }
            }
            maxNumMergeCand = 5 - static_cast<int>(r.ue());
            if (maxNumMergeCand < 1)
                return false;
        }
        qp = p.initQp + r.se();
        if (p.sliceChromaQpOffsets)
        {
            r.se();
            r.se();
        }
        if (p.chromaQpOffsetList)
            chromaQpOffsetEnabled = r.get(1);
        bool deblockingDisabled = p.deblockingDisabled;
        if (p.deblockingOverride && r.get(1))
        {
            deblockingDisabled = r.get(1);
            if (!deblockingDisabled)
            {
                r.se();
                r.se();
            }
        }
        if (p.loopFilterAcrossSlices && (saoLuma || saoChroma || !deblockingDisabled))
            r.skip(1);
    }
    if (p.tiles || p.wpp)
    {
        const uint32_t n = r.ue();
        if (n > static_cast<uint32_t>(pic.sizeCtbs))
            return false;
        if (n > 0)
        {
            const int bits = 1 + static_cast<int>(r.ue());
            if (bits > 32)
                return false;
            r.skip(static_cast<int64_t>(bits) * n);
        }
    }
    if (p.sliceHeaderExtension)
        r.skip(8 * static_cast<int64_t>(r.ue()));
    // byte_alignment( )
    if (!r.get(1))
        return false;
    while (!r.aligned())
        if (r.get(1))
            return false;
    log2MinCuQpDeltaSize = s.ctbLog2 - p.diffCuQpDeltaDepth;
    log2MinCuChromaQpOffsetSize = s.ctbLog2 - p.diffCuChromaQpOffsetDepth;
    return true;
}

// 7.3.8.3
void Segment::sao(Rbsp& r, const int rx, const int ry, const int ctbRs)
{
    const int ctbTs = pic.rsToTs[ctbRs];
    bool merge = false;
    if (rx > 0 && ctbRs > sliceAddrRs && pic.tileId[ctbTs] == pic.tileId[pic.rsToTs[ctbRs - 1]])
        merge = cabac.decision(r, ctx.saoMerge);
    if (ry > 0 && !merge)
    {
        const int up = ctbRs - pic.wCtbs;
        if (up >= sliceAddrRs && pic.tileId[ctbTs] == pic.tileId[pic.rsToTs[up]])
            merge = cabac.decision(r, ctx.saoMerge);
    }
    if (merge)
        return;
    int type[3] = {};
    for (int c = 0; c < (s.chromaFormat != 0 ? 3 : 1); ++c)
    {
        if (!((saoLuma && c == 0) || (saoChroma && c > 0)))
            continue;
        if (c < 2)
            type[c] = !cabac.decision(r, ctx.saoType) ? 0 : cabac.bypass(r) ? 2 : 1;
        else
            type[2] = type[1];
        if (type[c] == 0)
            continue;
        const int bitDepth = c == 0 ? s.bitDepthY : s.bitDepthC;
        const int cMax = (1 << (std::min(bitDepth, 10) - 5)) - 1;
        int offsets[4];
        for (int& o : offsets)
        {
            o = 0;
            while (o < cMax && cabac.bypass(r)) ++o;
        }
        if (type[c] == 1)
        {
            for (const int o : offsets)
                if (o != 0)
                    cabac.bypass(r);
            cabac.bypassBits(r, 5);
        }
        else if (c < 2)
            cabac.bypassBits(r, 2);
    }
}

// 7.3.8.4
void Segment::codingQuadtree(Rbsp& r, const int x0, const int y0, const int log2CbSize, const int depth)
{
    const int size = 1 << log2CbSize;
    bool split;
    if (x0 + size <= s.width && y0 + size <= s.height && log2CbSize > s.minCbLog2)
    {
        int inc = 0;
        if (pic.available(x0, y0, x0 - 1, y0) && pic.depth[pic.at(x0 - 1, y0)] > depth)
            ++inc;
        if (pic.available(x0, y0, x0, y0 - 1) && pic.depth[pic.at(x0, y0 - 1)] > depth)
            ++inc;
        split = cabac.decision(r, ctx.splitCu[inc]);
    }
    else
        split = log2CbSize > s.minCbLog2;
    if (p.cuQpDelta && log2CbSize >= log2MinCuQpDeltaSize)
        isCuQpDeltaCoded = false;
    if (chromaQpOffsetEnabled && log2CbSize >= log2MinCuChromaQpOffsetSize)
        isCuChromaQpOffsetCoded = false;
    if (split)
    {
        const int half = size / 2;
        for (int k = 0; k < 4; ++k)
        {
            const int x = x0 + (k % 2) * half, y = y0 + (k / 2) * half;
            if (x < s.width && y < s.height)
                codingQuadtree(r, x, y, log2CbSize - 1, depth + 1);
        }
        return;
    }
    ctDepth = depth;
    codingUnit(r, x0, y0, log2CbSize);
}

// 7.3.8.5
void Segment::codingUnit(Rbsp& r, const int x0, const int y0, const int log2CbSize)
{
    const int size = 1 << log2CbSize;
    transquantBypass = p.transquantBypass && cabac.decision(r, ctx.transquantBypass);
    bool skip = false;
    if (sliceType != I_SLICE)
    {
        int inc = 0;
        if (pic.available(x0, y0, x0 - 1, y0) && pic.skip[pic.at(x0 - 1, y0)])
            ++inc;
        if (pic.available(x0, y0, x0, y0 - 1) && pic.skip[pic.at(x0, y0 - 1)])
            ++inc;
        skip = cabac.decision(r, ctx.cuSkip[inc]);
    }
    pic.fill(pic.depth, x0, y0, size, static_cast<uint8_t>(ctDepth));
    pic.fill(pic.skip, x0, y0, size, skip);
    pic.fill(pic.intra, x0, y0, size, 0);
    pic.fill(pic.mode, x0, y0, size, INTRA_DC);
    intraCu = false;
    x0Cu = x0;
    y0Cu = y0;
    sizeCu = size;
    if (skip)
    {
        if (maxNumMergeCand > 1 && cabac.decision(r, ctx.mergeIdx))
            for (int i = 1; i < maxNumMergeCand - 1 && cabac.bypass(r); ++i)
            {
            }
        return;
    }
    intraCu = sliceType == I_SLICE || cabac.decision(r, ctx.predMode);
    partMode = 0;  // 0 2Nx2N, 1 2NxN, 2 Nx2N, 3 NxN, 4 2NxnU, 5 2NxnD, 6 nLx2N, 7 nRx2N
    if (!intraCu || log2CbSize == s.minCbLog2)
    {
        if (intraCu)
            partMode = cabac.decision(r, ctx.partMode[0]) ? 0 : 3;
        else if (!cabac.decision(r, ctx.partMode[0]))
        {
            const bool horizontal = cabac.decision(r, ctx.partMode[1]);
            if (log2CbSize > s.minCbLog2)
            {
                if (!s.amp)
                    partMode = horizontal ? 1 : 2;
                else if (cabac.decision(r, ctx.partMode[3]))
                    partMode = horizontal ? 1 : 2;
                else
                    partMode = (horizontal ? 4 : 6) + cabac.bypass(r);
            }
            else if (log2CbSize == 3)
                partMode = horizontal ? 1 : 2;
            else if (horizontal)
                partMode = 1;
            else
                partMode = cabac.decision(r, ctx.partMode[2]) ? 2 : 3;
        }
    }
    bool pcm = false, rqtRootCbf = true;
    if (intraCu)
    {
        pic.fill(pic.intra, x0, y0, size, 1);
        if (partMode == 0 && s.pcm && log2CbSize >= s.pcmMinLog2 && log2CbSize <= s.pcmMaxLog2)
            pcm = cabac.terminate(r);
        if (pcm)
        {
            pic.fill(pic.intra, x0, y0, size, 2);
            while (!r.aligned()) r.skip(1);
            // two chroma blocks of ( nCbS / SubWidthC ) x ( nCbS / SubHeightC )
            const int64_t chroma = s.chromaFormat == 0   ? 0
                                   : s.chromaFormat == 1 ? 2 * static_cast<int64_t>(size / 2) * (size / 2)
                                   : s.chromaFormat == 2 ? 2 * static_cast<int64_t>(size / 2) * size
                                                         : 2 * static_cast<int64_t>(size) * size;
            r.skip(static_cast<int64_t>(size) * size * s.pcmBitDepthY + chroma * s.pcmBitDepthC);
            cabac.init(r);
            return;
        }
        const int parts = partMode == 3 ? 4 : 1, pb = partMode == 3 ? size / 2 : size;
        bool prev[4];
        for (int k = 0; k < parts; ++k) prev[k] = cabac.decision(r, ctx.prevIntraLuma);
        for (int k = 0; k < parts; ++k)
        {
            const int xPb = x0 + (k % 2) * pb, yPb = y0 + (k / 2) * pb;
            // 8.4.2: the most probable modes from the left and the upper neighbour
            int cand[2];
            for (int n = 0; n < 2; ++n)
            {
                const int xN = n == 0 ? xPb - 1 : xPb, yN = n == 0 ? yPb : yPb - 1;
                cand[n] = INTRA_DC;
                if (pic.available(xPb, yPb, xN, yN) && pic.intra[pic.at(xN, yN)] == 1 &&
                    !(n == 1 && yPb - 1 < ((yPb >> s.ctbLog2) << s.ctbLog2)))
                    cand[n] = pic.mode[pic.at(xN, yN)];
            }
            int list[3];
            if (cand[0] == cand[1])
            {
                if (cand[0] < 2)
                {
                    list[0] = 0;
                    list[1] = 1;
                    list[2] = 26;
                }
                else
                {
                    list[0] = cand[0];
                    list[1] = 2 + ((cand[0] + 29) % 32);
                    list[2] = 2 + ((cand[0] - 2 + 1) % 32);
                }
            }
            else
            {
                list[0] = cand[0];
                list[1] = cand[1];
                list[2] = cand[0] != 0 && cand[1] != 0 ? 0 : cand[0] != 1 && cand[1] != 1 ? 1 : 26;
            }
            int mode;
            if (prev[k])
            {
                int idx = 0;
                while (idx < 2 && cabac.bypass(r)) ++idx;
                mode = list[idx];
            }
            else
            {
                mode = static_cast<int>(cabac.bypassBits(r, 5));
                std::sort(list, list + 3);
                for (const int l : list)
                    if (mode >= l)
                        ++mode;
            }
            pic.fill(pic.mode, xPb, yPb, pb, static_cast<uint8_t>(mode));
        }
        // 8.4.3: the chroma mode from intra_chroma_pred_mode and the luma mode (Table 8-2), mapped for 4:2:2 (Table
        // 8-3); 4:4:4 has one for each part
        for (int k = 0; k < (s.chromaFormat == 3 ? parts : s.chromaFormat != 0 ? 1 : 0); ++k)
        {
            int icpm = 4;
            if (cabac.decision(r, ctx.intraChroma))
                icpm = static_cast<int>(cabac.bypassBits(r, 2));
            const int luma = pic.mode[pic.at(x0 + (k % 2) * pb, y0 + (k / 2) * pb)];
            static const int modes[4] = {0, 26, 10, 1};
            int mode = icpm == 4 ? luma : modes[icpm] == luma ? 34 : modes[icpm];
            static const uint8_t map422[35] = {0,  1,  2,  2,  2,  2,  3,  5,  7,  8,  10, 12, 13, 15, 17, 18, 19, 20,
                                               21, 22, 23, 23, 24, 24, 25, 25, 26, 27, 27, 28, 28, 29, 29, 30, 31};
            if (s.chromaFormat == 2)
                mode = map422[mode];
            chromaMode[k] = mode;
            chromaDm[k] = icpm == 4;
        }
    }
    else
    {
        bool merge = false;
        const int h = size / 2, q = size / 4;
        switch (partMode)
        {
        case 0:
            predictionUnit(r, size, size, merge);
            break;
        case 1:
            predictionUnit(r, size, h, merge);
            predictionUnit(r, size, h, merge);
            break;
        case 2:
            predictionUnit(r, h, size, merge);
            predictionUnit(r, h, size, merge);
            break;
        case 3:
            for (int k = 0; k < 4; ++k) predictionUnit(r, h, h, merge);
            break;
        case 4:
            predictionUnit(r, size, q, merge);
            predictionUnit(r, size, size - q, merge);
            break;
        case 5:
            predictionUnit(r, size, size - q, merge);
            predictionUnit(r, size, q, merge);
            break;
        case 6:
            predictionUnit(r, q, size, merge);
            predictionUnit(r, size - q, size, merge);
            break;
        default:
            predictionUnit(r, size - q, size, merge);
            predictionUnit(r, q, size, merge);
            break;
        }
        if (!(partMode == 0 && merge))
            rqtRootCbf = cabac.decision(r, ctx.rqtRootCbf);
    }
    if (rqtRootCbf)
    {
        const bool intraSplit = intraCu && partMode == 3;
        const int maxDepth = intraCu ? s.maxTrDepthIntra + intraSplit : s.maxTrDepthInter;
        transformTree(r, x0, y0, x0, y0, log2CbSize, 0, 0, maxDepth, intraSplit, ChromaCbf{});
    }
}

// 7.3.8.6; mergeFlag: the merge_flag read (2Nx2N has one prediction unit, whose flag decides on rqt_root_cbf)
void Segment::predictionUnit(Rbsp& r, const int w, const int h, bool& mergeFlag)
{
    mergeFlag = cabac.decision(r, ctx.mergeFlag);
    if (mergeFlag)
    {
        if (maxNumMergeCand > 1 && cabac.decision(r, ctx.mergeIdx))
            for (int i = 1; i < maxNumMergeCand - 1 && cabac.bypass(r); ++i)
            {
            }
        return;
    }
    int pred = 0;  // 0 L0, 1 L1, 2 BI
    if (sliceType == B_SLICE)
    {
        if (w + h != 12 && cabac.decision(r, ctx.interPredIdc[ctDepth]))
            pred = 2;
        else
            pred = cabac.decision(r, ctx.interPredIdc[4]);
    }
    for (int list = 0; list < 2; ++list)
    {
        if (pred == 1 - list)
            continue;
        if (numRefIdx[list] > 1)
        {
            int v = 0;
            while (v < numRefIdx[list] - 1 && (v < 2 ? cabac.decision(r, ctx.refIdx[v]) : cabac.bypass(r))) ++v;
        }
        if (!(list == 1 && mvdL1Zero && pred == 2))
            mvdCoding(r);
        cabac.decision(r, ctx.mvp);
    }
}

// 7.3.8.9
void Segment::mvdCoding(Rbsp& r)
{
    const bool g0x = cabac.decision(r, ctx.mvdGreater0), g0y = cabac.decision(r, ctx.mvdGreater0);
    const bool g1x = g0x && cabac.decision(r, ctx.mvdGreater1), g1y = g0y && cabac.decision(r, ctx.mvdGreater1);
    if (g0x)
    {
        if (g1x)
            cabac.expGolomb(r, 1);
        cabac.bypass(r);
    }
    if (g0y)
    {
        if (g1y)
            cabac.expGolomb(r, 1);
        cabac.bypass(r);
    }
}

// 7.3.8.8
void Segment::transformTree(Rbsp& r, const int x0, const int y0, const int xBase, const int yBase, const int log2Size,
                            const int depth, const int blkIdx, const int maxDepth, const bool intraSplit,
                            const ChromaCbf& parent)
{
    bool split;
    if (log2Size <= s.maxTbLog2 && log2Size > s.minTbLog2 && depth < maxDepth && !(intraSplit && depth == 0))
        split = cabac.decision(r, ctx.splitTransform[5 - log2Size]);
    else
    {
        const bool interSplit = s.maxTrDepthInter == 0 && !intraCu && partMode != 0 && depth == 0;
        split = log2Size > s.maxTbLog2 || (intraSplit && depth == 0) || interSplit;
    }
    ChromaCbf own;
    if ((log2Size > 2 && s.chromaFormat != 0) || s.chromaFormat == 3)
    {
        const bool second = s.chromaFormat == 2 && (!split || log2Size == 3);
        if (depth == 0 || parent.cb[0])
        {
            own.cb[0] = cabac.decision(r, ctx.cbfChroma[depth]);
            if (second)
                own.cb[1] = cabac.decision(r, ctx.cbfChroma[depth]);
        }
        if (depth == 0 || parent.cr[0])
        {
            own.cr[0] = cabac.decision(r, ctx.cbfChroma[depth]);
            if (second)
                own.cr[1] = cabac.decision(r, ctx.cbfChroma[depth]);
        }
    }
    if (split)
    {
        const int half = 1 << (log2Size - 1);
        for (int k = 0; k < 4; ++k)
            transformTree(r, x0 + (k % 2) * half, y0 + (k / 2) * half, x0, y0, log2Size - 1, depth + 1, k, maxDepth,
                          intraSplit, own);
        return;
    }
    bool cbfLuma = true;
    if (intraCu || depth != 0 || own.any())
        cbfLuma = cabac.decision(r, ctx.cbfLuma[depth == 0 ? 1 : 0]);
    transformUnit(r, x0, y0, xBase, yBase, log2Size, blkIdx, cbfLuma, own, parent);
}

// the part of the coding unit holding a luma location: 0 unless the unit is split into four (NxN)
int Segment::partOf(const int x, const int y) const
{
    if (partMode != 3)
        return 0;
    return (y - y0Cu >= sizeCu / 2 ? 2 : 0) + (x - x0Cu >= sizeCu / 2 ? 1 : 0);
}

// 7.3.8.10, with chroma_qp_offset( ) of 7.3.8.13 and cross_comp_pred( ) of 7.3.8.12
void Segment::transformUnit(Rbsp& r, const int x0, const int y0, const int xBase, const int yBase, const int log2Size,
                            const int blkIdx, const bool cbfLuma, const ChromaCbf& own, const ChromaCbf& parent)
{
    // a 4x4 luma block of 4:2:0 and 4:2:2 has its chroma in the parent's block, read after the fourth luma block
    const bool chromaInParent = s.chromaFormat != 0 && s.chromaFormat != 3 && log2Size == 2;
    const ChromaCbf& c = chromaInParent ? parent : own;
    const bool cbfChroma = s.chromaFormat != 0 && c.any();
    if (!cbfLuma && !cbfChroma)
        return;
    if (p.cuQpDelta && !isCuQpDeltaCoded)
    {
        isCuQpDeltaCoded = true;
        int v = 0;
        while (v < 5 && cabac.decision(r, ctx.cuQpDeltaAbs[v == 0 ? 0 : 1])) ++v;
        if (v == 5)
            v += static_cast<int>(cabac.expGolomb(r, 0));
        if (v > 0)
            cabac.bypass(r);
    }
    if (cbfChroma && !transquantBypass && chromaQpOffsetEnabled && !isCuChromaQpOffsetCoded)
    {
        if (cabac.decision(r, ctx.chromaQpOffsetFlag) && p.chromaQpOffsetListLen > 1)
            for (int v = 0; v < p.chromaQpOffsetListLen - 1 && cabac.decision(r, ctx.chromaQpOffsetIdx); ++v)
            {
            }
        isCuChromaQpOffsetCoded = true;
    }
    if (cbfLuma)
        residualCoding(r, x0, y0, log2Size, 0);
    if (s.chromaFormat == 0)
        return;
    const int log2SizeC = std::max(2, log2Size - (s.chromaFormat == 3 ? 0 : 1));
    const int blocks = s.chromaFormat == 2 ? 2 : 1;
    if (!chromaInParent)
    {
        const bool crossComponent = p.crossComponentPrediction && cbfLuma && (!intraCu || chromaDm[partOf(x0, y0)]);
        for (int comp = 1; comp <= 2; ++comp)
        {
            if (crossComponent)
            {
                int v = 0;
                while (v < 4 && cabac.decision(r, ctx.resScaleAbs[4 * (comp - 1) + v])) ++v;
                if (v != 0)
                    cabac.decision(r, ctx.resScaleSign[comp - 1]);
            }
            for (int t = 0; t < blocks; ++t)
                if ((comp == 1 ? c.cb : c.cr)[t])
                    residualCoding(r, x0, y0 + (t << log2SizeC), log2SizeC, comp);
        }
    }
    else if (blkIdx == 3)
    {
        for (int comp = 1; comp <= 2; ++comp)
            for (int t = 0; t < blocks; ++t)
                if ((comp == 1 ? c.cb : c.cr)[t])
                    residualCoding(r, xBase, yBase + (t << log2SizeC), 2, comp);
    }
}

// 7.3.8.11
void Segment::residualCoding(Rbsp& r, const int x0, const int y0, const int log2Size, const int cIdx)
{
    if (p.transformSkip && !transquantBypass && log2Size <= p.log2MaxTransformSkipSize)
        cabac.decision(r, ctx.transformSkip[cIdx > 0]);
    // last_sig_coeff_x_prefix, last_sig_coeff_y_prefix: TR with cMax ( log2TrafoSize << 1 ) - 1
    const int offset = cIdx == 0 ? 3 * (log2Size - 2) + ((log2Size - 1) >> 2) : 15;
    const int shift = cIdx == 0 ? (log2Size + 1) >> 2 : log2Size - 2;
    const int cMax = (log2Size << 1) - 1;
    int prefix[2];
    for (int k = 0; k < 2; ++k)
    {
        int v = 0;
        while (v < cMax && cabac.decision(r, (k == 0 ? ctx.lastX : ctx.lastY)[offset + (v >> shift)])) ++v;
        prefix[k] = v;
    }
    int last[2];
    for (int k = 0; k < 2; ++k)
    {
        last[k] = prefix[k];
        if (prefix[k] > 3)
        {
            const int bits = (prefix[k] >> 1) - 1;
            last[k] = (1 << bits) * (2 + (prefix[k] & 1)) + static_cast<int>(cabac.bypassBits(r, bits));
        }
    }
    if (last[0] >= (1 << log2Size) || last[1] >= (1 << log2Size))
        throw Stop{false};
    // 7.4.9.11: the scan, from the intra prediction mode of small intra blocks
    int scanIdx = 0;
    if (intraCu && (log2Size == 2 || (log2Size == 3 && (cIdx == 0 || s.chromaFormat == 3))))
    {
        const int mode = cIdx == 0 ? pic.mode[pic.at(x0, y0)] : chromaMode[s.chromaFormat == 3 ? partOf(x0, y0) : 0];
        scanIdx = mode >= 6 && mode <= 14 ? 2 : mode >= 22 && mode <= 30 ? 1 : 0;
    }
    if (scanIdx == 2)
        std::swap(last[0], last[1]);
    const int l = log2Size - 2;  // sub-blocks per side: 1 << l
    const auto& sub = scans.pos[l][scanIdx];
    const auto& in = scans.pos[2][scanIdx];
    // the sub-block and the position in it of the last coefficient
    int lastSub = (1 << (2 * l)) - 1, lastPos = 16;
    for (;;)
    {
        if (lastPos == 0)
        {
            lastPos = 16;
            if (--lastSub < 0)
                throw Stop{false};
        }
        --lastPos;
        const int xC = (sub[lastSub][0] << 2) + in[lastPos][0], yC = (sub[lastSub][1] << 2) + in[lastPos][1];
        if (xC == last[0] && yC == last[1])
            break;
    }
    uint8_t csbf[8][8] = {};
    bool firstSubBlock = true;
    int prevGreater1Ctx = 1;  // greater1Ctx after the last flag of the previous sub-block that had some
    for (int i = lastSub; i >= 0; --i)
    {
        const int xS = sub[i][0], yS = sub[i][1];
        bool inferDc = false;
        if (i < lastSub && i > 0)
        {
            int c = 0;
            if (xS < (1 << l) - 1)
                c += csbf[xS + 1][yS];
            if (yS < (1 << l) - 1)
                c += csbf[xS][yS + 1];
            csbf[xS][yS] = static_cast<uint8_t>(cabac.decision(r, ctx.csbf[(cIdx ? 2 : 0) + std::min(c, 1)]));
            inferDc = true;
        }
        else
            csbf[xS][yS] = 1;
        bool sig[16] = {};
        if (i == lastSub)
            sig[lastPos] = true;
        for (int n = (i == lastSub ? lastPos - 1 : 15); n >= 0; --n)
        {
            const int xC = (xS << 2) + in[n][0], yC = (yS << 2) + in[n][1];
            if (!csbf[xS][yS])
                continue;
            if (n == 0 && inferDc)
            {
                sig[0] = true;
                continue;
            }
            // 9.3.4.2.5
            int sigCtx;
            if (log2Size == 2)
                sigCtx = ctxIdxMap[(yC << 2) + xC];
            else if (xC + yC == 0)
                sigCtx = 0;
            else
            {
                int prevCsbf = 0;
                if (xS < (1 << l) - 1)
                    prevCsbf += csbf[xS + 1][yS];
                if (yS < (1 << l) - 1)
                    prevCsbf += csbf[xS][yS + 1] << 1;
                const int xP = xC & 3, yP = yC & 3;
                if (prevCsbf == 0)
                    sigCtx = xP + yP == 0 ? 2 : xP + yP < 3 ? 1 : 0;
                else if (prevCsbf == 1)
                    sigCtx = yP == 0 ? 2 : yP == 1 ? 1 : 0;
                else if (prevCsbf == 2)
                    sigCtx = xP == 0 ? 2 : xP == 1 ? 1 : 0;
                else
                    sigCtx = 2;
                if (cIdx == 0)
                {
                    if (xS > 0 || yS > 0)
                        sigCtx += 3;
                    sigCtx += log2Size == 3 ? (scanIdx == 0 ? 9 : 15) : 21;
                }
                else
                    sigCtx += log2Size == 3 ? 9 : 12;
            }
            sig[n] = cabac.decision(r, ctx.sig[cIdx == 0 ? sigCtx : 27 + sigCtx]);
            if (sig[n])
                inferDc = false;
        }
        int firstSig = 16, lastSig = -1, numGreater1 = 0, lastGreater1Pos = -1;
        int ctxSet = 0, greater1Ctx = 1;
        bool greater1[16] = {};
        bool anyG1 = false;
        for (int n = 15; n >= 0; --n)
        {
            if (!sig[n])
                continue;
            if (numGreater1 < 8)
            {
                // 9.3.4.2.6
                if (!anyG1)
                {
                    ctxSet = i == 0 || cIdx > 0 ? 0 : 2;
                    const int lastGreater1Ctx = firstSubBlock ? 1 : prevGreater1Ctx;
                    if (lastGreater1Ctx == 0)
                        ++ctxSet;
                    greater1Ctx = 1;
                    anyG1 = true;
                }
                greater1[n] =
                    cabac.decision(r, ctx.greater1[(cIdx > 0 ? 16 : 0) + ctxSet * 4 + std::min(3, greater1Ctx)]);
                // the context of the next flag
                if (greater1Ctx > 0)
                    greater1Ctx = greater1[n] ? 0 : greater1Ctx + 1;
                ++numGreater1;
                if (greater1[n] && lastGreater1Pos == -1)
                    lastGreater1Pos = n;
            }
            if (lastSig == -1)
                lastSig = n;
            firstSig = n;
        }
        if (anyG1)
        {
            prevGreater1Ctx = greater1Ctx;
            firstSubBlock = false;
        }
        const bool signHidden = !transquantBypass && lastSig - firstSig > 3;
        bool greater2 = false;
        if (lastGreater1Pos != -1)
            greater2 = cabac.decision(r, ctx.greater2[(cIdx > 0 ? 4 : 0) + ctxSet]);
        for (int n = 15; n >= 0; --n)
            if (sig[n] && (!p.signHiding || !signHidden || n != firstSig))
                cabac.bypass(r);
        int numSig = 0, cLastAbs = 0, cLastRice = 0;
        bool firstRemaining = true;
        for (int n = 15; n >= 0; --n)
        {
            if (!sig[n])
                continue;
            const int base = 1 + greater1[n] + (n == lastGreater1Pos ? greater2 : 0);
            if (base == (numSig < 8 ? (n == lastGreater1Pos ? 3 : 2) : 1))
            {
                // 9.3.3.11
                const int rice =
                    firstRemaining ? 0 : std::min(cLastRice + (cLastAbs > 3 * (1 << cLastRice) ? 1 : 0), 4);
                int q = 0;
                while (q < 4 && cabac.bypass(r)) ++q;
                int64_t value;
                if (q < 4)
                    value = (static_cast<int64_t>(q) << rice) + cabac.bypassBits(r, rice);
                else
                    value = (int64_t{4} << rice) + cabac.expGolomb(r, rice + 1);
                cLastAbs = static_cast<int>(std::min<int64_t>(base + value, 1 << 30));
                cLastRice = rice;
                firstRemaining = false;
            }
            ++numSig;
        }
    }
}

// 7.3.8.1 and 7.3.8.2, with 9.3.1 and 9.3.2 for the contexts at the start of a segment, a tile and a CTU row
SegmentResult Segment::parseData(Rbsp& r, Contexts& wpp, Contexts& ds, bool& wppStored)
{
    SegmentResult res;
    try
    {
        int ts = pic.rsToTs[address];
        const auto startContexts = [&](const int rs, const bool segmentStart)
        {
            const int t = pic.rsToTs[rs];
            const bool firstInTile = t == 0 || pic.tileId[t] != pic.tileId[t - 1];
            const bool rowStart = p.wpp && (rs % pic.wCtbs == 0 || pic.tileId[t] != pic.tileId[pic.rsToTs[rs - 1]]);
            if (firstInTile)
                ctx.init(initType(), qp);
            else if (rowStart)
            {
                const int x = (rs % pic.wCtbs) * pic.ctbSize, y = (rs / pic.wCtbs) * pic.ctbSize;
                pic.ctbSlice[rs] = sliceAddrRs;  // for the test of availability against it
                if (wppStored && pic.available(x, y, x + pic.ctbSize, y - pic.ctbSize))
                    ctx = wpp;
                else
                    ctx.init(initType(), qp);
            }
            else if (segmentStart)
            {
                if (dependent)
                    ctx = ds;
                else
                    ctx.init(initType(), qp);
            }
            cabac.init(r);
        };
        startContexts(address, true);
        for (;;)
        {
            const int rs = pic.tsToRs[ts];
            res.stopCtb = rs;
            pic.ctbSlice[rs] = sliceAddrRs;
            const int x = (rs % pic.wCtbs) * pic.ctbSize, y = (rs / pic.wCtbs) * pic.ctbSize;
            if (saoLuma || saoChroma)
                sao(r, rs % pic.wCtbs, rs / pic.wCtbs, rs);
            codingQuadtree(r, x, y, s.ctbLog2, 0);
            res.lastTs = ts;
            const bool end = cabac.terminate(r);
            if (p.wpp && (rs % pic.wCtbs == 1 || (rs > 1 && pic.tileId[ts] != pic.tileId[pic.rsToTs[rs - 2]])))
            {
                wpp = ctx;
                wppStored = true;
            }
            if (end)
                break;
            if (++ts >= pic.sizeCtbs)
                throw Stop{false};
            const int next = pic.tsToRs[ts];
            if ((p.tiles && pic.tileId[ts] != pic.tileId[ts - 1]) ||
                (p.wpp && (next % pic.wCtbs == 0 || pic.tileId[ts] != pic.tileId[pic.rsToTs[next - 1]])))
            {
                if (!cabac.terminate(r))
                    throw Stop{false};  // end_of_subset_one_bit
                while (!r.aligned())
                    if (r.get(1))
                        throw Stop{false};
                startContexts(next, false);
            }
        }
        if (p.dependentSlices)
            ds = ctx;
        // the segment ends at its stop bit, the last bit read: zero bytes at most after that
        res.state = r.restZeroAfterByte() ? SegmentResult::whole : SegmentResult::broken;
    }
    catch (const Stop& e)
    {
        res.state = e.outOfData ? SegmentResult::outOfData : SegmentResult::broken;
    }
    return res;
}
}  // namespace

PictureEnd hevcPictureEnd(const uint8_t* data, const uint8_t* end, const std::vector<std::vector<uint8_t>>& paramSets)
{
    PictureEnd result;
    std::map<int, std::vector<uint8_t>> spsMap, ppsMap;
    const auto take = [&](const std::vector<uint8_t>& nal)
    {
        if (nal.size() < 3)
            return;
        const int type = nal[0] >> 1 & 63, layer = (nal[0] & 1) << 5 | nal[1] >> 3;
        if (layer != 0)
            return;
        int id = -1;
        if (type == 33)
        {
            // kept by its id also when it is of a profile not followed: the picture then gives "unknown"
            Sps s;
            if (parseSps(nal, s, id) || id >= 0)
                spsMap[id] = nal;
        }
        else if (type == 34)
        {
            Pps p;
            if (parsePps(nal, p, id) || id >= 0)
                ppsMap[id] = nal;
        }
    };
    for (const auto& ps : paramSets) take(unescape(ps.data(), ps.data() + ps.size()));
    // the NAL units of the data
    std::vector<std::pair<const uint8_t*, const uint8_t*>> nals;
    const uint8_t* start = nullptr;
    for (const uint8_t* q = data; q + 3 <= end; ++q)
        if (q[0] == 0 && q[1] == 0 && q[2] == 1)
        {
            if (start)
                nals.emplace_back(start, q);
            start = q + 3;
            q += 2;
        }
    if (start && start < end)
        nals.emplace_back(start, end);
    for (auto& [b, e] : nals)
        while (e > b && e[-1] == 0) --e;
    // the slice segments of the last picture, from its first one, with the parameter sets in force for them
    size_t firstOfPicture = nals.size();
    bool endOfStream = false;
    std::map<int, std::vector<uint8_t>> spsAt, ppsAt;
    for (size_t i = 0; i < nals.size(); ++i)
    {
        const auto [b, e] = nals[i];
        if (e - b < 3)
            continue;
        const int type = b[0] >> 1 & 63, layer = (b[0] & 1) << 5 | b[1] >> 3;
        if (layer != 0)
            continue;
        if (type == 33 || type == 34)
        {
            take(unescape(b, e));
            continue;
        }
        if (type == 36 || type == 37)
        {
            if (firstOfPicture < nals.size())
                endOfStream = true;
            continue;
        }
        if (!(type <= 9 || (type >= 16 && type <= 21)))
            continue;
        if (b[2] & 0x80)  // first_slice_segment_in_pic_flag
        {
            firstOfPicture = i;
            spsAt = spsMap;
            ppsAt = ppsMap;
            endOfStream = false;
        }
    }
    // the data ends one or two bytes into the header of a slice segment of a new picture: nothing of it is there
    if (!nals.empty())
    {
        const auto [b, e] = nals.back();
        const int type = e > b ? b[0] >> 1 & 63 : -1;
        if (e - b < 3 && e > b && (b[0] & 1) == 0 && (type <= 9 || (type >= 16 && type <= 21)))
        {
            result.state = PictureEnd::State::cut;
            return result;
        }
    }
    if (firstOfPicture >= nals.size())
        return result;
    if (endOfStream)
    {
        result.state = PictureEnd::State::whole;
        return result;
    }
    // the parameter sets of the picture: those given before its first slice segment
    Picture pic;
    Sps sps;
    Pps pps;
    {
        std::vector<uint8_t> first = unescape(nals[firstOfPicture].first, nals[firstOfPicture].second);
        Rbsp r(first);
        int ppsId = -1, spsId = -1, id;
        try
        {
            r.skip(16 + 1);
            if ((first[0] >> 1 & 63) >= 16 && (first[0] >> 1 & 63) <= 23)
                r.skip(1);
            ppsId = static_cast<int>(r.ue());
        }
        catch (const Stop&)
        {
            result.state = PictureEnd::State::cut;  // the data ends in the first fields of the picture's header
            return result;
        }
        if (!ppsAt.count(ppsId) || !parsePps(ppsAt[ppsId], pps, id))
            return result;
        spsId = pps.spsId;
        if (!spsAt.count(spsId) || !parseSps(spsAt[spsId], sps, id))
            return result;
        pic.setup(sps, pps);
    }
    result.rows = pic.hCtbs;
    Contexts wpp, ds;
    bool wppStored = false;
    std::unique_ptr<Segment> independent;
    int lastTs = -1;
    for (size_t i = firstOfPicture; i < nals.size(); ++i)
    {
        const auto [b, e] = nals[i];
        if (e - b < 3)
            continue;
        const int type = b[0] >> 1 & 63, layer = (b[0] & 1) << 5 | b[1] >> 3;
        if (layer != 0 || !(type <= 9 || (type >= 16 && type <= 21)))
            continue;
        const bool last = i + 1 == nals.size();
        std::vector<uint8_t> nal = unescape(b, e);
        Rbsp r(nal);
        auto seg = std::make_unique<Segment>(sps, pps, pic, type);
        bool header;
        try
        {
            header = seg->parseHeader(r, independent.get());
        }
        catch (const Stop& s)
        {
            if (s.outOfData && last)
            {
                result.state = PictureEnd::State::cut;
                result.row = lastTs < 0 ? 0 : pic.tsToRs[lastTs] / pic.wCtbs + 1;
                return result;
            }
            return result;
        }
        if (!header || (lastTs >= 0 && pic.rsToTs[seg->address] != lastTs + 1))
            return result;  // not followed, or segments missing between those of the data
        const SegmentResult sr = seg->parseData(r, wpp, ds, wppStored);
        if (sr.state == SegmentResult::outOfData && last)
        {
            result.state = PictureEnd::State::cut;
            result.row = sr.stopCtb / pic.wCtbs + 1;
            return result;
        }
        if (sr.state != SegmentResult::whole)
            return result;
        lastTs = sr.lastTs;
        if (!seg->dependent)
            independent = std::move(seg);
    }
    if (lastTs + 1 >= pic.sizeCtbs)
        result.state = PictureEnd::State::whole;
    else if (lastTs >= 0)
    {
        result.state = PictureEnd::State::cut;
        result.betweenSlices = true;
        result.row = pic.tsToRs[lastTs + 1] / pic.wCtbs + 1;
    }
    return result;
}
