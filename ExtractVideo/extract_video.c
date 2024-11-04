/* 从mp4文件中抽取h264数据步骤如下：
  *1.打开mp4文件并创建一个空文件用于存储H264数据
  *2.提取一路视频流资源
  *3.循环读取流中所有的包(AVPacket), 为每个包添加特征码和sps / pps等数据(只有关键帧前面要添加sps / pps数据，
      其他的只需要添加特征码)，都处理完后将数据写入文件保存。
*/
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <libavutil/log.h>
#include <libavformat/avio.h>
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>

// typedef struct H264BSFContext {
//   uint8_t *sps;
//   uint8_t *pps;
//   int      sps_size;
//   int      pps_size;
//   uint8_t  length_size;
//   uint8_t  new_idr;
//   uint8_t  idr_sps_seen;
//   uint8_t  idr_pps_seen;
//   int      extradata_parsed;
// } H264BSFContext;
// H264BSFContext* s;

#ifndef AV_WB32
#define AV_WB32(p, val) do {                 \
        uint32_t d = (val);                     \
        ((uint8_t*)(p))[3] = (d);               \
        ((uint8_t*)(p))[2] = (d)>>8;            \
        ((uint8_t*)(p))[1] = (d)>>16;           \
        ((uint8_t*)(p))[0] = (d)>>24;           \
    } while(0)
#endif

#ifndef AV_RB16
#define AV_RB16(x)                           \
    ((((const uint8_t*)(x))[0] << 8) |          \
      ((const uint8_t*)(x))[1])
#endif

//添加startcode(特征码)
//构建包含一个NAL单元(长度为sps_pps_size + in_size + nal_header+size)的AVPacket
/*
 在帧前面添加特征码(一般SPS/PPS的帧的特征码用4字节表示，为0X00000001，其他的帧特征码用3个字节表示，为0X000001。也有都用4字节表示的，我们这里采用前面的方式)
 out是要输出的AVPaket
 sps_pps是SPS和PPS数据的指针，对于非关键帧就传NULL
 sps_pps_size是SPS/PPS数据的大小，对于非关键帧传0
 in是指向当前要处理的帧的头信息的指针
 in_size是当前要处理的帧大小(nal_size)
*/
static int alloc_and_copy(AVPacket* out, const uint8_t* sps_pps, uint32_t sps_pps_size,
  const uint8_t* in, uint32_t in_size) {
  uint32_t offset = out->size; // 偏移量，就是out已有数据的大小，后面再写入数据就要从偏移量处开始操作
  uint8_t nal_header_size = offset ? 3 : 4; // startcode的大小
  int err;
  //扩充out的容量，扩容的大小就是此次要写入的内容的大小，也就是特征码大小加上sps/pps大小加上加上本帧数据大小
  err = av_grow_packet(out, sps_pps_size + in_size + nal_header_size);
  if (err < 0)
    return err;

  if (sps_pps) {
    //写入sps pps，第一个参数是找写入位置
    memcpy(out->data + offset, sps_pps, sps_pps_size);
  }
  //写入原始数据
  memcpy(out->data + sps_pps_size + nal_header_size + offset, in, in_size);
  if (!offset) {
    AV_WB32(out->data + sps_pps_size, 1);
  } else {
    (out->data + offset + sps_pps_size)[0] = 0;
    (out->data + offset + sps_pps_size)[1] = 0;
    (out->data + offset + sps_pps_size)[2] = 1;
  }

  return 0;
}

