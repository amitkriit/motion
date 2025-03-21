/*
 *    This file is part of Motion.
 *
 *    Motion is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    Motion is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with Motion.  If not, see <https://www.gnu.org/licenses/>.
 *
 *
*/

#include "motion.hpp"
#include "util.hpp"
#include "camera.hpp"
#include "conf.hpp"
#include "logger.hpp"
#include "jpegutils.hpp"
#include <setjmp.h>
#include <jpeglib.h>
#include <jerror.h>
#include <assert.h>

/* EXIF image data is always in TIFF format, even if embedded in another
 * file type. This consists of a constant header (TIFF file header,
 * IFD header) followed by the tags in the IFD and then the data
 * from any tags which do not fit inline in the IFD.
 *
 * The tags we write in the main IFD are:
 *  0x010E   Image description
 *  0x8769   Exif sub-IFD
 *  0x882A   Time zone of time stamps
 * and in the Exif sub-IFD:
 *  0x9000   Exif version
 *  0x9003   File date and time
 *  0x9291   File date and time subsecond info
 * But we omit any empty IFDs.
 */

#define TIFF_TAG_IMAGE_DESCRIPTION    0x010E
#define TIFF_TAG_DATETIME             0x0132
#define TIFF_TAG_EXIF_IFD             0x8769
#define TIFF_TAG_TZ_OFFSET            0x882A

#define EXIF_TAG_EXIF_VERSION         0x9000
#define EXIF_TAG_ORIGINAL_DATETIME    0x9003
#define EXIF_TAG_SUBJECT_AREA         0x9214
#define EXIF_TAG_TIFF_DATETIME_SS     0x9290
#define EXIF_TAG_ORIGINAL_DATETIME_SS 0x9291

#define TIFF_TYPE_ASCII  2  /* ASCII text */
#define TIFF_TYPE_USHORT 3  /* Unsigned 16-bit int */
#define TIFF_TYPE_LONG   4  /* Unsigned 32-bit int */
#define TIFF_TYPE_UNDEF  7  /* Byte blob */
#define TIFF_TYPE_SSHORT 8  /* Signed 16-bit int */

static const uint8_t EOI_data[2] = { 0xFF, 0xD9 };

static const char exif_marker_start[14] = {
    'E', 'x', 'i', 'f', 0, 0,   /* EXIF marker signature */
    'M', 'M', 0, 42,            /* TIFF file header (big-endian) */
    0, 0, 0, 8,                 /* Offset to first toplevel IFD */
};

static u_char exif_version_tag[12] = {
    0x90, 0x00,                 /* EXIF version tag, 0x9000 */
    0x00, 0x07,                 /* Data type 7 = "unknown" (raw byte blob) */
    0x00, 0x00, 0x00, 0x04,     /* Data length */
    0x30, 0x32, 0x32, 0x30      /* Inline data, EXIF version 2.2 */
};

static u_char exif_subifd_tag[8] = {
    0x87, 0x69,                 /* EXIF Sub-IFD tag */
    0x00, 0x04,                 /* Data type 4 = uint32 */
    0x00, 0x00, 0x00, 0x01,     /* Number of values */
};

static u_char exif_tzoffset_tag[12] = {
    0x88, 0x2A,                 /* TIFF/EP time zone offset tag */
    0x00, 0x08,                 /* Data type 8 = sint16 */
    0x00, 0x00, 0x00, 0x01,     /* Number of values */
    0, 0, 0, 0                  /* Dummy data */
};

static void put_uint16(JOCTET *buf, uint value)
{
    buf[0] = (u_char)(( value & 0xFF00 ) >> 8);
    buf[1] = (u_char)(( value & 0x00FF ));
}

static void put_sint16(JOCTET *buf, int value)
{
    buf[0] = (u_char)(( value & 0xFF00 ) >> 8);
    buf[1] = (u_char)(( value & 0x00FF ));
}

static void put_uint32(JOCTET *buf, uint value)
{
    buf[0] = (u_char)(( value & 0xFF000000 ) >> 24);
    buf[1] = (u_char)(( value & 0x00FF0000 ) >> 16);
    buf[2] = (u_char)(( value & 0x0000FF00 ) >> 8);
    buf[3] = (u_char)(( value & 0x000000FF ));
}

struct tiff_writing {
    JOCTET *base;
    JOCTET *buf;
    uint data_offset;
};

