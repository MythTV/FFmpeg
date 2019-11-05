/*
 * PNG image format
 * Copyright (c) 2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "avcodec.h"
#include "internal.h"
#include "bytestream.h"
#include "lossless_videoencdsp.h"
#include "png.h"
#include "apng.h"

#include "libavutil/avassert.h"
#include "libavutil/crc.h"
#include "libavutil/libm.h"
#include "libavutil/opt.h"
#include "libavutil/color_utils.h"
#include "libavutil/stereo3d.h"

#include <zlib.h>

#define IOBUF_SIZE 4096

typedef struct APNGFctlChunk {
    uint32_t sequence_number;
    uint32_t width, height;
    uint32_t x_offset, y_offset;
    uint16_t delay_num, delay_den;
    uint8_t dispose_op, blend_op;
} APNGFctlChunk;

typedef struct PNGEncContext {
    AVClass *class;
    LLVidEncDSPContext llvidencdsp;

    uint8_t *bytestream;
    uint8_t *bytestream_start;
    uint8_t *bytestream_end;

    int filter_type;

    z_stream zstream;
    uint8_t buf[IOBUF_SIZE];
    int dpi;                     ///< Physical pixel density, in dots per inch, if set
    int dpm;                     ///< Physical pixel density, in dots per meter, if set

    int is_progressive;
    int bit_depth;
    int color_type;
    int bits_per_pixel;

    // APNG
    uint32_t palette_checksum;   // Used to ensure a single unique palette
    uint32_t sequence_number;
    int extra_data_updated;
    uint8_t *extra_data;
    int extra_data_size;

    AVFrame *prev_frame;
    AVFrame *last_frame;
    APNGFctlChunk last_frame_fctl;
    uint8_t *last_frame_packet;
    size_t last_frame_packet_size;
} PNGEncContext;

static void png_get_interlaced_row(uint8_t *dst, int row_size,
                                   int bits_per_pixel, int pass,
                                   const uint8_t *src, int width)
{
    int x, mask, dst_x, j, b, bpp;
    uint8_t *d;
    const uint8_t *s;
    static const int masks[] = {0x80, 0x08, 0x88, 0x22, 0xaa, 0x55, 0xff};

    mask = masks[pass];
    switch (bits_per_pixel) {
    case 1:
        memset(dst, 0, row_size);
        dst_x = 0;
        for (x = 0; x < width; x++) {
            j = (x & 7);
            if ((mask << j) & 0x80) {
                b = (src[x >> 3] >> (7 - j)) & 1;
                dst[dst_x >> 3] |= b << (7 - (dst_x & 7));
                dst_x++;
            }
        }
        break;
    default:
        bpp = bits_per_pixel >> 3;
        d = dst;
        s = src;
        for (x = 0; x < width; x++) {
            j = x & 7;
            if ((mask << j) & 0x80) {
                memcpy(d, s, bpp);
                d += bpp;
            }
            s += bpp;
        }
        break;
    }
}

static void sub_png_paeth_prediction(uint8_t *dst, uint8_t *src, uint8_t *top,
                                     int w, int bpp)
{
    int i;
    for (i = 0; i < w; i++) {
        int a, b, c, p, pa, pb, pc;

        a = src[i - bpp];
        b = top[i];
        c = top[i - bpp];

        p  = b - c;
        pc = a - c;

        pa = abs(p);
        pb = abs(pc);
        pc = abs(p + pc);

        if (pa <= pb && pa <= pc)
            p = a;
        else if (pb <= pc)
            p = b;
        else
            p = c;
        dst[i] = src[i] - p;
    }
}

static void sub_left_prediction(PNGEncContext *c, uint8_t *dst, const uint8_t *src, int bpp, int size)
{
    const uint8_t *src1 = src + bpp;
    const uint8_t *src2 = src;
    int x, unaligned_w;

    memcpy(dst, src, bpp);
    dst += bpp;
    size -= bpp;
    unaligned_w = FFMIN(32 - bpp, size);
    for (x = 0; x < unaligned_w; x++)
        *dst++ = *src1++ - *src2++;
    size -= unaligned_w;
    c->llvidencdsp.diff_bytes(dst, src1, src2, size);
}

static void png_filter_row(PNGEncContext *c, uint8_t *dst, int filter_type,
                           uint8_t *src, uint8_t *top, int size, int bpp)
{
    int i;

    switch (filter_type) {
    case PNG_FILTER_VALUE_NONE:
        memcpy(dst, src, size);
        break;
    case PNG_FILTER_VALUE_SUB:
        sub_left_prediction(c, dst, src, bpp, size);
        break;
    case PNG_FILTER_VALUE_UP:
        c->llvidencdsp.diff_bytes(dst, src, top, size);
        break;
    case PNG_FILTER_VALUE_AVG:
        for (i = 0; i < bpp; i++)
            dst[i] = src[i] - (top[i] >> 1);
        for (; i < size; i++)
            dst[i] = src[i] - ((src[i - bpp] + top[i]) >> 1);
        break;
    case PNG_FILTER_VALUE_PAETH:
        for (i = 0; i < bpp; i++)
            dst[i] = src[i] - top[i];
        sub_png_paeth_prediction(dst + i, src + i, top + i, size - i, bpp);
        break;
    }
}

static uint8_t *png_choose_filter(PNGEncContext *s, uint8_t *dst,
                                  uint8_t *src, uint8_t *top, int size, int bpp)
{
    int pred = s->filter_type;
    av_assert0(bpp || !pred);
    if (!top && pred)
        pred = PNG_FILTER_VALUE_SUB;
    if (pred == PNG_FILTER_VALUE_MIXED) {
        int i;
        int cost, bcost = INT_MAX;
        uint8_t *buf1 = dst, *buf2 = dst + size + 16;
        for (pred = 0; pred < 5; pred++) {
            png_filter_row(s, buf1 + 1, pred, src, top, size, bpp);
            buf1[0] = pred;
            cost = 0;
            for (i = 0; i <= size; i++)
                cost += abs((int8_t) buf1[i]);
            if (cost < bcost) {
                bcost = cost;
                FFSWAP(uint8_t *, buf1, buf2);
            }
        }
        return buf2;
    } else {
        png_filter_row(s, dst + 1, pred, src, top, size, bpp);
        dst[0] = pred;
        return dst;
    }
}

static void png_write_chunk(uint8_t **f, uint32_t tag,
                            const uint8_t *buf, int length)
{
    const AVCRC *crc_table = av_crc_get_table(AV_CRC_32_IEEE_LE);
    uint32_t crc = ~0U;
    uint8_t tagbuf[4];

    bytestream_put_be32(f, length);
    AV_WL32(tagbuf, tag);
    crc = av_crc(crc_table, crc, tagbuf, 4);
    bytestream_put_be32(f, av_bswap32(tag));
    if (length > 0) {
        crc = av_crc(crc_table, crc, buf, length);
        memcpy(*f, buf, length);
        *f += length;
    }
    bytestream_put_be32(f, ~crc);
}

static void png_write_image_data(AVCodecContext *avctx,
                                 const uint8_t *buf, int length)
{
    PNGEncContext *s = avctx->priv_data;
    const AVCRC *crc_table = av_crc_get_table(AV_CRC_32_IEEE_LE);
    uint32_t crc = ~0U;

    if (avctx->codec_id == AV_CODEC_ID_PNG || avctx->frame_number == 0) {
        png_write_chunk(&s->bytestream, MKTAG('I', 'D', 'A', 'T'), buf, length);
        return;
    }

    bytestream_put_be32(&s->bytestream, length + 4);

    bytestream_put_be32(&s->bytestream, MKBETAG('f', 'd', 'A', 'T'));
    bytestream_put_be32(&s->bytestream, s->sequence_number);
    crc = av_crc(crc_table, crc, s->bytestream - 8, 8);

    crc = av_crc(crc_table, crc, buf, length);
    memcpy(s->bytestream, buf, length);
    s->bytestream += length;

    bytestream_put_be32(&s->bytestream, ~crc);

    ++s->sequence_number;
}

/* XXX: do filtering */
static int png_write_row(AVCodecContext *avctx, const uint8_t *data, int size)
{
    PNGEncContext *s = avctx->priv_data;
    int ret;

    s->zstream.avail_in = size;
    s->zstream.next_in  = data;
    while (s->zstream.avail_in > 0) {
        ret = deflate(&s->zstream, Z_NO_FLUSH);
        if (ret != Z_OK)
            return -1;
        if (s->zstream.avail_out == 0) {
            if (s->bytestream_end - s->bytestream > IOBUF_SIZE + 100)
                png_write_image_data(avctx, s->buf, IOBUF_SIZE);
            s->zstream.avail_out = IOBUF_SIZE;
            s->zstream.next_out  = s->buf;
        }
    }
    return 0;
}

