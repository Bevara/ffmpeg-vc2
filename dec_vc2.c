/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / VC-2 decoder filter.
 *
 *  VC-2 (SMPTE ST 2042, "Dirac Pro") is the intra-only profile family that
 *  came out of Dirac, and the High Quality profile is what production
 *  equipment and ffmpeg's vc2 encoder actually emit. schroedinger - which
 *  libschro drives, and which decodes Dirac here - predates that profile and
 *  refuses its pictures outright, parse code 0xE8.
 *
 *  The note in CODECS_STATUS.md said the only free decoder covering HQ was the
 *  BBC's vc2-reference, and that turned out to be wrong: ffmpeg's Dirac
 *  decoder covers VC-2 including HQ. So this is ffmpeg configured with
 *  --disable-everything and that one decoder, rather than a C++ reference
 *  implementation and a subset of boost.
 *
 *  Framing is done here rather than by a parser. A VC-2 stream is a chain of
 *  13-byte parse info headers - "BBCD", a parse code, then the offsets to the
 *  next and previous one - so walking it needs no bit reading, and one
 *  sequence header to the next is exactly one frame. The sequence header does
 *  need bit reading, for the picture size: everything downstream is resolved
 *  from the size announced at configure time, and a decoder cannot wait for
 *  its first frame to say what it produces.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>


#define VC2_PARSE_INFO_SIZE 13
#define VC2_CODE_SEQ_HEADER 0x00

typedef struct
{
	GF_FilterPid *ipid, *opid;
	u32 width, height, out_size;
	u32 timescale, frame_dur;
	u64 next_cts;
	AVCodecContext *dec;
	AVFrame *frame;
	AVPacket *pkt;
	struct SwsContext *sws;
	int sws_src_fmt;
	int sws_src_w, sws_src_h;
} GF_VC2DecCtx;

/* Dirac codes integers as interleaved exponential Golomb: a continuation bit,
 * then a data bit, repeated. Reading all the zeroes first and the data after -
 * the non-interleaved form - parses the same bytes into different numbers, and
 * silently: this stream's 320x180 comes out as 7x393. */
typedef struct
{
	const u8 *data;
	u32 size, bit;
} VC2Bits;

static u32 vc2_read_bit(VC2Bits *bs)
{
	u32 v;
	if ((bs->bit >> 3) >= bs->size)
		return 0;
	v = (bs->data[bs->bit >> 3] >> (7 - (bs->bit & 7))) & 1;
	bs->bit++;
	return v;
}

static u32 vc2_read_uint(VC2Bits *bs)
{
	u32 v = 1;
	while (!vc2_read_bit(bs))
	{
		if ((bs->bit >> 3) >= bs->size)
			return 0;
		v = 2 * v + vc2_read_bit(bs);
	}
	return v - 1;
}

/* Reads only as far as the frame size, which is all this filter needs from a
 * sequence header. Returns GF_FALSE when the stream gives its size through a
 * base video format index instead of stating it: those are the broadcast
 * formats, and the decoder reports the size for them on the first frame. */
static Bool vc2_parse_seq_header(const u8 *payload, u32 size, u32 *width, u32 *height)
{
	VC2Bits bs;
	bs.data = payload;
	bs.size = size;
	bs.bit = 0;

	vc2_read_uint(&bs); /* major_version */
	vc2_read_uint(&bs); /* minor_version */
	vc2_read_uint(&bs); /* profile */
	vc2_read_uint(&bs); /* level */
	vc2_read_uint(&bs); /* base_video_format */

	if (!vc2_read_bit(&bs)) /* custom_dimensions_flag */
		return GF_FALSE;

	*width = vc2_read_uint(&bs);
	*height = vc2_read_uint(&bs);
	return (*width && *height) ? GF_TRUE : GF_FALSE;
}

/* Walks the parse info chain looking for the first sequence header. */
static Bool vc2_probe_size(const u8 *data, u32 size, u32 *width, u32 *height)
{
	u32 off = 0;
	while (off + VC2_PARSE_INFO_SIZE <= size)
	{
		u32 next;
		if (memcmp(data + off, "BBCD", 4))
			return GF_FALSE;
		next = GF_4CC(data[off + 5], data[off + 6], data[off + 7], data[off + 8]);
		if (data[off + 4] == VC2_CODE_SEQ_HEADER)
		{
			u32 payload = (next > VC2_PARSE_INFO_SIZE) ? (next - VC2_PARSE_INFO_SIZE) : 0;
			if (payload && (off + next <= size))
				return vc2_parse_seq_header(data + off + VC2_PARSE_INFO_SIZE, payload, width, height);
			return GF_FALSE;
		}
		if (!next)
			return GF_FALSE;
		off += next;
	}
	return GF_FALSE;
}

