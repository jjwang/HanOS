#pragma once

typedef struct [[gnu::packed]] {
    uint8_t padding[8];
    uint16_t manufacturer_id_be;
    uint16_t edid_id_code;

    /* Serial number. 32 bits, little endian. */
    uint32_t serial_num;
    /* Week of manufacture */
    uint8_t man_week;
    /* Year of manufacture, less 1990. (1990-2245)
     * If week=255, it is the model year instead
     */
    uint8_t man_year;
    /* EDID version, usually 1 (for 1.3) */
    uint8_t edid_version;
    /* EDID revision, usually 3 (for 1.3) */
    uint8_t edid_revision;
    /* If Bit 7=1   Digital input. If set, the following bit definitions apply:
     *    Bits 6-1  Reserved, must be 0
     *    Bit 0     Signal is compatible with VESA DFP 1.x TMDS CRGB,
     *              1 pixel per clock, up to 8 bits per color, MSB aligned,
     * If Bit 7=0   Analog input. If clear, the following bit definitions apply:
     *    Bits 6-5  Video white and sync levels, relative to blank
     *              00=+0.7/-0.3 V; 01=+0.714/-0.286 V;
     *              10=+1.0/-0.4 V; 11=+0.7/0 V
     *    Bit 4     Blank-to-black setup (pedestal) expected
     *    Bit 3     Separate sync supported
     *    Bit 2     Composite sync (on HSync) supported
     *    Bit 1     Sync on green supported
     *    Bit 0     VSync pulse must be serrated when somposite or
     *              sync-on-green is used.
     */
    uint8_t video_input_type;
    /* Maximum horizontal image size, in centimetres
     * (max 292 cm/115 in at 16:9 aspect ratio)
     */
    uint8_t max_hor_size;
    /* Maximum vertical image size, in centimetres.
     * If either byte is 0, undefined (e.g. projector)
     */
    uint8_t max_ver_size;
    /* Display gamma, minus 1, times 100 (range 1.00-3.5) */
    uint8_t gamma_factor;
    /* Bit 7    DPMS standby supported
     * Bit 6    DPMS suspend supported
     * Bit 5    DPMS active-off supported
     * Bits 4-3 Display type: 00=monochrome; 01=RGB colour;
     *          10=non-RGB multicolour; 11=undefined
     * Bit 2    Standard sRGB colour space. Bytes 25-34 must contain
     *          sRGB standard values.
     * Bit 1    Preferred timing mode specified in descriptor block 1.
     * Bit 0    GTF supported with default parameter values.
     */
    uint8_t dpms_flags;

    uint8_t chroma_info[10];

    /* Established timings */
    /* Bit 7    720x400 @ 70 Hz
     * Bit 6    720x400 @ 88 Hz
     * Bit 5    640x480 @ 60 Hz
     * Bit 4    640x480 @ 67 Hz
     * Bit 3    640x480 @ 72 Hz
     * Bit 2    640x480 @ 75 Hz
     * Bit 1    800x600 @ 56 Hz
     * Bit 0    800x600 @ 60 Hz
     */
    uint8_t est_timings1;
    /* Bit 7    800x600 @ 72 Hz
     * Bit 6    800x600 @ 75 Hz
     * Bit 5    832x624 @ 75 Hz
     * Bit 4    1024x768 @ 87 Hz, interlaced (1024x768)
     * Bit 3    1024x768 @ 60 Hz
     * Bit 2    1024x768 @ 72 Hz
     * Bit 1    1024x768 @ 75 Hz
     * Bit 0    1280x1024 @ 75 Hz
     */
    uint8_t est_timings2;
    /* Bit 7    1152x870 @ 75 Hz (Apple Macintosh II)
     * Bits 6-0 Other manufacturer-specific display mod
     */
    uint8_t est_timings3;
    /* Standard timing */
    struct {
        uint8_t resolution;
        uint8_t frequency;
    } std_timing_id[8];
    struct {
        uint16_t pixel_clock;
        uint8_t horz_active;
        uint8_t horz_bank;
        uint8_t horz_active_blank_msb;
        uint8_t vert_active;
        uint8_t vert_blank;
        uint8_t vert_active_blank_msb;
        uint8_t horz_sync_sffset;
        uint8_t horz_sync_pulse;
        uint8_t vert_sync;
        uint8_t sync_msb;
        uint8_t dimension_width;
        uint8_t dimension_height;
        uint8_t dimension_msb;
        uint8_t horz_border;
        uint8_t vert_border;
        uint8_t features;
    } det_timings[4];
    uint8_t unused;
    uint8_t checksum;
} edid_info_t;