#define AV_WB32_PNG(buf, n) AV_WB32(buf, lrint((n) * 100000))
static int png_get_chrm(enum AVColorPrimaries prim,  uint8_t *buf)
{
    double rx, ry, gx, gy, bx, by, wx = 0.3127, wy = 0.3290;
    switch (prim) {
        case AVCOL_PRI_BT709:
            rx = 0.640; ry = 0.330;
            gx = 0.300; gy = 0.600;
            bx = 0.150; by = 0.060;
            break;
        case AVCOL_PRI_BT470M:
            rx = 0.670; ry = 0.330;
            gx = 0.210; gy = 0.710;
            bx = 0.140; by = 0.080;
            wx = 0.310; wy = 0.316;
            break;
        case AVCOL_PRI_BT470BG:
            rx = 0.640; ry = 0.330;
            gx = 0.290; gy = 0.600;
            bx = 0.150; by = 0.060;
            break;
        case AVCOL_PRI_SMPTE170M:
        case AVCOL_PRI_SMPTE240M:
            rx = 0.630; ry = 0.340;
            gx = 0.310; gy = 0.595;
            bx = 0.155; by = 0.070;
            break;
        case AVCOL_PRI_BT2020:
            rx = 0.708; ry = 0.292;
            gx = 0.170; gy = 0.797;
            bx = 0.131; by = 0.046;
            break;
        default:
            return 0;
    }

    AV_WB32_PNG(buf     , wx); AV_WB32_PNG(buf + 4 , wy);
    AV_WB32_PNG(buf + 8 , rx); AV_WB32_PNG(buf + 12, ry);
    AV_WB32_PNG(buf + 16, gx); AV_WB32_PNG(buf + 20, gy);
    AV_WB32_PNG(buf + 24, bx); AV_WB32_PNG(buf + 28, by);
    return 1;
}

static int png_get_gama(enum AVColorTransferCharacteristic trc, uint8_t *buf)
{
    double gamma = avpriv_get_gamma_from_trc(trc);
    if (gamma <= 1e-6)
        return 0;

    AV_WB32_PNG(buf, 1.0 / gamma);
    return 1;
}