/*
 *读取并拷贝sps/pps数据
 *codec_extradata是codecpar的扩展数据，sps/pps数据就在这个扩展数据里面
 *codec_extradata_size是扩展数据大小
 *out_extradata是输出sps/pps数据的AVPacket包
 *padding:就是宏AV_INPUT_BUFFER_PADDING_SIZE的值(64)，是用于解码的输入流的末尾必要的额外字节个数，
 *需要它主要是因为一些优化的流读取器一次读取32或者64比特，可能会读取超过size大小内存的末尾。
*/
int h264_extradata_to_annexb(const uint8_t* codec_extradata, const int codec_extradata_size, AVPacket* out_extradata, int padding) {
  uint16_t unit_size;
  uint64_t total_size = 0;
  uint8_t* out = NULL;
  uint8_t unit_nb;
  uint8_t sps_done = 0;
  uint8_t sps_seen = 0;
  uint8_t pps_seen = 0;

  /**
     * AVCC
     * bits
     *  8   version ( always 0x01 )
     *  8   avc profile ( sps[0][1] )
     *  8   avc compatibility ( sps[0][2] )
     *  8   avc level ( sps[0][3] )
     *  6   reserved ( all bits on )
     *  2   NALULengthSizeMinusOne    // 这个值是（前缀长度-1），值如果是3，那前缀就是4，因为4-1=3
     *  3   reserved ( all bits on )
     *  5   number of SPS NALUs (usually 1)
     *
     *  repeated once per SPS:
     *  16     SPS size
     *
     *  variable   SPS NALU data
     *  8   number of PPS NALUs (usually 1)
     *  repeated once per PPS
     *  16    PPS size
     *  variable PPS NALU data
     */

  const uint8_t* extradata = codec_extradata + 4; //扩展数据前4个字节是无用的，需要跳过
  static const uint8_t nalu_header[4] = { 0, 0, 0, 1 };  //每个H264裸数据都是以 0001 4个字节为开头的
  int length_size = (*extradata++ & 0x3) + 1; // retrieve length coded size, 用于指示表示编码数据长度所需字节数

  uint8_t sps_offset = -1;
  uint8_t pps_offset = -1;

  /* retrieve sps and pps unit(s) */
  unit_nb = *extradata++ & 0x1f; /* number of sps unit(s) */
  if (!unit_nb) { // unit_nb为0表示没有sps数据，直接跳转到处理pps的地方
    goto pps;
  } else { // unit_nb不为0表有sps数据，所以sps_seen赋值1，sps_offset赋值0
    sps_offset = 0;
    sps_seen = 1;
  }

  while (unit_nb--) { // 遍历每个sps或pps(先变量sps，然后再遍历pps)
    int err;
    unit_size = AV_RB16(extradata);
    total_size += unit_size + 4; //加上4字节的startcode, 即 0001
    if (total_size > INT_MAX - padding) { // total_size太大会造成数据溢出，所以要做判断
      av_log(NULL, AV_LOG_ERROR,
        "Too big extradata size, corrupted stream or invalid MP4/AVCC bitstream\n");
      av_free(out);
      return AVERROR(EINVAL);
    }

    //2:表示上面 unit_size 的所占字结数
    //这句的意思是 extradata 所指的地址，加两个字节，再加 unit 的大小所指向的地址
    //是否超过了能访问的有效地址空间
    if (extradata + 2 + unit_size > codec_extradata + codec_extradata_size) {
      av_log(NULL, AV_LOG_ERROR, "Packet header is not contained in global extradata, "
        "corrupted stream or invalid MP4/AVCC bitstream\n");
      av_free(out);
      return AVERROR(EINVAL);
    }
    //分配存放 SPS 的空间
    if ((err = av_reallocp(&out, total_size + padding)) < 0)
      return err;
    // 先将4字节的特征码拷贝进out
    memcpy(out + total_size - unit_size - 4, nalu_header, 4);
    // 再将sps/pps数据拷贝进out,extradata + 2是因为那2字节是表示sps/pps长度的，所以要跳过
    memcpy(out + total_size - unit_size, extradata + 2, unit_size);
    // 本次sps/pps数据处理完后，指针extradata跳过本次sps/pps数据
    extradata += 2 + unit_size;
  pps:
    //当 SPS 处理完后，开始处理 PPS
    if (!unit_nb && !sps_done++) {
      unit_nb = *extradata++; /* number of pps unit(s) */
      if (unit_nb) {
        pps_offset = total_size;
        pps_seen = 1;
      }
    }
  }

  if (out)
    memset(out + total_size, 0, padding); //余下的空间清0
  if (!sps_seen)
    av_log(NULL, AV_LOG_WARNING,
      "Warning: SPS NALU missing or invalid.The resulting stream may not play.\n");
  if (!pps_seen)
    av_log(NULL, AV_LOG_WARNING,
      "Warning: PPS NALU missing or invalid.The resulting stream may not play.\n");

  out_extradata->data = out;
  out_extradata->size = total_size;
  return length_size;
}