static void vc2dec_set_size(GF_VC2DecCtx *ctx, u32 w, u32 h)
{
	ctx->width = w;
	ctx->height = h;
	ctx->out_size = w * h * 3 / 2;
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(w));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(h));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(w));
}

static GF_Err vc2dec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_VC2DecCtx *ctx = (GF_VC2DecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	ctx->timescale = 25;
	ctx->frame_dur = 1;
	ctx->next_cts = 0;

	gf_filter_pid_copy_properties(ctx->opid, pid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_VISUAL));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	/* Always 4:2:0 out, whatever the stream codes: VC-2 is commonly 4:2:2 or
	 * 4:4:4, and announcing the real format would mean not knowing it until
	 * the first frame - which is after the graph is resolved. swscale
	 * converts, so the announcement is true from the start. */
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_YUV));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(ctx->timescale));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_FPS, &PROP_FRAC_INT(ctx->timescale, ctx->frame_dur));
	/* corrected from the sequence header before any frame is sent */
	vc2dec_set_size(ctx, 320, 180);

	return GF_OK;
}

static GF_Err vc2dec_send_frames(GF_VC2DecCtx *ctx)
{
	while (1)
	{
		u8 *output;
		GF_FilterPacket *dst_pck;
		u8 *dst_planes[4];
		int dst_stride[4];
		int ret = avcodec_receive_frame(ctx->dec, ctx->frame);

		if ((ret == AVERROR(EAGAIN)) || (ret == AVERROR_EOF))
			return GF_OK;
		if (ret < 0)
			return GF_NON_COMPLIANT_BITSTREAM;

		if (((u32)ctx->frame->width != ctx->width) || ((u32)ctx->frame->height != ctx->height))
			vc2dec_set_size(ctx, (u32)ctx->frame->width, (u32)ctx->frame->height);

		if (!ctx->sws || (ctx->sws_src_fmt != ctx->frame->format) ||
		    (ctx->sws_src_w != ctx->frame->width) || (ctx->sws_src_h != ctx->frame->height))
		{
			if (ctx->sws)
				sws_freeContext(ctx->sws);
			ctx->sws = sws_getContext(ctx->frame->width, ctx->frame->height, ctx->frame->format,
			                          ctx->frame->width, ctx->frame->height, AV_PIX_FMT_YUV420P,
			                          SWS_BICUBIC, NULL, NULL, NULL);
			ctx->sws_src_fmt = ctx->frame->format;
			ctx->sws_src_w = ctx->frame->width;
			ctx->sws_src_h = ctx->frame->height;
			if (!ctx->sws)
				return GF_OUT_OF_MEM;
		}

		dst_pck = gf_filter_pck_new_alloc(ctx->opid, ctx->out_size, &output);
		if (!dst_pck)
			return GF_OUT_OF_MEM;

		dst_planes[0] = output;
		dst_planes[1] = output + ctx->width * ctx->height;
		dst_planes[2] = dst_planes[1] + (ctx->width / 2) * (ctx->height / 2);
		dst_planes[3] = NULL;
		dst_stride[0] = (int)ctx->width;
		dst_stride[1] = dst_stride[2] = (int)ctx->width / 2;
		dst_stride[3] = 0;
		sws_scale(ctx->sws, (const u8 *const *)ctx->frame->data, ctx->frame->linesize,
		          0, ctx->frame->height, dst_planes, dst_stride);

		gf_filter_pck_set_cts(dst_pck, ctx->next_cts);
		gf_filter_pck_set_dts(dst_pck, ctx->next_cts);
		gf_filter_pck_set_duration(dst_pck, ctx->frame_dur);
		gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
		gf_filter_pck_send(dst_pck);
		ctx->next_cts += ctx->frame_dur;
	}
}

static GF_Err vc2dec_feed(GF_VC2DecCtx *ctx, const u8 *data, u32 size)
{
	GF_Err e;
	int ret;

	av_packet_unref(ctx->pkt);
	if (data)
	{
		ctx->pkt->data = (u8 *)data;
		ctx->pkt->size = (int)size;
	}
	ret = avcodec_send_packet(ctx->dec, data ? ctx->pkt : NULL);
	if ((ret < 0) && (ret != AVERROR(EAGAIN)))
		return GF_NON_COMPLIANT_BITSTREAM;
	e = vc2dec_send_frames(ctx);
	ctx->pkt->data = NULL;
	ctx->pkt->size = 0;
	return e;
}