static int encode_headers(AVCodecContext *avctx, const AVFrame *pict)
{
    AVFrameSideData *side_data;
    PNGEncContext *s = avctx->priv_data;

    /* write png header */
    AV_WB32(s->buf, avctx->width);
    AV_WB32(s->buf + 4, avctx->height);
    s->buf[8]  = s->bit_depth;
    s->buf[9]  = s->color_type;
    s->buf[10] = 0; /* compression type */
    s->buf[11] = 0; /* filter type */
    s->buf[12] = s->is_progressive; /* interlace type */
    png_write_chunk(&s->bytestream, MKTAG('I', 'H', 'D', 'R'), s->buf, 13);

    /* write physical information */
    if (s->dpm) {
      AV_WB32(s->buf, s->dpm);
      AV_WB32(s->buf + 4, s->dpm);
      s->buf[8] = 1; /* unit specifier is meter */
    } else {
      AV_WB32(s->buf, avctx->sample_aspect_ratio.num);
      AV_WB32(s->buf + 4, avctx->sample_aspect_ratio.den);
      s->buf[8] = 0; /* unit specifier is unknown */
    }
    png_write_chunk(&s->bytestream, MKTAG('p', 'H', 'Y', 's'), s->buf, 9);

    /* write stereoscopic information */
    side_data = av_frame_get_side_data(pict, AV_FRAME_DATA_STEREO3D);
    if (side_data) {
        AVStereo3D *stereo3d = (AVStereo3D *)side_data->data;
        switch (stereo3d->type) {
            case AV_STEREO3D_SIDEBYSIDE:
                s->buf[0] = ((stereo3d->flags & AV_STEREO3D_FLAG_INVERT) == 0) ? 1 : 0;
                png_write_chunk(&s->bytestream, MKTAG('s', 'T', 'E', 'R'), s->buf, 1);
                break;
            case AV_STEREO3D_2D:
                break;
            default:
                av_log(avctx, AV_LOG_WARNING, "Only side-by-side stereo3d flag can be defined within sTER chunk\n");
                break;
        }
    }

    /* write colorspace information */
    if (pict->color_primaries == AVCOL_PRI_BT709 &&
        pict->color_trc == AVCOL_TRC_IEC61966_2_1) {
        s->buf[0] = 1; /* rendering intent, relative colorimetric by default */
        png_write_chunk(&s->bytestream, MKTAG('s', 'R', 'G', 'B'), s->buf, 1);
    }

    if (png_get_chrm(pict->color_primaries, s->buf))
        png_write_chunk(&s->bytestream, MKTAG('c', 'H', 'R', 'M'), s->buf, 32);
    if (png_get_gama(pict->color_trc, s->buf))
        png_write_chunk(&s->bytestream, MKTAG('g', 'A', 'M', 'A'), s->buf, 4);

    /* put the palette if needed */
    if (s->color_type == PNG_COLOR_TYPE_PALETTE) {
        int has_alpha, alpha, i;
        unsigned int v;
        uint32_t *palette;
        uint8_t *ptr, *alpha_ptr;

        palette   = (uint32_t *)pict->data[1];
        ptr       = s->buf;
        alpha_ptr = s->buf + 256 * 3;
        has_alpha = 0;
        for (i = 0; i < 256; i++) {
            v     = palette[i];
            alpha = v >> 24;
            if (alpha != 0xff)
                has_alpha = 1;
            *alpha_ptr++ = alpha;
            bytestream_put_be24(&ptr, v);
        }
        png_write_chunk(&s->bytestream,
                        MKTAG('P', 'L', 'T', 'E'), s->buf, 256 * 3);
        if (has_alpha) {
            png_write_chunk(&s->bytestream,
                            MKTAG('t', 'R', 'N', 'S'), s->buf + 256 * 3, 256);
        }
    }

    return 0;
}