/*
 *为包数据添加起始码、SPS/PPS等信息后写入文件。
 *AVPacket数据包可能包含一帧或几帧数据，对于视频来说只有1帧，对音频来说就包含几帧
 *in为要处理的数据包
 *file为输出文件的指针
*/
int h264_mp4toannexb(AVFormatContext* fmt_ctx, AVPacket* in, FILE* dst_fd) {
  AVPacket* out = NULL;
  AVPacket spspps_pkt; // sps/pps数据的AVPaket
  uint8_t unit_type; // NALU头中nal_unit_type，也就是NALU类型，5表示是I帧，7表示SPS，8表示PPS
  uint32_t nal_size; // 一个NALU(也就是一帧，其第一个字节是头信息)的大小，它存放在NALU的前面的4个字节中
  uint32_t cumul_size = 0; // 已经处理的字节数，当cumul_size==buf_size时表示整个包的数据都处理完了
  int len;
  int ret = 0, i;

  out = av_packet_alloc();

  const uint8_t* buf = in->data;
  int buf_size = in->size;
  const uint8_t* buf_end = in->data + in->size;

  do {
    ret = AVERROR(EINVAL);
    //因为每个视频帧的前 4 个字节是视频帧的长度
    //如果buf中的数据都不能满足4字节，所以后面就没有必要再进行处理了
    if (buf + 4 /*s->length_size*/ > buf_end) {
      av_packet_free(&out);
      return ret;
    }
    for (nal_size = 0, i = 0; i < 4; i++)
      nal_size = (nal_size << 8) | buf[i];

    buf += 4;  //跳过4字节（也就是视频帧长度），从而指向真正的视频帧数据
    //一帧数据的第一个字节后五位是这一帧的类型sps pps
    unit_type = *buf & 0x1f; // 取出NALU头信息的后面5个bit，这5bit记录NALU的类型

    //如果视频帧长度大于从 AVPacket 中读到的数据大小，说明这个数据包出错了
    if (nal_size > buf_end - buf || nal_size < 0) {
      av_packet_free(&out);
      return ret;
    }

    //unit_type 可以在h264.h文件中查看，unit_type=5，即H264_NAL_IDR_SLICE 
    //http://ffmpeg.org/doxygen/4.4/h264_8h_source.html
    /* prepend only to the first type 5 NAL unit of an IDR picture, if no sps/pps are already present */
    // IDR frame
    if (unit_type == 5) {
      //关键帧IDR帧需要添加sps pps
      h264_extradata_to_annexb(fmt_ctx->streams[in->stream_index]->codecpar->extradata,
       fmt_ctx->streams[in->stream_index]->codecpar->extradata_size,
       &spspps_pkt,
       AV_INPUT_BUFFER_PADDING_SIZE);
      //添加特征码
      if ((ret = alloc_and_copy(out, spspps_pkt.data, spspps_pkt.size, buf, nal_size)) < 0) {
        av_packet_free(&out);
        return ret;
      }
    } else {
      //非关键帧不需要添加sps pps
      if ((ret = alloc_and_copy(out, NULL, 0, buf, nal_size)) < 0) {
        av_packet_free(&out);
        return ret;
      }
    }

    len = fwrite(out->data, 1, out->size, dst_fd);
    if (len != out->size) {
      av_log(NULL, AV_LOG_DEBUG, "warning, length of writed data isn't equal pkt->size(%d, %d)\n",
        len,
        out->size);
    }
    fflush(dst_fd);
  next_nal:
    buf += nal_size; // 一帧处理完后将指针移到下一帧
    cumul_size += nal_size + 4; // 累计已经处理好的数据长度
  } while (cumul_size < buf_size);

  return ret;
}

