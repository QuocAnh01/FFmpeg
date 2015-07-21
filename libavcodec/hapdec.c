/*
 * Vidvox Hap decoder
 * Copyright (C) 2015 Vittorio Giovara <vittorio.giovara@gmail.com>
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

/**
 * @file
 * Hap decoder
 *
 * Fourcc: Hap1, Hap5, HapY
 *
 * https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md
 */

#include <stdint.h>

#include "libavutil/imgutils.h"

#include "avcodec.h"
#include "bytestream.h"
#include "hap.h"
#include "internal.h"
#include "snappy.h"
#include "texturedsp.h"
#include "thread.h"

/* The first three bytes are the size of the section past the header, or zero
 * if the length is stored in the next long word. The fourth byte in the first
 * long word indicates the type of the current section. */
static int parse_section_header(AVCodecContext *avctx)
{
    HapContext *ctx = avctx->priv_data;
    GetByteContext *gbc = &ctx->gbc;
    int length;

    if (bytestream2_get_bytes_left(gbc) < 4)
        return AVERROR_INVALIDDATA;

    length = bytestream2_get_le24(gbc);

    ctx->section_type = bytestream2_get_byte(gbc);

    if (length == 0) {
        if (bytestream2_get_bytes_left(gbc) < 4)
            return AVERROR_INVALIDDATA;
        length = bytestream2_get_le32(gbc);
    }

    if (length > bytestream2_get_bytes_left(gbc) || length == 0)
        return AVERROR_INVALIDDATA;

    return length;
}

/* Prepare the texture to be decompressed */
static int setup_texture(AVCodecContext *avctx, size_t length)
{
    HapContext *ctx = avctx->priv_data;
    GetByteContext *gbc = &ctx->gbc;
    int64_t snappy_size;
    const char *compressorstr;
    int ret;

    if ((avctx->codec_tag == MKTAG('H','a','p','1') && (ctx->section_type & 0x0F) != HAP_FMT_RGBDXT1)
        || (avctx->codec_tag == MKTAG('H','a','p','5') && (ctx->section_type & 0x0F) != HAP_FMT_RGBADXT5)
        || (avctx->codec_tag == MKTAG('H','a','p','Y') && (ctx->section_type & 0x0F) != HAP_FMT_YCOCGDXT5)) {
        av_log(avctx, AV_LOG_ERROR, "Invalid texture format %#04x.\n", ctx->section_type & 0x0F);
        return AVERROR_INVALIDDATA;
    }

    switch (ctx->section_type & 0xF0) {
    case HAP_COMP_NONE:
        /* Only DXTC texture compression */
        ctx->tex_data = gbc->buffer;
        ctx->tex_size = length;
        compressorstr = "none";
        break;
    case HAP_COMP_SNAPPY:
        snappy_size = ff_snappy_peek_uncompressed_length(gbc);
        ret = av_reallocp(&ctx->snappied, snappy_size);
        if (ret < 0) {
            return ret;
        }
        /* Uncompress the frame */
        ret = ff_snappy_uncompress(gbc, ctx->snappied, &snappy_size);
        if (ret < 0) {
             av_log(avctx, AV_LOG_ERROR, "Snappy uncompress error\n");
             return ret;
        }

        ctx->tex_data = ctx->snappied;
        ctx->tex_size = snappy_size;
        compressorstr = "snappy";
        break;
    case HAP_COMP_COMPLEX:
        compressorstr = "complex";
        avpriv_request_sample(avctx, "Complex Hap compressor");
        return AVERROR_PATCHWELCOME;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR,
               "Invalid compressor mode %02X.\n", ctx->section_type);
        return AVERROR_INVALIDDATA;
    }

    av_log(avctx, AV_LOG_DEBUG, "%s compressor\n", compressorstr);

    return 0;
}