static int encode_frame(AVCodecContext *avctx, const AVFrame *pict)
{
    PNGEncContext *s       = avctx->priv_data;
    const AVFrame *const p = pict;
    int y, len, ret;
    int row_size, pass_row_size;
    uint8_t *ptr, *top, *crow_buf, *crow;
    uint8_t *crow_base       = NULL;
    uint8_t *progressive_buf = NULL;
    uint8_t *top_buf         = NULL;

    row_size = (pict->width * s->bits_per_pixel + 7) >> 3;

    crow_base = av_malloc((row_size + 32) << (s->filter_type == PNG_FILTER_VALUE_MIXED));
    if (!crow_base) {
        ret = AVERROR(ENOMEM);
        goto the_end;
    }
    // pixel data should be aligned, but there's a control byte before it
    crow_buf = crow_base + 15;
    if (s->is_progressive) {
        progressive_buf = av_malloc(row_size + 1);
        top_buf = av_malloc(row_size + 1);
        if (!progressive_buf || !top_buf) {
            ret = AVERROR(ENOMEM);
            goto the_end;
        }
    }

    /* put each row */
    s->zstream.avail_out = IOBUF_SIZE;
    s->zstream.next_out  = s->buf;
    if (s->is_progressive) {
        int pass;

        for (pass = 0; pass < NB_PASSES; pass++) {
            /* NOTE: a pass is completely omitted if no pixels would be
             * output */
            pass_row_size = ff_png_pass_row_size(pass, s->bits_per_pixel, pict->width);
            if (pass_row_size > 0) {
                top = NULL;
                for (y = 0; y < pict->height; y++)
                    if ((ff_png_pass_ymask[pass] << (y & 7)) & 0x80) {
                        ptr = p->data[0] + y * p->linesize[0];
                        FFSWAP(uint8_t *, progressive_buf, top_buf);
                        png_get_interlaced_row(progressive_buf, pass_row_size,
                                               s->bits_per_pixel, pass,
                                               ptr, pict->width);
                        crow = png_choose_filter(s, crow_buf, progressive_buf,
                                                 top, pass_row_size, s->bits_per_pixel >> 3);
                        png_write_row(avctx, crow, pass_row_size + 1);
                        top = progressive_buf;
                    }
            }
        }
    } else {
        top = NULL;
        for (y = 0; y < pict->height; y++) {
            ptr = p->data[0] + y * p->linesize[0];
            crow = png_choose_filter(s, crow_buf, ptr, top,
                                     row_size, s->bits_per_pixel >> 3);
            png_write_row(avctx, crow, row_size + 1);
            top = ptr;
        }
    }
    /* compress last bytes */
    for (;;) {
        ret = deflate(&s->zstream, Z_FINISH);
        if (ret == Z_OK || ret == Z_STREAM_END) {
            len = IOBUF_SIZE - s->zstream.avail_out;
            if (len > 0 && s->bytestream_end - s->bytestream > len + 100) {
                png_write_image_data(avctx, s->buf, len);
            }
            s->zstream.avail_out = IOBUF_SIZE;
            s->zstream.next_out  = s->buf;
            if (ret == Z_STREAM_END)
                break;
        } else {
            ret = -1;
            goto the_end;
        }
    }

    ret = 0;

the_end:
    av_freep(&crow_base);
    av_freep(&progressive_buf);
    av_freep(&top_buf);
    deflateReset(&s->zstream);
    return ret;
}

static int encode_png(AVCodecContext *avctx, AVPacket *pkt,
                      const AVFrame *pict, int *got_packet)
{
    PNGEncContext *s = avctx->priv_data;
    int ret;
    int enc_row_size;
    size_t max_packet_size;

    enc_row_size    = deflateBound(&s->zstream, (avctx->width * s->bits_per_pixel + 7) >> 3);
    max_packet_size =
        AV_INPUT_BUFFER_MIN_SIZE + // headers
        avctx->height * (
            enc_row_size +
            12 * (((int64_t)enc_row_size + IOBUF_SIZE - 1) / IOBUF_SIZE) // IDAT * ceil(enc_row_size / IOBUF_SIZE)
        );
    if (max_packet_size > INT_MAX)
        return AVERROR(ENOMEM);
    ret = ff_alloc_packet2(avctx, pkt, max_packet_size, 0);
    if (ret < 0)
        return ret;

    s->bytestream_start =
    s->bytestream       = pkt->data;
    s->bytestream_end   = pkt->data + pkt->size;

    AV_WB64(s->bytestream, PNGSIG);
    s->bytestream += 8;

    ret = encode_headers(avctx, pict);
    if (ret < 0)
        return ret;

    ret = encode_frame(avctx, pict);
    if (ret < 0)
        return ret;

    png_write_chunk(&s->bytestream, MKTAG('I', 'E', 'N', 'D'), NULL, 0);

    pkt->size = s->bytestream - s->bytestream_start;
    pkt->flags |= AV_PKT_FLAG_KEY;
    *got_packet = 1;

    return 0;
}

