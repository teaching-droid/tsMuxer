#ifndef HEVC_H_
#define HEVC_H_

#include "nalUnits.h"

struct HevcUnit
{
    HevcUnit() : nal_unit_type(), nuh_layer_id(0), nuh_temporal_id_plus1(0), m_nalBuffer(nullptr), m_nalBufferLen(0) {}

    enum class NalType
    {
        TRAIL_N = 0,  // first slice
        TRAIL_R = 1,
        TSA_N = 2,
        TSA_R = 3,
        STSA_N = 4,
        STSA_R = 5,
        RADL_N = 6,
        RADL_R = 7,
        RASL_N = 8,
        RASL_R = 9,
        BLA_W_LP = 16,
        BLA_W_RADL = 17,
        BLA_N_LP = 18,
        IDR_W_RADL = 19,
        IDR_N_LP = 20,
        CRA = 21,
        RSV_IRAP_VCL22 = 22,  // reserved
        RSV_IRAP_VCL23 = 23,  // reserved, last slice

        VPS = 32,
        SPS = 33,
        PPS = 34,
        AUD = 35,
        EOS = 36,
        EOB = 37,
        FD = 38,
        SEI_PREFIX = 39,
        SEI_SUFFIX = 40,
        RSV_NVCL45 = 45,
        RSV_NVCL47 = 47,
        UNSPEC56 = 56,
        DVRPU = 62,
        DVEL = 63,
    };

    void decodeBuffer(const uint8_t* buffer, const uint8_t* end);
    int deserialize();
    int serializeBuffer(uint8_t* dstBuffer, const uint8_t* dstEnd) const;

    [[nodiscard]] int nalBufferLen() const { return m_nalBufferLen; }

    NalType nal_unit_type;
    uint8_t nuh_layer_id;
    uint8_t nuh_temporal_id_plus1;

   protected:
    unsigned extractUEGolombCode();
    int extractSEGolombCode();
    [[nodiscard]] bool updateBits(int bitOffset, int bitLen, unsigned value) const;

    uint8_t* m_nalBuffer;
    int m_nalBufferLen;
    BitStreamReader m_reader;
};

struct HevcUnitWithProfile : HevcUnit
{
    HevcUnitWithProfile();
    [[nodiscard]] std::string getProfileString() const;

    // Rewrite general_level_idc in place. No pixel is touched: the level only declares how much
    // capability a decoder needs, and a higher level is a superset of a lower one, so a stream
    // conformant to the old level stays conformant. Returns false if the field was not located
    // while parsing, or the buffer is too small to write into.
    bool setLevel(uint8_t newLevel);

    uint8_t profile_idc;
    uint8_t level_idc;
    bool interlaced_source_flag;
    // Bit position of general_level_idc inside the UNESCAPED NAL, recorded while parsing. It has to
    // be recorded rather than computed: the reserved bits just before it are zeros, so emulation
    // prevention bytes land in that exact region and a fixed offset into the raw NAL is wrong.
    int level_idc_bit_pos;

   protected:
    int profile_tier_level(int subLayers);
};

struct HevcVpsUnit : HevcUnitWithProfile
{
    HevcVpsUnit();
    int deserialize();
    [[nodiscard]] double getFPS() const;
    [[nodiscard]] bool setFPS(double fps);
    [[nodiscard]] std::string getDescription() const;

    int vps_id;
    unsigned num_units_in_tick;
    unsigned time_scale;
    int num_units_in_tick_bit_pos;
};

struct HevcSpsUnit : HevcUnitWithProfile
{
    HevcSpsUnit();
    int deserialize();
    [[nodiscard]] double getFPS() const;
    [[nodiscard]] bool setFPS(double fps);
    [[nodiscard]] std::string getDescription() const;

    uint8_t vps_id;
    uint8_t max_sub_layers;
    unsigned sps_id;
    unsigned chromaFormat;
    bool separate_colour_plane_flag;
    unsigned pic_width_in_luma_samples;
    unsigned pic_height_in_luma_samples;
    // Conformance window, in chroma units. The coded picture is padded up to a coding block
    // multiple and this is how much of it is not displayed, so the displayed size is the coded
    // size minus these. They used to be parsed and thrown away.
    unsigned conf_win_left_offset;
    unsigned conf_win_right_offset;
    unsigned conf_win_top_offset;
    unsigned conf_win_bottom_offset;

    // Displayed size: the coded size minus the conformance window. Offsets are in chroma units.
    [[nodiscard]] unsigned getDisplayWidth() const
    {
        const unsigned subWidthC = (chromaFormat == 1 || chromaFormat == 2) ? 2 : 1;
        const unsigned crop = (conf_win_left_offset + conf_win_right_offset) * subWidthC;
        return crop < pic_width_in_luma_samples ? pic_width_in_luma_samples - crop : 0;
    }
    [[nodiscard]] unsigned getDisplayHeight() const
    {
        const unsigned subHeightC = chromaFormat == 1 ? 2 : 1;
        const unsigned crop = (conf_win_top_offset + conf_win_bottom_offset) * subHeightC;
        return crop < pic_height_in_luma_samples ? pic_height_in_luma_samples - crop : 0;
    }
    unsigned bit_depth_luma_minus8;
    unsigned bit_depth_chroma_minus8;
    unsigned log2_max_pic_order_cnt_lsb;
    bool nal_hrd_parameters_present_flag;
    bool vcl_hrd_parameters_present_flag;
    bool sub_pic_hrd_params_present_flag;

    std::vector<unsigned> num_delta_pocs;

    uint8_t colour_primaries;
    uint8_t transfer_characteristics;
    uint8_t matrix_coeffs;
    unsigned chroma_sample_loc_type_top_field;
    unsigned chroma_sample_loc_type_bottom_field;

    unsigned num_short_term_ref_pic_sets;
    unsigned num_units_in_tick;
    unsigned time_scale;
    // Where vui_num_units_in_tick begins, recorded during the parse the same way the VPS records
    // its own, because it cannot be found again afterwards: everything in front of it is variable
    // length. -1 means this SPS carries no timing info and there is nothing to rewrite.
    int num_units_in_tick_bit_pos;
    unsigned PicSizeInCtbsY_bits;

   private:
    int hrd_parameters(bool commonInfPresentFlag, int maxNumSubLayersMinus1);
    int sub_layer_hrd_parameters(unsigned cpb_cnt_minus1);
    int short_term_ref_pic_set(unsigned stRpsIdx);
    int vui_parameters();
    int scaling_list_data();
};

struct HevcPpsUnit : HevcUnit
{
    HevcPpsUnit();
    int deserialize();

    unsigned pps_id;
    unsigned sps_id;
    bool dependent_slice_segments_enabled_flag;
    bool output_flag_present_flag;
    uint8_t num_extra_slice_header_bits;
};

struct HevcHdrUnit : HevcUnit
{
    HevcHdrUnit();
    int deserialize();

    bool isHDR10;
    bool isHDR10plus;
    bool isDVRPU;
    bool isDVEL;
};

struct HevcSliceHeader : HevcUnit
{
    HevcSliceHeader();
    int deserialize(const HevcSpsUnit* sps, const HevcPpsUnit* pps);
    [[nodiscard]] bool isIDR() const;

    bool first_slice;
    unsigned pps_id;
    unsigned slice_type;
    uint16_t pic_order_cnt_lsb;
};

std::vector<std::vector<uint8_t>> hevc_extract_priv_data(const uint8_t* buff, int size, uint8_t* nal_size);

#endif  // _HEVC_H_
