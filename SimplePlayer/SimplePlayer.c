#include <SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/bprint.h>
#include <libavutil/channel_layout.h>
#include <libavutil/fifo.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <signal.h>

#define DEBUG 1

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10
/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30
/* Step size for volume control in dB */
#define SDL_VOLUME_STEP (0.75)
/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0
/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10
/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN 0.900
#define EXTERNAL_CLOCK_SPEED_MAX 1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001
/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB 20
/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

#define CURSOR_HIDE_DELAY 1000000  // us，1秒

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
#define SAMPLE_ARRAY_SIZE (8 * 65536)
// 这个值设置不宜过大，不然解码后帧数据占用内容会很大
#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE   \
  FFMAX(SAMPLE_QUEUE_SIZE, \
        FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)

const char program_name[] = "SimpleMediaPlayer";

typedef struct MyAVPacketList {
  AVPacket* pkt;
  int serial;
} MyAVPacketList;

typedef struct PacketQueue {
  AVFifo* pkt_list;
  int nb_packets;
  int size;
  int64_t duration;
  int abort_request;
  int serial;
  SDL_mutex* mutex;
  SDL_cond* cond;
} PacketQueue;

typedef struct AudioParams {
  int freq;                   // 采样率
  AVChannelLayout ch_layout;  // 通道布局，比如2.1声道，5.1声道等
  // 音频采样格式，比如AV_SAMPLE_FMT_S16表示为有符号16bit深度，交错排列模式。
  enum AVSampleFormat fmt;
  int frame_size;  // 一个采样单元占用的字节数（比如2通道时，则左右通道各采样一次合成一个采样单元）
  int bytes_per_sec;  // 一秒时间的字节数，比如采样率48Khz，2 channel，16bit，则一秒48000*2*16/8=192000
} AudioParams;

/* 用于管理视频和音频流的播放时钟 */
typedef struct Clock {
  double pts;        // 当前时钟时间
  double pts_drift;  // clock base minus time at which we updated the clock
  double last_updated;
  double speed;
  int serial;         // 用于seek，判断是否为同一序列
  int paused;         // 是否暂停
  int* queue_serial;  // 指向当前数据包队列序列的指针
} Clock;

/* 帧数据，为了⾳频、视频、字幕帧通⽤，部分成员只对特定类型 */
typedef struct Frame {
  AVFrame* frame;
  AVSubtitle sub;
  int serial;
  double pts;  // 当前帧的时间戳
  double duration;
  int64_t pos;
  int width;
  int height;
  int format;
  AVRational sar;  // 帧的宽高比，该值来⾃AVFrame结构体的sample_aspect_ratio变量
  int uploaded;  // 标志帧是否已上传到渲染缓冲区，即当前帧是否已显示
  // int flip_v;  // 帧是否需要垂直翻转
} Frame;

/* ========= FrameQueue =========
 帧队列，用数组实现的一个FIFO，一端写一端读
 引入了读取节点但节点不出队列的操作、读取下一节点也不出队列等等的操作
 缓存从解码器获取的视频/音频帧数据,以便于后续的播放。这样可以减少解码器和播放器之间的同步压力
 多线程环境，解码线程和播放线程可以并发地操作帧队列，提高整体的执行效率
*/
typedef struct FrameQueue {
  Frame queue[FRAME_QUEUE_SIZE];
  int rindex;  // 读索引。待播放时读取此帧进行播放，播放后此帧成为上一帧
  int windex;  // 写索引
  int size;    // 当前总帧数
  int max_size;
  int keep_last;     // 是否保持最后一帧
  int rindex_shown;  // 初始化为0，配合keep_last=1使用
  SDL_mutex* mutex;
  SDL_cond* cond;
  PacketQueue* p_queue;
} FrameQueue;

enum {
  AV_SYNC_AUDIO_MASTER,
  AV_SYNC_VIDEO_MASTER,
  AV_SYNC_EXTERNAL_CLOCK,
};

/* ========= 解码器相关 ========= */
typedef struct Decoder {
  AVPacket* pkt;
  PacketQueue* p_queue;
  AVCodecContext* av_ctx;
  int pkt_serial;
  int finished;  // 解码器是否处于工作状态；=0处于工作状态
  int packet_pending;  // 是否有待处理的数据包,避免丢失数据
  // 检查到packet队列空时发送 signal缓存read_thread读取数据
  SDL_cond* empty_queue_cond;
  int64_t start_pts;        // stream的start time
  AVRational start_pts_tb;  // stream的time_base
  int64_t next_pts;
  AVRational next_pts_tb;
  SDL_Thread* decoder_thread;
} Decoder;

typedef struct MediaState {
  SDL_Thread* read_tid;
  const AVInputFormat* iformat;
  AVFormatContext* fmt_ctx;
  int abort_request;  // 视频是否退出或中断播放
  int force_refresh;  // 立即强制刷新
  int paused;
  int last_paused;  // 暂存暂停/播放状态

  /* ====== seek ====== */
  int seek_req;
  int seek_flags;
  int64_t seek_pos;  // 当前位置+增量
  int64_t seek_rel;  // 增量

  int read_pause_return;  // 用于流媒体，网络流(e.g. RTSP stream)
  int realtime;           // 是否为实时流

  Clock audio_clk;
  Clock video_clk;
  Clock external_clk;

  FrameQueue pic_queue;     //视频帧队列
  FrameQueue sub_queue;     //字幕队列
  FrameQueue sample_queue;  //采样队列

  Decoder audio_dec;
  Decoder video_dec;
  Decoder sub_dec;

  int av_sync_type;

  /* ====== audio ====== */
  int audio_index;
  double audio_clock;      // 当前音频帧pts+当前帧duration
  int audio_clock_serial;  // 播放序列，用于seek

  double audio_diff_cum; /* used for AV difference average computation */
  double audio_diff_avg_coef;
  double audio_diff_threshold;
  int audio_diff_avg_count;

  AVStream* audio_stream;
  PacketQueue audio_queue;
  int audio_hw_buf_size;
  uint8_t* audio_buf;
  uint8_t* audio_buf1;
  unsigned int audio_buf_size; /* in bytes */
  unsigned int audio_buf1_size;
  int audio_buf_index;
  int audio_write_buf_size;
  int audio_volume;
  int muted;
  struct AudioParams audio_src;
  struct AudioParams audio_target;
  struct SwrContext* swr_ctx;
  int16_t sample_array[SAMPLE_ARRAY_SIZE];  // 采样数组
  int sample_array_index;                   // 采样索引

  // 用于统计和输出
  int frame_drops_early;  // 丢弃视频packet计数
  int frame_drops_late;   // 丢弃视频frame计数

  enum ShowMode {
    SHOW_MODE_NONE = -1,  // 无显示
    SHOW_MODE_VIDEO = 0,  // 显示视频
    // SHOW_MODE_WAVES,   // 显示声波
    // SHOW_MODE_RDFT,      // 自适应滤波器
    SHOW_MODE_NB
  } show_mode;

  SDL_Texture* sub_texture;
  SDL_Texture* video_texture;

  /* ====== subtitle ====== */
  int subtitle_index;
  AVStream* subtitle_stream;
  PacketQueue subtitle_queue;
  struct SwsContext* sub_convert_ctx;

  double frame_timer;  // 最后一帧播放的时间
  /* ====== video ====== */
  int video_index;
  AVStream* video_stream;
  PacketQueue video_queue;

  double max_frame_duration;  // 帧间最大间隔
  int eof;                    // 是否读取结束

  char* filename;
  int width, height, xleft, ytop;
  int step;  //是否逐帧播放

  // 与循环切换流相关参数
  //int last_video_stream;
  //int last_audio_stream;
  //int last_subtitle_stream;

  SDL_cond* continue_read_thread;
} MediaState;

/* options specified by the user */
static const AVInputFormat* file_iformat;
static const char* input_filename;
static const char* window_title;
static int default_width = 640;
static int default_height = 480;
static int screen_width = 1280;
static int screen_height = 960;
static int screen_left = SDL_WINDOWPOS_CENTERED;
static int screen_top = SDL_WINDOWPOS_CENTERED;

static int audio_disable = 0;
static int video_disable = 0;
static int subtitle_disable = 0;
static const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
static int seek_by_bytes = -1;
static int display_disable = 0;  // 是否显示视频内容
static int borderless = 0;
static int alwaysontop = 0;
static int startup_volume = 100;

// show_status主要作用是在播放过程中显示媒体的状态信息。
static int show_status = -1;
static int av_sync_type = AV_SYNC_AUDIO_MASTER;
static int64_t start_time = AV_NOPTS_VALUE;
static int64_t duration = AV_NOPTS_VALUE;
// 控制视频解码输出的分辨率，降低视频分辨率，节省处理资源，快速预览，这个不用设置，就让他等于0
static int lowres = 0;
static int autoexit = 0;  // exit at the end视频结束后是否自动退出
static int exit_on_keydown = 0;
static int exit_on_mousedown = 0;
static int loop = 1;
static int framedrop = -1;  // 无需设置
static int infinite_buffer = -1;  // =1的话不限制输入缓冲区大小（对实时流有用）
static enum ShowMode show_mode = SHOW_MODE_VIDEO;
static int64_t cursor_last_shown;
static int cursor_hidden = 0;
static int is_full_screen = 0;
static int64_t audio_callback_time;

/* global parameters */
static SDL_Window* window = NULL;
static SDL_Renderer* renderer = NULL;
static SDL_RendererInfo renderer_info = {0};
static SDL_AudioDeviceID audio_deviceId;

static const struct TextureFormatEntry {
  enum AVPixelFormat format;
  int texture_fmt;
} sdl_texture_format_map[] = {
    {AV_PIX_FMT_RGB8, SDL_PIXELFORMAT_RGB332},
    {AV_PIX_FMT_RGB444, SDL_PIXELFORMAT_RGB444},
    {AV_PIX_FMT_RGB555, SDL_PIXELFORMAT_RGB555},
    {AV_PIX_FMT_BGR555, SDL_PIXELFORMAT_BGR555},
    {AV_PIX_FMT_RGB565, SDL_PIXELFORMAT_RGB565},
    {AV_PIX_FMT_BGR565, SDL_PIXELFORMAT_BGR565},
    {AV_PIX_FMT_RGB24, SDL_PIXELFORMAT_RGB24},
    {AV_PIX_FMT_BGR24, SDL_PIXELFORMAT_BGR24},
    {AV_PIX_FMT_0RGB32, SDL_PIXELFORMAT_RGB888},
    {AV_PIX_FMT_0BGR32, SDL_PIXELFORMAT_BGR888},
    {AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888},
    {AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888},
    {AV_PIX_FMT_RGB32, SDL_PIXELFORMAT_ARGB8888},
    {AV_PIX_FMT_RGB32_1, SDL_PIXELFORMAT_RGBA8888},
    {AV_PIX_FMT_BGR32, SDL_PIXELFORMAT_ABGR8888},
    {AV_PIX_FMT_BGR32_1, SDL_PIXELFORMAT_BGRA8888},
    {AV_PIX_FMT_YUV420P, SDL_PIXELFORMAT_IYUV},
    {AV_PIX_FMT_YUYV422, SDL_PIXELFORMAT_YUY2},
    {AV_PIX_FMT_UYVY422, SDL_PIXELFORMAT_UYVY},
    {AV_PIX_FMT_NONE, SDL_PIXELFORMAT_UNKNOWN},
};

/* ============= about PacketQueue ============= */
static int packet_queue_put_private(PacketQueue* p_queue, AVPacket* pkt) {
  MyAVPacketList tmp_pkt;
  int ret;

  if (p_queue->abort_request) return -1;

  tmp_pkt.pkt = pkt;
  tmp_pkt.serial = p_queue->serial;

  // av_fifo_write确保数据包按它们到达的顺序被存储和处理。
  ret = av_fifo_write(p_queue->pkt_list, &tmp_pkt, 1);
  if (ret < 0) return ret;
  p_queue->nb_packets++;
  p_queue->size += tmp_pkt.pkt->size + sizeof(tmp_pkt);
  p_queue->duration += tmp_pkt.pkt->duration;
  SDL_CondSignal(p_queue->cond);
  return 0;
}

static int packet_queue_put(PacketQueue* p_queue, AVPacket* pkt) {
  AVPacket* pkt1;
  int ret;

  pkt1 = av_packet_alloc();
  if (!pkt1) {
    av_packet_unref(pkt);
    return -1;
  }
  av_packet_move_ref(pkt1, pkt);

  SDL_LockMutex(p_queue->mutex);
  ret = packet_queue_put_private(p_queue, pkt1);
  SDL_UnlockMutex(p_queue->mutex);
  if (ret < 0) {
    av_packet_free(&pkt1);
  }
  return ret;
}

// 放⼊空包意味着流的结束，⼀般在媒体数据读取完成的时候放⼊空包。放⼊空包，⽬的是为了冲刷解码器，将编码器⾥⾯所有frame都读取出来
static int packet_queue_put_nullpacket(PacketQueue* p_queue, AVPacket* pkt,
                                       int stream_index) {
  pkt->stream_index = stream_index;
  return packet_queue_put(p_queue, pkt);
}

static int packet_queue_init(PacketQueue* p_queue) {
  memset(p_queue, 0, sizeof(PacketQueue));
  p_queue->pkt_list =
      av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
  if (!p_queue->pkt_list) return AVERROR(ENOMEM);
  p_queue->mutex = SDL_CreateMutex();
  if (!p_queue->mutex) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  p_queue->cond = SDL_CreateCond();
  if (!p_queue->cond) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  p_queue->abort_request = 1;
  return 0;
}