static int apng_do_inverse_blend(AVFrame *output, const AVFrame *input,
                                  APNGFctlChunk *fctl_chunk, uint8_t bpp)
{
    // output: background, input: foreground
    // output the image such that when blended with the background, will produce the foreground

    unsigned int x, y;
    unsigned int leftmost_x = input->width;
    unsigned int rightmost_x = 0;
    unsigned int topmost_y = input->height;
    unsigned int bottommost_y = 0;
    const uint8_t *input_data = input->data[0];
    uint8_t *output_data = output->data[0];
    ptrdiff_t input_linesize = input->linesize[0];
    ptrdiff_t output_linesize = output->linesize[0];

    // Find bounding box of changes
    for (y = 0; y < input->height; ++y) {
        for (x = 0; x < input->width; ++x) {
            if (!memcmp(input_data + bpp * x, output_data + bpp * x, bpp))
                continue;

            if (x < leftmost_x)
                leftmost_x = x;
            if (x >= rightmost_x)
                rightmost_x = x + 1;
            if (y < topmost_y)
                topmost_y = y;
            if (y >= bottommost_y)
                bottommost_y = y + 1;
        }

        input_data += input_linesize;
        output_data += output_linesize;
    }

    if (leftmost_x == input->width && rightmost_x == 0) {
        // Empty frame
        // APNG does not support empty frames, so we make it a 1x1 frame
        leftmost_x = topmost_y = 0;
        rightmost_x = bottommost_y = 1;
    }

    // Do actual inverse blending
    if (fctl_chunk->blend_op == APNG_BLEND_OP_SOURCE) {
        output_data = output->data[0];
        for (y = topmost_y; y < bottommost_y; ++y) {
            memcpy(output_data,
                   input->data[0] + input_linesize * y + bpp * leftmost_x,
                   bpp * (rightmost_x - leftmost_x));
            output_data += output_linesize;
        }
    } else { // APNG_BLEND_OP_OVER
        size_t transparent_palette_index;
        uint32_t *palette;

        switch (input->format) {
        case AV_PIX_FMT_RGBA64BE:
        case AV_PIX_FMT_YA16BE:
        case AV_PIX_FMT_RGBA:
        case AV_PIX_FMT_GRAY8A:
            break;

        case AV_PIX_FMT_PAL8:
            palette = (uint32_t*)input->data[1];
            for (transparent_palette_index = 0; transparent_palette_index < 256; ++transparent_palette_index)
                if (palette[transparent_palette_index] >> 24 == 0)
                    break;
            break;

        default:
            // No alpha, so blending not possible
            return -1;
        }

        for (y = topmost_y; y < bottommost_y; ++y) {
            uint8_t *foreground = input->data[0] + input_linesize * y + bpp * leftmost_x;
            uint8_t *background = output->data[0] + output_linesize * y + bpp * leftmost_x;
            output_data = output->data[0] + output_linesize * (y - topmost_y);
            for (x = leftmost_x; x < rightmost_x; ++x, foreground += bpp, background += bpp, output_data += bpp) {
                if (!memcmp(foreground, background, bpp)) {
                    if (input->format == AV_PIX_FMT_PAL8) {
                        if (transparent_palette_index == 256) {
                            // Need fully transparent colour, but none exists
                            return -1;
                        }

                        *output_data = transparent_palette_index;
                    } else {
                        memset(output_data, 0, bpp);
                    }
                    continue;
                }

                // Check for special alpha values, since full inverse
                // alpha-on-alpha blending is rarely possible, and when
                // possible, doesn't compress much better than
                // APNG_BLEND_OP_SOURCE blending
                switch (input->format) {
                case AV_PIX_FMT_RGBA64BE:
                    if (((uint16_t*)foreground)[3] == 0xffff ||
                        ((uint16_t*)background)[3] == 0)
                        break;
                    return -1;

                case AV_PIX_FMT_YA16BE:
                    if (((uint16_t*)foreground)[1] == 0xffff ||
                        ((uint16_t*)background)[1] == 0)
                        break;
                    return -1;

                case AV_PIX_FMT_RGBA:
                    if (foreground[3] == 0xff || background[3] == 0)
                        break;
                    return -1;

                case AV_PIX_FMT_GRAY8A:
                    if (foreground[1] == 0xff || background[1] == 0)
                        break;
                    return -1;

                case AV_PIX_FMT_PAL8:
                    if (palette[*foreground] >> 24 == 0xff ||
                        palette[*background] >> 24 == 0)
                        break;
                    return -1;
                }

                memmove(output_data, foreground, bpp);
            }
        }
    }

    output->width = rightmost_x - leftmost_x;
    output->height = bottommost_y - topmost_y;
    fctl_chunk->width = output->width;
    fctl_chunk->height = output->height;
    fctl_chunk->x_offset = leftmost_x;
    fctl_chunk->y_offset = topmost_y;

    return 0;
}

static int apng_encode_frame(AVCodecContext *avctx, const AVFrame *pict,
                             APNGFctlChunk *best_fctl_chunk, APNGFctlChunk *best_last_fctl_chunk)
{
    PNGEncContext *s = avctx->priv_data;
    int ret;
    unsigned int y;
    AVFrame* diffFrame;
    uint8_t bpp = (s->bits_per_pixel + 7) >> 3;
    uint8_t *original_bytestream, *original_bytestream_end;
    uint8_t *temp_bytestream = 0, *temp_bytestream_end;
    uint32_t best_sequence_number;
    uint8_t *best_bytestream;
    size_t best_bytestream_size = SIZE_MAX;
    APNGFctlChunk last_fctl_chunk = *best_last_fctl_chunk;
    APNGFctlChunk fctl_chunk = *best_fctl_chunk;

    if (avctx->frame_number == 0) {
        best_fctl_chunk->width = pict->width;
        best_fctl_chunk->height = pict->height;
        best_fctl_chunk->x_offset = 0;
        best_fctl_chunk->y_offset = 0;
        best_fctl_chunk->blend_op = APNG_BLEND_OP_SOURCE;
        return encode_frame(avctx, pict);
    }

    diffFrame = av_frame_alloc();
    if (!diffFrame)
        return AVERROR(ENOMEM);

    diffFrame->format = pict->format;
    diffFrame->width = pict->width;
    diffFrame->height = pict->height;
    if ((ret = av_frame_get_buffer(diffFrame, 32)) < 0)
        goto fail;

    original_bytestream = s->bytestream;
    original_bytestream_end = s->bytestream_end;

    temp_bytestream = av_malloc(original_bytestream_end - original_bytestream);
    if (!temp_bytestream) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    temp_bytestream_end = temp_bytestream + (original_bytestream_end - original_bytestream);

    for (last_fctl_chunk.dispose_op = 0; last_fctl_chunk.dispose_op < 3; ++last_fctl_chunk.dispose_op) {
        // 0: APNG_DISPOSE_OP_NONE
        // 1: APNG_DISPOSE_OP_BACKGROUND
        // 2: APNG_DISPOSE_OP_PREVIOUS

        for (fctl_chunk.blend_op = 0; fctl_chunk.blend_op < 2; ++fctl_chunk.blend_op) {
            // 0: APNG_BLEND_OP_SOURCE
            // 1: APNG_BLEND_OP_OVER

            uint32_t original_sequence_number = s->sequence_number, sequence_number;
            uint8_t *bytestream_start = s->bytestream;
            size_t bytestream_size;

            // Do disposal
            if (last_fctl_chunk.dispose_op != APNG_DISPOSE_OP_PREVIOUS) {
                diffFrame->width = pict->width;
                diffFrame->height = pict->height;
                ret = av_frame_copy(diffFrame, s->last_frame);
                if (ret < 0)
                    goto fail;

                if (last_fctl_chunk.dispose_op == APNG_DISPOSE_OP_BACKGROUND) {
                    for (y = last_fctl_chunk.y_offset; y < last_fctl_chunk.y_offset + last_fctl_chunk.height; ++y) {
                        size_t row_start = diffFrame->linesize[0] * y + bpp * last_fctl_chunk.x_offset;
                        memset(diffFrame->data[0] + row_start, 0, bpp * last_fctl_chunk.width);
                    }
                }
            } else {
                if (!s->prev_frame)
                    continue;

                diffFrame->width = pict->width;
                diffFrame->height = pict->height;
                ret = av_frame_copy(diffFrame, s->prev_frame);
                if (ret < 0)
                    goto fail;
            }

            // Do inverse blending
            if (apng_do_inverse_blend(diffFrame, pict, &fctl_chunk, bpp) < 0)
                continue;

            // Do encoding
            ret = encode_frame(avctx, diffFrame);
            sequence_number = s->sequence_number;
            s->sequence_number = original_sequence_number;
            bytestream_size = s->bytestream - bytestream_start;
            s->bytestream = bytestream_start;
            if (ret < 0)
                goto fail;

            if (bytestream_size < best_bytestream_size) {
                *best_fctl_chunk = fctl_chunk;
                *best_last_fctl_chunk = last_fctl_chunk;

                best_sequence_number = sequence_number;
                best_bytestream = s->bytestream;
                best_bytestream_size = bytestream_size;

                if (best_bytestream == original_bytestream) {
                    s->bytestream = temp_bytestream;
                    s->bytestream_end = temp_bytestream_end;
                } else {
                    s->bytestream = original_bytestream;
                    s->bytestream_end = original_bytestream_end;
                }
            }
        }
    }

    s->sequence_number = best_sequence_number;
    s->bytestream = original_bytestream + best_bytestream_size;
    s->bytestream_end = original_bytestream_end;
    if (best_bytestream != original_bytestream)
        memcpy(original_bytestream, best_bytestream, best_bytestream_size);

    ret = 0;

fail:
    av_freep(&temp_bytestream);
    av_frame_free(&diffFrame);
    return ret;
}

