// 将视频转存为bitmap( .bmp )图片
// 使用命令行的形式查看 .bmp图片
// ffplay -f rawvideo -pixel_format bgr24 -video_size 1920*1080 filename.bmp

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

#define INBUF_SIZE 4096

#define WORD uint16_t
#define DWORD uint32_t
#define LONG int32_t

#pragma pack(2)
// 位图文件头（bitmap-file header）包含了图像类型、图像大小、图像数据存放地址和两个保留未使用的字段
typedef struct tagBITMAPFILEHEADER {
  WORD  bfType;
  DWORD bfSize;
  WORD  bfReserved1;
  WORD  bfReserved2;
  DWORD bfOffBits;
} BITMAPFILEHEADER, *PBITMAPFILEHEADER;

// 位图信息头（bitmap-information header）包含了位图信息头的大小、图像的宽高、图像的色深、压缩说明图像数据的大小和其他一些参数。
typedef struct tagBITMAPINFOHEADER {
  DWORD biSize;
  LONG  biWidth;
  LONG  biHeight;
  WORD  biPlanes;
  WORD  biBitCount;
  DWORD biCompression;
  DWORD biSizeImage;
  LONG  biXPelsPerMeter;
  LONG  biYPelsPerMeter;
  DWORD biClrUsed;
  DWORD biClrImportant;
} BITMAPINFOHEADER, *PBITMAPINFOHEADER;

//图片转换并保存
void saveBMP(struct SwsContext* img_convert_ctx, AVFrame* frame, char* filename) {
  /* 先进行转换， YUV420=>RGB24 */
  int w = frame->width;
  int h = frame->height;

  int numBytes = av_image_get_buffer_size(AV_PIX_FMT_BGR24, w, h, 1);
  uint8_t* buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));

  AVFrame* pFrameRGB = av_frame_alloc();
  //buffer is going to be written to rawvideo file,no alignment
  av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, buffer, AV_PIX_FMT_BGR24, w, h, 1);

  /*
  进行转换
  frame->data和pFrameRGB->data分别指输入和输出的buf
  frame->linesize和pFrameRGB->linesize可以看成是输入和输出的每列的byte数
  第4个参数是指第一列要处理的位置
  第5个参数是source slice 的高度
  */
  sws_scale(img_convert_ctx, frame->data, frame->linesize,
            0, h, pFrameRGB->data, pFrameRGB->linesize);

  /* 2. 构造BITMAPINFOHEADER */
  BITMAPINFOHEADER header;
  header.biSize = sizeof(BITMAPINFOHEADER);

  header.biWidth = w;
  header.biHeight = h * (-1);
  header.biBitCount = 24;
  header.biCompression = 0;
  header.biSizeImage = 0;
  header.biClrImportant = 0;
  header.biClrUsed = 0;
  header.biXPelsPerMeter = 0;
  header.biYPelsPerMeter = 0;
  header.biPlanes = 1;

  /* 3. 构造文件头 */
  BITMAPFILEHEADER bmpFileHeader = { 0, };
  //HANDLE hFile = NULL;
  DWORD dwTotalWriten = 0;
  DWORD dwWriten;

  bmpFileHeader.bfType = 0x4d42; //'BM';
  bmpFileHeader.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + numBytes;
  bmpFileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

  FILE* fp = fopen(filename, "wb");
  fwrite(&bmpFileHeader, sizeof(BITMAPFILEHEADER), 1, fp);
  fwrite(&header, sizeof(BITMAPINFOHEADER), 1, fp);
  fwrite(pFrameRGB->data[0], 1, numBytes, fp);
  fclose(fp);

  /* 释放资源 */
  av_freep(&pFrameRGB[0]);
  av_free(pFrameRGB);
}

static void decode(AVCodecContext* dec_ctx, AVFrame* frame, AVPacket* pkt,
                   struct SwsContext* img_convert_ctx, const char* filename) {
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
    snprintf(buf, sizeof(buf), "%s_%d_%d-%d.bmp", filename, frame->width, frame->height, dec_ctx->frame_number);
    //pgm_save(frame->data[0], frame->linesize[0],
    //  frame->width, frame->height, buf);
    // pgm_planner_save(frame, frame->width, frame->height, buf);
    saveBMP(img_convert_ctx, frame, buf);
  }
}