// ⽤于将packet队列中的所有节点清除，包括节点对应的AVPacket。⽐如⽤于退出播放和seek播放
static void packet_queue_flush(PacketQueue* p_queue) {
  MyAVPacketList pkt1;

  SDL_LockMutex(p_queue->mutex);
  while (av_fifo_read(p_queue->pkt_list, &pkt1, 1) >= 0) {
    av_packet_free(&pkt1.pkt);
  }
  p_queue->nb_packets = 0;
  p_queue->size = 0;
  p_queue->duration = 0;
  p_queue->serial++;
  SDL_UnlockMutex(p_queue->mutex);
}

static void packet_queue_destroy(PacketQueue* p_queue) {
  packet_queue_flush(p_queue);
  av_fifo_freep2(&p_queue->pkt_list);
  SDL_DestroyMutex(p_queue->mutex);
  SDL_DestroyCond(p_queue->cond);
}

static void packet_queue_abort(PacketQueue* p_queue) {
  SDL_LockMutex(p_queue->mutex);
  p_queue->abort_request = 1;
  SDL_CondSignal(p_queue->cond);
  SDL_UnlockMutex(p_queue->mutex);
}

static void packet_queue_start(PacketQueue* p_queue) {
  SDL_LockMutex(p_queue->mutex);
  p_queue->abort_request = 0;
  p_queue->serial++;
  SDL_UnlockMutex(p_queue->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue* p_queue, AVPacket* pkt, int block,
                            int* serial) {
  MyAVPacketList pkt1;
  int ret;

  SDL_LockMutex(p_queue->mutex);

  while (1) {
    if (p_queue->abort_request) {
      ret = -1;
      break;
    }

    if (av_fifo_read(p_queue->pkt_list, &pkt1, 1) >= 0) {
      p_queue->nb_packets--;
      p_queue->size -= pkt1.pkt->size + sizeof(pkt1);
      p_queue->duration -= pkt1.pkt->duration;
      av_packet_move_ref(pkt, pkt1.pkt);
      if (serial) {
        *serial = pkt1.serial;
      }
      av_packet_free(&pkt1.pkt);
      ret = 1;
      break;
    } else if (!block) {
      ret = 0;
      break;
    } else {
      SDL_CondWait(p_queue->cond, p_queue->mutex);
    }
  }
  SDL_UnlockMutex(p_queue->mutex);
  return ret;
}
/* ============= about PacketQueue ============= */

/* ============= about FrameQueue ============= */
static void frame_queue_unref_item(Frame* frame) {
  av_frame_unref(frame->frame);
  avsubtitle_free(&frame->sub);
}

static int frame_queue_init(FrameQueue* f_queue, PacketQueue* p_queue,
                            int max_size, int keep_last) {
  memset(f_queue, 0, sizeof(FrameQueue));
  if (!(f_queue->mutex = SDL_CreateMutex())) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }

  if (!(f_queue->cond = SDL_CreateCond())) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  f_queue->p_queue = p_queue;
  f_queue->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
  f_queue->keep_last = !!keep_last;
  for (int i = 0; i < f_queue->max_size; i++) {
    if (!(f_queue->queue[i].frame = av_frame_alloc())) {
      return AVERROR(ENOMEM);
    }
  }
  return 0;
}

static void frame_queue_destroy(FrameQueue* f_queue) {
  for (int i = 0; i < f_queue->max_size; i++) {
    Frame* vp = &f_queue->queue[i];
    frame_queue_unref_item(vp);
    av_frame_free(&vp->frame);
  }
  SDL_DestroyMutex(f_queue->mutex);
  SDL_DestroyCond(f_queue->cond);
}

// 发送条件信号，通知等待的线程
static void frame_queue_signal(FrameQueue* f_queue) {
  SDL_LockMutex(f_queue->mutex);
  SDL_CondSignal(f_queue->cond);
  SDL_UnlockMutex(f_queue->mutex);
}

// 返回当前读取位置的帧（不移除）
static Frame* frame_queue_peek(FrameQueue* f_queue) {
  return &f_queue->queue[(f_queue->rindex + f_queue->rindex_shown) %
                         f_queue->max_size];
}

// 返回下一读取位置的帧（不移除）
static Frame* frame_queue_peek_next(FrameQueue* f_queue) {
  return &f_queue->queue[(f_queue->rindex + f_queue->rindex_shown + 1) %
                         f_queue->max_size];
}

/* ========= 获取last Frame，返回最后读取位置的帧 =========
 * 当rindex_shown=0时，和frame_queue_peek效果一样
 * 当rindex_shown=1时，读取的是已经显示过的frame
*/
static Frame* frame_queue_peek_last(FrameQueue* f_queue) {
  return &f_queue->queue[f_queue->rindex];
}

// 等待直到有可写入的新帧空间，返回可写入帧的指针
static Frame* frame_queue_peek_writable(FrameQueue* f_queue) {
  SDL_LockMutex(f_queue->mutex);
  while (f_queue->size >= f_queue->max_size &&
         !f_queue->p_queue->abort_request) {
    SDL_CondWait(f_queue->cond, f_queue->mutex);
  }
  SDL_UnlockMutex(f_queue->mutex);

  if (f_queue->p_queue->abort_request) {
    return NULL;
  }
  return &f_queue->queue[f_queue->windex];
}

static Frame* frame_queue_peek_readable(FrameQueue* f_queue) {
  SDL_LockMutex(f_queue->mutex);
  while (f_queue->size - f_queue->rindex_shown <= 0 &&
         !f_queue->p_queue->abort_request) {
    SDL_CondWait(f_queue->cond, f_queue->mutex);
  }
  SDL_UnlockMutex(f_queue->mutex);

  if (f_queue->p_queue->abort_request) {
    return NULL;
  }
  return &f_queue->queue[(f_queue->rindex + f_queue->rindex_shown) %
                         f_queue->max_size];
}

static void frame_queue_push(FrameQueue* f_queue) {
  if (++f_queue->windex == f_queue->max_size) {
    f_queue->windex = 0;
  }
  SDL_LockMutex(f_queue->mutex);
  f_queue->size++;
  SDL_CondSignal(f_queue->cond);
  SDL_UnlockMutex(f_queue->mutex);
}

// 将读取位置推进到下一个位置，减少队列中的帧数，并发送条件信号。
// 更新读索引(同时删除旧frame)
static void frame_queue_next(FrameQueue* f_queue) {
  if (f_queue->keep_last && !f_queue->rindex_shown) {
    f_queue->rindex_shown = 1;
    return;
  }
  frame_queue_unref_item(&f_queue->queue[f_queue->rindex]);
  if (++f_queue->rindex == f_queue->max_size) {
    f_queue->rindex = 0;
  }
  SDL_LockMutex(f_queue->mutex);
  f_queue->size--;
  SDL_CondSignal(f_queue->cond);
  SDL_UnlockMutex(f_queue->mutex);
}

/* return the number of undisplayed frames in the queue */
static int frame_queue_nb_remaining(FrameQueue* f_queue) {
  return f_queue->size - f_queue->rindex_shown;
}

// 获取最近播放Frame对应数据在媒体⽂件的位置，主要在seek时使⽤
/* return last shown position */
static int64_t frame_queue_last_pos(FrameQueue* f_queue) {
  Frame* fp = &f_queue->queue[f_queue->rindex];
  if (f_queue->rindex_shown && fp->serial == f_queue->p_queue->serial)
    return fp->pos;
  else
    return -1;
}
/* ============= about FrameQueue ============= */

/* ============= about Decoder ============= */
static int decoder_init(Decoder* decoder, AVCodecContext* av_ctx,
                        PacketQueue* p_queue, SDL_cond* empty_queue_cond) {
  memset(decoder, 0, sizeof(Decoder));
  decoder->pkt = av_packet_alloc();
  if (!decoder->pkt) return AVERROR(ENOMEM);
  decoder->av_ctx = av_ctx;
  decoder->p_queue = p_queue;
  decoder->empty_queue_cond = empty_queue_cond;
  decoder->start_pts = AV_NOPTS_VALUE;
  decoder->pkt_serial = -1;
  return 0;
}

// 从解码队列中获取数据包，并将其解码为视频或音频帧。
static int decoder_decode_frame(Decoder* decoder, AVFrame* frame,
                                AVSubtitle* sub) {
  // 表示需要更多的数据才能进行解码，会去到packet_queue_get()
  int ret = AVERROR(EAGAIN);

  while (1) {
    if (decoder->p_queue->serial == decoder->pkt_serial) {
      // 解码操作，不断尝试从解码器中获取帧数据
      do {
        if (decoder->p_queue->abort_request) {
          return -1;
        }
        switch (decoder->av_ctx->codec_type) {
          case AVMEDIA_TYPE_VIDEO:
            ret = avcodec_receive_frame(decoder->av_ctx, frame);
            if (ret >= 0) {
              frame->pts = frame->best_effort_timestamp;
            }
            break;
          case AVMEDIA_TYPE_AUDIO:
            ret = avcodec_receive_frame(decoder->av_ctx, frame);
            if (ret >= 0) {
              AVRational tb = (AVRational){1, frame->sample_rate};
              if (frame->pts != AV_NOPTS_VALUE) {
                frame->pts =
                    av_rescale_q(frame->pts, decoder->av_ctx->pkt_timebase, tb);
              } else if (decoder->next_pts != AV_NOPTS_VALUE) {
                frame->pts =
                    av_rescale_q(decoder->next_pts, decoder->next_pts_tb, tb);
              }
              if (frame->pts != AV_NOPTS_VALUE) {
                decoder->next_pts = frame->pts + frame->nb_samples;
                decoder->next_pts_tb = tb;
              }
            }
            break;
        }
        // 解码完所有数据,需要刷新缓冲区并返回
        if (ret == AVERROR_EOF) {
          decoder->finished = decoder->pkt_serial;
          avcodec_flush_buffers(decoder->av_ctx);
          return 0;
        }
        // 解码成功
        if (ret >= 0) {
          return 1;
        }
      } while (ret != AVERROR(EAGAIN));
    }

    // 获取数据包
    do {
      if (decoder->p_queue->nb_packets == 0) {
        SDL_CondSignal(decoder->empty_queue_cond);
      }
      // 是否有待处理的数据包
      if (decoder->packet_pending) {
        decoder->packet_pending = 0;
      } else {
        int old_serial = decoder->pkt_serial;
        if (packet_queue_get(decoder->p_queue, decoder->pkt, 1,
                             &decoder->pkt_serial) < 0) {
          return -1;
        }
        if (old_serial != decoder->pkt_serial) {
          avcodec_flush_buffers(decoder->av_ctx);
          decoder->finished = 0;
          decoder->next_pts = decoder->start_pts;
          decoder->next_pts_tb = decoder->start_pts_tb;
        }
      }
      if (decoder->p_queue->serial == decoder->pkt_serial) {
        break;
      }
      av_packet_unref(decoder->pkt);
    } while (1);

    // 数据包处理，解码数据包
    if (decoder->av_ctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
      int got_frame = 0;
      ret = avcodec_decode_subtitle2(decoder->av_ctx, sub, &got_frame,
                                     decoder->pkt);
      if (ret < 0) {
        ret = AVERROR(EAGAIN);
      } else {
        // 如果got_frame大于 0 但 d->pkt->data 为空,则说明字幕帧还没有完全解码出来。此时会设置 d->packet_pending 标志位为 1,表示还有数据包需要继续处理。
        if (got_frame && !decoder->pkt->data) {
          decoder->packet_pending = 1;
        }
        ret = got_frame ? 0
                        : (decoder->pkt->data ? AVERROR(EAGAIN) : AVERROR_EOF);
      }
      av_packet_unref(decoder->pkt);
    } else {
      // 如果返回值为 AVERROR(EAGAIN)(表示解码器暂时无法接受更多数据),则会设置 d->packet_pending 标志位为 1,表示这个数据包需要等待下次继续处理。
      if (avcodec_send_packet(decoder->av_ctx, decoder->pkt) ==
          AVERROR(EAGAIN)) {
        // 解码器暂时无法接受更多数据,需要将数据包标记为待处理。
        av_log(decoder->av_ctx, AV_LOG_ERROR,
               "Receive_frame and send_packet both returned EAGAIN, which is "
               "an API violation.\n");
        decoder->packet_pending = 1;
      } else {
        av_packet_unref(decoder->pkt);
      }
    }
  }
}

static void decoder_destroy(Decoder* decoder) {
  av_packet_free(&decoder->pkt);
  avcodec_free_context(&decoder->av_ctx);
}

// 中止解码器：当收到信号或触发某些条件时,decoder_abort 函数会被调用,用于通知解码器立即中止当前的解码操作。这可能发生在用户手动停止播放、切换媒体源或者出现解码错误等情况下。
static void decoder_abort(Decoder* decoder, FrameQueue* f_queue) {
  packet_queue_abort(decoder->p_queue);
  frame_queue_signal(f_queue);
  SDL_WaitThread(decoder->decoder_thread, NULL);
  decoder->decoder_thread = NULL;
  packet_queue_flush(decoder->p_queue);
}