static void put_direntry(struct tiff_writing *into, const char *data, uint length)
{
    if (length <= 4) {
        /* Entries that fit in the directory entry are stored there */
        memset(into->buf, 0, 4);
        memcpy(into->buf, data, length);
    } else {
        /* Longer entries are stored out-of-line */
        uint offset = into->data_offset;

        while ((offset & 0x03) != 0) {  /* Alignment */
            into->base[offset] = 0;
            offset ++;
        }

        put_uint32(into->buf, offset);
        memcpy(into->base + offset, data, length);
        into->data_offset = offset + length;
    }
}

static void put_stringentry(struct tiff_writing *into, uint tag, const char *str, int with_nul)
{
    uint stringlength = (uint)strlen(str) + (with_nul?1:0);

    put_uint16(into->buf, tag);
    put_uint16(into->buf + 2, TIFF_TYPE_ASCII);
    put_uint32(into->buf + 4, stringlength);
    into->buf += 8;
    put_direntry(into, str, stringlength);
    into->buf += 4;
}

static void put_subjectarea(struct tiff_writing *into, ctx_coord *box)
{
    put_uint16(into->buf    , EXIF_TAG_SUBJECT_AREA);
    put_uint16(into->buf + 2, TIFF_TYPE_USHORT);
    put_uint32(into->buf + 4, 4 /* Four USHORTs */);
    put_uint32(into->buf + 8, into->data_offset);
    into->buf += 12;
    JOCTET *ool = into->base + into->data_offset;
    put_uint16(ool  , (uint)box->x); /* Center.x */
    put_uint16(ool+2, (uint)box->y); /* Center.y */
    put_uint16(ool+4, (uint)box->width);
    put_uint16(ool+6, (uint)box->height);
    into->data_offset += 8;
}

struct ctx_exif_info {
    cls_camera *cam;
    timespec *ts_in1;
    ctx_coord *box;
    struct tm timestamp_tm;
    char *description;
    char *datetime;
    char *subtime;
    uint ifd0_tagcount;
    uint ifd1_tagcount;
    uint datasize;
    uint ifds_size;
    struct tiff_writing writing;
};

void jpgutl_exif_date(ctx_exif_info *exif_info)
{
    char tmpbuf[45];
    struct timespec ts1;

    clock_gettime(CLOCK_REALTIME, &ts1);
    if (exif_info->ts_in1 != NULL) {
        ts1.tv_sec = exif_info->ts_in1->tv_sec;
        ts1.tv_nsec = exif_info->ts_in1->tv_nsec;
    }

    localtime_r(&ts1.tv_sec, &exif_info->timestamp_tm);
    /* Exif requires this exact format */
    /* The compiler is twitchy on truncating formats and the exif is twitchy
     * on the length of the whole string.  So we do it in two steps of printing
     * into a large buffer which compiler wants, then print that into the smaller
     * buffer that exif wants..TODO  Find better method
     */
    snprintf(tmpbuf, 45, "%04d:%02d:%02d %02d:%02d:%02d",
            exif_info->timestamp_tm.tm_year + 1900,
            exif_info->timestamp_tm.tm_mon + 1,
            exif_info->timestamp_tm.tm_mday,
            exif_info->timestamp_tm.tm_hour,
            exif_info->timestamp_tm.tm_min,
            exif_info->timestamp_tm.tm_sec);

    exif_info->datetime =(char*)mymalloc(PATH_MAX);
    snprintf(exif_info->datetime, 22,"%.21s",tmpbuf);

    exif_info->subtime = nullptr;

    if (exif_info->cam->cfg->picture_exif != "") {
        exif_info->description =(char*)mymalloc(PATH_MAX);
        mystrftime(exif_info->cam, exif_info->description, PATH_MAX-1
            , exif_info->cam->cfg->picture_exif.c_str(), NULL);
    } else {
        exif_info->description = nullptr;
    }

}

