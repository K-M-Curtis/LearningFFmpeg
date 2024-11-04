// muxing 复用，封装
// Output a media file in any supported libavformat format. The default codecs are used.
/*
 *  运行本程序的时候要带一个输出文件名参数，然后程序将生成一个同步的视频和音频流，
 *  视频和音频流数据是由程序自己生成的，也就是没有所谓的输入文件，凭程序生成的音视频数据
 *  从get_audio_fream, get_video_frame拿到数据
 *  编码复用到指定的文件中去。输出的格式是根据给定的文件扩展名自动猜取的。
 */

#define _CRT_SECURE_NO_WARNINGS
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavcodec/avcodec.h>

#define STREAM_DURATION 10.0
#define STREAM_FRAME_RATE 25  // 帧率
#define STREAM_PIX_FMT AV_PIX_FMT_YUV420P /* default pix_fmt */

#define SCALE_FLAGS SWS_BICUBIC

 // a wrapper around a single output AVStream
typedef struct OutputStream {
  AVStream* st;  // 视频或音频流
  AVCodecContext* enc; // 编码配置

  // pts of the next frame that will be generated
  int64_t next_pts;  // 下一帧的pts，用于音视频同步
  int samples_count;  // 声音采样计数
  AVFrame* frame;  // 视频/音频帧
  AVFrame* tmp_frame;  // 临时帧

  AVPacket* tmp_pkt;

  float t, tincr, tincr2;  // 用于声音生成

  struct SwsContext* sws_ctx; // 视频转换配置
  struct SwrContext* swr_ctx; // 声音重采样配置
} OutputStream;

static void log_packet(const AVFormatContext* fmt_ctx, const AVPacket* pkt) {
  AVRational* time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

  printf("pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
    av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
    av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
    av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
    pkt->stream_index);
}

static int write_frame(AVFormatContext* fmt_ctx, AVCodecContext* codec_ctx,
  AVStream* st, AVFrame* frame, AVPacket* pkt) {
  int ret;
  // send the frame to the encoder
  ret = avcodec_send_frame(codec_ctx, frame);
  if (ret < 0) {
    fprintf(stderr, "Error sending a frame to the encoder: %s\n",
      av_err2str(ret));
    exit(1);
  }

  while (ret >= 0) {
    ret = avcodec_receive_packet(codec_ctx, pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      break;
    }
    else if (ret < 0) {
      fprintf(stderr, "Error encoding a frame: %s\n", av_err2str(ret));
      exit(1);
    }

    /* rescale output packet timestamp values from codec to stream timebase */
    av_packet_rescale_ts(pkt, codec_ctx->time_base, st->time_base);
    pkt->stream_index = st->index;

    /* Write the compressed frame to the media file. */
    log_packet(fmt_ctx, pkt);
    ret = av_interleaved_write_frame(fmt_ctx, pkt);
    /* pkt is now blank (av_interleaved_write_frame() takes ownership of
     * its contents and resets pkt), so that no unreferencing is necessary.
     * This would be different if one used av_write_frame(). */
    if (ret < 0) {
      fprintf(stderr, "Error while writing output packet: %s\n", av_err2str(ret));
      exit(1);
    }
  }

  return ret == AVERROR_EOF ? 1 : 0;
}

