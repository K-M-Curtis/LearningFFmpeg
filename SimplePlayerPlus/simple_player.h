#pragma once

#ifndef SIMPLE_PLAYER_H
#define SIMPLE_PLAYER_H

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/bprint.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <math.h>
}

#include "frame_queue_manager.h"
#include "packet_queue_manager.h"

#define AVRATIONAL(a, b) \
  AVRational { a, b }

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

class DecoderManager {
 public:
  static int decoder_init(Decoder* decoder, AVCodecContext* av_ctx,
                          PacketQueue* p_queue, SDL_cond* empty_queue_cond);
  static int decoder_start(Decoder* decoder, int (*fn)(void*),
                           const char* thread_name, void* arg);
  static int decoder_decode_frame(Decoder* decoder, AVFrame* frame,
                                  AVSubtitle* sub);
  static void decoder_destroy(Decoder* decoder);
  static void decoder_abort(Decoder* decoder, FrameQueue* f_queue);
};

class ClockManager {
 public:
  static void init_clock(Clock* clock, int* queue_serial);

  static double get_clock(Clock* clock);
  static void set_clock_at(Clock* clock, double pts, int serial, double time);
  static void set_clock(Clock* clock, double pts, int serial);
  static void set_clock_speed(Clock* clock, double speed);

  static void sync_clock_to_slave(Clock* c, Clock* slave);
  static int get_master_sync_type(MediaState* ms);
  static double get_master_clock(MediaState* ms);
  static void check_external_clock_speed(MediaState* ms);
};

class SimplePlayer {
 public:
  SimplePlayer(const char* filename);
  ~SimplePlayer();

  MediaState* getMediaState() { return ms; }
  void initialize();
  void Runable();
  void event_loop(MediaState* ms);

  void stream_seek(MediaState* ms, int64_t pos, int64_t rel, int by_bytes);
  /* pause or resume the video */
  void stream_toggle_pause(MediaState* ms);
  void toggle_pause(MediaState* ms);
  void toggle_mute(MediaState* ms);
  void update_volume(MediaState* ms, int sign, double step);
  void step_to_next_frame(MediaState* ms);
  void toggle_full_screen(MediaState* ms);

  int demuxing(MediaState* ms);
  int upload_texture(SDL_Texture** texture, AVFrame* frame);
  void video_image_display(MediaState* ms);
  void set_default_window_size(int width, int height, AVRational aspect_ratio);
  int video_open(MediaState* ms);
  void video_display(MediaState* ms);

  void stream_component_close(MediaState* ms, int stream_index);
  void stream_close(MediaState* ms);
  void do_exit(MediaState* ms);

  int audio_thread_ex(MediaState* ms);
  int video_thread_ex(MediaState* ms);
  int subtitle_thread_ex(MediaState* ms);

  int synchronize_audio(MediaState* ms, int nb_samples);
  int audio_decode_frame(MediaState* ms);

  double compute_target_delay(double delay, MediaState* ms);
  double vp_duration(MediaState* ms, Frame* vp, Frame* nextvp);
  void update_video_pts(MediaState* ms, double pts, int serial);
  void video_refresh(void* opaque, double* remaining_time);
  int queue_picture(MediaState* ms, AVFrame* src_frame, double pts,
                    double duration, int64_t pos, int serial);
  int get_video_frame(MediaState* ms, AVFrame* frame);

  int audio_open(void* opaque, AVChannelLayout* wanted_channel_layout,
                 int wanted_sample_rate, struct AudioParams* audio_hw_params);
  int stream_component_open(MediaState* ms, int stream_index);
  int stream_has_enough_packets(AVStream* stream, int stream_id,
                                PacketQueue* p_queue);
  int is_realtime(AVFormatContext* fmt_ctx);
  MediaState* stream_open(const char* filename, const AVInputFormat* iformat);
  void refresh_loop_wait_event(MediaState* ms, SDL_Event* event);

  /* ======SetOption====== */
  void set_width(double width) {
    if (width > 0) screen_width_ = width;
  }

  void set_height(double height) {
    if (height > 0) screen_height_ = height;
  }

  int set_format(const char* input_name) {
    file_iformat_ = av_find_input_format(input_name);
    if (!file_iformat_) {
      av_log(NULL, AV_LOG_FATAL, "Unknown input format: %s\n", input_name);
      return AVERROR(EINVAL);
    }
    return 0;
  }

