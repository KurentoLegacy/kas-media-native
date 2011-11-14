/*
 * Kurento Android Media: Android Media Library based on FFmpeg.
 * Copyright (C) 2011  Tikal Technologies
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * 
 * @author Miguel París Díaz
 * 
 */

#include <my-cmdutils.h>
#include <init-media.h>

#include <jni.h>
#include <pthread.h>
#include <android/log.h>

#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavcodec/opt.h"
#include "libavformat/rtpenc.h"

#include <time.h>

/*
	see	libavformat/output-example.c
		libavcodec/api-example.c
*/

static char buf[256]; //Log
static char* LOG_TAG = "NDK-video-tx";

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static int sws_flags = SWS_BICUBIC;

enum {
	OUTBUF_SIZE = 800*1024,
	SRC_PIX_FMT = PIX_FMT_NV21,
};

//Coupled with Java
int VIDEO_CODECS[] = {CODEC_ID_H264, CODEC_ID_MPEG4, CODEC_ID_H263P};
char* VIDEO_CODEC_NAMES[] = {"h264", "mpeg4", "h263p"};



//FIXME Tener en cuenta si hay error, que deberemos liberar las estructuras


static AVFrame *picture, *tmp_picture;
static uint8_t *video_outbuf;
static int video_outbuf_size;

static AVOutputFormat *fmt;
static AVFormatContext *oc;
static AVStream *video_st;

static uint8_t *picture_buf;


static int frame_num;
static unsigned int total_out_size;

uint64_t init_encode_time;


////////////////////////////////////////////////////////////////////////////////////////
//INIT VIDEO

static AVFrame *alloc_picture(enum PixelFormat pix_fmt, int width, int height)
{
	AVFrame *picture;

	picture = avcodec_alloc_frame();
	if (!picture)
		return NULL;
	
	return picture;
}


static int open_video(AVFormatContext *oc, AVStream *st)
{
	int ret;
	
	AVCodec *codec;
	AVCodecContext *c;
	
	c = st->codec;
	
	/* find the video encoder */
	codec = avcodec_find_encoder(c->codec_id);
	if (!codec) {
		__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "Codec not found");
		return -1;
	}

	/* open the codec */
	if ((ret = avcodec_open(c, codec)) < 0) {
		__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "Could not open codec");
		return ret;
	}
	

	video_outbuf = NULL;
	if (!(oc->oformat->flags & AVFMT_RAWPICTURE)) {
		/* allocate output buffer */
		/* XXX: API change will be done */
		/* buffers passed into lav* can be allocated any way you prefer,
		as long as they're aligned enough for the architecture, and
		they're freed appropriately (such as using av_free for buffers
		allocated with av_malloc) */
		video_outbuf_size = 800000;
		video_outbuf = av_malloc(video_outbuf_size);
	}

	/* allocate the encoded raw picture */
	picture = alloc_picture(c->pix_fmt, c->width, c->height);
	
	
	
	if (!picture) {
		__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "Could not allocate picture");
		return -3;
	}

	/* if the output format is not YUV420P, then a temporary YUV420P
	picture is needed too. It is then converted to the required
	output format */
	tmp_picture =  alloc_picture(PIX_FMT_YUV420P, c->width, c->height);
	if (!tmp_picture) {
		__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "Could not allocate temporary picture");
		return -4;
	}
	
	return 0;
}




//Based on opt_preset from ffmpeg.c
static int opt_preset(const char *preset_file)
{
	FILE *f = NULL;
	char tmp[1000], tmp2[1000], line[1000];
	int i;
	
	f = fopen(preset_file, "r");
	
	if(!f){
		snprintf(buf, sizeof(buf), "File for preset '%s' not found", preset_file);
		__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, buf);
		return -1;
	}

	while(!feof(f)){
		int e= fscanf(f, "%999[^\n]\n", line) - 1;
		if(line[0] == '#' && !e)
			continue;
		e|= sscanf(line, "%999[^=]=%999[^\n]\n", tmp, tmp2) - 2;
		if(e){
			snprintf(buf, sizeof(buf), "%s: Invalid syntax: '%s'", preset_file, line);
			__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, buf);
			return -1;
		}
		if(opt_default(tmp, tmp2) < 0){
			snprintf(buf, sizeof(buf), "%s: Invalid option or argument: '%s', parsed as '%s' = '%s'", preset_file, line, tmp, tmp2);
			__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, buf);
			return -1;
		}
	}

	fclose(f);
		
	return 0;
}