static int decoder_start(Decoder* decoder, int (*fn)(void*),
                         const char* thread_name, void* arg) {
  packet_queue_start(decoder->p_queue);
  decoder->decoder_thread = SDL_CreateThread(fn, thread_name, arg);
  if (!decoder->decoder_thread) {
    av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  return 0;
}
/* ============= about Decoder ============= */

/* ============= about VideoDisplay ============= */
static int realloc_texture(SDL_Texture** texture, Uint32 new_format,
                           int new_width, int new_height,
                           SDL_BlendMode blendmode, int init_texture) {
  Uint32 format;
  int access, w, h;
  // SDL_QueryTexture用于获取SDL纹理的基本属性信息，比如它的像素格式、访问模式、宽度和高度等。
  if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 ||
      new_width != w || new_height != h || new_format != format) {
    void* pixels;
    int pitch;
    if (*texture) SDL_DestroyTexture(*texture);
    if (!(*texture = SDL_CreateTexture(renderer, new_format,
                                       SDL_TEXTUREACCESS_STREAMING, new_width,
                                       new_height))) {
      return -1;
    }
    if (SDL_SetTextureBlendMode(*texture, blendmode) < 0) return -1;
    if (init_texture) {
      if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0) return -1;
      memset(pixels, 0, pitch * new_height);
      SDL_UnlockTexture(*texture);
    }
    av_log(NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width,
           new_height, SDL_GetPixelFormatName(new_format));
  }
  return 0;
}

// 根据给定的屏幕尺寸和视频尺寸,计算出一个合适的显示区域大小和位置
// 根据屏幕宽高和视频长宽，计算出视频在屏幕上的最大宽度或高度，并确保这个宽度值是一个偶数。
static void calculate_display_rect(SDL_Rect* rect, int screen_xleft,
                                   int screen_ytop, int screen_width,
                                   int screen_height, int pic_width,
                                   int pic_height, AVRational pic_sar) {
  AVRational aspect_ratio = pic_sar;
  int64_t width, height, x, y;

  // av_make_q根据给定的分子 num 和分母 den 创建一个 AVRational 类型的分数
  // av_cmp_q比较两个分数 a 和 b的大小
  if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
    aspect_ratio = av_make_q(1, 1);
  // av_mul_q将两个 AVRational 类型的分数相乘,并返回乘积
  aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

  /* XXX: we suppose the screen has a 1.0 pixel ratio */
  height = screen_height;
  width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
  if (width > screen_width) {
    width = screen_width;
    height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
  }
  /*
  & ~1:
  这个操作是为了将计算出的 width 值向下取整为一个偶数。
  原因是某些视频硬件或软件可能要求视频宽度必须是偶数，否则可能会出现显示问题。
  & ~1 的原理是将 width 值的最低位位置为 0，即取偶数。
  */
  x = (screen_width - width) / 2;
  y = (screen_height - height) / 2;
  rect->x = screen_xleft + x;
  rect->y = screen_ytop + y;
  rect->w = FFMAX((int)width, 1);
  rect->h = FFMAX((int)height, 1);
}

// 获取 SDL 的像素格式和混合模式
static void get_sdl_pix_fmt_and_blendmode(int format, Uint32* sdl_pix_fmt,
                                          SDL_BlendMode* sdl_blendmode) {
  *sdl_blendmode = SDL_BLENDMODE_NONE;
  *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
  if (format == AV_PIX_FMT_RGB32 || format == AV_PIX_FMT_RGB32_1 ||
      format == AV_PIX_FMT_BGR32 || format == AV_PIX_FMT_BGR32_1)
    *sdl_blendmode = SDL_BLENDMODE_BLEND;
  for (int i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
    if (format == sdl_texture_format_map[i].format) {
      *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
      return;
    }
  }
}

/*
 根据 AVFrame 的像素格式获取对应的 SDL 像素格式和混合模式
 如果需要,会根据 AVFrame 的宽高重新分配 SDL_Texture 的大小。
 上传 YUV 数据
 上传其他格式数据
*/
static int upload_texture(SDL_Texture** texture, AVFrame* frame) {
  int ret = 0;
  Uint32 sdl_pix_fmt;
  SDL_BlendMode sdl_blendmode;

  get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
  if (realloc_texture(texture,
                      sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN
                          ? SDL_PIXELFORMAT_ARGB8888
                          : sdl_pix_fmt,
                      frame->width, frame->height, sdl_blendmode, 0) < 0) {
    return -1;
  }

  /*
  根据 SDL 的像素格式,采取不同的上传方式:
  如果是未知的像素格式,需要先用 sws_scale 函数进行颜色空间转换,然后再上传到纹理。
  如果是 SDL_PIXELFORMAT_IYUV(即 YUV 420P 格式),可以直接使用 SDL_UpdateYUVTexture 函数上传。
  对于其他像素格式,可以直接使用 SDL_UpdateTexture 函数上传。
  对于有负线宽的数据,需要特殊处理以确保正确上传。
  */
  switch (sdl_pix_fmt) {
    case SDL_PIXELFORMAT_UNKNOWN:
      av_log(NULL, AV_LOG_ERROR,
             "Cannot initialize the conversion context when the pix_fmt is "
             "SDL_PIXELFORMAT_UNKNOWN.\n");
      break;
    case SDL_PIXELFORMAT_IYUV:
      if (frame->linesize[0] > 0 && frame->linesize[1] > 0 &&
          frame->linesize[2] > 0) {
        ret = SDL_UpdateYUVTexture(
            *texture, NULL, frame->data[0], frame->linesize[0], frame->data[1],
            frame->linesize[1], frame->data[2], frame->linesize[2]);
      } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 &&
                 frame->linesize[2] < 0) {
        ret = SDL_UpdateYUVTexture(
            *texture, NULL,
            frame->data[0] + frame->linesize[0] * (frame->height - 1),
            -frame->linesize[0],
            frame->data[1] +
                frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1),
            -frame->linesize[1],
            frame->data[2] +
                frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1),
            -frame->linesize[2]);
        // AV_CEIL_RSHIFT对一个数值进行向上取整的右移操作。最后的结果不是0就是1
      } else {
        av_log(NULL, AV_LOG_ERROR,
               "Mixed negative and positive linesizes are not supported.\n");
        return -1;
      }
      break;
    default:
      if (frame->linesize[0] < 0) {
        ret = SDL_UpdateTexture(
            *texture, NULL,
            frame->data[0] + frame->linesize[0] * (frame->height - 1),
            -frame->linesize[0]);
      } else {
        ret = SDL_UpdateTexture(*texture, NULL, frame->data[0],
                                frame->linesize[0]);
      }
      break;
  }
  return ret;
}

// 将一个 AVFrame 中的视频数据上传到一个 SDL_Texture 对象中
static void video_image_display(MediaState* ms) {
  Frame* video_pic;
  Frame* subtitle_pic = NULL;
  SDL_Rect rect;

  video_pic = frame_queue_peek_last(&ms->pic_queue);
  if (ms->subtitle_stream) {
    if (frame_queue_nb_remaining(&ms->sub_queue) > 0) {
      subtitle_pic = frame_queue_peek(&ms->sub_queue);

      if (video_pic->pts >
          subtitle_pic->pts +
              ((float)subtitle_pic->sub.start_display_time / 1000)) {
        if (!subtitle_pic->uploaded) {
          uint8_t* pixels[4];
          int pitch[4];
          if (!subtitle_pic->width || !subtitle_pic->height) {
            subtitle_pic->width = video_pic->width;
            subtitle_pic->height = video_pic->height;
          }
          if (realloc_texture(&ms->sub_texture, SDL_PIXELFORMAT_ARGB8888,
                              subtitle_pic->width, subtitle_pic->height,
                              SDL_BLENDMODE_BLEND, 1) < 0) {
            return;
          }

          for (int i = 0; i < subtitle_pic->sub.num_rects; i++) {
            AVSubtitleRect* sub_rect = subtitle_pic->sub.rects[i];

            sub_rect->x = av_clip(sub_rect->x, 0, subtitle_pic->width);
            sub_rect->y = av_clip(sub_rect->y, 0, subtitle_pic->height);
            sub_rect->w =
                av_clip(sub_rect->w, 0, subtitle_pic->width - sub_rect->x);
            sub_rect->h =
                av_clip(sub_rect->h, 0, subtitle_pic->height - sub_rect->y);

            ms->sub_convert_ctx = sws_getCachedContext(
                ms->sub_convert_ctx, sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA, 0, NULL, NULL, NULL);
            if (!ms->sub_convert_ctx) {
              av_log(NULL, AV_LOG_FATAL,
                     "Cannot initialize the conversion context\n");
              return;
            }
            if (!SDL_LockTexture(ms->sub_texture, (SDL_Rect*)sub_rect,
                                 (void**)pixels, pitch)) {
              sws_scale(ms->sub_convert_ctx,
                        (const uint8_t* const*)sub_rect->data,
                        sub_rect->linesize, 0, sub_rect->h, pixels, pitch);
              SDL_UnlockTexture(ms->sub_texture);
            }
          }
          subtitle_pic->uploaded = 1;
        }
      } else {
        subtitle_pic = NULL;
      }
    }
  }

  calculate_display_rect(&rect, ms->xleft, ms->ytop, ms->width, ms->height,
                         video_pic->width, video_pic->height, video_pic->sar);

  if (!video_pic->uploaded) {
    if (upload_texture(&ms->video_texture, video_pic->frame) < 0) {
      return;
    }
    video_pic->uploaded = 1;
    // video_pic->flip_v = video_pic->frame->linesize[0] < 0;
  }

  SDL_RenderCopyEx(renderer, ms->video_texture, NULL, &rect, 0, NULL, 0);
  if (subtitle_pic) {
    // 字幕一次性渲染出来
    SDL_RenderCopy(renderer, ms->sub_texture, NULL, &rect);
  }
}

static void set_default_window_size(int width, int height,
                                    AVRational aspect_ratio) {
  SDL_Rect rect;
  int max_width = screen_width ? screen_width : INT_MAX;
  int max_height = screen_height ? screen_height : INT_MAX;
  if (max_width == INT_MAX && max_height == INT_MAX) {
    max_height = height;
  }
  calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height,
                         aspect_ratio);
  default_width = rect.w;
  default_height = rect.h;
}

static int video_open(MediaState* ms) {
  int w, h;

  w = screen_width ? screen_width : default_width;
  h = screen_height ? screen_height : default_height;

  if (!window_title) {
    window_title = input_filename;
  }
  SDL_SetWindowTitle(window, window_title);

  SDL_SetWindowSize(window, w, h);
  SDL_SetWindowPosition(window, screen_left, screen_top);
  if (is_full_screen)
    SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
  SDL_ShowWindow(window);

  ms->width = w;
  ms->height = h;

  return 0;
}

/* display the current picture, if any */
static void video_display(MediaState* ms) {
  if (!ms->width) {
    video_open(ms);
  }

  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderClear(renderer);
  if (ms->audio_stream && ms->show_mode != SHOW_MODE_VIDEO) {
    // video_audio_display(is);  // 根据显示模式显示波形图或频谱分析图，先不处理
  } else if (ms->video_stream) {
    video_image_display(ms);
  }
  SDL_RenderPresent(renderer);
}
/* ============= about VideoDisplay ============= */

static void stream_component_close(MediaState* ms, int stream_index) {
  AVFormatContext* ic = ms->fmt_ctx;
  AVCodecParameters* codecpar;
  if (stream_index < 0 || stream_index >= ic->nb_streams) {
    return;
  }
  codecpar = ic->streams[stream_index]->codecpar;

  switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
      decoder_abort(&ms->audio_dec, &ms->sample_queue);
      SDL_CloseAudioDevice(audio_deviceId);
      decoder_destroy(&ms->audio_dec);
      swr_free(&ms->swr_ctx);
      av_freep(&ms->audio_buf1);
      ms->audio_buf1_size = 0;
      ms->audio_buf = NULL;
      break;
    case AVMEDIA_TYPE_VIDEO:
      decoder_abort(&ms->video_dec, &ms->pic_queue);
      decoder_destroy(&ms->video_dec);
      break;
    case AVMEDIA_TYPE_SUBTITLE:
      decoder_abort(&ms->sub_dec, &ms->sub_queue);
      decoder_destroy(&ms->sub_dec);
      break;
    default:
      break;
  }

  // 控制流的丢弃策略
  // AVDISCARD_ALL 代表完全丢弃该流,不会对其进行任何解码和处理。
  ic->streams[stream_index]->discard = AVDISCARD_ALL;
  switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
      ms->audio_stream = NULL;
      ms->audio_index = -1;
      break;
    case AVMEDIA_TYPE_VIDEO:
      ms->video_stream = NULL;
      ms->video_index = -1;
      break;
    case AVMEDIA_TYPE_SUBTITLE:
      ms->subtitle_stream = NULL;
      ms->subtitle_index = -1;
      break;
    default:
      break;
  }
}

static void stream_close(MediaState* ms) {
  /* XXX: use a special url_shutdown call to abort parse cleanly */
  ms->abort_request = 1;
  SDL_WaitThread(ms->read_tid, NULL);

  /* close each stream */
  if (ms->audio_index >= 0) stream_component_close(ms, ms->audio_index);
  if (ms->video_index >= 0) stream_component_close(ms, ms->video_index);
  if (ms->subtitle_index >= 0) stream_component_close(ms, ms->subtitle_index);

  avformat_close_input(&ms->fmt_ctx);
  packet_queue_destroy(&ms->video_queue);
  packet_queue_destroy(&ms->audio_queue);
  packet_queue_destroy(&ms->subtitle_queue);

  /* free all pictures */
  frame_queue_destroy(&ms->pic_queue);
  frame_queue_destroy(&ms->sample_queue);
  frame_queue_destroy(&ms->sub_queue);
  SDL_DestroyCond(ms->continue_read_thread);
  sws_freeContext(ms->sub_convert_ctx);
  av_free(ms->filename);
  if (ms->video_texture) SDL_DestroyTexture(ms->video_texture);
  if (ms->sub_texture) SDL_DestroyTexture(ms->sub_texture);
  av_free(ms);
}

static void do_exit(MediaState* ms) {
  if (ms) {
    stream_close(ms);
  }
  if (renderer) SDL_DestroyRenderer(renderer);
  if (window) SDL_DestroyWindow(window);
  av_freep(&input_filename);
  avformat_network_deinit();
  if (show_status) {
    printf("\n");
  }
  SDL_Quit();
  av_log(NULL, AV_LOG_QUIET, "%s", "");
  exit(0);
}