static int decompress_texture_thread(AVCodecContext *avctx, void *arg,
                                     int slice, int thread_nb)
{
    HapContext *ctx = avctx->priv_data;
    AVFrame *frame = arg;
    const uint8_t *d = ctx->tex_data;
    int w_block = avctx->coded_width / TEXTURE_BLOCK_W;
    int x, y;
    int start_slice, end_slice;

    start_slice = slice * ctx->slice_size;
    end_slice   = FFMIN(start_slice + ctx->slice_size, avctx->coded_height);

    start_slice /= TEXTURE_BLOCK_H;
    end_slice   /= TEXTURE_BLOCK_H;

    for (y = start_slice; y < end_slice; y++) {
        uint8_t *p = frame->data[0] + y * frame->linesize[0] * TEXTURE_BLOCK_H;
        int off  = y * w_block;
        for (x = 0; x < w_block; x++) {
            ctx->tex_fun(p + x * 16, frame->linesize[0],
                         d + (off + x) * ctx->tex_rat);
        }
    }

    return 0;
}

static int hap_decode(AVCodecContext *avctx, void *data,
                      int *got_frame, AVPacket *avpkt)
{
    HapContext *ctx = avctx->priv_data;
    ThreadFrame tframe;
    int ret, length;
    int slices = FFMIN(avctx->thread_count,
                       avctx->coded_height / TEXTURE_BLOCK_H);

    ctx->slice_size = avctx->coded_height / slices;

    bytestream2_init(&ctx->gbc, avpkt->data, avpkt->size);

    /* Check for section header */
    length = parse_section_header(avctx);
    if (length < 0) {
        av_log(avctx, AV_LOG_ERROR, "Frame is too small.\n");
        return length;
    }

    /* Prepare the texture buffer and decompress function */
    ret = setup_texture(avctx, length);
    if (ret < 0)
        return ret;

    /* Get the output frame ready to receive data */
    tframe.f = data;
    ret = ff_thread_get_buffer(avctx, &tframe, 0);
    if (ret < 0)
        return ret;
    if (avctx->codec->update_thread_context)
        ff_thread_finish_setup(avctx);

    /* Use the decompress function on the texture, one block per thread */
    avctx->execute2(avctx, decompress_texture_thread, tframe.f, NULL, slices);

    /* Frame is ready to be output */
    tframe.f->pict_type = AV_PICTURE_TYPE_I;
    tframe.f->key_frame = 1;
    *got_frame = 1;

    return avpkt->size;
}

static av_cold int hap_init(AVCodecContext *avctx)
{
    HapContext *ctx = avctx->priv_data;
    const char *texture_name;
    int ret = av_image_check_size(avctx->width, avctx->height, 0, avctx);

    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid video size %dx%d.\n",
               avctx->width, avctx->height);
        return ret;
    }

    /* Since codec is based on 4x4 blocks, size is aligned to 4 */
    avctx->coded_width  = FFALIGN(avctx->width,  TEXTURE_BLOCK_W);
    avctx->coded_height = FFALIGN(avctx->height, TEXTURE_BLOCK_H);

    /* Technically only one mode has alpha, but 32 bits are easier to handle */
    avctx->pix_fmt = AV_PIX_FMT_RGBA;

    ff_texturedsp_init(&ctx->dxtc);

    switch (avctx->codec_tag) {
    case MKTAG('H','a','p','1'):
        texture_name = "DXT1";
        ctx->tex_rat = 8;
        ctx->tex_fun = ctx->dxtc.dxt1_block;
        break;
    case MKTAG('H','a','p','5'):
        texture_name = "DXT5";
        ctx->tex_rat = 16;
        ctx->tex_fun = ctx->dxtc.dxt5_block;
        break;
    case MKTAG('H','a','p','Y'):
        texture_name = "DXT5-YCoCg-scaled";
        ctx->tex_rat = 16;
        ctx->tex_fun = ctx->dxtc.dxt5ys_block;
        break;
    default:
        return AVERROR_DECODER_NOT_FOUND;
    }

    av_log(avctx, AV_LOG_DEBUG, "%s texture\n", texture_name);

    return 0;
}

static av_cold int hap_close(AVCodecContext *avctx)
{
    HapContext *ctx = avctx->priv_data;

    av_freep(&ctx->snappied);

    return 0;
}

AVCodec ff_hap_decoder = {
    .name           = "hap",
    .long_name      = NULL_IF_CONFIG_SMALL("Vidvox Hap decoder"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_HAP,
    .init           = hap_init,
    .decode         = hap_decode,
    .close          = hap_close,
    .priv_data_size = sizeof(HapContext),
    .capabilities   = CODEC_CAP_FRAME_THREADS | CODEC_CAP_SLICE_THREADS |
                      CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP,
};