static int encode_apng(AVCodecContext *avctx, AVPacket *pkt,
                       const AVFrame *pict, int *got_packet)
{
    PNGEncContext *s = avctx->priv_data;
    int ret;
    int enc_row_size;
    size_t max_packet_size;
    APNGFctlChunk fctl_chunk = {0};

    if (pict && avctx->codec_id == AV_CODEC_ID_APNG && s->color_type == PNG_COLOR_TYPE_PALETTE) {
        uint32_t checksum = ~av_crc(av_crc_get_table(AV_CRC_32_IEEE_LE), ~0U, pict->data[1], 256 * sizeof(uint32_t));

        if (avctx->frame_number == 0) {
            s->palette_checksum = checksum;
        } else if (checksum != s->palette_checksum) {
            av_log(avctx, AV_LOG_ERROR,
                   "Input contains more than one unique palette. APNG does not support multiple palettes.\n");
            return -1;
        }
    }

    enc_row_size    = deflateBound(&s->zstream, (avctx->width * s->bits_per_pixel + 7) >> 3);
    max_packet_size =
        AV_INPUT_BUFFER_MIN_SIZE + // headers
        avctx->height * (
            enc_row_size +
            (4 + 12) * (((int64_t)enc_row_size + IOBUF_SIZE - 1) / IOBUF_SIZE) // fdAT * ceil(enc_row_size / IOBUF_SIZE)
        );
    if (max_packet_size > INT_MAX)
        return AVERROR(ENOMEM);

    if (avctx->frame_number == 0) {
        if (!pict)
            return AVERROR(EINVAL);

        s->bytestream = s->extra_data = av_malloc(AV_INPUT_BUFFER_MIN_SIZE);
        if (!s->extra_data)
            return AVERROR(ENOMEM);

        ret = encode_headers(avctx, pict);
        if (ret < 0)
            return ret;

        s->extra_data_size = s->bytestream - s->extra_data;

        s->last_frame_packet = av_malloc(max_packet_size);
        if (!s->last_frame_packet)
            return AVERROR(ENOMEM);
    } else if (s->last_frame) {
        ret = ff_alloc_packet2(avctx, pkt, max_packet_size, 0);
        if (ret < 0)
            return ret;

        memcpy(pkt->data, s->last_frame_packet, s->last_frame_packet_size);
        pkt->size = s->last_frame_packet_size;
        pkt->pts = pkt->dts = s->last_frame->pts;
    }

    if (pict) {
        s->bytestream_start =
        s->bytestream       = s->last_frame_packet;
        s->bytestream_end   = s->bytestream + max_packet_size;

        // We're encoding the frame first, so we have to do a bit of shuffling around
        // to have the image data write to the correct place in the buffer
        fctl_chunk.sequence_number = s->sequence_number;
        ++s->sequence_number;
        s->bytestream += 26 + 12;

        ret = apng_encode_frame(avctx, pict, &fctl_chunk, &s->last_frame_fctl);
        if (ret < 0)
            return ret;

        fctl_chunk.delay_num = 0; // delay filled in during muxing
        fctl_chunk.delay_den = 0;
    } else {
        s->last_frame_fctl.dispose_op = APNG_DISPOSE_OP_NONE;
    }

    if (s->last_frame) {
        uint8_t* last_fctl_chunk_start = pkt->data;
        uint8_t buf[26];
        if (!s->extra_data_updated) {
            uint8_t *side_data = av_packet_new_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, s->extra_data_size);
            if (!side_data)
                return AVERROR(ENOMEM);
            memcpy(side_data, s->extra_data, s->extra_data_size);
            s->extra_data_updated = 1;
        }

        AV_WB32(buf + 0, s->last_frame_fctl.sequence_number);
        AV_WB32(buf + 4, s->last_frame_fctl.width);
        AV_WB32(buf + 8, s->last_frame_fctl.height);
        AV_WB32(buf + 12, s->last_frame_fctl.x_offset);
        AV_WB32(buf + 16, s->last_frame_fctl.y_offset);
        AV_WB16(buf + 20, s->last_frame_fctl.delay_num);
        AV_WB16(buf + 22, s->last_frame_fctl.delay_den);
        buf[24] = s->last_frame_fctl.dispose_op;
        buf[25] = s->last_frame_fctl.blend_op;
        png_write_chunk(&last_fctl_chunk_start, MKTAG('f', 'c', 'T', 'L'), buf, 26);

        *got_packet = 1;
    }

    if (pict) {
        if (!s->last_frame) {
            s->last_frame = av_frame_alloc();
            if (!s->last_frame)
                return AVERROR(ENOMEM);
        } else if (s->last_frame_fctl.dispose_op != APNG_DISPOSE_OP_PREVIOUS) {
            if (!s->prev_frame) {
                s->prev_frame = av_frame_alloc();
                if (!s->prev_frame)
                    return AVERROR(ENOMEM);

                s->prev_frame->format = pict->format;
                s->prev_frame->width = pict->width;
                s->prev_frame->height = pict->height;
                if ((ret = av_frame_get_buffer(s->prev_frame, 32)) < 0)
                    return ret;
            }

            // Do disposal, but not blending
            av_frame_copy(s->prev_frame, s->last_frame);
            if (s->last_frame_fctl.dispose_op == APNG_DISPOSE_OP_BACKGROUND) {
                uint32_t y;
                uint8_t bpp = (s->bits_per_pixel + 7) >> 3;
                for (y = s->last_frame_fctl.y_offset; y < s->last_frame_fctl.y_offset + s->last_frame_fctl.height; ++y) {
                    size_t row_start = s->prev_frame->linesize[0] * y + bpp * s->last_frame_fctl.x_offset;
                    memset(s->prev_frame->data[0] + row_start, 0, bpp * s->last_frame_fctl.width);
                }
            }
        }

        av_frame_unref(s->last_frame);
        ret = av_frame_ref(s->last_frame, (AVFrame*)pict);
        if (ret < 0)
            return ret;

        s->last_frame_fctl = fctl_chunk;
        s->last_frame_packet_size = s->bytestream - s->bytestream_start;
    } else {
        av_frame_free(&s->last_frame);
    }

    return 0;
}