  int set_sync_type(const char* arg) {
    if (!strcmp(arg, "audio"))
      av_sync_type_ = AV_SYNC_AUDIO_MASTER;
    else if (!strcmp(arg, "video"))
      av_sync_type_ = AV_SYNC_VIDEO_MASTER;
    else if (!strcmp(arg, "ext"))
      av_sync_type_ = AV_SYNC_EXTERNAL_CLOCK;
    else {
      av_log(NULL, AV_LOG_ERROR, "Unknown value for: %s\n", arg);
      exit(1);
    }
    return 0;
  }

  int set_show_mode(enum MediaState::ShowMode mode) {
    // can't support wave and rdtf mode
    if (show_mode_ == MediaState::ShowMode::SHOW_MODE_NONE) {
      if (show_mode_ > 0 &&
          show_mode_ < MediaState::ShowMode::SHOW_MODE_NB - 1) {
        show_mode_ = mode;
      }
    }
    return 0;
  }

  void set_play_start_time(double play_start_time) {
    if (play_start_time > 0) {
      start_time_ = play_start_time;
    } else {
      start_time_ = 0;
    }
  }

  void set_window_always_on_top(int always_on_top) {
    alwaysontop_ = always_on_top;
  }
  void set_full_screen(int full_screen) { is_full_screen_ = full_screen; }
  void set_loop_count(int loop_count) { loop_ = loop_count; }
  void set_disable_to_show_video(int no_display) {
    display_disable_ = no_display;
  }
  void set_audio_disable(int disable_audio) { audio_disable_ = disable_audio; }
  void set_video_disable(int disable_video) { video_disable_ = disable_video; }
  void set_disable_subtitling(int disable_subtitling) {
    subtitle_disable_ = disable_subtitling;
  }
  // 这个duration在read_thread里面使用到
  void set_play_duration(double play_duration) { duration_ = play_duration; }
  void set_is_need_seek_by_bytes(int is_need_seek_by_bytes) {
    seek_by_bytes_ = is_need_seek_by_bytes;
  }
  void set_window_bordless(int noborder) { borderless_ = noborder; }
  void set_start_volume(int volume) { startup_volume_ = volume; }
  void set_auto_exit_at_end(int auto_exit) { autoexit_ = auto_exit; }
  void set_exit_on_keydown(int keydown) { exit_on_keydown_ = keydown; }
  void set_exit_on_mousedown(int mousedown) { exit_on_mousedown_ = mousedown; }
  /* ======SetOption====== */

  int64_t audio_callback_time_;

 private:
  int realloc_texture(SDL_Texture** texture, Uint32 new_format, int new_width,
                      int new_height, SDL_BlendMode blendmode,
                      int init_texture);

  MediaState* ms;

  int audio_disable_ = 0;
  int video_disable_ = 0;
  int subtitle_disable_ = 0;
  const char* wanted_stream_spec_[AVMEDIA_TYPE_NB] = {0};
  int seek_by_bytes_ = -1;
  int display_disable_ = 0;  // 是否显示视频内容
  int borderless_ = 0;
  int alwaysontop_ = 0;
  int startup_volume_ = 100;

  // show_status主要作用是在播放过程中显示媒体的状态信息。
  int show_status_ = -1;
  int av_sync_type_ = AV_SYNC_AUDIO_MASTER;
  int64_t start_time_ = AV_NOPTS_VALUE;
  int64_t duration_ = AV_NOPTS_VALUE;
  // 控制视频解码输出的分辨率，降低视频分辨率，节省处理资源，快速预览，这个不用设置，就让他等于0
  int lowres_ = 0;
  int autoexit_ = 0;  // exit at the end视频结束后是否自动退出
  int exit_on_keydown_ = 0;
  int exit_on_mousedown_ = 0;
  int loop_ = 1;
  int framedrop_ = -1;  // 无需设置
  int infinite_buffer_ = -1;  // =1的话不限制输入缓冲区大小（对实时流有用）
  enum MediaState::ShowMode show_mode_;
  int64_t cursor_last_shown_;
  int cursor_hidden_ = 0;
  int is_full_screen_ = 0;

  SDL_Window* window_ = NULL;
  SDL_Renderer* renderer_ = NULL;
  SDL_RendererInfo renderer_info_ = {0};
  SDL_AudioDeviceID audio_deviceId_;

  /* options specified by the user */
  const AVInputFormat* file_iformat_;
  const char* input_filename_;
  const char* window_title_;
  int default_width_ = 640;
  int default_height_ = 480;
  int screen_width_ = 1280;
  int screen_height_ = 960;
  int screen_left_ = SDL_WINDOWPOS_CENTERED;
  int screen_top_ = SDL_WINDOWPOS_CENTERED;
};

#endif  // SIMPLE_PLAYER_H