/**
 * add a video output stream
 * see new_video_stream in ffmpeg.c
 */
static AVStream *add_video_stream(AVFormatContext *oc, enum CodecID codec_id, int width, int height,
				int frame_rate_num, int frame_rate_den, int bit_rate, int gop_size, const char *preset_file)
{
	AVCodecContext *c;
	AVStream *st;
	
	st = av_new_stream(oc, 0);
	if (!st) {
		__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "Could not alloc stream");
		return NULL;
	}
	
	c = st->codec;
	c->codec_id = codec_id;
	c->codec_type = AVMEDIA_TYPE_VIDEO;
	
	/* put sample parameters */
	c->bit_rate = bit_rate;
	c->rc_max_rate = bit_rate;
	c->rc_min_rate = bit_rate;
	
	/* resolution must be a multiple of two */
	c->width = width;
	c->height = height;
	/* time base: this is the fundamental unit of time (in seconds) in terms
	of which frame timestamps are represented. for fixed-fps content,
	timebase should be 1/framerate and timestamp increments should be
	identically 1. */
	c->time_base.den = frame_rate_num;	//15;
	c->time_base.num = frame_rate_den;
	c->gop_size = gop_size;//12; /* emit one intra frame every twelve frames at most */
	c->keyint_min = gop_size;
	c->max_b_frames = 0;

	c->qmin=31;
	c->qmax=31;
//	c->qcompress=0.6;

	c->pix_fmt = PIX_FMT_YUV420P;
//	c->pix_fmt = PIX_FMT_NV21;

	c->rc_buffer_size = INT_MAX;	//((c->bit_rate * (int64_t)c->time_base.num) / (int64_t)c->time_base.den) + 1;

snprintf(buf, sizeof(buf), "bit_rate: %d", c->bit_rate);
__android_log_write(ANDROID_LOG_DEBUG, LOG_TAG, buf);
snprintf(buf, sizeof(buf), "rc_min_rate: %d", c->rc_min_rate);
__android_log_write(ANDROID_LOG_DEBUG, LOG_TAG, buf);
snprintf(buf, sizeof(buf), "rc_max_rate: %d", c->rc_max_rate);
__android_log_write(ANDROID_LOG_DEBUG, LOG_TAG, buf);
snprintf(buf, sizeof(buf), "gop_size: %d\tkeyint_min: %d\tmax_b_frames: %d", c->gop_size, c->keyint_min, c->max_b_frames);
__android_log_write(ANDROID_LOG_DEBUG, LOG_TAG, buf);

snprintf(buf, sizeof(buf), "qmin: %d", c->qmin);
__android_log_write(ANDROID_LOG_DEBUG, LOG_TAG, buf);
snprintf(buf, sizeof(buf), "qmax: %d", c->qmax);
__android_log_write(ANDROID_LOG_DEBUG, LOG_TAG, buf);
snprintf(buf, sizeof(buf), "qcompress: %d", c->qcompress);
__android_log_write(ANDROID_LOG_DEBUG, LOG_TAG, buf);

	if (c->codec_id == CODEC_ID_MPEG2VIDEO) {
		/* just for testing, we also add B frames */
		c->max_b_frames = 2;
	}
	if (c->codec_id == CODEC_ID_MPEG1VIDEO){
		/* Needed to avoid using macroblocks in which some coeffs overflow.
		This does not happen with normal video, it just happens here as
		the motion of the chroma plane does not match the luma plane. */
		c->mb_decision=2;
	}
	if((c->codec_id == CODEC_ID_H264) && preset_file) {
		__android_log_write(ANDROID_LOG_DEBUG, LOG_TAG, "Apply presets");
		opt_preset(preset_file);
		set_context_opts(c, avcodec_opts[AVMEDIA_TYPE_VIDEO], AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM);
		

AVCodecContext *pCodecCtx = c;

//pCodecCtx->bit_rate=384000;
//pCodecCtx->time_base.den=20;
pCodecCtx->gop_size=0;//200;
//pCodecCtx->height=288;
//pCodecCtx->width=352;
pCodecCtx->max_b_frames=0;
pCodecCtx->max_qdiff=4;
pCodecCtx->me_range=16;
pCodecCtx->qmin=10;
pCodecCtx->qmax=41;
pCodecCtx->qcompress=0.6;
pCodecCtx->keyint_min=10;
pCodecCtx->trellis=0;

pCodecCtx->level=13; //Level 1.3
pCodecCtx->profile=66; //Baseline

pCodecCtx->me_method=7;
pCodecCtx->thread_count=2;
pCodecCtx->qblur=0.5;
//pCodecCtx->pix_fmt=0; //YUV420P

pCodecCtx->rc_min_rate = 1000000;

	}
	

	
	// some formats want stream headers to be separate
	if(oc->oformat->flags & AVFMT_GLOBALHEADER)
		c->flags |= CODEC_FLAG_GLOBAL_HEADER;
	
	return st;
}

