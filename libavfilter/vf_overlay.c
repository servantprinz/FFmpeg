/*
 * Copyright (c) 2010 Stefano Sabatini
 * Copyright (c) 2010 Baptiste Coudurier
 * Copyright (c) 2007 Bobby Bingham
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
 * overlay one video on top of another
 */

/* #define DEBUG */

#include "avfilter.h"
#include "formats.h"
#include "libavutil/common.h"
#include "libavutil/eval.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/timestamp.h"
#include "internal.h"
#include "bufferqueue.h"
#include "drawutils.h"
#include "video.h"

static const char *const var_names[] = {
    "main_w",    "W", ///< width  of the main    video
    "main_h",    "H", ///< height of the main    video
    "overlay_w", "w", ///< width  of the overlay video
    "overlay_h", "h", ///< height of the overlay video
    NULL
};

enum var_name {
    VAR_MAIN_W,    VAR_MW,
    VAR_MAIN_H,    VAR_MH,
    VAR_OVERLAY_W, VAR_OW,
    VAR_OVERLAY_H, VAR_OH,
    VAR_VARS_NB
};

#define MAIN    0
#define OVERLAY 1

#define R 0
#define G 1
#define B 2
#define A 3

#define Y 0
#define U 1
#define V 2

typedef struct {
    const AVClass *class;
    int x, y;                   ///< position of overlayed picture

    int allow_packed_rgb;
    uint8_t frame_requested;
    uint8_t overlay_eof;
    uint8_t main_is_packed_rgb;
    uint8_t main_rgba_map[4];
    uint8_t main_has_alpha;
    uint8_t overlay_is_packed_rgb;
    uint8_t overlay_rgba_map[4];
    uint8_t overlay_has_alpha;

    AVFilterBufferRef *overpicref;
    struct FFBufQueue queue_main;
    struct FFBufQueue queue_over;

    int main_pix_step[4];       ///< steps per pixel for each plane of the main output
    int overlay_pix_step[4];    ///< steps per pixel for each plane of the overlay
    int hsub, vsub;             ///< chroma subsampling values

    char *x_expr, *y_expr;
} OverlayContext;

#define OFFSET(x) offsetof(OverlayContext, x)

static const AVOption overlay_options[] = {
    { "x", "set the x expression", OFFSET(x_expr), AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MIN, CHAR_MAX },
    { "y", "set the y expression", OFFSET(y_expr), AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MIN, CHAR_MAX },
    {"rgb", "force packed RGB in input and output", OFFSET(allow_packed_rgb), AV_OPT_TYPE_INT, {.dbl=0}, 0, 1 },
    {NULL},
};

AVFILTER_DEFINE_CLASS(overlay);

static av_cold int init(AVFilterContext *ctx, const char *args)
{
    OverlayContext *over = ctx->priv;
    char *args1 = av_strdup(args);
    char *expr, *bufptr = NULL;
    int ret = 0;

    over->class = &overlay_class;
    av_opt_set_defaults(over);

    if (expr = av_strtok(args1, ":", &bufptr)) {
        av_free(over->x_expr);
        if (!(over->x_expr = av_strdup(expr))) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
    }
    if (expr = av_strtok(NULL, ":", &bufptr)) {
        av_free(over->y_expr);
        if (!(over->y_expr = av_strdup(expr))) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
    }

    if (bufptr && (ret = av_set_options_string(over, bufptr, "=", ":")) < 0)
        goto end;

end:
    av_free(args1);
    return ret;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    OverlayContext *over = ctx->priv;

    av_freep(&over->x_expr);
    av_freep(&over->y_expr);

    avfilter_unref_bufferp(&over->overpicref);
    ff_bufqueue_discard_all(&over->queue_main);
    ff_bufqueue_discard_all(&over->queue_over);
}

