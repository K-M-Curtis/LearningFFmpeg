// FFmpeg5.1 的 audio encoding 例子
//该例子为生成一段音频数据并编码为对应的音频格式
//音频格式在avcodec_find_encoder中指定

#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>

#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>

/* check that a given sample format is supported by the encoder */
static int check_sample_fmt(const AVCodec* codec, enum AVSampleFormat sample_fmt) {
  const enum AVSampleFormat* p = codec->sample_fmts;
  while (*p != AV_SAMPLE_FMT_NONE) {
    if (*p == sample_fmt) {
      return 1;
    }
    p++;
  }
  return 0;
}

/* just pick the highest supported samplerate */
static int select_sample_rate(const AVCodec* codec) {
  const int* p = NULL;
  int best_samplerate = 0;
  if (!codec->supported_samplerates)
    return 44100;
  p = codec->supported_samplerates;
  while (*p) {
    if (!best_samplerate || abs(44100 - *p) < abs(44100 - best_samplerate)) {
      best_samplerate = *p;
    }
    p++;
  }
  return best_samplerate;
}

/* select layout with the highest channel count */
/* FFmpeg 5.0 */
//static int select_channel_layout(const AVCodec* codec) {
//  const uint64_t* p = NULL;
//  uint64_t best_ch_layout = 0;
//  int best_nb_channels = 0;
//
//  if (!codec->channel_layouts)
//    return AV_CH_LAYOUT_STEREO;
//  p = codec->channel_layouts;
//  while (*p) {
//    int nb_channels = av_get_channel_layout_nb_channels(*p);
//    if (nb_channels > best_nb_channels) {
//      best_ch_layout = *p;
//      best_nb_channels = nb_channels;
//    }
//    p++;
//  }
//  return best_ch_layout;
//}

/* FFmpeg 5.1 */
static int select_channel_layout(const AVCodec* codec, AVChannelLayout* dst) {
  const AVChannelLayout* p = NULL;
  const AVChannelLayout* best_ch_layout = NULL;
  int best_nb_channels = 0;

  if (!codec->ch_layouts)
    return av_channel_layout_copy(dst, &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);

  p = codec->ch_layouts;
  while (p->nb_channels) {
    int nb_channels = p->nb_channels;
    if (nb_channels > best_nb_channels) {
      best_ch_layout = p;
      best_nb_channels = nb_channels;
    }
    p++;
  }
  return av_channel_layout_copy(dst, best_ch_layout);
}

//将packed数据模式转换为planar数据模式
void f32le_convert_to_fltp(float* f32le, float* fltp, int nb_samples) {
  float* fltp_l = fltp;      // 左声道
  float* fltp_r = fltp + nb_samples;    // 右声道
  for (int i = 0; i < nb_samples; i++) {
    fltp_l[i] = f32le[i * 2];
    fltp_r[i] = f32le[i * 2 + 1];  // 可以尝试注释掉左声道或者右声道听听声音
  }
}

static void encode(AVCodecContext* codec_ctx, AVFrame* frame, AVPacket* pkt, FILE* output) {
  /* send the frame for encoding */
  int ret = avcodec_send_frame(codec_ctx, frame);
  if (ret < 0) {
    fprintf(stderr, "Error sending the frame to the encoder\n");
    exit(1);
  }

  /* read all the available output packets (in general there may be any
   * number of them */
  while (ret >= 0) {
    ret = avcodec_receive_packet(codec_ctx, pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      return;
    else if (ret < 0) {
      fprintf(stderr, "Error encoding audio frame\n");
      exit(1);
    }

    fwrite(pkt->data, 1, pkt->size, output);
    av_packet_unref(pkt);
  }
}


int main(int argc, char* argv[])
{
  const AVCodec* codec;
  AVCodecContext* codec_ctx = NULL;
  AVFrame* frame;
  AVPacket* pkt;
  uint16_t* samples;
  float t, tincr;
  int ret;

  if (argc <= 1) {
    fprintf(stderr, "Usage: %s <output file>\n", argv[0]);
    return 0;
  }
  const char* filename = argv[1];

  /* find the MP2 encoder */
  codec = avcodec_find_encoder(AV_CODEC_ID_MP2);
  if (!codec) {
    fprintf(stderr, "Codec not found\n");
    exit(1);
  }

  codec_ctx = avcodec_alloc_context3(codec);
  if (!codec_ctx) {
    fprintf(stderr, "Could not allocate audio codec context\n");
    exit(1);
  }

  /* put sample parameters */
  codec_ctx->bit_rate = 64000;
  /* check that the encoder supports s16 pcm input */
  codec_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
  if (!check_sample_fmt(codec, codec_ctx->sample_fmt)) {
    fprintf(stderr, "Encoder does not support sample format %s",
      av_get_sample_fmt_name(codec_ctx->sample_fmt));
    exit(1);
  }

  /* select other audio parameters supported by the encoder */
  codec_ctx->sample_rate = select_sample_rate(codec);
  /* FFmpeg 5.0 */
  //codec_ctx->channel_layout = select_channel_layout(codec);
  //codec_ctx->channels = av_get_channel_layout_nb_channels(codec_ctx->channel_layout);

  /* FFmpeg 5.1 */
  ret = select_channel_layout(codec, &codec_ctx->ch_layout);
  if (ret < 0)
    exit(1);

  /* open it */
  if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
    fprintf(stderr, "Could not open codec\n");
    exit(1);
  }

  FILE* fp = fopen(filename, "wb");
  if (!fp) {
    fprintf(stderr, "Could not open %s\n", filename);
    exit(1);
  }

  /* packet for holding encoded output */
  pkt = av_packet_alloc();
  if (!pkt) {
    fprintf(stderr, "could not allocate the packet\n");
    exit(1);
  }

  /* frame containing input raw audio */
  frame = av_frame_alloc();
  if (!frame) {
    fprintf(stderr, "Could not allocate audio frame\n");
    exit(1);
  }

  frame->nb_samples = codec_ctx->frame_size;
  frame->format = codec_ctx->sample_fmt;
  /* FFmpeg 5.0 */
  //frame->channel_layout = codec_ctx->channel_layout;
  ret = av_channel_layout_copy(&frame->ch_layout, &codec_ctx->ch_layout);
  if (ret < 0)
    exit(1);

  /* allocate the data buffers */
  ret = av_frame_get_buffer(frame, 0);
  if (ret < 0) {
    fprintf(stderr, "Could not allocate audio data buffers\n");
    exit(1);
  }

  /* encode a single tone sound */
  t = 0;
  tincr = 2 * M_PI * 440.0 / codec_ctx->sample_rate;
  for (int i = 0; i < 200; i++) {
    /* make sure the frame is writable -- makes a copy if the encoder
     * kept a reference internally */
    ret = av_frame_make_writable(frame);
    if (ret < 0)
      exit(1);
    samples = (uint16_t*)frame->data[0];
    for (int j = 0; j < codec_ctx->frame_size; j++) {
      samples[2 * j] = (int)(sin(t) * 10000);

      for (int k = 1; k < codec_ctx->ch_layout.nb_channels; k++) {
        samples[2 * j + k] = samples[2 * j];
      }
      t += tincr;
    }
    encode(codec_ctx, frame, pkt, fp);
  }

  /* flush the encoder */
  encode(codec_ctx, NULL, pkt, fp);

  fclose(fp);

  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&codec_ctx);

  return 0;
}