jint
Java_com_kurento_kas_media_tx_MediaTx_initVideo(JNIEnv* env,
			jobject thiz,
			jstring outfile, jint width, jint height, jint frame_rate_num, jint frame_rate_den,
			jint bit_rate, jint gop_size, jint codecId, jint payload_type, jstring presetFile)
{
	int i, ret;
	
	const char *pOutFile = NULL;
	const char *pPresetFile = NULL;
	URLContext *urlContext;
	
	pthread_mutex_lock(&mutex);

	frame_num = 0;
	total_out_size = 0;
	init_encode_time = 0;

#ifndef USE_X264_TREE
	__android_log_write(ANDROID_LOG_DEBUG, LOG_TAG, "USE_X264_TREE no def");
	/* TODO: Improve this hack to disable H264 */
	if (VIDEO_CODECS[codecId] == CODEC_ID_H264) {
		__android_log_write(ANDROID_LOG_WARN, LOG_TAG, "H264 not supported");
		ret = -1;
		goto end;
	}
#endif

	pOutFile = (*env)->GetStringUTFChars(env, outfile, NULL);
	if (pOutFile == NULL) {
    		ret = -1; // OutOfMemoryError already thrown
    		goto end;
    	}
    	
    	pPresetFile = (*env)->GetStringUTFChars(env, presetFile, NULL);
	
	if ( (ret= init_media()) != 0) {
		__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "Couldn't init media");
		goto end;
	}
/*	
	for(i=0; i<AVMEDIA_TYPE_NB; i++){
		avcodec_opts[i]= avcodec_alloc_context2(i);
	}
	avformat_opts = avformat_alloc_context();
	sws_opts = sws_getContext(16,16,0, 16,16,0, sws_flags, NULL,NULL,NULL);a
+/	
	/* auto detect the output format from the name. default is mp4. */
	fmt = av_guess_format(NULL, pOutFile, NULL);
	if (!fmt) {
		__android_log_write(ANDROID_LOG_DEBUG, LOG_TAG, "Could not deduce output format from file extension: using RTP.");
		fmt = av_guess_format("rtp", NULL, NULL);
	}
	if (!fmt) {
		__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "Could not find suitable output format");
		ret = -1;
		goto end;
	}
	fmt->video_codec = VIDEO_CODECS[codecId];

	/* allocate the output media context */
	oc = avformat_alloc_context();
	if (!oc) {
		__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "Memory error");
		ret = -2;
		goto end;
	}
	oc->oformat = fmt;
	snprintf(oc->filename, sizeof(oc->filename), "%s", pOutFile);
	
	/* add the  video stream using the default format codecs
	and initialize the codecs */
	video_st = NULL;
	
	if (fmt->video_codec != CODEC_ID_NONE) {
		video_st = add_video_stream(oc, fmt->video_codec, width, height, frame_rate_num, frame_rate_den, bit_rate, gop_size, pPresetFile);
		if(!video_st) {
			ret = -3;
			goto end;
		}
	}

	/* set the output parameters (must be done even if no
	parameters). */
	if (av_set_parameters(oc, NULL) < 0) {
		__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "Invalid output format parameters");
		ret = -4;
		goto end;
	}
	
	av_dump_format(oc, 0, pOutFile, 1);
	
	
	
	/* now that all the parameters are set, we can open the
	video codec and allocate the necessary encode buffers */
	if (video_st) {
		if((ret = open_video(oc, video_st)) < 0) {
			__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "Could not open video");
			goto end;
		}
	}

	/* open the output file, if needed */
	if (!(fmt->flags & AVFMT_NOFILE)) {
		if ((ret = avio_open(&oc->pb, pOutFile, URL_WRONLY)) < 0) {
			snprintf(buf, sizeof(buf), "Could not open '%s'", pOutFile);
			__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, buf);
			goto end;
		}
	}
	
	//Free old URLContext
	if ( (ret=ffurl_close(oc->pb->opaque)) < 0) {
		__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "Could not free URLContext");
		goto end;
	}
	
	urlContext = get_video_connection(0);
	if ((ret=rtp_set_remote_url (urlContext, pOutFile)) < 0) {
		snprintf(buf, sizeof(buf), "Could not open '%s' AVERROR_NOENT:%d", pOutFile, AVERROR_NOENT);
		__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, buf);
		goto end;
	}
	
	oc->pb->opaque = urlContext;
	
	
	/* write the stream header, if any */
	av_write_header(oc);
	
	RTPMuxContext *rptmc = oc->priv_data;
	rptmc->payload_type = payload_type;
	
	(*env)->ReleaseStringUTFChars(env, outfile, pOutFile);
	(*env)->ReleaseStringUTFChars(env, presetFile, pPresetFile);
	
	ret = 0;
	