// add an output stream
static void add_stream(OutputStream* ost, AVFormatContext* fmt_ctx, AVCodec** codec, enum AVCodecID codec_id) {
  AVCodecContext* codec_ctx;

  /* find the encoder */
  *codec = avcodec_find_encoder(codec_id);
  if (!(*codec)) {
    fprintf(stderr, "Could not find encoder for '%s'\n", avcodec_get_name(codec_id));
    exit(1);
  }

  ost->tmp_pkt = av_packet_alloc();
  if (!ost->tmp_pkt) {
    fprintf(stderr, "Could not allocate AVPacket\n");
    exit(1);
  }

  ost->st = avformat_new_stream(fmt_ctx, NULL);
  if (!ost->st) {
    fprintf(stderr, "Could not allocate stream\n");
    exit(1);
  }
  ost->st->id = fmt_ctx->nb_streams - 1;
  codec_ctx = avcodec_alloc_context3(*codec);
  if (!codec_ctx) {
    fprintf(stderr, "Could not alloc an encoding context\n");
    exit(1);
  }
  ost->enc = codec_ctx;

  switch ((*codec)->type) {
  case AVMEDIA_TYPE_AUDIO:
    codec_ctx->sample_fmt = (*codec)->sample_fmts ? (*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    codec_ctx->bit_rate = 64000;
    codec_ctx->sample_rate = 44100;
    if ((*codec)->supported_samplerates) {
      codec_ctx->sample_rate = (*codec)->supported_samplerates[0];
      for (int i = 0; (*codec)->supported_samplerates[i]; i++) {
        if ((*codec)->supported_samplerates[i] == 44100)
          codec_ctx->sample_rate = 44100;
      }
    }
    /* FFmpeg 5.0 */
    //codec_ctx->channels = av_get_channel_layout_nb_channels(codec_ctx->channel_layout);
    //codec_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
    //if ((*codec)->channel_layouts) {
    //  codec_ctx->channel_layout = (*codec)->channel_layouts[0];
    //  for (int i = 0; (*codec)->channel_layouts[i]; i++) {
    //    if ((*codec)->channel_layouts[i] == AV_CH_LAYOUT_STEREO)
    //      codec_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
    //  }
    //}
    //codec_ctx->channels = av_get_channel_layout_nb_channels(codec_ctx->channel_layout);

    /* FFmpeg 5.1 */
    av_channel_layout_copy(&codec_ctx->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);
    ost->st->time_base = (AVRational){ 1, codec_ctx->sample_rate };
    break;

  case AVMEDIA_TYPE_VIDEO:
    codec_ctx->codec_id = codec_id;
    codec_ctx->bit_rate = 400000;
    /* Resolution must be a multiple of two. */
    codec_ctx->width = 352;
    codec_ctx->height = 288;
    /* timebase: This is the fundamental unit of time (in seconds) in terms
     * of which frame timestamps are represented. For fixed-fps content,
     * timebase should be 1/framerate and timestamp increments should be
     * identical to 1. */
    ost->st->time_base = (AVRational){ 1, STREAM_FRAME_RATE };
    codec_ctx->time_base = ost->st->time_base;

    codec_ctx->gop_size = 12; /* emit one intra frame every twelve frames at most */
    codec_ctx->pix_fmt = STREAM_PIX_FMT;
    if (codec_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
      /* just for testing, we also add B-frames */
      codec_ctx->max_b_frames = 2;
    }
    if (codec_ctx->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
      /* Needed to avoid using macroblocks in which some coeffs overflow.
       * This does not happen with normal video, it just happens here as
       * the motion of the chroma plane does not match the luma plane. */
      codec_ctx->mb_decision = 2;
    }
    break;

  default:
    break;
  }

  /* Some formats want stream headers to be separate. */
  if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
    codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

/* ----------------------------------------------------------------- */
// audio output
// 该函数为open_audio服务，分配音频帧的内存空间
static AVFrame* alloc_audio_frame(enum AVSampleFormat sample_fmt, const AVChannelLayout* channel_layout,
  int sample_rate, int nb_samples) {
  AVFrame* frame = av_frame_alloc();
  if (!frame) {
    fprintf(stderr, "Error allocating an audio frame\n");
    exit(1);
  }
  frame->format = sample_fmt;
  /* FFmpeg 5.0 */
  //frame->channel_layout = channel_layout;
  av_channel_layout_copy(&frame->ch_layout, channel_layout);
  frame->sample_rate = sample_rate;
  frame->nb_samples = nb_samples;
  if (nb_samples) {
    if (av_frame_get_buffer(frame, 0) < 0) {
      fprintf(stderr, "Error allocating an audio buffer\n");
      exit(1);
    }
  }
  return frame;
}

// 做准备工作，准备编解码器以及分配内存空间
static void open_audio(AVFormatContext* fmt_ctx, AVCodec* codec, OutputStream* ost, AVDictionary* opt_arg) {
  AVCodecContext* codec_ctx;
  int nb_samples;
  AVDictionary* opt = NULL;

  codec_ctx = ost->enc;

  av_dict_copy(&opt, opt_arg, 0);
  int ret = avcodec_open2(codec_ctx, codec, &opt);
  av_dict_free(&opt);
  if (ret < 0) {
    fprintf(stderr, "Could not open audio codec: %s\n", av_err2str(ret));
    exit(1);
  }

  /* init signal generator */
  ost->t = 0;
  ost->tincr = 2 * M_PI * 110.0 / codec_ctx->sample_rate;
  /* increment frequency by 110 Hz per second 频率每秒增加110Hz */
  ost->tincr2 = 2 * M_PI * 110.0 / codec_ctx->sample_rate / codec_ctx->sample_rate;

  if (codec_ctx->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
    nb_samples = 10000;
  else
    nb_samples = codec_ctx->frame_size;

  ost->frame = alloc_audio_frame(codec_ctx->sample_fmt, &codec_ctx->ch_layout,
                                 codec_ctx->sample_rate, nb_samples);
  ost->tmp_frame = alloc_audio_frame(AV_SAMPLE_FMT_S16, &codec_ctx->ch_layout,
                                     codec_ctx->sample_rate, nb_samples);

  /* copy the stream parameters to the muxer */
  ret = avcodec_parameters_from_context(ost->st->codecpar, codec_ctx);
  if (ret < 0) {
    fprintf(stderr, "Could not copy the stream parameters\n");
    exit(1);
  }

  /* create resampler context */
  ost->swr_ctx = swr_alloc();
  if (!ost->swr_ctx) {
    fprintf(stderr, "Could not allocate resampler context\n");
    exit(1);
  }

  /* set options */
  //av_opt_set_int(ost->swr_ctx, "in_channel_count", codec_ctx->channels, 0);
  av_opt_set_chlayout  (ost->swr_ctx, "in_chlayout", &codec_ctx->ch_layout, 0);
  av_opt_set_int       (ost->swr_ctx, "in_sample_rate", codec_ctx->sample_rate, 0);
  av_opt_set_sample_fmt(ost->swr_ctx, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
  //av_opt_set_int(ost->swr_ctx, "out_channel_count", codec_ctx->channels, 0);
  av_opt_set_chlayout  (ost->swr_ctx, "out_chlayout", &codec_ctx->ch_layout, 0);
  av_opt_set_int       (ost->swr_ctx, "out_sample_rate", codec_ctx->sample_rate, 0);
  av_opt_set_sample_fmt(ost->swr_ctx, "out_sample_fmt", codec_ctx->sample_fmt, 0);

  /* initialize the resampling context */
  if ((ret = swr_init(ost->swr_ctx)) < 0) {
    fprintf(stderr, "Failed to initialize the resampling context\n");
    exit(1);
  }
}

// Prepare a 16 bit dummy audio frame of 'frame_size' samples and 'nb_channels' channels. 
// 音频帧由代码生成，生成dummy audio，生成音频数据
static AVFrame* get_audio_frame(OutputStream* ost) {
  AVFrame* frame = ost->tmp_frame;
  int j, i, v;
  int16_t* q = (int16_t*)frame->data[0];
  /* check if we want to generate more frames */
  if (av_compare_ts(ost->next_pts, ost->enc->time_base,
    STREAM_DURATION, (AVRational){ 1, 1 }) > 0)
    return NULL;
  for (j = 0; j < frame->nb_samples; j++) {
    v = (int)(sin(ost->t) * 10000);
    for (i = 0; i < ost->enc->ch_layout.nb_channels; i++)
      *q++ = v;
    ost->t += ost->tincr;
    ost->tincr += ost->tincr2;
  }
  frame->pts = ost->next_pts;
  ost->next_pts += frame->nb_samples;
  return frame;
}

/* encode one audio frame and send it to the muxer
 return 1 when encoding is finished, 0 otherwise */
static int write_audio_frame(AVFormatContext* fmt_ctx, OutputStream* ost) {
  AVCodecContext* codec_ctx;
  AVFrame* frame;
  codec_ctx = ost->enc;
  int ret;
  frame = get_audio_frame(ost);

  if (frame) {
    /* convert samples from native format to destination codec format, using the resampler */
    /* compute destination number of samples */
    int dst_nb_samples = av_rescale_rnd(swr_get_delay(ost->swr_ctx, codec_ctx->sample_rate) + frame->nb_samples,
                                        codec_ctx->sample_rate, codec_ctx->sample_rate, AV_ROUND_UP);
    av_assert0(dst_nb_samples == frame->nb_samples);

    /* when we pass a frame to the encoder, it may keep a reference to it internally;
     * make sure we do not overwrite it here
     */
    ret = av_frame_make_writable(ost->frame);
    if (ret < 0)
      exit(1);

    /* convert to destination format */
    ret = swr_convert(ost->swr_ctx, ost->frame->data, dst_nb_samples,
                      (const uint8_t**)frame->data, frame->nb_samples);
    if (ret < 0) {
      fprintf(stderr, "Error while converting\n");
      exit(1);
    }
    frame = ost->frame;

    frame->pts = av_rescale_q(ost->samples_count, (AVRational){1, codec_ctx->sample_rate}, codec_ctx->time_base);
    ost->samples_count += dst_nb_samples;
  }
  return write_frame(fmt_ctx, codec_ctx, ost->st, frame, ost->tmp_pkt);
}

/* ----------------------------------------------------------------- */
// video output
// 该函数为open_video服务，分配视频帧的内存空间
static AVFrame* alloc_frame(enum AVPixelFormat pix_fmt, int width, int height) {
  AVFrame* frame;
  int ret;
  frame = av_frame_alloc();
  if (!frame)
    return NULL;
  frame->format = pix_fmt;
  frame->width = width;
  frame->height = height;
  /* allocate the buffers for the frame data */
  ret = av_frame_get_buffer(frame, 0);
  if (ret < 0) {
    fprintf(stderr, "Could not allocate frame data.\n");
    exit(1);
  }
  return frame;
}

// 做准备工作，准备编解码器以及分配内存空间
static void open_video(AVFormatContext* fmt_ctx, const AVCodec* codec, OutputStream* ost, AVDictionary* opt_arg) {
  AVCodecContext* codec_ctx = ost->enc;
  AVDictionary* opt = NULL;

  av_dict_copy(&opt, opt_arg, 0);

  /* open the codec */
  int ret = avcodec_open2(codec_ctx, codec, &opt);
  av_dict_free(&opt);
  if (ret < 0) {
    fprintf(stderr, "Could not open video codec: %s\n", av_err2str(ret));
    exit(1);
  }

  /* allocate and init a re-usable frame */
  ost->frame = alloc_frame(codec_ctx->pix_fmt, codec_ctx->width, codec_ctx->height);
  if (!ost->frame) {
    fprintf(stderr, "Could not allocate video frame\n");
    exit(1);
  }

  /* If the output format is not YUV420P, then a temporary YUV420P
   * picture is needed too. It is then converted to the required
   * output format. */
  ost->tmp_frame = NULL;
  if (codec_ctx->pix_fmt != AV_PIX_FMT_YUV420P) {
    ost->tmp_frame = alloc_frame(AV_PIX_FMT_YUV420P, codec_ctx->width, codec_ctx->height);
    if (!ost->tmp_frame) {
      fprintf(stderr, "Could not allocate temporary video frame\n");
      exit(1);
    }
  }

  /* copy the stream parameters to the muxer */
  ret = avcodec_parameters_from_context(ost->st->codecpar, codec_ctx);
  if (ret < 0) {
    fprintf(stderr, "Could not copy the stream parameters\n");
    exit(1);
  }
}

// prepare a dummy image 
// 程序的视频帧由代码生成，生成dummy image，生成视频数据
static void fill_yuv_image(AVFrame* pict, int frame_index, int width, int height) {
  int x, y, i;

  i = frame_index;

  /* Y */
  for (y = 0; y < height; y++)
    for (x = 0; x < width; x++)
      pict->data[0][y * pict->linesize[0] + x] = x + y + i * 3;

  /* Cb and Cr */
  for (y = 0; y < height / 2; y++) {
    for (x = 0; x < width / 2; x++) {
      pict->data[1][y * pict->linesize[1] + x] = 128 + y + i * 2;
      pict->data[2][y * pict->linesize[2] + x] = 64 + x + i * 5;
    }
  }
}

// 获取视频帧，转换格式，用于写入文件
static AVFrame* get_video_frame(OutputStream* ost) {
  AVCodecContext* codec_ctx = ost->enc;
  /* check if we want to generate more frames */
  if (av_compare_ts(ost->next_pts, codec_ctx->time_base, STREAM_DURATION, (AVRational){ 1, 1 }) > 0)
    return NULL;

  /* when we pass a frame to the encoder, it may keep a reference to it
   * internally; make sure we do not overwrite it here */
  if (av_frame_make_writable(ost->frame) < 0)
    exit(1);

  if (codec_ctx->pix_fmt != AV_PIX_FMT_YUV420P) {
    /* as we only generate a YUV420P picture, we must convert it
     * to the codec pixel format if needed */
    if (!ost->sws_ctx) {
      ost->sws_ctx = sws_getContext(codec_ctx->width, codec_ctx->height, AV_PIX_FMT_YUV420P,
                                    codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
                                    SCALE_FLAGS, NULL, NULL, NULL);
      if (!ost->sws_ctx) {
        fprintf(stderr, "Could not initialize the conversion context\n");
        exit(1);
      }
    }
    fill_yuv_image(ost->tmp_frame, ost->next_pts, codec_ctx->width, codec_ctx->height);
    sws_scale(ost->sws_ctx, (const uint8_t* const*)ost->tmp_frame->data,
              ost->tmp_frame->linesize, 0, codec_ctx->height, ost->frame->data,
              ost->frame->linesize);
  } else {
    fill_yuv_image(ost->frame, ost->next_pts, codec_ctx->width, codec_ctx->height);
  }

  ost->frame->pts = ost->next_pts++;
  return ost->frame;
}

/* encode one video frame and send it to the muxer
   return 1 when encoding is finished, 0 otherwise */
static int write_video_frame(AVFormatContext* fmt_ctx, OutputStream* ost) {
  return write_frame(fmt_ctx, ost->enc, ost->st, get_video_frame(ost), ost->tmp_pkt);
}

static void close_stream(AVFormatContext* fmt_ctx, OutputStream* ost) {
  avcodec_free_context(&ost->enc);
  av_frame_free(&ost->frame);
  av_frame_free(&ost->tmp_frame);
  av_packet_free(&ost->tmp_pkt);
  sws_freeContext(ost->sws_ctx);
  swr_free(&ost->swr_ctx);
}

/* ----------------------------------------------------------------- */
// media file output

int main(int argc, char* argv[])
{
  OutputStream video_st = { 0 }, audio_st = { 0 };
  const AVOutputFormat* out_fmt;
  AVFormatContext* fmt_ctx;
  const AVCodec* audio_codec = NULL;
  const AVCodec* video_codec = NULL;
  int have_video = 0, have_audio = 0;
  int encode_video = 0, encode_audio = 0;
  AVDictionary* opt = NULL;
  int ret;

  if (argc < 2) {
    printf("usage: %s output_file\n"
      "API example program to output a media file with libavformat.\n"
      "This program generates a synthetic audio and video stream, encodes and\n"
      "muxes them into a file named output_file.\n"
      "The output format is automatically guessed according to the file extension.\n"
      "Raw images can also be output by using '%%d' in the filename.\n"
      "\n", argv[0]);
    return 1;
  }

  const char* filename = argv[1];
  for (int i = 2; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "-flags") || !strcmp(argv[i], "-fflags"))
      av_dict_set(&opt, argv[i] + 1, argv[i + 1], 0);
  }

  /* allocate the output media context */
  avformat_alloc_output_context2(&fmt_ctx, NULL, NULL, filename);
  if (!fmt_ctx) {
    printf("Could not deduce output format from file extension: using MPEG.\n");
    avformat_alloc_output_context2(&fmt_ctx, NULL, "mpeg", filename);
  }
  if (!fmt_ctx)
    return 1;

  out_fmt = fmt_ctx->oformat;


  /* Add the audio and video streams using the default format codecs
   * and initialize the codecs. */
  if (out_fmt->video_codec != AV_CODEC_ID_NONE) {
    add_stream(&video_st, fmt_ctx, &video_codec, out_fmt->video_codec);
    have_video = 1;
    encode_video = 1;
  }
  if (out_fmt->audio_codec != AV_CODEC_ID_NONE) {
    add_stream(&audio_st, fmt_ctx, &audio_codec, out_fmt->audio_codec);
    have_audio = 1;
    encode_audio = 1;
  }

  /* Now that all the parameters are set, we can open the audio and
   * video codecs and allocate the necessary encode buffers. */
  if (have_video)
    open_video(fmt_ctx, video_codec, &video_st, opt);

  if (have_audio)
    open_audio(fmt_ctx, audio_codec, &audio_st, opt);

  av_dump_format(fmt_ctx, 0, filename, 1);

  /* open the output file, if needed */
  if (!(out_fmt->flags & AVFMT_NOFILE)) {
    ret = avio_open(&fmt_ctx->pb, filename, AVIO_FLAG_WRITE);
    if (ret < 0) {
      fprintf(stderr, "Could not open '%s': %s\n", filename,
        av_err2str(ret));
      return 1;
    }
  }

  /* Write the stream header, if any. */
  ret = avformat_write_header(fmt_ctx, &opt);
  if (ret < 0) {
    fprintf(stderr, "Error occurred when opening output file: %s\n",
      av_err2str(ret));
    return 1;
  }

  while (encode_video || encode_audio) {
    /* select the stream to encode */
    if (encode_video &&
      (!encode_audio || av_compare_ts(video_st.next_pts, video_st.enc->time_base,
                                      audio_st.next_pts, audio_st.enc->time_base) <= 0)) {
      encode_video = !write_video_frame(fmt_ctx, &video_st);
    } else {
      encode_audio = !write_audio_frame(fmt_ctx, &audio_st);
    }
  }

  /* Write the trailer, if any. The trailer must be written before you
   * close the CodecContexts open when you wrote the header; otherwise
   * av_write_trailer() may try to use memory that was freed on
   * av_codec_close(). */
  av_write_trailer(fmt_ctx);

  /* Close each codec */
  if (have_video)
    close_stream(fmt_ctx, &video_st);
  if (have_audio)
    close_stream(fmt_ctx, &audio_st);

  if (!(out_fmt->flags & AVFMT_NOFILE)) {
    /* Close the output file */
    avio_closep(&fmt_ctx->pb);
  }

  /* free the stream */
  avformat_free_context(fmt_ctx);
  return 0;
}