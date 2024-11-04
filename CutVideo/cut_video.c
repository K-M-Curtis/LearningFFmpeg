/* 视频裁剪的本质就是将音视频流中特定范围内的数据帧提取出来，封装成对应格式的视频流然后输出到本地. */
//pts:Presentation Time Stamp 解码之后的视频帧什么时候展示出来
//dts:Decode Time Stamp 内存中的数据什么时候开始编码
//time_base 时间刻度,也就是用来标记一秒钟被拆分成了多少份time_base={1,25}1秒拆分成了25份
//pts/dts 即时时间都是时间刻度。
//av_q2d(time_base)=每个刻度是多少秒
//pts*av_q2d(time_base)才是帧的显示时间戳

#include <stdlib.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

static void log_packet(const AVFormatContext* fmt_ctx, const AVPacket* pkt, const char* tag) {
  AVRational* time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

  printf("%s: pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
    tag,
    av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
    av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
    av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
    pkt->stream_index);
}

int cut_video(double from_seconds, double end_seconds, const char* in_filename, const char* out_filename) {
  AVOutputFormat* ofmt = NULL;
  AVFormatContext* ifmt_ctx = NULL;
  AVFormatContext* ofmt_ctx = NULL;
  AVPacket pkt;
  const AVCodec* codec;
  int ret;
  
  if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0)) < 0) {
    fprintf(stderr, "Could not open input file '%s'", in_filename);
    goto end;
  }
  
  if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
    fprintf(stderr, "Failed to retrieve input stream information");
    goto end;
  }
  
  av_dump_format(ifmt_ctx, 0, in_filename, 0);
  
  avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out_filename);
  if (!ofmt_ctx) {
    fprintf(stderr, "Could not create output context\n");
    ret = AVERROR_UNKNOWN;
    goto end;
  }
  
  ofmt = ofmt_ctx->oformat;
  for (int i = 0; i < ifmt_ctx->nb_streams; i++) {
    AVStream* in_stream = ifmt_ctx->streams[i];
    codec = avcodec_find_decoder(in_stream->codecpar->codec_id);
    AVStream* out_stream = avformat_new_stream(ofmt_ctx, codec);
    if (!out_stream) {
      fprintf(stderr, "Failed allocating output stream\n");
      ret = AVERROR_UNKNOWN;
      goto end;
    }
  
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
      fprintf(stderr, "Failed allocating Codec Context\n");
      ret = AVERROR_UNKNOWN;
      goto end;
    }
  
    ret = avcodec_parameters_to_context(codec_ctx, in_stream->codecpar);
    if (ret < 0) {
      fprintf(stderr, "Failed to copy in_stream codecpar to codec context\n");
      goto end;
    }
    codec_ctx->codec_tag = 0;
    if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
      codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  
    ret = avcodec_parameters_from_context(out_stream->codecpar, codec_ctx);
    if (ret < 0) {
      fprintf(stderr, "Failed to copy codec context to out_stream codecpar context\n");
      goto end;
    }
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
  
  ret = av_seek_frame(ifmt_ctx, -1, from_seconds * AV_TIME_BASE, AVSEEK_FLAG_ANY);
  if (ret < 0) {
    fprintf(stderr, "Error seek\n");
    goto end;
  }
  
  int64_t* dts_start_from = malloc(sizeof(int64_t) * ifmt_ctx->nb_streams);
  memset(dts_start_from, 0, sizeof(int64_t) * ifmt_ctx->nb_streams);
  int64_t* pts_start_from = malloc(sizeof(int64_t) * ifmt_ctx->nb_streams);
  memset(pts_start_from, 0, sizeof(int64_t) * ifmt_ctx->nb_streams);
  
  while (1) {
    AVStream* in_stream;
    AVStream* out_stream;
  
    ret = av_read_frame(ifmt_ctx, &pkt);
    if (ret < 0)
      break;
    in_stream = ifmt_ctx->streams[pkt.stream_index];
    out_stream = ofmt_ctx->streams[pkt.stream_index];
  
    log_packet(ifmt_ctx, &pkt, "in");
  
    if (av_q2d(in_stream->time_base) * pkt.pts > end_seconds) {
      av_packet_unref(&pkt);
      break;
    }
  
    // 将截取后的每个流的起始dts 、pts保存下来，作为开始时间，用来做后面的时间基转换
    if (dts_start_from[pkt.stream_index] == 0) {
      dts_start_from[pkt.stream_index] = pkt.dts;
      printf("dts_start_from: %s\n", av_ts2str(dts_start_from[pkt.stream_index]));
    }
    if (pts_start_from[pkt.stream_index] == 0) {
      pts_start_from[pkt.stream_index] = pkt.pts;
      printf("pts_start_from: %s\n", av_ts2str(pts_start_from[pkt.stream_index]));
    }
  
    // av_rescale_q_rnd中参数a减掉pts_start_from[pkt.stream_index]有待探究和实验，减掉后会出现丢帧的问题
    pkt.pts = av_rescale_q_rnd(pkt.pts /*- pts_start_from[pkt.stream_index]*/, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
    pkt.dts = av_rescale_q_rnd(pkt.dts /*- dts_start_from[pkt.stream_index]*/, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
    if (pkt.pts < 0) {
      pkt.pts = 0;
    }
    if (pkt.dts < 0) {
      pkt.dts = 0;
    }
    pkt.duration = (int)av_rescale_q((int64_t)pkt.duration, in_stream->time_base, out_stream->time_base);
    pkt.pos = -1;
    log_packet(ofmt_ctx, &pkt, "out");
    printf("\n");
  
    //一帧视频播放时间必须在解码时间点之后，当出现pkt.pts < pkt.dts时会导致程序异常，所以我们丢掉有问题的帧，不会有太大影响。
    //不加这个判断的话，写入时会有问题，加这个判断的话
    if (pkt.pts < pkt.dts)
      continue;

    ret = av_interleaved_write_frame(ofmt_ctx, &pkt);
    if (ret < 0) {
      fprintf(stderr, "Error muxing packet\n");
      break;
    }
    av_packet_unref(&pkt);
  }
  
  free(dts_start_from);
  free(pts_start_from);
  
  av_write_trailer(ofmt_ctx);
  
end:
  avformat_close_input(&ifmt_ctx);
  
  /* close output */
  if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
    avio_closep(&ofmt_ctx->pb);
  avformat_free_context(ofmt_ctx);
  
  if (ret < 0 && ret != AVERROR_EOF) {
    fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
    return 1;
  }
  return 0;
}

int main(int argc, char* argv[]) {
  if (argc < 5) {
    fprintf(stderr, "Usage: \
                command startime, endtime, srcfile, outfile");
    return -1;
  }
  double starttime = atoi(argv[1]);
  double endtime = atoi(argv[2]);
  cut_video(starttime, endtime, argv[3], argv[4]);

  return 0;
}
