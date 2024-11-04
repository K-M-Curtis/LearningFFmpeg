// FFmpeg5.1的 video decoding 例子
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>

#define INBUF_SIZE 4096

//保存的是packed模式的yuv数据，这里是采用PGM格式来保存yuv数据
static void pgm_save(unsigned char* buf, int wrap, int xsize, int ysize, char* filename) {
  FILE* f;
  int i;

  f = fopen(filename, "wb");
  fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255);
  for (i = 0; i < ysize; i++)
    fwrite(buf + i * wrap, 1, xsize, f);
  fclose(f);
}

//保存的是planner模式的yuv数据，这里直接保存yuv数据裸流
//使用 ffplay - f rawvideo - video_size 宽x高 - pixel_format 数据格式（如yuv420p）filename 进行查看
static void pgm_planner_save(AVFrame* frame, int xsize, int ysize, char* filename) {
  FILE* f;
  
  f = fopen(filename, "wb");
  for (int i = 0; i < ysize; i++)
    fwrite(frame->data[0] + i * frame->linesize[0], 1, xsize, f);

  for (int i = 0; i < ysize / 2; i++)
    fwrite(frame->data[1] + i * frame->linesize[1], 1, xsize / 2, f);

  for (int i = 0; i < ysize / 2; i++)
    fwrite(frame->data[2] + i * frame->linesize[2], 1, xsize / 2, f);

  fflush(f);
  fclose(f);
}

static void decode(AVCodecContext* dec_ctx, AVFrame* frame, AVPacket* pkt, const char* filename) {
  char buf[1024];
  int ret = avcodec_send_packet(dec_ctx, pkt);
  if (ret < 0) {
    fprintf(stderr, "Error sending a packet for decoding\n");
    exit(1);
  }

  while (ret >= 0) {
    ret = avcodec_receive_frame(dec_ctx, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      return;
    else if (ret < 0) {
      fprintf(stderr, "Error during decoding\n");
      exit(1);
    }

    printf("saving frame %3d\n", dec_ctx->frame_number);
    fflush(stdout);

    /* the picture is allocated by the decoder. no need to
       free it */
    snprintf(buf, sizeof(buf), "%s_%d_%d-%d", filename, frame->width, frame->height, dec_ctx->frame_number);
    //pgm_save(frame->data[0], frame->linesize[0],
    //  frame->width, frame->height, buf);
    pgm_planner_save(frame, frame->width, frame->height, buf);
  }
}


int main(int argc, char* argv[]) {
  const AVCodec* codec = NULL;
  AVCodecParserContext* parser = NULL;
  AVCodecContext* codec_ctx = NULL;
  AVFrame* frame = NULL;
  AVPacket* pkt = NULL;
  /*
  * AV_INPUT_BUFFER_PADDING_SIZE： 在申请内存时，额外的增加一个size， 原因：在做解码时，
  * 一些优化过的解码器往往一次性解析 32bit 或 64bit 的数据，避免读取数据越界或丢失的问题、
  * 注意：如果前23bit 不为0， 可能会导致读取越界，或者段错误。
  */
  uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
  uint8_t* data;
  size_t data_size;
  int ret, eof;

  if (argc <= 2) {
    fprintf(stderr, "Usage: %s <input file> <output file>\n"
      "And check your input file is encoded by h264 please.\n", argv[0]);
    exit(0);
  }
  const char* in_filename = argv[1];
  const char* out_filename = argv[2];

  pkt = av_packet_alloc();
  if (!pkt)
    exit(1);
  /* set end of buffer to 0 (this ensures that no overreading happens for damaged MPEG streams) */
  memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);

  /* find the H264 video decoder */
  codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec) {
    fprintf(stderr, "Codec not found\n");
    exit(1);
  }

  parser = av_parser_init(codec->id);
  if (!parser) {
    fprintf(stderr, "parser not found\n");
    exit(1);
  }

  codec_ctx = avcodec_alloc_context3(codec);
  if (!codec_ctx) {
    fprintf(stderr, "Could not allocate video codec context\n");
    exit(1);
  }

  /* For some codecs, such as msmpeg4 and mpeg4, width and height
     MUST be initialized there because this information is not
     available in the bitstream. */

     /* open it */
  if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
    fprintf(stderr, "Could not open codec\n");
    exit(1);
  }

  FILE* fp = fopen(in_filename, "rb");
  if (!fp) {
    fprintf(stderr, "Could not open %s\n", in_filename);
    exit(1);
  }

  frame = av_frame_alloc();
  if (!frame) {
    fprintf(stderr, "Could not allocate video frame\n");
    exit(1);
  }

  do {
    /* read raw data from the input file */
    data_size = fread(inbuf, 1, INBUF_SIZE, fp);
    if (ferror(fp))
      break;
    eof = !data_size;

    /* use the parser to split the data into frames */
    data = inbuf;
    while (data_size > 0 || eof) {
      ret = av_parser_parse2(parser, codec_ctx, &pkt->data, &pkt->size, data, data_size,
        AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
      if (ret < 0) {
        fprintf(stderr, "Error while parsing\n");
        exit(1);
      }
      data += ret;
      data_size -= ret;

      if (pkt->size)
        decode(codec_ctx, frame, pkt, out_filename);
      else if (eof)
        break;
    }
  } while (!eof);

  /* flush the decoder */
  decode(codec_ctx, frame, NULL, out_filename);

  fclose(fp);
  av_parser_close(parser);
  avcodec_free_context(&codec);
  av_frame_free(&frame);
  av_packet_free(&pkt);

  return 0;
}