static av_cold int png_enc_init(AVCodecContext *avctx)
{
    PNGEncContext *s = avctx->priv_data;
    int compression_level;

    switch (avctx->pix_fmt) {
    case AV_PIX_FMT_RGBA:
        avctx->bits_per_coded_sample = 32;
        break;
    case AV_PIX_FMT_RGB24:
        avctx->bits_per_coded_sample = 24;
        break;
    case AV_PIX_FMT_GRAY8:
        avctx->bits_per_coded_sample = 0x28;
        break;
    case AV_PIX_FMT_MONOBLACK:
        avctx->bits_per_coded_sample = 1;
        break;
    case AV_PIX_FMT_PAL8:
        avctx->bits_per_coded_sample = 8;
    }

#if FF_API_CODED_FRAME
FF_DISABLE_DEPRECATION_WARNINGS
    avctx->coded_frame->pict_type = AV_PICTURE_TYPE_I;
    avctx->coded_frame->key_frame = 1;
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    ff_llvidencdsp_init(&s->llvidencdsp);

#if FF_API_PRIVATE_OPT
FF_DISABLE_DEPRECATION_WARNINGS
    if (avctx->prediction_method)
        s->filter_type = av_clip(avctx->prediction_method,
                                 PNG_FILTER_VALUE_NONE,
                                 PNG_FILTER_VALUE_MIXED);
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    if (avctx->pix_fmt == AV_PIX_FMT_MONOBLACK)
        s->filter_type = PNG_FILTER_VALUE_NONE;

    if (s->dpi && s->dpm) {
      av_log(avctx, AV_LOG_ERROR, "Only one of 'dpi' or 'dpm' options should be set\n");
      return AVERROR(EINVAL);
    } else if (s->dpi) {
      s->dpm = s->dpi * 10000 / 254;
    }

    s->is_progressive = !!(avctx->flags & AV_CODEC_FLAG_INTERLACED_DCT);
    switch (avctx->pix_fmt) {
    case AV_PIX_FMT_RGBA64BE:
        s->bit_depth = 16;
        s->color_type = PNG_COLOR_TYPE_RGB_ALPHA;
        break;
    case AV_PIX_FMT_RGB48BE:
        s->bit_depth = 16;
        s->color_type = PNG_COLOR_TYPE_RGB;
        break;
    case AV_PIX_FMT_RGBA:
        s->bit_depth  = 8;
        s->color_type = PNG_COLOR_TYPE_RGB_ALPHA;
        break;
    case AV_PIX_FMT_RGB24:
        s->bit_depth  = 8;
        s->color_type = PNG_COLOR_TYPE_RGB;
        break;
    case AV_PIX_FMT_GRAY16BE:
        s->bit_depth  = 16;
        s->color_type = PNG_COLOR_TYPE_GRAY;
        break;
    case AV_PIX_FMT_GRAY8:
        s->bit_depth  = 8;
        s->color_type = PNG_COLOR_TYPE_GRAY;
        break;
    case AV_PIX_FMT_GRAY8A:
        s->bit_depth = 8;
        s->color_type = PNG_COLOR_TYPE_GRAY_ALPHA;
        break;
    case AV_PIX_FMT_YA16BE:
        s->bit_depth = 16;
        s->color_type = PNG_COLOR_TYPE_GRAY_ALPHA;
        break;
    case AV_PIX_FMT_MONOBLACK:
        s->bit_depth  = 1;
        s->color_type = PNG_COLOR_TYPE_GRAY;
        break;
    case AV_PIX_FMT_PAL8:
        s->bit_depth  = 8;
        s->color_type = PNG_COLOR_TYPE_PALETTE;
        break;
    default:
        return -1;
    }
    s->bits_per_pixel = ff_png_get_nb_channels(s->color_type) * s->bit_depth;

    s->zstream.zalloc = ff_png_zalloc;
    s->zstream.zfree  = ff_png_zfree;
    s->zstream.opaque = NULL;
    compression_level = avctx->compression_level == FF_COMPRESSION_DEFAULT
                      ? Z_DEFAULT_COMPRESSION
                      : av_clip(avctx->compression_level, 0, 9);
    if (deflateInit2(&s->zstream, compression_level, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return -1;

    return 0;
}

static av_cold int png_enc_close(AVCodecContext *avctx)
{
    PNGEncContext *s = avctx->priv_data;

    deflateEnd(&s->zstream);
    av_frame_free(&s->last_frame);
    av_frame_free(&s->prev_frame);
    av_freep(&s->last_frame_packet);
    av_freep(&s->extra_data);
    s->extra_data_size = 0;
    return 0;
}

#define OFFSET(x) offsetof(PNGEncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    {"dpi", "Set image resolution (in dots per inch)",  OFFSET(dpi), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 0x10000, VE},
    {"dpm", "Set image resolution (in dots per meter)", OFFSET(dpm), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 0x10000, VE},
    { "pred", "Prediction method", OFFSET(filter_type), AV_OPT_TYPE_INT, { .i64 = PNG_FILTER_VALUE_NONE }, PNG_FILTER_VALUE_NONE, PNG_FILTER_VALUE_MIXED, VE, "pred" },
        { "none",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = PNG_FILTER_VALUE_NONE },  INT_MIN, INT_MAX, VE, "pred" },
        { "sub",   NULL, 0, AV_OPT_TYPE_CONST, { .i64 = PNG_FILTER_VALUE_SUB },   INT_MIN, INT_MAX, VE, "pred" },
        { "up",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = PNG_FILTER_VALUE_UP },    INT_MIN, INT_MAX, VE, "pred" },
        { "avg",   NULL, 0, AV_OPT_TYPE_CONST, { .i64 = PNG_FILTER_VALUE_AVG },   INT_MIN, INT_MAX, VE, "pred" },
        { "paeth", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = PNG_FILTER_VALUE_PAETH }, INT_MIN, INT_MAX, VE, "pred" },
        { "mixed", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = PNG_FILTER_VALUE_MIXED }, INT_MIN, INT_MAX, VE, "pred" },
    { NULL},
};