static GF_Err vc2dec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck;
	const u8 *data;
	u32 size, off = 0, unit_start = 0;
	Bool seen_picture = GF_FALSE;
	GF_VC2DecCtx *ctx = (GF_VC2DecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = gf_filter_pck_get_data(pck, &size);
	if (!data || (size < VC2_PARSE_INFO_SIZE) || memcmp(data, "BBCD", 4))
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[VC2Dec] Not a VC-2 stream: no BBCD parse info at the start\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	{
		u32 w = 0, h = 0;
		if (vc2_probe_size(data, size, &w, &h))
			vc2dec_set_size(ctx, w, h);
	}

	/* One sequence header to the next is one frame. Grouping on the picture
	 * unit instead would work too, but a sequence header belongs with the
	 * picture that follows it, not with the one before. */
	while (off + VC2_PARSE_INFO_SIZE <= size)
	{
		u32 next, code;
		if (memcmp(data + off, "BBCD", 4))
			break;
		code = data[off + 4];
		next = GF_4CC(data[off + 5], data[off + 6], data[off + 7], data[off + 8]);
		if (!next || (off + next > size))
		{
			off = size;
			break;
		}
		if ((code == VC2_CODE_SEQ_HEADER) && seen_picture)
		{
			GF_Err e = vc2dec_feed(ctx, data + unit_start, off - unit_start);
			if (e)
			{
				gf_filter_pid_drop_packet(ctx->ipid);
				return e;
			}
			unit_start = off;
			seen_picture = GF_FALSE;
		}
		/* pictures are the parse codes with bit 3 set */
		if (code & 0x08)
			seen_picture = GF_TRUE;
		off += next;
	}
	if (seen_picture && (off > unit_start))
		vc2dec_feed(ctx, data + unit_start, off - unit_start);

	vc2dec_feed(ctx, NULL, 0);

	gf_filter_pid_drop_packet(ctx->ipid);
	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static GF_Err vc2dec_initialize(GF_Filter *filter)
{
	const AVCodec *codec;
	GF_VC2DecCtx *ctx = (GF_VC2DecCtx *)gf_filter_get_udta(filter);

	codec = avcodec_find_decoder(AV_CODEC_ID_DIRAC);
	if (!codec)
		return GF_NOT_SUPPORTED;
	ctx->dec = avcodec_alloc_context3(codec);
	if (!ctx->dec)
		return GF_OUT_OF_MEM;
	/* No threads in a side module: leaving thread_count at 0 makes diracdec
	 * size its per-thread coefficient buffer with av_realloc_f(ptr, 0, n),
	 * which returns NULL. */
	ctx->dec->thread_count = 1;
	if (avcodec_open2(ctx->dec, codec, NULL) < 0)
		return GF_NOT_SUPPORTED;
	ctx->frame = av_frame_alloc();
	ctx->pkt = av_packet_alloc();
	if (!ctx->frame || !ctx->pkt)
		return GF_OUT_OF_MEM;
	return GF_OK;
}

static void vc2dec_finalize(GF_Filter *filter)
{
	GF_VC2DecCtx *ctx = (GF_VC2DecCtx *)gf_filter_get_udta(filter);
	if (ctx->sws)
		sws_freeContext(ctx->sws);
	if (ctx->frame)
		av_frame_free(&ctx->frame);
	if (ctx->pkt)
		av_packet_free(&ctx->pkt);
	if (ctx->dec)
		avcodec_free_context(&ctx->dec);
}

static const GF_FilterCapability VC2DecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "vc2"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "video/vc2|video/x-vc2"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister VC2DecoderRegister = {
	.name = "vc2dec",
	GF_FS_SET_DESCRIPTION("VC-2 (SMPTE ST 2042) decoder")
		GF_FS_SET_HELP("This filter decodes VC-2 elementary streams, including the High Quality profile that schroedinger predates and refuses, using a reduced FFMPEG build carrying the Dirac decoder only. Output is 4:2:0 whatever the stream codes.")
			.private_size = sizeof(GF_VC2DecCtx),
	SETCAPS(VC2DecCaps),
	.initialize = vc2dec_initialize,
	.configure_pid = vc2dec_configure_pid,
	.process = vc2dec_process,
	.finalize = vc2dec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE vc2dec_register(GF_FilterSession *session)
{
	return &VC2DecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_vc2dec(void)
{
	gf_filter_auto_register("vc2dec", vc2dec_register);
}