void jpgutl_exif_tags(ctx_exif_info *exif_info)
{
    /* Count up the number of tags and max amount of OOL data */
    if (exif_info->description != nullptr) {
        exif_info->ifd0_tagcount ++;
        exif_info->datasize += 5 + (uint)strlen(exif_info->description); /* Add 5 for NUL and alignment */
    }

    if (exif_info->datetime != nullptr) {
    /* We write this to both the TIFF datetime tag (which most programs
     * treat as "last-modified-date") and the EXIF "time of creation of
     * original image" tag (which many programs ignore). This is
     * redundant but seems to be the thing to do.
     */
        exif_info->ifd0_tagcount++;
        exif_info->ifd1_tagcount++;
        /* We also write the timezone-offset tag in IFD0 */
        exif_info->ifd0_tagcount++;
        /* It would be nice to use the same offset for both tags' values,
        * but I don't want to write the bookkeeping for that right now */
        exif_info->datasize += 2 * (5 + (uint)strlen(exif_info->datetime));
    }

    if (exif_info->subtime != nullptr) {
        exif_info->ifd1_tagcount++;
        exif_info->datasize += 5 + (uint)strlen(exif_info->subtime);
    }

    if (exif_info->box) {
        exif_info->ifd1_tagcount++;
        exif_info->datasize += 2 * 4;  /* Four 16-bit ints */
    }

    if (exif_info->ifd1_tagcount > 0) {
        /* If we're writing the Exif sub-IFD, account for the
        * two tags that requires */
        exif_info->ifd0_tagcount ++; /* The tag in IFD0 that points to IFD1 */
        exif_info->ifd1_tagcount ++; /* The EXIF version tag */
    }
    /* Each IFD takes 12 bytes per tag, plus six more (the tag count and the
     * pointer to the next IFD, always zero in our case)
     */
    exif_info->ifds_size =
        (exif_info->ifd1_tagcount > 0 ? ( 12 * exif_info->ifd1_tagcount + 6 ) : 0 ) +
        (exif_info->ifd0_tagcount > 0 ? ( 12 * exif_info->ifd0_tagcount + 6 ) : 0 );

}

void jpgutl_exif_writeifd0(ctx_exif_info *exif_info)
{
    /* Note that tags are stored in numerical order */
    put_uint16(exif_info->writing.buf, (uint)exif_info->ifd0_tagcount);
    exif_info->writing.buf += 2;

    if (exif_info->description) {
        put_stringentry(&exif_info->writing
            , TIFF_TAG_IMAGE_DESCRIPTION, exif_info->description, 1);
    }

    if (exif_info->datetime) {
        put_stringentry(&exif_info->writing
            , TIFF_TAG_DATETIME, exif_info->datetime, 1);
    }

    if (exif_info->ifd1_tagcount > 0) {
        /* Offset of IFD1 - TIFF header + IFD0 size. */
        uint ifd1_offset = 8 + 6 + ( 12 * (uint)exif_info->ifd0_tagcount);
        memcpy(exif_info->writing.buf, exif_subifd_tag, 8);
        put_uint32(exif_info->writing.buf + 8, ifd1_offset);
        exif_info->writing.buf += 12;
    }

    if (exif_info->datetime) {
        memcpy(exif_info->writing.buf, exif_tzoffset_tag, 12);
        put_sint16(exif_info->writing.buf+8
            , (int)(exif_info->timestamp_tm.tm_gmtoff / 3600));
        exif_info->writing.buf += 12;
    }

    put_uint32(exif_info->writing.buf, 0); /* Next IFD offset = 0 (no next IFD) */
    exif_info->writing.buf += 4;

}

void jpgutl_exif_writeifd1(ctx_exif_info *exif_info)
{
    /* Write IFD 1 */
    if (exif_info->ifd1_tagcount > 0) {
        /* (remember that the tags in any IFD must be in numerical order by tag) */
        put_uint16(exif_info->writing.buf, (uint)exif_info->ifd1_tagcount);
        memcpy(exif_info->writing.buf + 2, exif_version_tag, 12); /* tag 0x9000 */
        exif_info->writing.buf += 14;

        if (exif_info->datetime) {
            put_stringentry(&exif_info->writing
                , EXIF_TAG_ORIGINAL_DATETIME, exif_info->datetime, 1);
        }

        if (exif_info->box) {
            put_subjectarea(&exif_info->writing, exif_info->box);
        }

        if (exif_info->subtime) {
            put_stringentry(&exif_info->writing
                , EXIF_TAG_ORIGINAL_DATETIME_SS, exif_info->subtime, 0);
        }

        put_uint32(exif_info->writing.buf, 0); /* Next IFD = 0 (no next IFD) */
        exif_info->writing.buf += 4;
    }

}