static int query_formats(AVFilterContext *ctx)
{
    OverlayContext *over = ctx->priv;

    /* overlay formats contains alpha, for avoiding conversion with alpha information loss */
    const enum PixelFormat main_pix_fmts_yuv[] = { PIX_FMT_YUV420P,  PIX_FMT_NONE };
    const enum PixelFormat overlay_pix_fmts_yuv[] = { PIX_FMT_YUVA420P, PIX_FMT_NONE };
    const enum PixelFormat main_pix_fmts_rgb[] = {
        PIX_FMT_ARGB,  PIX_FMT_RGBA,
        PIX_FMT_ABGR,  PIX_FMT_BGRA,
        PIX_FMT_RGB24, PIX_FMT_BGR24,
        PIX_FMT_NONE
    };
    const enum PixelFormat overlay_pix_fmts_rgb[] = {
        PIX_FMT_ARGB,  PIX_FMT_RGBA,
        PIX_FMT_ABGR,  PIX_FMT_BGRA,
        PIX_FMT_NONE
    };

    AVFilterFormats *main_formats;
    AVFilterFormats *overlay_formats;

    if (over->allow_packed_rgb) {
        main_formats    = ff_make_format_list(main_pix_fmts_rgb);
        overlay_formats = ff_make_format_list(overlay_pix_fmts_rgb);
    } else {
        main_formats    = ff_make_format_list(main_pix_fmts_yuv);
        overlay_formats = ff_make_format_list(overlay_pix_fmts_yuv);
    }

    ff_formats_ref(main_formats,    &ctx->inputs [MAIN   ]->out_formats);
    ff_formats_ref(overlay_formats, &ctx->inputs [OVERLAY]->out_formats);
    ff_formats_ref(main_formats,    &ctx->outputs[MAIN   ]->in_formats );

    return 0;
}

static const enum PixelFormat alpha_pix_fmts[] = {
    PIX_FMT_YUVA420P, PIX_FMT_ARGB, PIX_FMT_ABGR, PIX_FMT_RGBA,
    PIX_FMT_BGRA, PIX_FMT_NONE
};

static int config_input_main(AVFilterLink *inlink)
{
    OverlayContext *over = inlink->dst->priv;
    const AVPixFmtDescriptor *pix_desc = &av_pix_fmt_descriptors[inlink->format];

    av_image_fill_max_pixsteps(over->main_pix_step,    NULL, pix_desc);

    over->hsub = pix_desc->log2_chroma_w;
    over->vsub = pix_desc->log2_chroma_h;

    over->main_is_packed_rgb =
        ff_fill_rgba_map(over->main_rgba_map, inlink->format) >= 0;
    over->main_has_alpha = ff_fmt_is_in(inlink->format, alpha_pix_fmts);
    return 0;
}