static void sigterm_handler(int sig) {
  /*
   终止当前进程的函数调用。
   当调用 exit() 函数时,进程会以传入的参数值(这里是 1001)作为退出状态码退出。
   退出状态码是一个整数值,通常用于表示进程的退出状态。零表示正常退出,非零表示异常退出。
   在 Unix/Linux 系统中,退出状态码可以被父进程或操作系统读取,用于判断子进程的执行结果。
  */
  exit(1001);
}

/* ============= about Clock ============= */
// 用于获取当前时钟的时间值
static double get_clock(Clock* clock) {
  if (*clock->queue_serial != clock->serial) {
    return NAN;
  }
  if (clock->paused) {
    return clock->pts;
  } else {
    double time = av_gettime_relative() / 1000000.0;
    return clock->pts_drift + time -
           (time - clock->last_updated) * (1.0 - clock->speed);
  }
}

// 设置时钟 c 的当前时间为 pts，并将时钟的 serial 编号更新为指定值。
// 在收到新的时间戳信息时更新时钟,确保时钟时间与视频/音频数据同步。
static void set_clock_at(Clock* clock, double pts, int serial, double time) {
  clock->pts = pts;
  clock->last_updated = time;
  clock->pts_drift = clock->pts - time;
  clock->serial = serial;
}

// 用于设置当前时钟的时间值。
static void set_clock(Clock* clock, double pts, int serial) {
  double time = av_gettime_relative() / 1000000.0;
  set_clock_at(clock, pts, serial, time);
}

// 设置时钟 c 的播放速率为 speed。
// 主要用于同步音视频时钟,而不是用于实现倍速播放。
static void set_clock_speed(Clock* clock, double speed) {
  set_clock(clock, get_clock(clock), clock->serial);
  clock->speed = speed;
}

static void init_clock(Clock* clock, int* queue_serial) {
  clock->speed = 1.0;
  clock->paused = 0;
  clock->queue_serial = queue_serial;
  set_clock(clock, NAN, -1);
  // NAN: 表示无法用正常数值表示的情况，不代表任何数值
}

// 将时钟 c 与 slave 时钟同步。
// 用于在多路流播放时,将视频时钟与音频时钟进行同步。确保视频和音频保持良好的同步。
static void sync_clock_to_slave(Clock* c, Clock* slave) {
  double clock = get_clock(c);
  double slave_clock = get_clock(slave);
  if (!isnan(slave_clock) &&
      (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD)) {
    set_clock(c, slave_clock, slave->serial);
  }
}

static int get_master_sync_type(MediaState* ms) {
  if (ms->av_sync_type == AV_SYNC_VIDEO_MASTER) {
    if (ms->video_stream)
      return AV_SYNC_VIDEO_MASTER;
    else
      return AV_SYNC_AUDIO_MASTER;
  } else if (ms->av_sync_type == AV_SYNC_AUDIO_MASTER) {
    if (ms->audio_stream)
      return AV_SYNC_AUDIO_MASTER;
    else
      return AV_SYNC_EXTERNAL_CLOCK;
  } else {
    return AV_SYNC_EXTERNAL_CLOCK;
  }
}

/* get the current master clock value */
static double get_master_clock(MediaState* ms) {
  double val;

  switch (get_master_sync_type(ms)) {
    case AV_SYNC_VIDEO_MASTER:
      val = get_clock(&ms->video_clk);
      break;
    case AV_SYNC_AUDIO_MASTER:
      val = get_clock(&ms->audio_clk);
      break;
    default:
      val = get_clock(&ms->external_clk);
      break;
  }
  return val;
}