end:
	pthread_mutex_unlock(&mutex);
	return ret;
}

////////////////////////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////////////////////
//PUT VIDEO FRAME


static int64_t timespecDiff(struct timespec *timeA_p, struct timespec *timeB_p)
{
	return ( ((timeA_p->tv_sec * 1000000000) + timeA_p->tv_nsec) -
		((timeB_p->tv_sec * 1000000000) + timeB_p->tv_nsec) )/1000000;
}


/**
 * see ffmpeg.c
 */
static int write_video_frame(AVFormatContext *oc, AVStream *st, int srcWidth, int srcHeight)
{
	int out_size, ret;
	AVCodecContext *c;
	struct SwsContext *img_convert_ctx;

	struct timespec start, end;
	uint64_t encode_time;

	c = st->codec;
	
	img_convert_ctx = sws_getContext(srcWidth, srcHeight,
					SRC_PIX_FMT,
					c->width, c->height,
					c->pix_fmt,
					sws_flags, NULL, NULL, NULL);
	if (img_convert_ctx == NULL) {
		__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "Cannot initialize the conversion context");
		return -1;
	}
	sws_scale(img_convert_ctx, tmp_picture->data, tmp_picture->linesize,
		0, c->height, picture->data, picture->linesize);
	sws_freeContext(img_convert_ctx);
	
	if (oc->oformat->flags & AVFMT_RAWPICTURE) {
		/* raw video case. The API will change slightly in the near
		futur for that */
		AVPacket pkt;
		av_init_packet(&pkt);
		pkt.flags |= AV_PKT_FLAG_KEY;
		pkt.stream_index= st->index;
		pkt.data= (uint8_t *)picture;
		pkt.size= sizeof(AVPicture);
		ret = av_interleaved_write_frame(oc, &pkt);
	} else {
		clock_gettime(CLOCK_MONOTONIC, &start);
		/* encode the image */
		out_size = avcodec_encode_video(c, video_outbuf, video_outbuf_size, picture);
		clock_gettime(CLOCK_MONOTONIC, &end);
		encode_time = timespecDiff(&end, &start);

		total_out_size += out_size;
		frame_num ++;

		snprintf(buf, sizeof(buf), "%d - out_size: %d Bytes \ttotal_out_size: %d Bytes\tencode_time: %llu ms", frame_num , out_size, total_out_size, encode_time);
		__android_log_write(ANDROID_LOG_INFO, LOG_TAG, buf);

		/* if zero size, it means the image was buffered */
		if (out_size > 0) {
			AVPacket pkt;
			av_init_packet(&pkt);
			if (c->coded_frame->pts != AV_NOPTS_VALUE)
				pkt.pts= av_rescale_q(c->coded_frame->pts, c->time_base, st->time_base);
			if(c->coded_frame->key_frame)
				pkt.flags |= AV_PKT_FLAG_KEY;
			pkt.stream_index= st->index;
			pkt.data= video_outbuf;
			pkt.size= out_size;
		
			/* write the compressed frame in the media file */
			ret = av_interleaved_write_frame(oc, &pkt);
		} else {
			ret = 0;
		}
	}

	if (ret != 0) {
		__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "Error while writing video frame");
		return ret;
	}
}