int main(int argc, char* argv[]) {
  AVFormatContext* fmt_ctx = NULL;
  const AVCodec* codec = NULL;
  AVCodecContext* codec_ctx = NULL;
  AVStream* stream = NULL;
  int stream_index;

  AVFrame* frame = NULL;
  AVPacket* pkt = NULL;
  struct SwsContext* img_convert_ctx;
  int ret;

  if (argc <= 2) {
    fprintf(stderr, "Usage: %s <input file> <output file>\n", argv[0]);
    exit(0);
  }
  const char* in_filename = argv[1];
  const char* out_filename = argv[2];

  /* open input file, and allocate format context */
  if (avformat_open_input(&fmt_ctx, in_filename, NULL, NULL) < 0) {
    fprintf(stderr, "Could not open source file %s\n", in_filename);
    exit(1);
  }

  /* retrieve stream information */
  if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
    fprintf(stderr, "Could not find stream information\n");
    exit(1);
  }

  /* dump input information to stderr */
  av_dump_format(fmt_ctx, 0, in_filename, 0);

  pkt = av_packet_alloc();
  if (!pkt)
    exit(1);

  ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  if (ret < 0) {
    fprintf(stderr, "Could not find %s stream in input file '%s'\n",
      av_get_media_type_string(AVMEDIA_TYPE_VIDEO), in_filename);
    return ret;
  }

  stream_index = ret;
  stream = fmt_ctx->streams[stream_index];

  /* find decoder for the stream */
  codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec) {
    fprintf(stderr, "Failed to find %s codec\n",
      av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
    return AVERROR(EINVAL);
  }

  codec_ctx = avcodec_alloc_context3(codec);
  if (!codec_ctx) {
    fprintf(stderr, "Could not allocate video codec context\n");
    exit(1);
  }

  /* Copy codec parameters from input stream to output codec context */
  if ((ret = avcodec_parameters_to_context(codec_ctx, stream->codecpar)) < 0) {
    fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
      av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
    return ret;
  }

     /* open it */
  if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
    fprintf(stderr, "Could not open codec\n");
    exit(1);
  }

  /* AV_PIX_FMT_BGR24 改为 AV_PIX_FMT_RGB24 解码出来的图片可能会颜色不对 */
  img_convert_ctx = sws_getContext(codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
                                   codec_ctx->width, codec_ctx->height, AV_PIX_FMT_BGR24,
                                   SWS_BICUBIC, NULL, NULL, NULL);
  if (img_convert_ctx == NULL) {
    fprintf(stderr, "Cannot initialize the conversion context\n");
    exit(1);
  }

  frame = av_frame_alloc();
  if (!frame) {
    fprintf(stderr, "Could not allocate video frame\n");
    exit(1);
  }

  while (av_read_frame(fmt_ctx, pkt) >= 0) {
    /* NOTE1: some codecs are stream based (mpegvideo, mpegaudio)
        and this is the only method to use them because you cannot
        know the compressed data size before analysing it.
        
        BUT some other codecs (msmpeg4, mpeg4) are inherently frame
        based, so you must call them with all the data for one
        frame exactly. You must also initialize 'width' and
        'height' before initializing them. */
        
    /* NOTE2: some codecs allow the raw parameters (frame_size,
       sample_rate) to be changed at any frame. We handle this, so
       you should also take care of it */
    
    /* here, we use a stream based decoder (mpeg1video), so we
       feed decoder and see if it could decode a frame */
    if (pkt->stream_index == stream_index) {
      decode(codec_ctx, frame, pkt, img_convert_ctx, out_filename);
    }
    av_packet_unref(pkt);
  }

  /* flush the decoder */
  /* Some codecs, such as MPEG, transmit the I- and P-frame with a
       latency of one frame. You must do the following to have a
       chance to get the last frame of the video. */
  pkt->data = NULL;
  pkt->size = 0;
  decode(codec_ctx, frame, pkt, img_convert_ctx, out_filename);

  avformat_close_input(&fmt_ctx);
  sws_freeContext(img_convert_ctx);
  avcodec_free_context(&codec);
  av_frame_free(&frame);
  av_packet_free(&pkt);

  return 0;
}