//Remux streams from one container format to another.
//从一种封装容器到另一种封装容器的转换，这个例子不做转码，比如改变分辨率，帧率，编码方式等，而是将整个视频从头到尾转存

#define _CRT_SECURE_NO_WARNINGS
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>

static void log_packet(const AVFormatContext* fmt_ctx, const AVPacket* pkt, const char* tag) {
  AVRational* time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

  printf("%s: pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
    tag,
    av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
    av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
    av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
    pkt->stream_index);
}

int main(int argc, char* argv[])
{
  AVFormatContext* ifmt_ctx = NULL;
  AVFormatContext* ofmt_ctx = NULL;
  AVOutputFormat* ofmt = NULL;
  AVPacket* pkt = NULL;

  int stream_index = 0;
  int* stream_mapping = NULL;  // 数组用于存放输出文件流的index
  int stream_mapping_size = 0;  // 输入文件中流的总数量

  if (argc < 3) {
    printf("usage: %s input output\n"
      "API example program to remux a media file with libavformat and libavcodec.\n"
      "The output format is guessed according to the file extension.\n"
      "\n", argv[0]);
    return 1;
  }

  int ret;
  const char* in_filename = argv[1];
  const char* out_filename = argv[2];

  //av_register_all();
  pkt = av_packet_alloc();
  if (!pkt) {
    fprintf(stderr, "Could not allocate AVPacket\n");
    return 1;
  }

  if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0)) < 0) {
    fprintf(stderr, "Could not open input file '%s'", in_filename);
    goto end;
  }

  if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
    fprintf(stderr, "Failed to retrieve input stream information");
    goto end;
  }
  // 打印输入文件的相关信息
  av_dump_format(ifmt_ctx, 0, in_filename, 0);

  avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out_filename);
  if (!ofmt_ctx) {
    fprintf(stderr, "Could not create output context\n");
    ret = AVERROR_UNKNOWN;
    goto end;
  }

  stream_mapping_size = ifmt_ctx->nb_streams;
  // av_mallocz_array() 这个函数在新版本中好像已经丢弃了，改为av_calloc
  stream_mapping = av_calloc(stream_mapping_size, sizeof(*stream_mapping));
  if (!stream_mapping) {
    ret = AVERROR(ENOMEM);
    goto end;
  }

  ofmt = ofmt_ctx->oformat;
  // 遍历输入文件中的每一路流，对于每一路流都要创建一个新的流进行输出
  for (int i = 0; i < ifmt_ctx->nb_streams; i++) {
    AVStream* out_stream;
    AVStream* in_stream = ifmt_ctx->streams[i];
    AVCodecParameters* in_codecpar = in_stream->codecpar;

    // 只保留音频、视频、字幕流，其他流不要
    if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
      in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
      in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
      stream_mapping[i] = -1;
      continue;
    }
    stream_mapping[i] = stream_index++;

    // 创建一个对应的输出流
    out_stream = avformat_new_stream(ofmt_ctx, NULL);
    if (!out_stream) {
      fprintf(stderr, "Failed allocating output stream\n");
      ret = AVERROR_UNKNOWN;
      goto end;
    }

    // 将输入流的编解码参数拷贝到输出流中
    ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
    if (ret < 0) {
      fprintf(stderr, "Failed to copy codec parameters\n");
      goto end;
    }
    out_stream->codecpar->codec_tag = 0;
  }
  av_dump_format(ofmt_ctx, 0, out_filename, 1);

  if (!(ofmt->flags & AVFMT_NOFILE)) {
    ret = avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
    if (ret < 0) {
      fprintf(stderr, "Could not open output file '%s'", out_filename);
      goto end;
    }
  }

  ret = avformat_write_header(ofmt_ctx, NULL);
  if (ret < 0) {
    fprintf(stderr, "Error occurred when opening output file\n");
    goto end;
  }

  while (1) {
    AVStream* in_stream;
    AVStream* out_stream;
    ret = av_read_frame(ifmt_ctx, pkt);
    if (ret < 0)
      break;

    // 如果当前包的流不是音频、视频或字幕，就不用做接下来的处理
    in_stream = ifmt_ctx->streams[pkt->stream_index];
    if (pkt->stream_index >= stream_mapping_size ||
      stream_mapping[pkt->stream_index] < 0) {
      av_packet_unref(pkt);
      continue;
    }

    // 按照输出流的index给pkt重新编号
    pkt->stream_index = stream_mapping[pkt->stream_index];
    out_stream = ofmt_ctx->streams[pkt->stream_index];
    log_packet(ifmt_ctx, pkt, "in");

    // copy packet
    /*pkt->pts = av_rescale_q_rnd(pkt->pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
    pkt->dts = av_rescale_q_rnd(pkt->dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
    pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);*/
    // 这个函数用于转封装过程中的时间基转换，将AVPacket中的各种时间值从一种时间基转换为另一种时间基
    // 比如说mp4转flv，如果这两种容器的时间基不一样，需要重新计算pts和dts，否则可能会有音视频同步问题
    av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
    pkt->pos = -1;
    log_packet(ofmt_ctx, pkt, "out");

    // 将处理好的pkt写入输出文件
    ret = av_interleaved_write_frame(ofmt_ctx, pkt);
    if (ret < 0) {
      fprintf(stderr, "Error muxing packet\n");
      break;
    }
    //av_packet_unref(pkt);
  }
  av_write_trailer(ofmt_ctx);

end:
  av_packet_free(&pkt);

  avformat_close_input(&ifmt_ctx);

  // close output
  if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
    avio_closep(&ofmt_ctx->pb);
  avformat_free_context(ofmt_ctx);

  av_freep(&stream_mapping);
  if (ret < 0 && ret != AVERROR_EOF) {
    fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
    return 1;
  }

  return 0;
}