static int config_input_overlay(AVFilterLink *inlink)
{
    AVFilterContext *ctx  = inlink->dst;
    OverlayContext  *over = inlink->dst->priv;
    char *expr;
    double var_values[VAR_VARS_NB], res;
    int ret;
    const AVPixFmtDescriptor *pix_desc = &av_pix_fmt_descriptors[inlink->format];

    av_image_fill_max_pixsteps(over->overlay_pix_step, NULL, pix_desc);

    /* Finish the configuration by evaluating the expressions
       now when both inputs are configured. */
    var_values[VAR_MAIN_W   ] = var_values[VAR_MW] = ctx->inputs[MAIN   ]->w;
    var_values[VAR_MAIN_H   ] = var_values[VAR_MH] = ctx->inputs[MAIN   ]->h;
    var_values[VAR_OVERLAY_W] = var_values[VAR_OW] = ctx->inputs[OVERLAY]->w;
    var_values[VAR_OVERLAY_H] = var_values[VAR_OH] = ctx->inputs[OVERLAY]->h;

    if ((ret = av_expr_parse_and_eval(&res, (expr = over->x_expr), var_names, var_values,
                                      NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    over->x = res;
    if ((ret = av_expr_parse_and_eval(&res, (expr = over->y_expr), var_names, var_values,
                                      NULL, NULL, NULL, NULL, NULL, 0, ctx)))
        goto fail;
    over->y = res;
    /* x may depend on y */
    if ((ret = av_expr_parse_and_eval(&res, (expr = over->x_expr), var_names, var_values,
                                      NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    over->x = res;

    over->overlay_is_packed_rgb =
        ff_fill_rgba_map(over->overlay_rgba_map, inlink->format) >= 0;
    over->overlay_has_alpha = ff_fmt_is_in(inlink->format, alpha_pix_fmts);

    av_log(ctx, AV_LOG_VERBOSE,
           "main w:%d h:%d fmt:%s overlay x:%d y:%d w:%d h:%d fmt:%s\n",
           ctx->inputs[MAIN]->w, ctx->inputs[MAIN]->h,
           av_pix_fmt_descriptors[ctx->inputs[MAIN]->format].name,
           over->x, over->y,
           ctx->inputs[OVERLAY]->w, ctx->inputs[OVERLAY]->h,
           av_pix_fmt_descriptors[ctx->inputs[OVERLAY]->format].name);

    if (over->x < 0 || over->y < 0 ||
        over->x + var_values[VAR_OVERLAY_W] > var_values[VAR_MAIN_W] ||
        over->y + var_values[VAR_OVERLAY_H] > var_values[VAR_MAIN_H]) {
        av_log(ctx, AV_LOG_ERROR,
               "Overlay area (%d,%d)<->(%d,%d) not within the main area (0,0)<->(%d,%d) or zero-sized\n",
               over->x, over->y,
               (int)(over->x + var_values[VAR_OVERLAY_W]),
               (int)(over->y + var_values[VAR_OVERLAY_H]),
               (int)var_values[VAR_MAIN_W], (int)var_values[VAR_MAIN_H]);
        return AVERROR(EINVAL);
    }
    return 0;

fail:
    av_log(NULL, AV_LOG_ERROR,
           "Error when evaluating the expression '%s'\n", expr);
    return ret;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    int exact;
    // common timebase computation:
    AVRational tb1 = ctx->inputs[MAIN   ]->time_base;
    AVRational tb2 = ctx->inputs[OVERLAY]->time_base;
    AVRational *tb = &ctx->outputs[0]->time_base;
    exact = av_reduce(&tb->num, &tb->den,
                      av_gcd((int64_t)tb1.num * tb2.den,
                             (int64_t)tb2.num * tb1.den),
                      (int64_t)tb1.den * tb2.den, INT_MAX);
    av_log(ctx, AV_LOG_VERBOSE,
           "main_tb:%d/%d overlay_tb:%d/%d -> tb:%d/%d exact:%d\n",
           tb1.num, tb1.den, tb2.num, tb2.den, tb->num, tb->den, exact);
    if (!exact)
        av_log(ctx, AV_LOG_WARNING,
               "Timestamp conversion inexact, timestamp information loss may occurr\n");

    outlink->w = ctx->inputs[MAIN]->w;
    outlink->h = ctx->inputs[MAIN]->h;

    return 0;
}

static AVFilterBufferRef *get_video_buffer(AVFilterLink *link, int perms, int w, int h)
{
    return ff_get_video_buffer(link->dst->outputs[0], perms, w, h);
}

// divide by 255 and round to nearest
// apply a fast variant: (X+127)/255 = ((X+127)*257+257)>>16 = ((X+128)*257)>>16
#define FAST_DIV255(x) ((((x) + 128) * 257) >> 16)

static void blend_slice(AVFilterContext *ctx,
                        AVFilterBufferRef *dst, AVFilterBufferRef *src,
                        int x, int y, int w, int h,
                        int slice_y, int slice_w, int slice_h)
{
    OverlayContext *over = ctx->priv;
    int i, j, k;
    int width, height;
    int overlay_end_y = y+h;
    int slice_end_y = slice_y+slice_h;
    int end_y, start_y;

    width = FFMIN(slice_w - x, w);
    end_y = FFMIN(slice_end_y, overlay_end_y);
    start_y = FFMAX(y, slice_y);
    height = end_y - start_y;

    if (over->main_is_packed_rgb) {
        uint8_t *dp = dst->data[0] + x * over->main_pix_step[0] +
                      start_y * dst->linesize[0];
        uint8_t *sp = src->data[0];
        uint8_t alpha;          ///< the amount of overlay to blend on to main
        const int dr = over->main_rgba_map[R];
        const int dg = over->main_rgba_map[G];
        const int db = over->main_rgba_map[B];
        const int da = over->main_rgba_map[A];
        const int dstep = over->main_pix_step[0];
        const int sr = over->overlay_rgba_map[R];
        const int sg = over->overlay_rgba_map[G];
        const int sb = over->overlay_rgba_map[B];
        const int sa = over->overlay_rgba_map[A];
        const int sstep = over->overlay_pix_step[0];
        const int main_has_alpha = over->main_has_alpha;
        if (slice_y > y)
            sp += (slice_y - y) * src->linesize[0];
        for (i = 0; i < height; i++) {
            uint8_t *d = dp, *s = sp;
            for (j = 0; j < width; j++) {
                alpha = s[sa];

                // if the main channel has an alpha channel, alpha has to be calculated
                // to create an un-premultiplied (straight) alpha value
                if (main_has_alpha && alpha != 0 && alpha != 255) {
                    // apply the general equation:
                    // alpha = alpha_overlay / ( (alpha_main + alpha_overlay) - (alpha_main * alpha_overlay) )
                    alpha =
                        // the next line is a faster version of: 255 * 255 * alpha
                        ( (alpha << 16) - (alpha << 9) + alpha )
                        /
                        // the next line is a faster version of: 255 * (alpha + d[da])
                        ( ((alpha + d[da]) << 8 ) - (alpha + d[da])
                          - d[da] * alpha );
                }

                switch (alpha) {
                case 0:
                    break;
                case 255:
                    d[dr] = s[sr];
                    d[dg] = s[sg];
                    d[db] = s[sb];
                    break;
                default:
                    // main_value = main_value * (1 - alpha) + overlay_value * alpha
                    // since alpha is in the range 0-255, the result must divided by 255
                    d[dr] = FAST_DIV255(d[dr] * (255 - alpha) + s[sr] * alpha);
                    d[dg] = FAST_DIV255(d[dg] * (255 - alpha) + s[sg] * alpha);
                    d[db] = FAST_DIV255(d[db] * (255 - alpha) + s[sb] * alpha);
                }
                if (main_has_alpha) {
                    switch (alpha) {
                    case 0:
                        break;
                    case 255:
                        d[da] = s[sa];
                        break;
                    default:
                        // apply alpha compositing: main_alpha += (1-main_alpha) * overlay_alpha
                        d[da] += FAST_DIV255((255 - d[da]) * s[sa]);
                    }
                }
                d += dstep;
                s += sstep;
            }
            dp += dst->linesize[0];
            sp += src->linesize[0];
        }
    } else {
        for (i = 0; i < 3; i++) {
            int hsub = i ? over->hsub : 0;
            int vsub = i ? over->vsub : 0;
            uint8_t *dp = dst->data[i] + (x >> hsub) +
                (start_y >> vsub) * dst->linesize[i];
            uint8_t *sp = src->data[i];
            uint8_t *ap = src->data[3];
            int wp = FFALIGN(width, 1<<hsub) >> hsub;
            int hp = FFALIGN(height, 1<<vsub) >> vsub;
            if (slice_y > y) {
                sp += ((slice_y - y) >> vsub) * src->linesize[i];
                ap += (slice_y - y) * src->linesize[3];
            }
            for (j = 0; j < hp; j++) {
                uint8_t *d = dp, *s = sp, *a = ap;
                for (k = 0; k < wp; k++) {
                    // average alpha for color components, improve quality
                    int alpha_v, alpha_h, alpha;
                    if (hsub && vsub && j+1 < hp && k+1 < wp) {
                        alpha = (a[0] + a[src->linesize[3]] +
                                 a[1] + a[src->linesize[3]+1]) >> 2;
                    } else if (hsub || vsub) {
                        alpha_h = hsub && k+1 < wp ?
                            (a[0] + a[1]) >> 1 : a[0];
                        alpha_v = vsub && j+1 < hp ?
                            (a[0] + a[src->linesize[3]]) >> 1 : a[0];
                        alpha = (alpha_v + alpha_h) >> 1;
                    } else
                        alpha = a[0];
                    *d = FAST_DIV255(*d * (255 - alpha) + *s * alpha);
                    s++;
                    d++;
                    a += 1 << hsub;
                }
                dp += dst->linesize[i];
                sp += src->linesize[i];
                ap += (1 << vsub) * src->linesize[3];
            }
        }
    }
}

static int try_start_frame(AVFilterContext *ctx, AVFilterBufferRef *mainpic)
{
    OverlayContext *over = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFilterBufferRef *next_overpic, *outpicref;

    /* Discard obsolete overlay frames: if there is a next frame with pts is
     * before the main frame, we can drop the current overlay. */
    while (1) {
        next_overpic = ff_bufqueue_peek(&over->queue_over, 0);
        if (!next_overpic || next_overpic->pts > mainpic->pts)
            break;
        ff_bufqueue_get(&over->queue_over);
        avfilter_unref_buffer(over->overpicref);
        over->overpicref = next_overpic;
    }
    /* If there is no next frame and no EOF and the overlay frame is before
     * the main frame, we can not know yet if it will be superseded. */
    if (!over->queue_over.available && !over->overlay_eof &&
        (!over->overpicref || over->overpicref->pts < mainpic->pts))
        return AVERROR(EAGAIN);
    /* At this point, we know that the current overlay frame extends to the
     * time of the main frame. */
    outlink->out_buf = outpicref = avfilter_ref_buffer(mainpic, ~0);

    av_dlog(ctx, "main_pts:%s main_pts_time:%s",
            av_ts2str(outpicref->pts), av_ts2timestr(outpicref->pts, &outlink->time_base));
    if (over->overpicref)
        av_dlog(ctx, " over_pts:%s over_pts_time:%s",
                av_ts2str(over->overpicref->pts), av_ts2timestr(over->overpicref->pts, &outlink->time_base));
    av_dlog(ctx, "\n");

    ff_start_frame(ctx->outputs[0], avfilter_ref_buffer(outpicref, ~0));
    over->frame_requested = 0;
    return 0;
}

static int try_start_next_frame(AVFilterContext *ctx)
{
    OverlayContext *over = ctx->priv;
    AVFilterBufferRef *next_mainpic = ff_bufqueue_peek(&over->queue_main, 0);
    if (!next_mainpic || try_start_frame(ctx, next_mainpic) < 0)
        return AVERROR(EAGAIN);
    avfilter_unref_buffer(ff_bufqueue_get(&over->queue_main));
    return 0;
}

static int try_push_frame(AVFilterContext *ctx)
{
    OverlayContext *over = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFilterBufferRef *outpicref;

    if (try_start_next_frame(ctx) < 0)
        return AVERROR(EAGAIN);
    outpicref = outlink->out_buf;
    if (over->overpicref)
        blend_slice(ctx, outpicref, over->overpicref, over->x, over->y,
                    over->overpicref->video->w, over->overpicref->video->h,
                    0, outpicref->video->w, outpicref->video->h);
    ff_draw_slice(outlink, 0, outpicref->video->h, +1);
    ff_end_frame(outlink);
    return 0;
}

static void flush_frames(AVFilterContext *ctx)
{
    while (!try_push_frame(ctx));
}

static int start_frame_main(AVFilterLink *inlink, AVFilterBufferRef *inpicref)
{
    AVFilterContext *ctx = inlink->dst;
    OverlayContext *over = ctx->priv;

    flush_frames(ctx);
    inpicref->pts = av_rescale_q(inpicref->pts, ctx->inputs[MAIN]->time_base,
                                 ctx->outputs[0]->time_base);
    if (try_start_frame(ctx, inpicref) < 0) {
        ff_bufqueue_add(ctx, &over->queue_main, inpicref);
        av_assert1(inpicref == inlink->cur_buf);
        inlink->cur_buf = NULL;
    }
    return 0;
}

static int draw_slice_main(AVFilterLink *inlink, int y, int h, int slice_dir)
{
    AVFilterContext *ctx = inlink->dst;
    OverlayContext *over = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFilterBufferRef *outpicref = outlink->out_buf;

    if (!outpicref)
        return 0;
    if (over->overpicref &&
        y + h > over->y && y < over->y + over->overpicref->video->h) {
        blend_slice(ctx, outpicref, over->overpicref, over->x, over->y,
                    over->overpicref->video->w, over->overpicref->video->h,
                    y, outpicref->video->w, h);
    }
    return ff_draw_slice(outlink, y, h, slice_dir);
}

static int end_frame_main(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFilterBufferRef *outpicref = outlink->out_buf;
    flush_frames(ctx);

    if (!outpicref)
        return 0;
    return ff_end_frame(ctx->outputs[0]);
}

static int start_frame_over(AVFilterLink *inlink, AVFilterBufferRef *inpicref)
{
    return 0;
}

static int end_frame_over(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    OverlayContext *over = ctx->priv;
    AVFilterBufferRef *inpicref = inlink->cur_buf;
    inlink->cur_buf = NULL;

    flush_frames(ctx);
    inpicref->pts = av_rescale_q(inpicref->pts, ctx->inputs[OVERLAY]->time_base,
                                 ctx->outputs[0]->time_base);
    ff_bufqueue_add(ctx, &over->queue_over, inpicref);
    return try_push_frame(ctx);
}

static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    OverlayContext *over = ctx->priv;
    int input, ret;

    if (!try_push_frame(ctx))
        return 0;
    over->frame_requested = 1;
    while (over->frame_requested) {
        /* TODO if we had a frame duration, we could guess more accurately */
        input = !over->overlay_eof && (over->queue_main.available ||
                                       over->queue_over.available < 2) ?
                OVERLAY : MAIN;
        ret = ff_request_frame(ctx->inputs[input]);
        /* EOF on main is reported immediately */
        if (ret == AVERROR_EOF && input == OVERLAY) {
            over->overlay_eof = 1;
            if (!try_start_next_frame(ctx))
                return 0;
            ret = 0; /* continue requesting frames on main */
        }
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int null_draw_slice(AVFilterLink *inlink, int y, int h, int slice_dir)
{
    return 0;
}

AVFilter avfilter_vf_overlay = {
    .name      = "overlay",
    .description = NULL_IF_CONFIG_SMALL("Overlay a video source on top of the input."),

    .init      = init,
    .uninit    = uninit,

    .priv_size = sizeof(OverlayContext),

    .query_formats = query_formats,

    .inputs    = (const AVFilterPad[]) {{ .name            = "main",
                                          .type            = AVMEDIA_TYPE_VIDEO,
                                          .get_video_buffer= get_video_buffer,
                                          .config_props    = config_input_main,
                                          .start_frame     = start_frame_main,
                                          .draw_slice      = draw_slice_main,
                                          .end_frame       = end_frame_main,
                                          .min_perms       = AV_PERM_READ,
                                          .rej_perms       = AV_PERM_REUSE2|AV_PERM_PRESERVE, },
                                        { .name            = "overlay",
                                          .type            = AVMEDIA_TYPE_VIDEO,
                                          .config_props    = config_input_overlay,
                                          .start_frame     = start_frame_over,
                                          .draw_slice      = null_draw_slice,
                                          .end_frame       = end_frame_over,
                                          .min_perms       = AV_PERM_READ,
                                          .rej_perms       = AV_PERM_REUSE2, },
                                        { .name = NULL}},
    .outputs   = (const AVFilterPad[]) {{ .name            = "default",
                                          .type            = AVMEDIA_TYPE_VIDEO,
                                          .config_props    = config_output,
                                          .request_frame   = request_frame, },
                                        { .name = NULL}},
};