// 动态调整外部时钟的速度
// 根据视频和音频队列中的数据包数量来动态调整外部时钟的速度,以保持播放的同步和流畅。
static void check_external_clock_speed(MediaState* ms) {
  if (ms->video_index >= 0 &&
          ms->video_queue.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
      ms->audio_index >= 0 &&
          ms->audio_queue.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) {
    // 时钟速度需要降低以防止队列中的数据包耗尽
    set_clock_speed(&ms->external_clk,
                    FFMAX(EXTERNAL_CLOCK_SPEED_MIN,
                          ms->external_clk.speed - EXTERNAL_CLOCK_SPEED_STEP));
  } else if ((ms->video_index < 0 ||
              ms->video_queue.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
             (ms->audio_index < 0 ||
              ms->audio_queue.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
    // 时钟速度需要增加以防止队列中的数据包过多
    set_clock_speed(&ms->external_clk,
                    FFMIN(EXTERNAL_CLOCK_SPEED_MAX,
                          ms->external_clk.speed + EXTERNAL_CLOCK_SPEED_STEP));
  } else {
    // 如果上述两种情况都不满足,则根据 extclk 的当前速度做一个平滑的调整,使其逐渐接近 1.0。
    // 这种情况下,说明队列中的数据包数量处于合理范围内,不需要大幅调整时钟速度
    double speed = ms->external_clk.speed;
    if (speed != 1.0) {
      set_clock_speed(&ms->external_clk, speed + EXTERNAL_CLOCK_SPEED_STEP *
                                                     (1.0 - speed) /
                                                     fabs(1.0 - speed));
    }
  }
}
/* ============= about Clock ============= */

/* ============= about Media Operation ============= */
/* seek in the stream */
static void stream_seek(MediaState* ms, int64_t pos, int64_t rel,
                        int by_bytes) {
  if (!ms->seek_req) {
    ms->seek_pos = pos;
    ms->seek_rel = rel;
    ms->seek_flags &= ~AVSEEK_FLAG_BYTE;
    if (by_bytes) {
      ms->seek_flags |= AVSEEK_FLAG_BYTE;
    }
    ms->seek_req = 1;
    SDL_CondSignal(ms->continue_read_thread);
  }
}

/* pause or resume the video */
static void stream_toggle_pause(MediaState* ms) {
  if (ms->paused) {
    ms->frame_timer +=
        av_gettime_relative() / 1000000.0 - ms->video_clk.last_updated;
    if (ms->read_pause_return != AVERROR(ENOSYS)) {
      ms->video_clk.paused = 0;
    }
    set_clock(&ms->video_clk, get_clock(&ms->video_clk), ms->video_clk.serial);
  }
  set_clock(&ms->external_clk, get_clock(&ms->external_clk),
            ms->external_clk.serial);
  // 将 paused 标志应用到所有相关时钟
  ms->paused = !ms->paused;
  ms->audio_clk.paused = !ms->paused;
  ms->video_clk.paused = !ms->paused;
  ms->external_clk.paused = !ms->paused;
}

static void toggle_pause(MediaState* ms) {
  stream_toggle_pause(ms);
  ms->step = 0;
}

static void toggle_mute(MediaState* ms) { ms->muted = !ms->muted; }

// 这里涉及到一些数学运算
// sign 参数: 这个参数用于控制音量的增加或减少方向。
// step 参数: 这个参数用于控制每次音量调整的步长大小。
static void update_volume(MediaState* ms, int sign, double step) {
  /*
   lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
   这行代码的作用是根据当前的音量级别和调整步长,计算出新的音量值。
   具体步骤如下:
   volume_level + sign * step
   这一步计算出新的音量级别。
   如果 sign 为 1,表示要增加音量,则加上 step。
   如果 sign 为 -1,表示要降低音量,则减去 step。
   
   pow(10.0, (volume_level + sign * step) / 20.0)
   这一步将新的音量级别转换为0到1之间的比例值。
   
   公式 10^((volume_level + sign * step) / 20) 是将音量级别(以分贝为单位)转换为0到1之间的比例值。
   SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0)
   这一步将比例值转换为SDL音量范围(0到SDL_MIX_MAXVOLUME)内的实际值。
   也就是说,将新计算出的比例值乘以SDL_MIX_MAXVOLUME,得到新的音量值。
   
   lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0))
   最后, 使用lrint()函数将计算出的浮点数音量值四舍五入为整数, 因为音量值需要是整数。
  */

  // 计算出当前的音量级别
  double volume_level =
      ms->audio_volume
          ? (20 * log(ms->audio_volume / (double)SDL_MIX_MAXVOLUME) / log(10))
          : -1000.0;
  // 根据 sign 和 step 参数, 计算出新的音量级别
  int new_volume =
      lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
  // 将 is->audio_volume 限制在 0 到 SDL_MIX_MAXVOLUME 的范围内
  ms->audio_volume = av_clip(
      ms->audio_volume == new_volume ? (ms->audio_volume + sign) : new_volume,
      0, SDL_MIX_MAXVOLUME);
}

static void step_to_next_frame(MediaState* ms) {
  /* if the stream is paused unpause it, then step */
  if (ms->paused) {
    stream_toggle_pause(ms);
  }
  ms->step = 1;
}

static void toggle_full_screen(MediaState* ms) {
  is_full_screen = !is_full_screen;
  SDL_SetWindowFullscreen(window,
                          is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}
/* ============= about Media Operation ============= */

// 根据当前的同步状态,计算出视频帧需要的延迟时间
static double compute_target_delay(double delay, MediaState* ms) {
  double sync_threshold, diff = 0;

  /* update delay to follow master synchronisation source */
  if (get_master_sync_type(ms) != AV_SYNC_VIDEO_MASTER) {
    /* if video is slave, we try to correct big delays by
            duplicating or deleting a frame */
    /* 根据diff决定是需要跳过还是重复播放某些帧 */
    diff = get_clock(&ms->video_clk) - get_master_clock(ms);

    /* skip or repeat frame. We take into account the
            delay to compute the threshold. I still don't know
            if it is the best guess */
    /* 
          如果 diff 小于 -sync_threshold, 增加 delay 时间。
          如果 diff 大于 sync_threshold 且 delay 大于 AV_SYNC_FRAMEDUP_THRESHOLD, 增加 delay 时间。
          如果 diff 大于 sync_threshold, 将 delay 时间加倍。
    */
    sync_threshold =
        FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
    if (!isnan(diff) && fabs(diff) < ms->max_frame_duration) {
      if (diff <= -sync_threshold)
        delay = FFMAX(0, delay + diff);
      else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
        delay = delay + diff;
      else if (diff >= sync_threshold)
        delay = 2 * delay;
    }
  }

  av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);
  return delay;
}

// 获取两个连续视频帧之间的时间间隔
static double vp_duration(MediaState* ms, Frame* vp, Frame* nextvp) {
  //检查两个视频帧是否属于同一个序列
  if (vp->serial == nextvp->serial) {
    double duration = nextvp->pts - vp->pts;
    if (isnan(duration) || duration <= 0 || duration > ms->max_frame_duration)
      return vp->duration;
    else
      return duration;
  } else {
    return 0.0;
  }
}

static void update_video_pts(MediaState* ms, double pts, int serial) {
  /* update current video pts */
  set_clock(&ms->video_clk, pts, serial);
  sync_clock_to_slave(&ms->external_clk, &ms->video_clk);
}

// https://blog.csdn.net/weixin_41910694/article/details/115795452 --有相关说明
/* called to display each frame */
static void video_refresh(void* opaque, double* remaining_time) {
  MediaState* ms = opaque;
  double time;
  Frame* subtitle_pic;
  Frame* subtitle_pic2;

  if (!ms->paused && get_master_sync_type(ms) == AV_SYNC_EXTERNAL_CLOCK &&
      ms->realtime) {
    check_external_clock_speed(ms);
  }

  /* 
     写队列中，应⽤程序写⼊⼀个新帧后通常总是将写索引加1。
     ⽽读队列中，“读取”和“更新读索引(同时删除旧帧)”⼆者是独⽴的，可以只读取⽽不更新读索引，
     也可以只更新读索引(只删除)⽽不读取（只有更新读索引的时候才真正释放对应的Frame数据）。⽽且读队列引⼊了是否保留已显示的最后⼀帧的机制，导致读队列⽐写队列要复杂很多。
     读队列和写队列步骤是类似的，基本步骤如下：
     调⽤frame_queue_peek_readable获取可读Frame；
     如果需要更新读索引rindex（出队列该节点）则调⽤frame_queue_peek_next；
  */
  if (ms->video_stream) {
    while (1) {
      if (frame_queue_nb_remaining(&ms->pic_queue) == 0) {
        // nothing to do, no picture to display in the queue
        break;
      } else {
        double last_duration, duration, delay;
        Frame* video_pic;
        Frame* last_video_pic;

        /* dequeue the picture */
        last_video_pic = frame_queue_peek_last(&ms->pic_queue);
        video_pic = frame_queue_peek(&ms->pic_queue);

        if (video_pic->serial != ms->video_queue.serial) {
          frame_queue_next(&ms->pic_queue);
          continue;
        }

        if (last_video_pic->serial != video_pic->serial)
          ms->frame_timer = av_gettime_relative() / 1000000.0;

        if (ms->paused) {
          /* display picture */
          if (!display_disable && ms->force_refresh &&
              ms->show_mode == SHOW_MODE_VIDEO && ms->pic_queue.rindex_shown) {
            video_display(ms);
          }
          break;
        }

        last_duration = vp_duration(ms, last_video_pic, video_pic);
        delay = compute_target_delay(last_duration, ms);

        time = av_gettime_relative() / 1000000.0;
        if (time < ms->frame_timer + delay) {
          *remaining_time =
              FFMIN(ms->frame_timer + delay - time, *remaining_time);
          /* display picture */
          if (!display_disable && ms->force_refresh &&
              ms->show_mode == SHOW_MODE_VIDEO && ms->pic_queue.rindex_shown)
            video_display(ms);
          break;
        }

        ms->frame_timer += delay;
        if (delay > 0 && time - ms->frame_timer > AV_SYNC_THRESHOLD_MAX)
          ms->frame_timer = time;

        SDL_LockMutex(ms->pic_queue.mutex);
        if (!isnan(video_pic->pts)) {
          update_video_pts(ms, video_pic->pts, video_pic->serial);
        }
        SDL_UnlockMutex(ms->pic_queue.mutex);

        if (frame_queue_nb_remaining(&ms->pic_queue) > 1) {
          Frame* next_video_pic = frame_queue_peek_next(&ms->pic_queue);
          duration = vp_duration(ms, video_pic, next_video_pic);
          if (!ms->step &&
              (framedrop > 0 || (framedrop && get_master_sync_type(ms) !=
                                                  AV_SYNC_VIDEO_MASTER)) &&
              time > ms->frame_timer + duration) {
            ms->frame_drops_late++;
            frame_queue_next(&ms->pic_queue);
            continue;
          }
        }

        if (ms->subtitle_stream) {
          while (frame_queue_nb_remaining(&ms->sub_queue) > 0) {
            subtitle_pic = frame_queue_peek(&ms->sub_queue);

            if (frame_queue_nb_remaining(&ms->sub_queue) > 1)
              subtitle_pic2 = frame_queue_peek_next(&ms->sub_queue);
            else
              subtitle_pic2 = NULL;

            if (subtitle_pic->serial != ms->subtitle_queue.serial ||
                (ms->video_clk.pts >
                 (subtitle_pic->pts +
                  ((float)subtitle_pic->sub.end_display_time / 1000))) ||
                (subtitle_pic2 &&
                 ms->video_clk.pts >
                     (subtitle_pic2->pts +
                      ((float)subtitle_pic2->sub.start_display_time / 1000)))) {
              if (subtitle_pic->uploaded) {
                for (int i = 0; i < subtitle_pic->sub.num_rects; i++) {
                  AVSubtitleRect* sub_rect = subtitle_pic->sub.rects[i];
                  uint8_t* pixels;
                  int pitch;

                  if (!SDL_LockTexture(ms->sub_texture, (SDL_Rect*)sub_rect,
                                       (void**)&pixels, &pitch)) {
                    for (int j = 0; j < sub_rect->h; j++, pixels += pitch)
                      memset(pixels, 0, sub_rect->w << 2);
                    SDL_UnlockTexture(ms->sub_texture);
                  }
                }
              }
              frame_queue_next(&ms->sub_queue);
            } else {
              break;
            }
          }
        }

        frame_queue_next(&ms->pic_queue);
        ms->force_refresh = 1;

        if (ms->step && !ms->paused) {
          stream_toggle_pause(ms);
        }
      }

      /* display picture */
      if (!display_disable && ms->force_refresh &&
          ms->show_mode == SHOW_MODE_VIDEO && ms->pic_queue.rindex_shown) {
        video_display(ms);
      }
      break;
    }
    ms->force_refresh = 0;
    if (show_status) {
      AVBPrint buf;
      static int64_t last_time;
      int64_t cur_time;
      int aqsize, vqsize, sqsize;
      double av_diff;

      cur_time = av_gettime_relative();
      if (!last_time || (cur_time - last_time) >= 30000) {
        aqsize = 0;
        vqsize = 0;
        sqsize = 0;
        if (ms->audio_stream) aqsize = ms->audio_queue.size;
        if (ms->video_stream) vqsize = ms->video_queue.size;
        if (ms->subtitle_stream) sqsize = ms->subtitle_queue.size;
        av_diff = 0;
        if (ms->audio_stream && ms->video_stream)
          av_diff = get_clock(&ms->audio_clk) - get_clock(&ms->video_clk);
        else if (ms->video_stream)
          av_diff = get_master_clock(ms) - get_clock(&ms->video_clk);
        else if (ms->audio_stream)
          av_diff = get_master_clock(ms) - get_clock(&ms->audio_clk);

        av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
        av_bprintf(
            &buf, "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB \r",
            get_master_clock(ms),
            (ms->audio_stream && ms->video_stream)
                ? "A-V"
                : (ms->video_stream ? "M-V"
                                    : (ms->audio_stream ? "M-A" : "   ")),
            av_diff, ms->frame_drops_early + ms->frame_drops_late,
            aqsize / 1024, vqsize / 1024, sqsize);

        if (show_status == 1 && AV_LOG_INFO > av_log_get_level())
          fprintf(stderr, "%s", buf.str);
        else
          av_log(NULL, AV_LOG_INFO, "%s", buf.str);

        fflush(stderr);
        av_bprint_finalize(&buf, NULL);

        last_time = cur_time;
      }
    }
  }
}

// frame 入队列之前的初始化赋值处理
static int queue_picture(MediaState* ms, AVFrame* src_frame, double pts,
                         double duration, int64_t pos, int serial) {
  Frame* video_pic;
  // 用于获取视频帧的帧类型
  //  printf("frame_type=%c pts=%0.3f\n",
  //         av_get_picture_type_char(src_frame->pict_type), pts);

  if (!(video_pic = frame_queue_peek_writable(&ms->pic_queue))) {
    return -1;
  }

  video_pic->sar = src_frame->sample_aspect_ratio;
  video_pic->uploaded = 0;

  video_pic->width = src_frame->width;
  video_pic->height = src_frame->height;
  video_pic->format = src_frame->format;

  video_pic->pts = pts;
  video_pic->duration = duration;
  video_pic->pos = pos;
  video_pic->serial = serial;

  set_default_window_size(video_pic->width, video_pic->height, video_pic->sar);
  // 将src中所有数据拷贝到dst中，并复位src。
  av_frame_move_ref(video_pic->frame, src_frame);
  frame_queue_push(&ms->pic_queue);
  return 0;
}

// 获取视频帧
static int get_video_frame(MediaState* ms, AVFrame* frame) {
  int got_picture;

  if ((got_picture = decoder_decode_frame(&ms->video_dec, frame, NULL)) < 0)
    return -1;

  if (got_picture) {
    double dpts = NAN;

    if (frame->pts != AV_NOPTS_VALUE)
      dpts = av_q2d(ms->video_stream->time_base) * frame->pts;

    frame->sample_aspect_ratio =
        av_guess_sample_aspect_ratio(ms->fmt_ctx, ms->video_stream, frame);

    if (framedrop > 0 ||
        (framedrop && get_master_sync_type(ms) != AV_SYNC_VIDEO_MASTER)) {
      if (frame->pts != AV_NOPTS_VALUE) {
        double diff = dpts - get_master_clock(ms);
        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD && diff < 0 &&
            ms->video_dec.pkt_serial == ms->video_clk.serial &&
            ms->video_queue.nb_packets) {
          ms->frame_drops_early++;
          av_frame_unref(frame);
          got_picture = 0;
        }
      }
    }
  }

  return got_picture;
}

static int audio_thread(void* arg) {
  MediaState* ms = arg;
  AVFrame* frame = av_frame_alloc();
  Frame* audio_frame = NULL;
  int got_frame = 0;
  AVRational tb;
  int ret = 0;

  if (!frame) {
    return AVERROR(ENOMEM);
  }

  do {
    if ((got_frame = decoder_decode_frame(&ms->audio_dec, frame, NULL)) < 0) {
      av_frame_free(&frame);
      return ret;
    }

    if (got_frame) {
      tb = (AVRational){1, frame->sample_rate};

      if (!(audio_frame = frame_queue_peek_writable(&ms->sample_queue))) {
        av_frame_free(&frame);
        return ret;
      }

      audio_frame->pts =
          (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
      audio_frame->pos = frame->pkt_pos;
      audio_frame->serial = ms->audio_dec.pkt_serial;
      audio_frame->duration =
          av_q2d((AVRational){frame->nb_samples, frame->sample_rate});

      av_frame_move_ref(audio_frame->frame, frame);
      frame_queue_push(&ms->sample_queue);
    }
  } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
  av_frame_free(&frame);
  return ret;
}

static int video_thread(void* arg) {
  MediaState* ms = arg;
  AVFrame* frame = av_frame_alloc();
  double pts;
  double duration;
  int ret;
  AVRational tb = ms->video_stream->time_base;
  AVRational frame_rate =
      av_guess_frame_rate(ms->fmt_ctx, ms->video_stream, NULL);

  if (!frame) {
    return AVERROR(ENOMEM);
  }

  while (1) {
    ret = get_video_frame(ms, frame);
    if (ret < 0) {
      av_frame_free(&frame);
      return ret;
    }
    if (!ret) continue;

    duration = (frame_rate.num && frame_rate.den
                    ? av_q2d((AVRational){frame_rate.den, frame_rate.num})
                    : 0);
    pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
    ret = queue_picture(ms, frame, pts, duration, frame->pkt_pos,
                        ms->video_dec.pkt_serial);
    av_frame_unref(frame);

    if (ret < 0) {
      av_frame_free(&frame);
      return 0;
    }
  }
  av_frame_free(&frame);
  return 0;
}

static int subtitle_thread(void* arg) {
  MediaState* ms = arg;
  Frame* sub_pic;
  int got_subtitle;
  double pts;

  while (1) {
    if (!(sub_pic = frame_queue_peek_writable(&ms->sub_queue))) {
      return 0;
    }

    if ((got_subtitle =
             decoder_decode_frame(&ms->sub_dec, NULL, &sub_pic->sub)) < 0) {
      break;
    }

    pts = 0;

    if (got_subtitle && sub_pic->format == 0) {
      if (sub_pic->pts != AV_NOPTS_VALUE)
        pts = sub_pic->sub.pts / (double)AV_TIME_BASE;
      sub_pic->pts = pts;
      sub_pic->serial = ms->sub_dec.pkt_serial;
      sub_pic->width = ms->sub_dec.av_ctx->width;
      sub_pic->height = ms->sub_dec.av_ctx->height;
      sub_pic->uploaded = 0;

      /* now we can update the picture count */
      frame_queue_push(&ms->sub_queue);
    } else if (got_subtitle) {
      avsubtitle_free(&sub_pic->sub);
    }
  }
  return 0;
}

/* return the wanted number of samples to get better sync if sync_type is video
  * or external master clock */
static int synchronize_audio(MediaState* ms, int nb_samples) {
  int wanted_nb_samples = nb_samples;

  /* if not master, then we try to remove or add samples to correct the clock */
  if (get_master_sync_type(ms) != AV_SYNC_AUDIO_MASTER) {
    double diff, avg_diff;
    int min_nb_samples, max_nb_samples;

    diff = get_clock(&ms->audio_clk) - get_master_clock(ms);
    if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
      ms->audio_diff_cum = diff + ms->audio_diff_avg_coef * ms->audio_diff_cum;
      if (ms->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
        /* not enough measures to have a correct estimate */
        ms->audio_diff_avg_count++;
      } else {
        /* estimate the A-V difference */
        avg_diff = ms->audio_diff_cum * (1.0 - ms->audio_diff_avg_coef);

        if (fabs(avg_diff) >= ms->audio_diff_threshold) {
          wanted_nb_samples = nb_samples + (int)(diff * ms->audio_src.freq);
          min_nb_samples =
              ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
          max_nb_samples =
              ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
          wanted_nb_samples =
              av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
        }
        av_log(NULL, AV_LOG_TRACE,
               "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n", diff,
               avg_diff, wanted_nb_samples - nb_samples, ms->audio_clock,
               ms->audio_diff_threshold);
      }
    } else {
      /* too big difference : may be initial PTS errors, so
                reset A-V filter */
      ms->audio_diff_avg_count = 0;
      ms->audio_diff_cum = 0;
    }
  }

  return wanted_nb_samples;
}

/**
  * Decode one audio frame and return its uncompressed size.
  *
  * The processed audio frame is decoded, converted if required, and
  * stored in ms->audio_buf, with size in bytes given by the return
  * value.
  */
static int audio_decode_frame(MediaState* ms) {
  int data_size, resampled_data_size;
  int wanted_nb_samples;
  Frame* audio_frame;

  if (ms->paused) {
    return -1;
  }

  do {
    // ffplay针对 32位的系统做的优化，避免由于硬件性能问题导致获取数据延迟更长时间，
    // 硬件差的环境在临界时间点不要轻易返回静音数据，延迟音频帧的播放。
#if defined(_WIN32)
    while (frame_queue_nb_remaining(&ms->sample_queue) == 0) {
      if ((av_gettime_relative() - audio_callback_time) >
          1000000LL * ms->audio_hw_buf_size / ms->audio_target.bytes_per_sec /
              2) {
        return -1;
      }

      av_usleep(1000);
    }
#endif
    if (!(audio_frame = frame_queue_peek_readable(&ms->sample_queue))) {
      return -1;
    }
    frame_queue_next(&ms->sample_queue);
  } while (audio_frame->serial != ms->audio_queue.serial);

  // av_samples_get_buffer_size根据给定的音频参数(采样率、声道数、样本格式等)计算出所需的缓冲区大小(以字节为单位)
  data_size = av_samples_get_buffer_size(
      NULL, audio_frame->frame->ch_layout.nb_channels,
      audio_frame->frame->nb_samples, audio_frame->frame->format, 1);

  wanted_nb_samples = synchronize_audio(ms, audio_frame->frame->nb_samples);

  // av_channel_layout_compare 比较两个音频通道布局
  // 0 if chl and chl1 are equal, 1 if they are not equal. A negative AVERROR code if one or both are invalid.
  if (audio_frame->frame->format != ms->audio_src.fmt ||
      av_channel_layout_compare(&audio_frame->frame->ch_layout,
                                &ms->audio_src.ch_layout) ||
      audio_frame->frame->sample_rate != ms->audio_src.freq ||
      (wanted_nb_samples != audio_frame->frame->nb_samples && !ms->swr_ctx)) {
    swr_free(&ms->swr_ctx);
    // 设置重采样上下文的选项
    swr_alloc_set_opts2(
        &ms->swr_ctx, &ms->audio_target.ch_layout, ms->audio_target.fmt,
        ms->audio_target.freq, &audio_frame->frame->ch_layout,
        audio_frame->frame->format, audio_frame->frame->sample_rate, 0, NULL);
    if (!ms->swr_ctx || swr_init(ms->swr_ctx) < 0) {
      av_log(NULL, AV_LOG_ERROR,
             "Cannot create sample rate converter for conversion of %d Hz %s "
             "%d channels to %d Hz %s %d channels!\n",
             audio_frame->frame->sample_rate,
             av_get_sample_fmt_name(audio_frame->frame->format),
             audio_frame->frame->ch_layout.nb_channels, ms->audio_target.freq,
             av_get_sample_fmt_name(ms->audio_target.fmt),
             ms->audio_target.ch_layout.nb_channels);
      swr_free(&ms->swr_ctx);
      return -1;
    }
    if (av_channel_layout_copy(&ms->audio_src.ch_layout,
                               &audio_frame->frame->ch_layout) < 0)
      return -1;
    ms->audio_src.freq = audio_frame->frame->sample_rate;
    ms->audio_src.fmt = audio_frame->frame->format;
  }

  if (ms->swr_ctx) {
    const uint8_t** in = (const uint8_t**)audio_frame->frame->extended_data;
    uint8_t** out = &ms->audio_buf1;
    int out_count = (int64_t)wanted_nb_samples * ms->audio_target.freq /
                        audio_frame->frame->sample_rate +
                    256;
    int out_size =
        av_samples_get_buffer_size(NULL, ms->audio_target.ch_layout.nb_channels,
                                   out_count, ms->audio_target.fmt, 0);
    if (out_size < 0) {
      av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
      return -1;
    }
    if (wanted_nb_samples != audio_frame->frame->nb_samples) {
      // 在音频重采样过程中,由于采样率不同,可能会产生微小的时间偏差。
      // swr_set_compensation 函数可以设置补偿这种时间偏差,以确保重采样的精度。
      if (swr_set_compensation(
              ms->swr_ctx,
              (wanted_nb_samples - audio_frame->frame->nb_samples) *
                  ms->audio_target.freq / audio_frame->frame->sample_rate,
              wanted_nb_samples * ms->audio_target.freq /
                  audio_frame->frame->sample_rate) < 0) {
        av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
        return -1;
      }
    }

    // 高效分配内存
    av_fast_malloc(&ms->audio_buf1, &ms->audio_buf1_size, out_size);
    if (!ms->audio_buf1) {
      return AVERROR(ENOMEM);
    }
    int len2 = swr_convert(ms->swr_ctx, out, out_count, in,
                           audio_frame->frame->nb_samples);
    if (len2 < 0) {
      av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
      return -1;
    }
    if (len2 == out_count) {
      av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
      if (swr_init(ms->swr_ctx) < 0) {
        swr_free(&ms->swr_ctx);
      }
    }
    ms->audio_buf = ms->audio_buf1;
    resampled_data_size = len2 * ms->audio_target.ch_layout.nb_channels *
                          av_get_bytes_per_sample(ms->audio_target.fmt);
  } else {
    ms->audio_buf = audio_frame->frame->data[0];
    resampled_data_size = data_size;
  }

  double temp_audio_clock = ms->audio_clock;
  /* update the audio clock with the pts */
  if (!isnan(audio_frame->pts))
    ms->audio_clock =
        audio_frame->pts + (double)audio_frame->frame->nb_samples /
                               audio_frame->frame->sample_rate;
  else
    ms->audio_clock = NAN;
  ms->audio_clock_serial = audio_frame->serial;
#ifdef DEBUG
  {
    static double last_clock;
    printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
           ms->audio_clock - last_clock, ms->audio_clock, temp_audio_clock);
    last_clock = ms->audio_clock;
  }
#endif
  return resampled_data_size;
}