static const AVClass pngenc_class = {
    .class_name = "PNG encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVClass apngenc_class = {
    .class_name = "APNG encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_png_encoder = {
    .name           = "png",
    .long_name      = NULL_IF_CONFIG_SMALL("PNG (Portable Network Graphics) image"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_PNG,
    .priv_data_size = sizeof(PNGEncContext),
    .init           = png_enc_init,
    .close          = png_enc_close,
    .encode2        = encode_png,
    .capabilities   = AV_CODEC_CAP_FRAME_THREADS | AV_CODEC_CAP_INTRA_ONLY,
    .pix_fmts       = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_RGB24, AV_PIX_FMT_RGBA,
        AV_PIX_FMT_RGB48BE, AV_PIX_FMT_RGBA64BE,
        AV_PIX_FMT_PAL8,
        AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAY8A,
        AV_PIX_FMT_GRAY16BE, AV_PIX_FMT_YA16BE,
        AV_PIX_FMT_MONOBLACK, AV_PIX_FMT_NONE
    },
    .priv_class     = &pngenc_class,
};

AVCodec ff_apng_encoder = {
    .name           = "apng",
    .long_name      = NULL_IF_CONFIG_SMALL("APNG (Animated Portable Network Graphics) image"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_APNG,
    .priv_data_size = sizeof(PNGEncContext),
    .init           = png_enc_init,
    .close          = png_enc_close,
    .encode2        = encode_apng,
    .capabilities   = AV_CODEC_CAP_DELAY,
    .pix_fmts       = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_RGB24, AV_PIX_FMT_RGBA,
        AV_PIX_FMT_RGB48BE, AV_PIX_FMT_RGBA64BE,
        AV_PIX_FMT_PAL8,
        AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAY8A,
        AV_PIX_FMT_GRAY16BE, AV_PIX_FMT_YA16BE,
        AV_PIX_FMT_MONOBLACK, AV_PIX_FMT_NONE
    },
    .priv_class     = &apngenc_class,
};