uint jpgutl_exif(u_char **exif, cls_camera *cam, timespec *ts_in1, ctx_coord *box)
{
    struct ctx_exif_info *exif_info;
    uint buffer_size;
    uint marker_len;
    JOCTET *marker;

    exif_info = (ctx_exif_info*)mymalloc(sizeof(ctx_exif_info));
    memset(exif_info, 0, sizeof(sizeof(ctx_exif_info)));
    exif_info->cam = cam;
    exif_info->ts_in1 = ts_in1;
    exif_info->box = box;

    jpgutl_exif_date(exif_info);

    jpgutl_exif_tags(exif_info);

    if (exif_info->ifds_size == 0) {
        return 0;
    }

    buffer_size = 14 + /* EXIF and TIFF headers */
        exif_info->ifds_size + exif_info->datasize;

    marker =(JOCTET *)mymalloc(buffer_size);
    memcpy(marker, exif_marker_start, 14); /* EXIF and TIFF headers */

    exif_info->writing.base = marker + 6; /* base address for intra-TIFF offsets */
    exif_info->writing.buf = marker + 14; /* current write position */
    exif_info->writing.data_offset =(uint)(8 + exif_info->ifds_size); /* where to start storing data */

    jpgutl_exif_writeifd0(exif_info);
    jpgutl_exif_writeifd1(exif_info);

    marker_len = exif_info->writing.data_offset + 6;

    myfree(exif_info->description);
    myfree(exif_info->datetime);

    myfree(exif_info);

    *exif = marker;

    return marker_len;
}

struct jpgutl_error_mgr {
    struct jpeg_error_mgr pub;   /* "public" fields */
    jmp_buf setjmp_buffer;       /* For return to caller */

    /* Original emit_message method. */
    JMETHOD(void, original_emit_message, (j_common_ptr cinfo, int msg_level));
    /* Was a corrupt-data warning seen. */
    int warning_seen;
};

/*  These huffman tables are required by the old jpeg libs included with 14.04 */
static void add_huff_table(j_decompress_ptr dinfo, JHUFF_TBL **htblptr, const UINT8 *bits, const UINT8 *val){
/* Define a Huffman table */
    int nsymbols, len;

    if (*htblptr == NULL) {
        *htblptr = jpeg_alloc_huff_table((j_common_ptr) dinfo);
    }

    /* Copy the number-of-symbols-of-each-code-length counts. */
    memcpy((*htblptr)->bits, bits, sizeof((*htblptr)->bits));

    /*
     * Validate the counts.  We do this here mainly so we can copy the right
     * number of symbols from the val[] array, without risking marching off
     * the end of memory.  jchuff.c will do a more thorough test later.
     */
    nsymbols = 0;

    for (len = 1; len <= 16; len++)
        nsymbols += bits[len];

    if (nsymbols < 1 || nsymbols > 256) {
        MOTION_LOG(ERR, TYPE_ALL, NO_ERRNO, _("%s: Given jpeg buffer was too small"));
    }

    memcpy((*htblptr)->huffval, val, (uint)nsymbols * sizeof(UINT8));
}