/* prepare a new audio buffer */
static void sdl_audio_callback(void* opaque, Uint8* stream, int len) {
  MediaState* ms = opaque;
  int audio_size, rel_len;

  audio_callback_time = av_gettime_relative();

  while (len > 0) {
    if (ms->audio_buf_index >= ms->audio_buf_size) {
      audio_size = audio_decode_frame(ms);
      if (audio_size < 0) {
        /* if error, just output silence */
        ms->audio_buf = NULL;
        ms->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE /
                             ms->audio_target.frame_size *
                             ms->audio_target.frame_size;
      } else {
        ms->audio_buf_size = audio_size;
      }
      ms->audio_buf_index = 0;
    }

    rel_len = ms->audio_buf_size - ms->audio_buf_index;
    if (rel_len > len) {
      rel_len = len;
    }
    if (!ms->muted && ms->audio_buf && ms->audio_volume == SDL_MIX_MAXVOLUME) {
      memcpy(stream, (uint8_t*)ms->audio_buf + ms->audio_buf_index, rel_len);
    } else {
      memset(stream, 0, rel_len);
      if (!ms->muted && ms->audio_buf) {
        SDL_MixAudioFormat(stream,
                           (uint8_t*)ms->audio_buf + ms->audio_buf_index,
                           AUDIO_S16SYS, rel_len, ms->audio_volume);
      }
    }
    len -= rel_len;
    stream += rel_len;
    ms->audio_buf_index += rel_len;
  }
  ms->audio_write_buf_size = ms->audio_buf_size - ms->audio_buf_index;
  /* Let's assume the audio driver that is used by SDL has two periods. */
  if (!isnan(ms->audio_clock)) {
    set_clock_at(&ms->audio_clk,
                 ms->audio_clock - (double)(2 * ms->audio_hw_buf_size +
                                            ms->audio_write_buf_size) /
                                       ms->audio_target.bytes_per_sec,
                 ms->audio_clock_serial, audio_callback_time / 1000000.0);
    sync_clock_to_slave(&ms->external_clk, &ms->audio_clk);
  }
}

// 配置和打开音频设备并设置音频参数，以便进行音频播放
static int audio_open(void* opaque, AVChannelLayout* wanted_channel_layout,
                      int wanted_sample_rate,
                      struct AudioParams* audio_hw_params) {
  SDL_AudioSpec wanted_spec, spec;
  static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
  static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
  // #define FF_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))
  // FF_ARRAY_ELEMS计算数组的大小(元素个数)。
  int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
  int wanted_nb_channels = wanted_channel_layout->nb_channels;

  // 确保采用本地系统的默认通道顺序。为了确保音频通道的排列顺序与系统默认的一致，从而能够正确地播放音频。
  // 使用本地系统的默认通道顺序可以避免一些由于通道顺序不匹配而导致的音频播放问题。
  if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
    av_channel_layout_uninit(wanted_channel_layout);
    av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
  }
  wanted_nb_channels = wanted_channel_layout->nb_channels;
  wanted_spec.channels = wanted_nb_channels;
  wanted_spec.freq = wanted_sample_rate;
  if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
    av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
    return -1;
  }
  while (next_sample_rate_idx &&
         next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq) {
    next_sample_rate_idx--;
  }

  wanted_spec.format = AUDIO_S16SYS;
  wanted_spec.silence = 0;
  wanted_spec.samples =
      FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE,
            2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
  wanted_spec.callback = sdl_audio_callback;
  wanted_spec.userdata = opaque;
  // 如果失败，则调整通道数和采样率继续尝试，直到成功或没有更多组合可尝试，提高音频设备的兼容性
  // SDL_AUDIO_ALLOW_FREQUENCY_CHANGE音频设备无法支持所需的采样率时,SDL 会允许尝试使用最接近的可用采样率。
  // SDL_AUDIO_ALLOW_CHANNELS_CHANGE当音频设备无法支持所需的通道数时,SDL 会允许尝试使用最接近的可用通道数。
  while (!(audio_deviceId =
               SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec,
                                   SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                       SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
    av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
           wanted_spec.channels, wanted_spec.freq, SDL_GetError());
    wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
    if (!wanted_spec.channels) {
      wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
      wanted_spec.channels = wanted_nb_channels;
      if (!wanted_spec.freq) {
        av_log(NULL, AV_LOG_ERROR,
               "No more combinations to try, audio open failed\n");
        return -1;
      }
    }
    av_channel_layout_default(wanted_channel_layout, wanted_spec.channels);
  }
  if (spec.format != AUDIO_S16SYS) {
    av_log(NULL, AV_LOG_ERROR,
           "SDL advised audio format %d is not supported!\n", spec.format);
    return -1;
  }
  if (spec.channels != wanted_spec.channels) {
    av_channel_layout_uninit(wanted_channel_layout);
    av_channel_layout_default(wanted_channel_layout, spec.channels);
    if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
      av_log(NULL, AV_LOG_ERROR,
             "SDL advised channel count %d is not supported!\n", spec.channels);
      return -1;
    }
  }

  audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
  audio_hw_params->freq = spec.freq;
  if (av_channel_layout_copy(&audio_hw_params->ch_layout,
                             wanted_channel_layout) < 0)
    return -1;
  audio_hw_params->frame_size = av_samples_get_buffer_size(
      NULL, audio_hw_params->ch_layout.nb_channels, 1, audio_hw_params->fmt, 1);
  audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(
      NULL, audio_hw_params->ch_layout.nb_channels, audio_hw_params->freq,
      audio_hw_params->fmt, 1);
  if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
    av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
    return -1;
  }
  return spec.size;
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(MediaState* ms, int stream_index) {
  AVFormatContext* ic = ms->fmt_ctx;
  AVCodecContext* avctx;
  const AVCodec* codec;
  AVDictionary* opts = NULL;
  const AVDictionaryEntry* t = NULL;
  int sample_rate;
  AVChannelLayout ch_layout = {0};
  int ret = 0;
  int stream_lowres = lowres;

  if (stream_index < 0 || stream_index >= ic->nb_streams) {
    return -1;
  }

  avctx = avcodec_alloc_context3(NULL);
  if (!avctx) {
    return AVERROR(ENOMEM);
  }

  ret =
      avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
  if (ret < 0) {
    goto fail;
  }
  avctx->pkt_timebase = ic->streams[stream_index]->time_base;

  codec = avcodec_find_decoder(avctx->codec_id);
  avctx->codec_id = codec->id;
  // 优化处理，降低分辨率， 0 表示不降低分辨率，看是否需要
  if (stream_lowres > codec->max_lowres) {
    av_log(avctx, AV_LOG_WARNING,
           "The maximum value for lowres supported by the decoder is %d\n",
           codec->max_lowres);
    stream_lowres = codec->max_lowres;
  }
  avctx->lowres = stream_lowres;

  if (!av_dict_get(opts, "threads", NULL, 0))
    av_dict_set(&opts, "threads", "auto", 0);
  if (stream_lowres) {
    av_dict_set_int(&opts, "lowres", stream_lowres, 0);
  }
  if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
    goto fail;
  }
  if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
    av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
    ret = AVERROR_OPTION_NOT_FOUND;
    goto fail;
  }

  ms->eof = 0;
  ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
  switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
      sample_rate = avctx->sample_rate;
      ret = av_channel_layout_copy(&ch_layout, &avctx->ch_layout);
      if (ret < 0) {
        goto fail;
      }

      /* prepare audio output */
      if ((ret = audio_open(ms, &ch_layout, sample_rate, &ms->audio_target)) <
          0)
        goto fail;
      ms->audio_hw_buf_size = ret;
      ms->audio_src = ms->audio_target;
      ms->audio_buf_size = 0;
      ms->audio_buf_index = 0;

      /* init averaging filter */
      // exp(log(0.01) / AUDIO_DIFF_AVG_NB) 这个表达式的含义是:
      // 首先计算 log(0.01) 的结果是 -4.60517。
      // 然后将 -4.60517 除以 20，得到 -0.230258。
      // 最后计算 exp(-0.230258)，结果是 0.794328。
      ms->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
      ms->audio_diff_avg_count = 0;
      /* since we do not have a precise anough audio FIFO fullness,
            we correct audio sync only if larger than this threshold */
      ms->audio_diff_threshold =
          (double)(ms->audio_hw_buf_size) / ms->audio_target.bytes_per_sec;

      ms->audio_index = stream_index;
      ms->audio_stream = ic->streams[stream_index];

      if ((ret = decoder_init(&ms->audio_dec, avctx, &ms->audio_queue,
                              ms->continue_read_thread)) < 0) {
        goto fail;
      }
      if (ms->fmt_ctx->iformat->flags & AVFMT_NOTIMESTAMPS) {
        ms->audio_dec.start_pts = ms->audio_stream->start_time;
        ms->audio_dec.start_pts_tb = ms->audio_stream->time_base;
      }

      if ((ret = decoder_start(&ms->audio_dec, audio_thread, "audio_decoder",
                               ms)) < 0) {
        goto out;
      }
      SDL_PauseAudioDevice(audio_deviceId, 0);
      break;
    case AVMEDIA_TYPE_VIDEO:
      ms->video_index = stream_index;
      ms->video_stream = ic->streams[stream_index];
      if ((ret = decoder_init(&ms->video_dec, avctx, &ms->video_queue,
                              ms->continue_read_thread)) < 0) {
        goto fail;
      }
      if ((ret = decoder_start(&ms->video_dec, video_thread, "video_decoder",
                               ms)) < 0) {
        goto out;
      }
      break;
    case AVMEDIA_TYPE_SUBTITLE:
      ms->subtitle_index = stream_index;
      ms->subtitle_stream = ic->streams[stream_index];
      if ((ret = decoder_init(&ms->sub_dec, avctx, &ms->subtitle_queue,
                              ms->continue_read_thread)) < 0) {
        goto fail;
      }
      if ((ret = decoder_start(&ms->sub_dec, subtitle_thread,
                               "subtitle_decoder", ms)) < 0) {
        goto out;
      }
      break;
    default:
      break;
  }
  goto out;

