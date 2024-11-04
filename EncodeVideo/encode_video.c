// FFmpeg5.1 的 video encoding 例子
// 根据输入的编码格式选择对应的编码器对生成的视频进行对应格式的编码

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>

#include <libavutil/opt.h>
#include <libavutil/imgutils.h>


static void encode(AVCodecContext* enc_ctx, AVFrame* frame, AVPacket* pkt, FILE* outfile) {
  /* send the frame to the encoder */
  if (frame) {
    printf("Send frame %3"PRId64"\n", frame->pts);
  }

  int ret = avcodec_send_frame(enc_ctx, frame);
  if (ret < 0) {
    fprintf(stderr, "Error sending a frame for encoding\n");
    exit(1);
  }

  while (ret >= 0) {
    ret = avcodec_receive_packet(enc_ctx, pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      return;
    else if (ret < 0) {
      fprintf(stderr, "Error during encoding\n");
      exit(1);
    }
    printf("Write packet %3"PRId64" (size=%5d)\n", pkt->pts, pkt->size);
    fwrite(pkt->data, 1, pkt->size, outfile);
    av_packet_unref(pkt);
  }
}


int main(int argc, char* argv[])
{
  AVCodecContext* codec_ctx = NULL;
  const AVCodec* codec;
  AVFrame* frame;
  AVPacket* pkt;
  uint8_t endcode[] = { 0, 0, 1, 0xb7 };
  int ret;

  if (argc <= 2) {
    fprintf(stderr, "Usage: %s <output file> <codec name>\n", argv[0]);
    exit(0);
  }
  const char* filename = argv[1];
  const char* codec_name = argv[2];

  /* find the mpeg1video encoder */
  codec = avcodec_find_encoder_by_name(codec_name);
  if (!codec) {
    fprintf(stderr, "Codec '%s' not found\n", codec_name);
    exit(1);
  }

  codec_ctx = avcodec_alloc_context3(codec);
  if (!codec_ctx) {
    fprintf(stderr, "Could not allocate video codec context\n");
    exit(1);
  }

  pkt = av_packet_alloc();
  if (!pkt)
    exit(1);

  /* put sample parameters */
  codec_ctx->bit_rate = 400000;
  /* resolution must be a multiple of two */
  codec_ctx->width = 352;
  codec_ctx->height = 288;
  /* frames per second */
  codec_ctx->time_base = (AVRational){1, 25};
  codec_ctx->framerate = (AVRational){25, 1};

  /* emit one intra frame every ten frames
   * check frame pict_type before passing frame
   * to encoder, if frame->pict_type is AV_PICTURE_TYPE_I
   * then gop_size is ignored and the output of encoder
   * will always be I frame irrespective to gop_size
   */
  codec_ctx->gop_size = 10;
  codec_ctx->max_b_frames = 1;
  codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

  if (codec->id == AV_CODEC_ID_H264)
    av_opt_set(codec_ctx->priv_data, "preset", "slow", 0);

  /* open it */
  ret = avcodec_open2(codec_ctx, codec, NULL);
  if (ret < 0) {
    fprintf(stderr, "Could not open codec: %s\n", av_err2str(ret));
    exit(1);
  }

  FILE* fp = fopen(filename, "wb");
  if (!fp) {
    fprintf(stderr, "Could not open %s\n", filename);
    exit(1);
  }

  frame = av_frame_alloc();
  if (!frame) {
    fprintf(stderr, "Could not allocate video frame\n");
    exit(1);
  }
  frame->format = codec_ctx->pix_fmt;
  frame->width = codec_ctx->width;
  frame->height = codec_ctx->height;

  ret = av_frame_get_buffer(frame, 0);
  if (ret < 0) {
    fprintf(stderr, "Could not allocate the video frame data\n");
    exit(1);
  }

  /* encode 1 second of video */
  for (int i = 0; i < 25; i++) {
    fflush(stdout);
    /* Make sure the frame data is writable.
       On the first round, the frame is fresh from av_frame_get_buffer()
       and therefore we know it is writable.
       But on the next rounds, encode() will have called
       avcodec_send_frame(), and the codec may have kept a reference to
       the frame in its internal structures, that makes the frame
       unwritable.
       av_frame_make_writable() checks that and allocates a new buffer
       for the frame only if necessary.
     */
    ret = av_frame_make_writable(frame);
    if (ret < 0)
      exit(1);

    /* Prepare a dummy image.
       In real code, this is where you would have your own logic for
       filling the frame. FFmpeg does not care what you put in the
       frame.
     */
    /* YUV420P */
    /* Y */
    for (int y = 0; y < codec_ctx->height; y++) {
      for (int x = 0; x < codec_ctx->width; x++) {
        frame->data[0][y * frame->linesize[0] + x] = x + y + i * 3;
      }
    }

    /* Cb and Cr */
    for (int y = 0; y < codec_ctx->height / 2; y++) {
      for (int x = 0; x < codec_ctx->width / 2; x++) {
        frame->data[1][y * frame->linesize[1] + x] = 128 + y + i * 2;
        frame->data[2][y * frame->linesize[2] + x] = 64 + x + i * 5;
      }
    }
    frame->pts = i;

    /* encode the image */
    encode(codec_ctx, frame, pkt, fp);
  }

  /* flush the encoder */
  encode(codec_ctx, NULL, pkt, fp);
  /* Add sequence end code to have a real MPEG file.
     It makes only sense because this tiny examples writes packets
     directly. This is called "elementary stream" and only works for some
     codecs. To create a valid file, you usually need to write packets
     into a proper file format or protocol; see muxing.c.
   */
  /* 对于mpeg1 mpeg2 添加一个结束序列码 */
  if (codec->id == AV_CODEC_ID_MPEG1VIDEO || codec->id == AV_CODEC_ID_MPEG2VIDEO) {
    fwrite(endcode, 1, sizeof(endcode), fp);
  }
  fclose(fp);

  avcodec_free_context(&codec_ctx);
  av_frame_free(&frame);
  av_packet_free(&pkt);

  return 0;
}