jint
Java_com_kurento_kas_media_tx_MediaTx_putVideoFrame(JNIEnv* env,
						jobject thiz,
						jbyteArray frame, jint width, jint height)
{
	int ret;
	
	uint8_t *picture2_buf;
	int size;
	
	struct timespec total_start, total_end;
	uint64_t total_encode_time, kbps;
	
	
	pthread_mutex_lock(&mutex);

	clock_gettime(CLOCK_MONOTONIC, &total_start);
	

	if (!oc) {
		__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "No video initiated.");
		ret = -1;
		goto end;
	}
	size = avpicture_get_size(video_st->codec->pix_fmt, video_st->codec->width, video_st->codec->height);
	picture2_buf = av_malloc(size);
	avpicture_fill((AVPicture *)picture, picture2_buf,
			video_st->codec->pix_fmt, video_st->codec->width, video_st->codec->height);

	
	picture_buf = (uint8_t*)((*env)->GetByteArrayElements(env, frame, JNI_FALSE));
	//Asociamos el frame a tmp_picture por si el pix_fmt es distinto de PIX_FMT_YUV420P
	avpicture_fill((AVPicture *)tmp_picture, picture_buf,
			SRC_PIX_FMT, width, height);
			
		
	
	if (write_video_frame(oc, video_st,  width, height) < 0) {
		(*env)->ReleaseByteArrayElements(env, frame, picture_buf, 0);
		__android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "Could not write video frame");
		ret = -2;
		goto end;
	}

	(*env)->ReleaseByteArrayElements(env, frame, picture_buf, 0);
	av_free(picture2_buf);
	ret = 0;
	
	clock_gettime(CLOCK_MONOTONIC, &total_end);
	total_encode_time = timespecDiff(&total_end, &total_start);
	init_encode_time += total_encode_time;

	kbps = ((total_out_size*8) / init_encode_time);

	snprintf(buf, sizeof(buf), "total_encode_time: %llu ms\tinit_encode_time: %llu ms\t%llu kbps", total_encode_time, init_encode_time, kbps);
	__android_log_write(ANDROID_LOG_DEBUG, LOG_TAG, buf);

end:
	pthread_mutex_unlock(&mutex);
	return ret;
}

////////////////////////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////////////////////
//FINISH VIDEO

static void close_video(AVFormatContext *oc, AVStream *st)
{
	if (st)
		avcodec_close(st->codec);
	av_free(picture);
	av_free(tmp_picture);
	av_free(video_outbuf);
}


jint
Java_com_kurento_kas_media_tx_MediaTx_finishVideo (JNIEnv* env,
						jobject thiz)
{
	int i;
	/* write the trailer, if any.  the trailer must be written
	* before you close the CodecContexts open when you wrote the
	* header; otherwise write_trailer may try to use memory that
	* was freed on av_codec_close() */
	pthread_mutex_lock(&mutex);
	if (oc) {
		av_write_trailer(oc);		
		/* close codec */
		if (video_st)
			close_video(oc, video_st);
		/* free the streams */
		for(i = 0; i < oc->nb_streams; i++) {
			av_freep(&oc->streams[i]->codec);
			av_freep(&oc->streams[i]);
		}
		close_context(oc);
		oc = NULL;
	}	
/*	for (i=0;i<AVMEDIA_TYPE_NB;i++)
		av_free(avcodec_opts[i]);
	av_free(avformat_opts);
	av_free(sws_opts);
*/
	pthread_mutex_unlock(&mutex);
	return 0;
}

////////////////////////////////////////////////////////////////////////////////////////