int main(int argc, char* argv[])
{
  int err_code;
  char errors[1024];

  char* src_filename = NULL;
  char* dst_filename = NULL;

  av_log_set_level(AV_LOG_DEBUG);

  if (argc < 3) {
    av_log(NULL, AV_LOG_DEBUG, "the count of parameters should be more than three!\n");
    return -1;
  }

  src_filename = argv[1];
  dst_filename = argv[2];
  if (src_filename == NULL || dst_filename == NULL) {
    av_log(NULL, AV_LOG_ERROR, "src or dts file is null, please check them!\n");
    return -1;
  }

  FILE* dst_fd = NULL;
  dst_fd = fopen(dst_filename, "wb");
  if (!dst_fd) {
    av_log(NULL, AV_LOG_DEBUG, "Could not open destination file %s\n", dst_filename);
    return -1;
  }

  AVFormatContext* fmt_ctx = NULL;
  //open input media file, and allocate format context
  if ((err_code = avformat_open_input(&fmt_ctx, src_filename, NULL, NULL)) < 0) {
    av_strerror(err_code, errors, 1024);
    av_log(NULL, AV_LOG_DEBUG, "Could not open source file: %s, %d(%s)\n",
      src_filename,
      err_code,
      errors);
    return -1;
  }

  //dump input information
  av_dump_format(fmt_ctx, 0, src_filename, 0);

  //find best video stream
  int video_stream_index = -1;
  video_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  if (video_stream_index < 0) {
    av_log(NULL, AV_LOG_DEBUG, "Could not find %s stream in input file %s\n",
      av_get_media_type_string(AVMEDIA_TYPE_VIDEO),
      src_filename);
    return AVERROR(EINVAL);
  }

  //initialize packet
  AVPacket* pkt = NULL;
  //av_init_packet(&pkt);
  pkt = av_packet_alloc();
  pkt->data = NULL;
  pkt->size = 0;

  //// init h264_mp4toannexb_filter
  //// 获取对应的比特流过滤器
  //const AVBitStreamFilter* bsfilter = av_bsf_get_by_name("h264_mp4toannexb");
  //AVBSFContext* bsf_ctx = NULL;
  //// 初始化过滤器上下文
  //av_bsf_alloc(bsfilter, &bsf_ctx);
  //// 添加解码器属性
  //avcodec_parameters_copy(bsf_ctx->par_in, fmt_ctx->streams[video_stream_index]->codecpar);
  //av_bsf_init(bsf_ctx);

  //read frame from media file
  while (av_read_frame(fmt_ctx, pkt) >= 0) {
    if (pkt->stream_index == video_stream_index) {
      h264_mp4toannexb(fmt_ctx, pkt, dst_fd);

      ////使用h264_mp4toannexb_filter过滤器进行处理
      //int input_size = pkt->size;
      //int out_pkt_count = 0;
      //if (av_bsf_send_packet(bsf_ctx, pkt) != 0) {
      //  av_packet_unref(pkt);
      //  continue;
      //}
      //av_packet_unref(pkt);
      //while (av_bsf_receive_packet(bsf_ctx, pkt) == 0) {
      //  out_pkt_count++;
      //  size_t size = fwrite(pkt->data, 1, pkt->size, dst_fd);
      //  if (size != pkt->size) {
      //    av_log(NULL, AV_LOG_DEBUG, "fwrite failed-> %u, pkt_size: %u\n",
      //      size, pkt->size);
      //  }
      //  av_packet_unref(pkt);
      //}
      //if (out_pkt_count >= 2) {
      //  av_log(NULL, AV_LOG_INFO, "cur pkt(size: %d) only get 1 out pkt, it get %d pkts\n",
      //    input_size, out_pkt_count);
      //}
    }
    //release pkt->data
    av_packet_unref(pkt);
  }

  //if (bsf_ctx)
  //  av_bsf_free(&bsf_ctx);
  //close input media file
  avformat_close_input(&fmt_ctx);
  if (dst_fd) {
    fclose(dst_fd);
  }

  return 0;
}