fail:
  avcodec_free_context(&avctx);
out:
  av_channel_layout_uninit(&ch_layout);
  av_dict_free(&opts);

  return ret;
}

static int decode_interrupt_cb(void* arg) {
  MediaState* ms = arg;
  return ms->abort_request;
}

// AV_DISPOSITION_ATTACHED_PIC 所表示的意思，可查看下方文档
// https://segmentfault.com/a/1190000018373504
// 特殊的video_stream，如果一个流中含有这个标志的话，那么就是说这个流是 *.mp3 文件中的一个 Video Stream 。
// 并且该流只有一个 AVPacket ，也就是 attached_pic 。这个 AVPacket 中所存储的内容就是这个 *.mp3 文件的封面图片。
static int stream_has_enough_packets(AVStream* stream, int stream_id,
                                     PacketQueue* p_queue) {
  return stream_id < 0 || p_queue->abort_request ||
         (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
         p_queue->nb_packets > MIN_FRAMES &&
             (!p_queue->duration ||
              av_q2d(stream->time_base) * p_queue->duration > 1.0);
}

static int is_realtime(AVFormatContext* fmt_ctx) {
  if (!strcmp(fmt_ctx->iformat->name, "rtp") ||
      !strcmp(fmt_ctx->iformat->name, "rtsp") ||
      !strcmp(fmt_ctx->iformat->name, "sdp")) {
    return 1;
  }

  if (fmt_ctx->pb && (!strncmp(fmt_ctx->url, "rtp:", 4) ||
                      !strncmp(fmt_ctx->url, "udp:", 4))) {
    return 1;
  }
  return 0;
}

/* this thread gets the stream from the disk or the network */
static int read_thread(void* arg) {
  MediaState* ms = arg;
  AVFormatContext* ic = NULL;
  int err, ret;
  int stream_index[AVMEDIA_TYPE_NB];
  int64_t stream_start_time;
  int pkt_in_play_range = 0;
  const AVDictionaryEntry* t;
  int64_t pkt_ts;

  SDL_mutex* wait_mutex = SDL_CreateMutex();
  if (!wait_mutex) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
    ret = AVERROR(ENOMEM);
    goto fail;
  }

  memset(stream_index, -1, sizeof(stream_index));
  ms->eof = 0;

  AVPacket* pkt = av_packet_alloc();
  if (!pkt) {
    av_log(NULL, AV_LOG_FATAL, "Could not allocate packet.\n");
    ret = AVERROR(ENOMEM);
    goto fail;
  }
  ic = avformat_alloc_context();
  if (!ic) {
    av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
    ret = AVERROR(ENOMEM);
    goto fail;
  }
  // decode_interrupt_cb用于处理解码过程中的中断
  ic->interrupt_callback.callback = decode_interrupt_cb;
  ic->interrupt_callback.opaque = ms;

  err = avformat_open_input(&ic, ms->filename, ms->iformat, NULL);
  if (err < 0) {
    av_log(NULL, AV_LOG_ERROR, "%s could not open, err: %d\n", ms->filename,
           err);
    ret = -1;
    goto fail;
  }

  ms->fmt_ctx = ic;

  err = avformat_find_stream_info(ic, NULL);
  if (err < 0) {
    av_log(NULL, AV_LOG_WARNING, "%s: could not find codec parameters\n",
           ms->filename);
    ret = -1;
    goto fail;
  }

  if (ic->pb) {
    // FIXME hack, ffplay maybe should not use avio_feof() to test for the end
    ic->pb->eof_reached = 0;
  }

  // 当输入格式支持基于字节偏移量的seek,且时间戳信息不连续,如果时间戳不连续,并且不是 Ogg 格式时,seek_by_bytes 变量会被设置为 true
  if (seek_by_bytes < 0)
    seek_by_bytes = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
                    !!(ic->iformat->flags & AVFMT_TS_DISCONT) &&
                    strcmp("ogg", ic->iformat->name);

  ms->max_frame_duration =
      (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

  // 与 sprintf() 相比, av_asprintf() 的优势在于它能够自动分配足够的内存空间, 避免了缓冲区溢出的风险。同时, 返回的是一个动态分配的字符串指针, 使用更加灵活。
  // 动态内存分配和字符串构建函数，在格式化字符串的同时，分配内存，返回字符串指针
  if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
    window_title = av_asprintf("%s - %s", t->value, input_filename);

  /* if seeking requested, we execute it */
  if (start_time != AV_NOPTS_VALUE) {
    int64_t timestamp;
    timestamp = start_time;
    /* add the stream start time */
    if (ic->start_time != AV_NOPTS_VALUE) {
      timestamp += ic->start_time;
    }
    ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
    if (ret < 0) {
      av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
             ms->filename, (double)timestamp / AV_TIME_BASE);
    }
  }

  ms->realtime = is_realtime(ic);

  if (show_status) {
    av_dump_format(ic, 0, ms->filename, 0);
  }

  // 获取流索引之前，先进行遍历，看播放前有没有选择想要播放对应索引的数据流，没有选择的话，wanted_stream_spec[type]为false，即无效
  // 如果没有设置选择想要播放的流索引的话，这块可以去掉处理，抄写时说明下把它注释掉
  for (int i = 0; i < ic->nb_streams; i++) {
    AVStream* st = ic->streams[i];
    enum AVMediaType type = st->codecpar->codec_type;
    st->discard = AVDISCARD_ALL;
    if (type >= 0 && wanted_stream_spec[type] && stream_index[type] == -1)
      if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0)
        stream_index[type] = i;
  }
  for (int i = 0; i < AVMEDIA_TYPE_NB; i++) {
    if (wanted_stream_spec[i] && stream_index[i] == -1) {
      av_log(NULL, AV_LOG_ERROR,
             "Stream specifier %s does not match any %s stream\n",
             wanted_stream_spec[i], av_get_media_type_string(i));
      stream_index[i] = INT_MAX;
    }
  }

  if (!video_disable)
    stream_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(
        ic, AVMEDIA_TYPE_VIDEO, stream_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
  if (!audio_disable)
    stream_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(
        ic, AVMEDIA_TYPE_AUDIO, stream_index[AVMEDIA_TYPE_AUDIO],
        stream_index[AVMEDIA_TYPE_VIDEO], NULL, 0);
  if (!video_disable && !subtitle_disable)
    stream_index[AVMEDIA_TYPE_SUBTITLE] = av_find_best_stream(
        ic, AVMEDIA_TYPE_SUBTITLE, stream_index[AVMEDIA_TYPE_SUBTITLE],
        (stream_index[AVMEDIA_TYPE_AUDIO] >= 0
             ? stream_index[AVMEDIA_TYPE_AUDIO]
             : stream_index[AVMEDIA_TYPE_VIDEO]),
        NULL, 0);

  ms->show_mode = show_mode;
  if (stream_index[AVMEDIA_TYPE_VIDEO] >= 0) {
    AVStream* stream = ic->streams[stream_index[AVMEDIA_TYPE_VIDEO]];
    AVCodecParameters* codecpar = stream->codecpar;
    AVRational sar = av_guess_sample_aspect_ratio(ic, stream, NULL);
    if (codecpar->width)
      set_default_window_size(codecpar->width, codecpar->height, sar);
  }

  /* open the streams */
  if (stream_index[AVMEDIA_TYPE_AUDIO] >= 0) {
    stream_component_open(ms, stream_index[AVMEDIA_TYPE_AUDIO]);
  }

  ret = -1;
  if (stream_index[AVMEDIA_TYPE_VIDEO] >= 0) {
    ret = stream_component_open(ms, stream_index[AVMEDIA_TYPE_VIDEO]);
  }

  if (stream_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
    stream_component_open(ms, stream_index[AVMEDIA_TYPE_SUBTITLE]);
  }

  if (ms->video_index < 0 && ms->audio_index < 0) {
    av_log(NULL, AV_LOG_FATAL,
           "Failed to open file '%s' or configure filtergraph\n", ms->filename);
    ret = -1;
    goto fail;
  }

  // 与实时流有关
  if (infinite_buffer < 0 && ms->realtime) {
    infinite_buffer = 1;
  }

  // 暂停和恢复媒体流读取的完整功能。av_read_pause 用于在用户暂停视频播放时停止读取数据,
  // 而 av_read_play 用于在用户恢复播放时重新开始读取和解码数据。用于流媒体，网络流(e.g. RTSP stream)
  while (1) {
    if (ms->abort_request == 1) break;
    if (ms->paused != ms->last_paused) {
      ms->last_paused = ms->paused;
      if (ms->paused)
        ms->read_pause_return = av_read_pause(ic);
      else
        av_read_play(ic);
    }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
    if (is->paused && (!strcmp(ic->iformat->name, "rtsp") ||
                       (ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
      /* wait 10 ms to avoid trying to get another packet */
      /* XXX: horrible */
      SDL_Delay(10);
      continue;
    }
#endif

    if (ms->seek_req) {
      int64_t seek_target = ms->seek_pos;
      int64_t seek_min =
          ms->seek_rel > 0 ? seek_target - ms->seek_rel + 2 : INT64_MIN;
      int64_t seek_max =
          ms->seek_rel < 0 ? seek_target - ms->seek_rel - 2 : INT64_MAX;
      // +-2 其实就是为了正确的向上/向下取整
      // FIXME the +-2 is due to rounding being not done in the correct direction in generation
      //      of the seek_pos/seek_rel variables

      ret = avformat_seek_file(ms->fmt_ctx, -1, seek_min, seek_target, seek_max,
                               ms->seek_flags);
      if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n",
               ms->fmt_ctx->url);
      } else {
        if (ms->audio_index >= 0) packet_queue_flush(&ms->audio_queue);
        if (ms->subtitle_index >= 0) packet_queue_flush(&ms->subtitle_queue);
        if (ms->video_index >= 0) packet_queue_flush(&ms->video_queue);
        if (ms->seek_flags & AVSEEK_FLAG_BYTE)
          set_clock(&ms->external_clk, NAN, 0);
        else
          set_clock(&ms->external_clk, seek_target / (double)AV_TIME_BASE, 0);
      }
      ms->seek_req = 0;
      ms->eof = 0;
      if (ms->paused) {
        step_to_next_frame(ms);
      }
    }

    /* if the queue are full, no need to read more */
    /* 限制从流获取packet数据，实时流的话就不限制 */
    if (infinite_buffer < 1 &&
        (ms->audio_queue.size + ms->video_queue.size + ms->subtitle_queue.size >
             MAX_QUEUE_SIZE ||
         (stream_has_enough_packets(ms->audio_stream, ms->audio_index,
                                    &ms->audio_queue) &&
          stream_has_enough_packets(ms->video_stream, ms->video_index,
                                    &ms->video_queue) &&
          stream_has_enough_packets(ms->subtitle_stream, ms->subtitle_index,
                                    &ms->subtitle_queue)))) {
      /* wait 10 ms */
      SDL_LockMutex(wait_mutex);
      SDL_CondWaitTimeout(ms->continue_read_thread, wait_mutex, 10);
      SDL_UnlockMutex(wait_mutex);
      continue;
    }

    // 是否循环播放
    if (!ms->paused &&
        (!ms->audio_stream ||
         (ms->audio_dec.finished == ms->audio_queue.serial &&
          frame_queue_nb_remaining(&ms->sample_queue) == 0)) &&
        (!ms->video_stream ||
         (ms->video_dec.finished == ms->video_queue.serial &&
          frame_queue_nb_remaining(&ms->pic_queue) == 0))) {
      if (loop != 1 && (!loop || --loop)) {
        stream_seek(ms, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
      } else if (autoexit) {
        ret = AVERROR_EOF;
        goto fail;
      }
    }

    ret = av_read_frame(ic, pkt);
    if (ret < 0) {
      // 检查输入流是否到达文件末尾，到文件末尾，推入空包
      if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !ms->eof) {
        if (ms->video_index >= 0)
          packet_queue_put_nullpacket(&ms->video_queue, pkt, ms->video_index);
        if (ms->audio_index >= 0)
          packet_queue_put_nullpacket(&ms->audio_queue, pkt, ms->audio_index);
        if (ms->subtitle_index >= 0)
          packet_queue_put_nullpacket(&ms->subtitle_queue, pkt,
                                      ms->subtitle_index);
        ms->eof = 1;
      }
      if (ic->pb && ic->pb->error) {
        if (autoexit)
          goto fail;
        else
          break;
      }
      SDL_LockMutex(wait_mutex);
      SDL_CondWaitTimeout(ms->continue_read_thread, wait_mutex, 10);
      SDL_UnlockMutex(wait_mutex);
      continue;
    } else {
      ms->eof = 0;
    }

    /* check if packet is in play range specified by user, then queue, otherwise discard */
    stream_start_time = ic->streams[pkt->stream_index]->start_time;
    pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;

    // 这个packet包的数据是否在需要播放的范围里面
    pkt_in_play_range =
        duration == AV_NOPTS_VALUE ||
        (pkt_ts -
         (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                    av_q2d(ic->streams[pkt->stream_index]->time_base) -
                (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) /
                    1000000 <=
            ((double)duration / 1000000);
    if (pkt->stream_index == ms->audio_index && pkt_in_play_range) {
      packet_queue_put(&ms->audio_queue, pkt);
    } else if (pkt->stream_index == ms->video_index && pkt_in_play_range &&
               !(ms->video_stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
      packet_queue_put(&ms->video_queue, pkt);
    } else if (pkt->stream_index == ms->subtitle_index && pkt_in_play_range) {
      packet_queue_put(&ms->subtitle_queue, pkt);
    } else {
      av_packet_unref(pkt);
    }
  }

  ret = 0;
fail:
  if (ic && !ms->fmt_ctx) {
    avformat_close_input(&ic);
  }

  av_packet_free(&pkt);
  if (ret != 0) {
    SDL_Event event;
    event.type = FF_QUIT_EVENT;
    event.user.data1 = ms;
    SDL_PushEvent(&event);
  }
  SDL_DestroyMutex(wait_mutex);
  return 0;
}

static MediaState* stream_open(const char* filename,
                               const AVInputFormat* iformat) {
  MediaState* ms = NULL;

  ms = av_mallocz(sizeof(MediaState));
  if (!ms) {
    return NULL;
  }
  ms->video_index = -1;
  ms->audio_index = -1;
  ms->subtitle_index = -1;
  ms->filename = av_strdup(filename);
  if (!ms->filename) {
    stream_close(ms);
    return NULL;
  }
  ms->iformat = iformat;
  ms->xleft = 0;
  ms->ytop = 0;

  /* start video display */
  if (frame_queue_init(&ms->pic_queue, &ms->video_queue,
                       VIDEO_PICTURE_QUEUE_SIZE, 1) < 0 ||
      frame_queue_init(&ms->sub_queue, &ms->subtitle_queue,
                       SUBPICTURE_QUEUE_SIZE, 0) < 0 ||
      frame_queue_init(&ms->sample_queue, &ms->audio_queue, SAMPLE_QUEUE_SIZE,
                       1) < 0) {
    stream_close(ms);
    return NULL;
  }

  if (packet_queue_init(&ms->video_queue) < 0 ||
      packet_queue_init(&ms->audio_queue) < 0 ||
      packet_queue_init(&ms->subtitle_queue) < 0) {
    stream_close(ms);
    return NULL;
  }

  if (!(ms->continue_read_thread = SDL_CreateCond())) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
    stream_close(ms);
    return NULL;
  }

  init_clock(&ms->video_clk, &ms->video_queue.serial);
  init_clock(&ms->audio_clk, &ms->audio_queue.serial);
  init_clock(&ms->external_clk, &ms->external_clk.serial);
  ms->audio_clock_serial = -1;

  // 设置起始播放的音量
  if (startup_volume < 0) {
    av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n",
           startup_volume);
  }
  if (startup_volume > 100) {
    av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n",
           startup_volume);
  }
  startup_volume = av_clip(startup_volume, 0, 100);
  startup_volume =
      av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
  ms->audio_volume = startup_volume;
  ms->muted = 0;
  ms->av_sync_type = av_sync_type;
  ms->read_tid = SDL_CreateThread(read_thread, "read_thread", ms);
  if (!ms->read_tid) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
    stream_close(ms);
    return NULL;
  }
  return ms;
}