static void std_huff_tables (j_decompress_ptr dinfo){
/* Set up the standard Huffman tables (cf. JPEG standard section K.3) */
/* IMPORTANT: these are only valid for 8-bit data precision! */

    static const UINT8 bits_dc_luminance[17] =
    { /* 0-base */ 0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
    static const UINT8 val_dc_luminance[] =
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

    static const UINT8 bits_dc_chrominance[17] =
    { /* 0-base */ 0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
    static const UINT8 val_dc_chrominance[] =
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

    static const UINT8 bits_ac_luminance[17] =
    { /* 0-base */ 0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
    static const UINT8 val_ac_luminance[] =
    { 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
      0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
      0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
      0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
      0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
      0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
      0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
      0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
      0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
      0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
      0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
      0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
      0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
      0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
      0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
      0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
      0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
      0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
      0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
      0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
      0xf9, 0xfa };

    static const UINT8 bits_ac_chrominance[17] =
    { /* 0-base */ 0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
    static const UINT8 val_ac_chrominance[] =
    { 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
      0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
      0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
      0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
      0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
      0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
      0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
      0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
      0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
      0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
      0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
      0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
      0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
      0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
      0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
      0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
      0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
      0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
      0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
      0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
      0xf9, 0xfa };

    add_huff_table(dinfo, &dinfo->dc_huff_tbl_ptrs[0],
                   bits_dc_luminance, val_dc_luminance);
    add_huff_table(dinfo, &dinfo->ac_huff_tbl_ptrs[0],
                   bits_ac_luminance, val_ac_luminance);
    add_huff_table(dinfo, &dinfo->dc_huff_tbl_ptrs[1],
                   bits_dc_chrominance, val_dc_chrominance);
    add_huff_table(dinfo, &dinfo->ac_huff_tbl_ptrs[1],
                   bits_ac_chrominance, val_ac_chrominance);
}

static void guarantee_huff_tables(j_decompress_ptr dinfo)
{
    if ((dinfo->dc_huff_tbl_ptrs[0] == NULL) &&
        (dinfo->dc_huff_tbl_ptrs[1] == NULL) &&
        (dinfo->ac_huff_tbl_ptrs[0] == NULL) &&
        (dinfo->ac_huff_tbl_ptrs[1] == NULL)) {
        std_huff_tables(dinfo);
    }
}

/*
 * Initialize source --- called by jpeg_read_header
 * before any data is actually read.
 */
static void jpgutl_init_source(j_decompress_ptr cinfo)
{
    (void)cinfo;
    /* No work necessary here */
}

/*
 * Fill the input buffer --- called whenever buffer is emptied.
 *
 * Should never be called since all data should be already provided.
 * Is nevertheless sometimes called - sets the input buffer to data
 * which is the JPEG EOI marker;
 *
 */
static boolean jpgutl_fill_input_buffer(j_decompress_ptr cinfo)
{
    cinfo->src->next_input_byte = EOI_data;
    cinfo->src->bytes_in_buffer = 2;
    return TRUE;
}

/*
 * Skip data --- used to skip over a potentially large amount of
 * uninteresting data (such as an APPn marker).
 *
 */
static void jpgutl_skip_data(j_decompress_ptr cinfo, long num_bytes)
{
    if (num_bytes > 0) {
        if (num_bytes > (long) cinfo->src->bytes_in_buffer) {
            num_bytes = (long) cinfo->src->bytes_in_buffer;
        }
        cinfo->src->next_input_byte += (size_t) num_bytes;
        cinfo->src->bytes_in_buffer -= (size_t) num_bytes;
    }
}

/*
 * Terminate source --- called by jpeg_finish_decompress
 * after all data has been read.  Often a no-op.
 */
static void jpgutl_term_source(j_decompress_ptr cinfo)
{
    (void)cinfo;
    /* No work necessary here */
}

/*
 * The source object and input buffer are made permanent so that a series
 * of JPEG images can be read from the same buffer by calling jpgutl_buffer_src
 * only before the first one.  (If we discarded the buffer at the end of
 * one image, we'd likely lose the start of the next one.)
 * This makes it unsafe to use this manager and a different source
 * manager serially with the same JPEG object.  Caveat programmer.
 */
/**
 * jpgutl_buffer_src
 *  Purpose:
 *    Establish the input buffer source for the JPEG libary and associated helper functions.
 *  Parameters:
 *    cinfo      The jpeg library compression/decompression information
 *    buffer     The buffer of JPEG data to decompress.
 *    buffer_len The length of the buffer.
 *  Return values:
 *    None
 */
static void jpgutl_buffer_src(j_decompress_ptr cinfo, u_char *buffer, long buffer_len)
{

    if (cinfo->src == NULL) {    /* First time for this JPEG object? */
        cinfo->src = (struct jpeg_source_mgr *)
                     (*cinfo->mem->alloc_small) ((j_common_ptr) cinfo, JPOOL_PERMANENT,
                     sizeof (struct jpeg_source_mgr));
    }

    cinfo->src->init_source = jpgutl_init_source;
    cinfo->src->fill_input_buffer = jpgutl_fill_input_buffer;
    cinfo->src->skip_input_data = jpgutl_skip_data;
    cinfo->src->resync_to_restart = jpeg_resync_to_restart;    /* Use default method */
    cinfo->src->term_source = jpgutl_term_source;
    cinfo->src->bytes_in_buffer = (ulong)buffer_len;
    cinfo->src->next_input_byte = (JOCTET *) buffer;


}

/**
 * jpgutl_error_exit
 *  Purpose:
 *    Exit routine for errors thrown by JPEG library.
 *  Parameters:
 *    cinfo      The jpeg library compression/decompression information
 *  Return values:
 *    None
 */
static void jpgutl_error_exit(j_common_ptr cinfo)
{
    char buffer[JMSG_LENGTH_MAX];

    /* cinfo->err really points to a jpgutl_error_mgr struct, so coerce pointer. */
    struct jpgutl_error_mgr *myerr = (struct jpgutl_error_mgr *) cinfo->err;

    /*
     * Always display the message.
     * We could postpone this until after returning, if we chose.
     */
    (*cinfo->err->format_message) (cinfo, buffer);

    MOTION_LOG(ERR, TYPE_ALL, NO_ERRNO, "%s", buffer);

    /* Return control to the setjmp point. */
    longjmp (myerr->setjmp_buffer, 1);
}

/**
 * jpgutl_emit_message
 *  Purpose:
 *    Process the messages thrown by the JPEG library
 *  Parameters:
 *    cinfo      The jpeg library compression/decompression information
 *    msg_level  Integer indicating the severity of the message.
 *  Return values:
 *    None
 */
static void jpgutl_emit_message(j_common_ptr cinfo, int msg_level)
{
    char buffer[JMSG_LENGTH_MAX];
    /* cinfo->err really points to a jpgutl_error_mgr struct, so coerce pointer. */
    struct jpgutl_error_mgr *myerr = (struct jpgutl_error_mgr *) cinfo->err;
    /*
     *  The JWRN_EXTRANEOUS_DATA is sent a lot without any particular negative effect.
     *  There are some messages above zero but they are just informational and not something
     *  that we are interested in.
    */
    if ((cinfo->err->msg_code != JWRN_EXTRANEOUS_DATA) && (msg_level < 0) ) {
        myerr->warning_seen++ ;
        (*cinfo->err->format_message) (cinfo, buffer);
            MOTION_LOG(DBG, TYPE_VIDEO, NO_ERRNO, "msg_level: %d, %s", msg_level, buffer);
    }

}

/*
 * The following declarations and 5 functions are jpeg related
 * functions used by put_jpeg_grey_memory and put_jpeg_yuv420p_memory.
 */
typedef struct {
    struct jpeg_destination_mgr pub;
    JOCTET *buf;
    size_t bufsize;
    size_t jpegsize;
} mem_destination_mgr;

typedef mem_destination_mgr *mem_dest_ptr;


METHODDEF(void) init_destination(j_compress_ptr cinfo)
{
    mem_dest_ptr dest = (mem_dest_ptr) cinfo->dest;
    dest->pub.next_output_byte = dest->buf;
    dest->pub.free_in_buffer = dest->bufsize;
    dest->jpegsize = 0;
}

METHODDEF(boolean) empty_output_buffer(j_compress_ptr cinfo)
{
    mem_dest_ptr dest = (mem_dest_ptr) cinfo->dest;
    dest->pub.next_output_byte = dest->buf;
    dest->pub.free_in_buffer = dest->bufsize;

    return FALSE;
    ERREXIT(cinfo, JERR_BUFFER_SIZE);
}

METHODDEF(void) term_destination(j_compress_ptr cinfo)
{
    mem_dest_ptr dest = (mem_dest_ptr) cinfo->dest;
    dest->jpegsize = dest->bufsize - dest->pub.free_in_buffer;
}

static GLOBAL(void) _jpeg_mem_dest(j_compress_ptr cinfo, JOCTET* buf, size_t bufsize)
{
    mem_dest_ptr dest;

    if (cinfo->dest == NULL) {
        cinfo->dest = (struct jpeg_destination_mgr *)
                      (*cinfo->mem->alloc_small)((j_common_ptr)cinfo, JPOOL_PERMANENT,
                       sizeof(mem_destination_mgr));
    }

    dest = (mem_dest_ptr) cinfo->dest;

    dest->pub.init_destination    = init_destination;
    dest->pub.empty_output_buffer = empty_output_buffer;
    dest->pub.term_destination    = term_destination;

    dest->buf      = buf;
    dest->bufsize  = bufsize;
    dest->jpegsize = 0;
}

static GLOBAL(int) _jpeg_mem_size(j_compress_ptr cinfo)
{
    mem_dest_ptr dest = (mem_dest_ptr) cinfo->dest;
    return (int)dest->jpegsize;
}

/*
 * put_jpeg_exif writes the EXIF APP1 chunk to the jpeg file.
 * It must be called after jpeg_start_compress() but before
 * any image data is written by jpeg_write_scanlines().
 */
static void put_jpeg_exif(j_compress_ptr cinfo, cls_camera *cam,
        timespec *ts1, ctx_coord *box)
{
    u_char *exif = NULL;
    uint exif_len = jpgutl_exif(&exif, cam, ts1, box);

    if(exif_len > 0) {
        /* EXIF data lives in a JPEG APP1 marker */
        jpeg_write_marker(cinfo, JPEG_APP0 + 1, exif, exif_len);
        free(exif);
    }
}


/**
 * jpgutl_decode_jpeg
 *  Purpose:  Decompress the jpeg data_in into the img_out buffer.
 *
 *  Parameters:
 *  jpeg_data_in     The jpeg data sent in
 *  jpeg_data_len    The length of the jpeg data
 *  width            The width of the image
 *  height           The height of the image
 *  img_out          Pointer to the image output
 *
 *  Return Values
 *    Success 0, Failure -1
 */
int jpgutl_decode_jpeg (u_char *jpeg_data_in, int jpeg_data_len,
        uint width, uint height, u_char *volatile img_out)
{
    JSAMPARRAY      line;           /* Array of decomp data lines */
    u_char  *wline;          /* Will point to line[0] */
    uint    i;
    u_char  *img_y, *img_cb, *img_cr;
    u_char   offset_y;

    struct jpeg_decompress_struct dinfo;
    struct jpgutl_error_mgr jerr;

    /* We set up the normal JPEG error routines, then override error_exit. */
    dinfo.err = jpeg_std_error (&jerr.pub);
    jerr.pub.error_exit = jpgutl_error_exit;
    /* Also hook the emit_message routine to note corrupt-data warnings. */
    jerr.original_emit_message = jerr.pub.emit_message;
    jerr.pub.emit_message = jpgutl_emit_message;
    jerr.warning_seen = 0;

    jpeg_create_decompress (&dinfo);

    /* Establish the setjmp return context for jpgutl_error_exit to use. */
    if (setjmp (jerr.setjmp_buffer)) {
        /* If we get here, the JPEG code has signaled an error. */
        jpeg_destroy_decompress (&dinfo);
        return -1;
    }

    jpgutl_buffer_src (&dinfo, jpeg_data_in, jpeg_data_len);

    jpeg_read_header (&dinfo, TRUE);

    //420 sampling is the default for YCbCr so no need to override.
    dinfo.out_color_space = JCS_YCbCr;
    dinfo.dct_method = JDCT_DEFAULT;
    guarantee_huff_tables(&dinfo);  /* Required by older versions of the jpeg libs */
    jpeg_start_decompress (&dinfo);

    if ((dinfo.output_width == 0) || (dinfo.output_height == 0)) {
        MOTION_LOG(WRN, TYPE_VIDEO, NO_ERRNO,_("Invalid JPEG image dimensions"));
        jpeg_destroy_decompress(&dinfo);
        return -1;
    }

    if ((dinfo.output_width != width) || (dinfo.output_height != height)) {
        MOTION_LOG(WRN, TYPE_VIDEO, NO_ERRNO
            ,_("JPEG image size %dx%d, JPEG was %dx%d")
            ,width, height, dinfo.output_width, dinfo.output_height);
        jpeg_destroy_decompress(&dinfo);
        return -1;
    }

    img_y  = img_out;
    img_cb = img_y + dinfo.output_width * dinfo.output_height;
    img_cr = img_cb + (dinfo.output_width * dinfo.output_height) / 4;

    /* Allocate space for one line. */
    line = (*dinfo.mem->alloc_sarray)
        ((j_common_ptr) &dinfo, JPOOL_IMAGE
        ,dinfo.output_width * (uint)dinfo.output_components, 1);

    wline = line[0];
    offset_y = 0;

    while (dinfo.output_scanline < dinfo.output_height) {
        jpeg_read_scanlines(&dinfo, line, 1);

        for (i = 0; i < (dinfo.output_width * 3); i += 3) {
            img_y[i / 3] = wline[i];
            if (i & 1) {
                img_cb[(i / 3) / 2] = wline[i + 1];
                img_cr[(i / 3) / 2] = wline[i + 2];
            }
        }

        img_y += dinfo.output_width;

        if (offset_y++ & 1) {
            img_cb += dinfo.output_width / 2;
            img_cr += dinfo.output_width / 2;
        }
    }

    jpeg_finish_decompress(&dinfo);
    jpeg_destroy_decompress(&dinfo);

    /*
     * If there are too many warnings, this means that
     * only a partial image could be returned which would
     * trigger many false positive motion detections
    */
    if (jerr.warning_seen > 2) {
        return -1;
    }

    return 0;

}

int jpgutl_put_yuv420p(u_char *dest_image, int image_size,
        u_char *input_image, int width, int height, int quality,
        cls_camera *cam, timespec *ts1, ctx_coord *box)

{
    int i, j, jpeg_image_size;

    JSAMPROW y[16],cb[16],cr[16]; // y[2][5] = color sample of row 2 and pixel column 5; (one plane)
    JSAMPARRAY data[3]; // t[0][2][5] = color sample 0 of row 2 and column 5

    struct jpeg_compress_struct cinfo;
    struct jpgutl_error_mgr jerr;

    data[0] = y;
    data[1] = cb;
    data[2] = cr;

    cinfo.err = jpeg_std_error (&jerr.pub);
    jerr.pub.error_exit = jpgutl_error_exit;
    /* Also hook the emit_message routine to note corrupt-data warnings. */
    jerr.original_emit_message = jerr.pub.emit_message;
    jerr.pub.emit_message = jpgutl_emit_message;
    jerr.warning_seen = 0;

    jpeg_create_compress(&cinfo);

    /* Establish the setjmp return context for jpgutl_error_exit to use. */
    if (setjmp (jerr.setjmp_buffer)) {
        /* If we get here, the JPEG code has signaled an error. */
        jpeg_destroy_compress (&cinfo);
        return -1;
    }

    cinfo.image_width = (uint)width;
    cinfo.image_height = (uint)height;
    cinfo.input_components = 3;
    jpeg_set_defaults(&cinfo);

    jpeg_set_colorspace(&cinfo, JCS_YCbCr);

    cinfo.raw_data_in = TRUE; // Supply downsampled data
    #if JPEG_LIB_VERSION >= 70
        cinfo.do_fancy_downsampling = FALSE;  // Fix segfault with v7
    #endif
    cinfo.comp_info[0].h_samp_factor = 2;
    cinfo.comp_info[0].v_samp_factor = 2;
    cinfo.comp_info[1].h_samp_factor = 1;
    cinfo.comp_info[1].v_samp_factor = 1;
    cinfo.comp_info[2].h_samp_factor = 1;
    cinfo.comp_info[2].v_samp_factor = 1;

    jpeg_set_quality(&cinfo, quality, TRUE);
    cinfo.dct_method = JDCT_FASTEST;

    _jpeg_mem_dest(&cinfo, dest_image, (uint)image_size);

    jpeg_start_compress(&cinfo, TRUE);

    if (cam != NULL) {
        put_jpeg_exif(&cinfo, cam, ts1, box);
    }

    /* If the image is not a multiple of 16, this overruns the buffers
     * we'll just pad those last bytes with zeros
     */
    for (j = 0; j < height; j += 16) {
        for (i = 0; i < 16; i++) {
            if ((width * (i + j)) < (width * height)) {
                y[i] = input_image + width * (i + j);
                if (i % 2 == 0) {
                    cb[i / 2] = input_image + width * height + width / 2 * ((i + j) /2);
                    cr[i / 2] = input_image + width * height + width * height / 4 + width / 2 * ((i + j) / 2);
                }
            } else {
                y[i] = 0x00;
                cb[i] = 0x00;
                cr[i] = 0x00;
            }
        }
        jpeg_write_raw_data(&cinfo, data, 16);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_image_size = _jpeg_mem_size(&cinfo);
    jpeg_destroy_compress(&cinfo);

    return jpeg_image_size;
}


int jpgutl_put_grey(u_char *dest_image, int image_size,
        u_char *input_image, int width, int height, int quality,
        cls_camera *cam, timespec *ts1, ctx_coord *box)
{
    int y, dest_image_size;
    JSAMPROW row_ptr[1];
    struct jpeg_compress_struct cjpeg;
    struct jpgutl_error_mgr jerr;

    cjpeg.err = jpeg_std_error (&jerr.pub);
    jerr.pub.error_exit = jpgutl_error_exit;
    /* Also hook the emit_message routine to note corrupt-data warnings. */
    jerr.original_emit_message = jerr.pub.emit_message;
    jerr.pub.emit_message = jpgutl_emit_message;
    jerr.warning_seen = 0;

    jpeg_create_compress(&cjpeg);

    /* Establish the setjmp return context for jpgutl_error_exit to use. */
    if (setjmp (jerr.setjmp_buffer)) {
        /* If we get here, the JPEG code has signaled an error. */
        jpeg_destroy_compress (&cjpeg);
        return -1;
    }

    cjpeg.image_width = (uint)width;
    cjpeg.image_height = (uint)height;
    cjpeg.input_components = 1; /* One colour component */
    cjpeg.in_color_space = JCS_GRAYSCALE;

    jpeg_set_defaults(&cjpeg);

    jpeg_set_quality(&cjpeg, quality, TRUE);
    cjpeg.dct_method = JDCT_FASTEST;
    _jpeg_mem_dest(&cjpeg, dest_image, (uint)image_size);

    jpeg_start_compress (&cjpeg, TRUE);

    if (cam != NULL) {
        put_jpeg_exif(&cjpeg, cam, ts1, box);
    }

    row_ptr[0] = input_image;

    for (y = 0; y < height; y++) {
        jpeg_write_scanlines(&cjpeg, row_ptr, 1);
        row_ptr[0] += width;
    }

    jpeg_finish_compress(&cjpeg);
    dest_image_size = _jpeg_mem_size(&cjpeg);
    jpeg_destroy_compress(&cjpeg);

    return dest_image_size;
}