// 用于处理视频刷新循环中的事件等待，负责控制视频帧的显示时机,确保视频与音频的同步,并处理各种用户输入和系统事件
static void refresh_loop_wait_event(MediaState* ms, SDL_Event* event) {
  double remaining_time = 0.0;
  // SDL_PumpEvents作用是获取并处理系统中发生的所有事件。
  SDL_PumpEvents();
  // SDL_PeepEvents 用于从 SDL 的内部事件队列中获取、查看或删除事件
  // 成功读取到事件,循环会退出
  while (
      !SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
    if (!cursor_hidden &&
        av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
      SDL_ShowCursor(0);
      cursor_hidden = 1;
    }
    if (remaining_time > 0.0) {
      av_usleep((int64_t)(remaining_time * 1000000.0));
    }
    remaining_time = REFRESH_RATE;
    if (ms->show_mode != SHOW_MODE_NONE && (!ms->paused || ms->force_refresh))
      video_refresh(ms, &remaining_time);
    SDL_PumpEvents();
  }
}

/* handle an event sent by the GUI */
static void event_loop(MediaState* ms) {
  SDL_Event event;
  double increase, pos;

  while (1) {
    refresh_loop_wait_event(ms, &event);
    switch (event.type) {
      case SDL_KEYDOWN:
        if (exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE ||
            event.key.keysym.sym == SDLK_q) {
          do_exit(ms);
          break;
        }
        // If we don't yet have a window, skip all key events, because read_thread might still be initializing...
        if (!ms->width) {
          continue;
        }
        switch (event.key.keysym.sym) {
          case SDLK_f:
            toggle_full_screen(ms);
            ms->force_refresh = 1;
            break;
          case SDLK_SPACE:
            toggle_pause(ms);
            break;
          case SDLK_m:
            toggle_mute(ms);
            break;
          case SDLK_0:
            update_volume(ms, 1, SDL_VOLUME_STEP);
            break;
          case SDLK_9:
            update_volume(ms, -1, SDL_VOLUME_STEP);
            break;
          case SDLK_s:
            step_to_next_frame(ms);
            break;
          case SDLK_LEFT:
            increase = -10.0;
            goto do_seek;
          case SDLK_RIGHT:
            increase = 10.0;
            goto do_seek;
          case SDLK_UP:
            increase = 60.0;
            goto do_seek;
          case SDLK_DOWN:
            increase = -60.0;
          do_seek:
            if (seek_by_bytes) {
              pos = -1;
              if (pos < 0 && ms->video_index >= 0)
                pos = frame_queue_last_pos(&ms->pic_queue);
              if (pos < 0 && ms->audio_index >= 0)
                pos = frame_queue_last_pos(&ms->sample_queue);
              if (pos < 0) {
                pos = avio_tell(ms->fmt_ctx->pb);
              }
              if (ms->fmt_ctx->bit_rate)
                increase *= ms->fmt_ctx->bit_rate / 8.0;
              else
                increase *= 180000.0;
              pos += increase;
              stream_seek(ms, pos, increase, 1);
            } else {
              pos = get_master_clock(ms);
              if (isnan(pos)) {
                pos = (double)ms->seek_pos / AV_TIME_BASE;
              }
              pos += increase;
              if (ms->fmt_ctx->start_time != AV_NOPTS_VALUE &&
                  pos < ms->fmt_ctx->start_time / (double)AV_TIME_BASE) {
                pos = ms->fmt_ctx->start_time / (double)AV_TIME_BASE;
              }
              stream_seek(ms, (int64_t)(pos * AV_TIME_BASE),
                          (int64_t)(increase * AV_TIME_BASE), 0);
            }
            break;
          default:
            break;
        }
        break;
      case SDL_MOUSEBUTTONDOWN:
        if (exit_on_mousedown) {
          do_exit(ms);
          break;
        }
        if (event.button.button == SDL_BUTTON_LEFT) {
          // 双击全屏操作
          static int64_t last_mouse_left_click = 0;
          if (av_gettime_relative() - last_mouse_left_click <= 500000) {
            toggle_full_screen(ms);
            ms->force_refresh = 1;
            last_mouse_left_click = 0;
          } else {
            last_mouse_left_click = av_gettime_relative();
          }
        }
      case SDL_MOUSEMOTION:
        if (cursor_hidden) {
          SDL_ShowCursor(1);
          cursor_hidden = 0;
        }
        cursor_last_shown = av_gettime_relative();
        break;
      case SDL_WINDOWEVENT:
        switch (event.window.event) {
          case SDL_WINDOWEVENT_SIZE_CHANGED:
            screen_width = ms->width = event.window.data1;
            screen_height = ms->height = event.window.data2;
          case SDL_WINDOWEVENT_EXPOSED:
            // 当窗口状态发生变化,比如从最小化状态恢复或从遮挡状态露出时,会触发 SDL_WINDOWEVENT_EXPOSED 事件。这可用于重新渲染视频画面。
            ms->force_refresh = 1;
        }
        break;
      case SDL_QUIT:
      case FF_QUIT_EVENT:
        do_exit(ms);
        break;
      default:
        break;
    }
  }
}

/* ============= Playback option settings, set by user ============= */
static void set_width(double width) {
  if (width > 0) screen_width = width;
}

static void set_height(double height) {
  if (height > 0) screen_height = height;
}

static int set_format(const char* input_name) {
  file_iformat = av_find_input_format(input_name);
  if (!file_iformat) {
    av_log(NULL, AV_LOG_FATAL, "Unknown input format: %s\n", input_name);
    return AVERROR(EINVAL);
  }
  return 0;
}

static int set_sync_type(const char* arg) {
  if (!strcmp(arg, "audio"))
    av_sync_type = AV_SYNC_AUDIO_MASTER;
  else if (!strcmp(arg, "video"))
    av_sync_type = AV_SYNC_VIDEO_MASTER;
  else if (!strcmp(arg, "ext"))
    av_sync_type = AV_SYNC_EXTERNAL_CLOCK;
  else {
    av_log(NULL, AV_LOG_ERROR, "Unknown value for: %s\n", arg);
    exit(1);
  }
  return 0;
}

static int set_show_mode(enum ShowMode mode) {
  // can't support wave and rdtf mode
  if (show_mode == SHOW_MODE_NONE) {
    if (show_mode > 0 && show_mode < SHOW_MODE_NB - 1) {
      show_mode = mode;
    }
  }
  return 0;
}

static void set_play_start_time(double play_start_time) {
  if (play_start_time > 0) {
    start_time = play_start_time;
  } else {
    start_time = 0;
  }
}

static void set_window_always_on_top(int always_on_top) {
  alwaysontop = always_on_top;
}

static void set_full_screen(int full_screen) { is_full_screen = full_screen; }

static void set_loop_count(int loop_count) { loop = loop_count; }

static void set_disable_to_show_video(int no_display) {
  display_disable = no_display;
}

static void set_audio_disable(int disable_audio) {
  audio_disable = disable_audio;
}

static void set_video_disable(int disable_video) {
  video_disable = disable_video;
}

static void set_disable_subtitling(int disable_subtitling) {
  subtitle_disable = disable_subtitling;
}

// 这个duration在read_thread里面使用到
static void set_play_duration(double play_duration) {
  duration = play_duration;
}

void set_is_need_seek_by_bytes(int is_need_seek_by_bytes) {
  seek_by_bytes = is_need_seek_by_bytes;
}

static void set_window_bordless(int noborder) { borderless = noborder; }

static void set_start_volume(int volume) { startup_volume = volume; }

static void set_auto_exit_at_end(int auto_exit) { autoexit = auto_exit; }

static void set_exit_on_keydown(int keydown) { exit_on_keydown = keydown; }

static void set_exit_on_mousedown(int mousedown) {
  exit_on_mousedown = mousedown;
}
/* ============= Playback option settings, set by user ============= */

static void show_usage(void) {
  av_log(NULL, AV_LOG_INFO, "Simple media player\n");
  av_log(NULL, AV_LOG_INFO, "usage: %s [options] input_file\n", program_name);
  av_log(NULL, AV_LOG_INFO, "\n");
  printf(
      "\nWhile playing:\n"
      "q, ESC              quit\n"
      "f                   toggle full screen\n"
      "SPC                 pause\n"
      "m                   toggle mute\n"
      "9, 0                decrease and increase volume respectively\n"
      "/, *                decrease and increase volume respectively\n"
      "s                   activate frame-step mode\n"
      "left/right          seek backward/forward 10 seconds or to custom "
      "interval if -seek_interval is set\n"
      "down/up             seek backward/forward 1 minute\n"
      "page down/page up   seek backward/forward 10 minutes\n"
      "right mouse click   seek to percentage in file corresponding to "
      "fraction of width\n"
      "left double-click   toggle full screen\n");
}

int main(int argc, char** argv) {
  MediaState* ms;

  avformat_network_init();

  signal(SIGINT, sigterm_handler);  /* Interrupt (ANSI).    */
  signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */

  input_filename = argv[1];
  if (!input_filename) {
    show_usage();
    exit(1);
  }

  if (display_disable) {
    video_disable = 1;
  }

  int flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
  if (audio_disable) {
    flags &= ~SDL_INIT_AUDIO;
  }
  if (display_disable) {
    flags &= ~SDL_INIT_VIDEO;
  }
  if (SDL_Init(flags)) {
    av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n",
           SDL_GetError());
    av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
    exit(1);
  }

  // 用于控制特定类型事件的状态.
  // SDL_IGNORE将事件自动从事件队列中删除，并且不会被过滤。优化性能或者过滤掉不需要的事件
  SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
  SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

  if (!display_disable) {
    int flags = SDL_WINDOW_HIDDEN;
    if (alwaysontop) {
      flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    }
    if (borderless)
      flags |= SDL_WINDOW_BORDERLESS;
    else
      flags |= SDL_WINDOW_RESIZABLE;

    window = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, default_width,
                              default_height, flags);

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (window) {
      renderer = SDL_CreateRenderer(
          window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
      if (!renderer) {
        av_log(NULL, AV_LOG_WARNING,
               "Failed to initialize a hardware accelerated renderer: %s\n",
               SDL_GetError());
        renderer = SDL_CreateRenderer(window, -1, 0);
      }
      if (renderer) {
        if (!SDL_GetRendererInfo(renderer, &renderer_info))
          av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n",
                 renderer_info.name);
      }
    }

    if (!window || !renderer || !renderer_info.num_texture_formats) {
      av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s",
             SDL_GetError());
      do_exit(NULL);
    }
  }

  ms = stream_open(input_filename, file_iformat);
  if (!ms) {
    av_log(NULL, AV_LOG_FATAL, "Failed to initialize MediaState!\n");
    do_exit(NULL);
  }

  event_loop(ms);
  /* never returns */
  return 0;
}
