#include "simple_player.h"

int DecoderManager::decoder_init(Decoder* decoder, AVCodecContext* av_ctx,
                                 PacketQueue* p_queue,
                                 SDL_cond* empty_queue_cond) {
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
int DecoderManager::decoder_decode_frame(Decoder* decoder, AVFrame* frame,
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
              AVRational tb = AVRATIONAL(1, frame->sample_rate);
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
        if (PacketQueueManager::packet_queue_get(decoder->p_queue, decoder->pkt,
                                                 1, &decoder->pkt_serial) < 0) {
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

void DecoderManager::decoder_destroy(Decoder* decoder) {
  av_packet_free(&decoder->pkt);
  avcodec_free_context(&decoder->av_ctx);
}

// 中止解码器：当收到信号或触发某些条件时,decoder_abort 函数会被调用,用于通知解码器立即中止当前的解码操作。这可能发生在用户手动停止播放、切换媒体源或者出现解码错误等情况下。
void DecoderManager::decoder_abort(Decoder* decoder, FrameQueue* f_queue) {
  PacketQueueManager::packet_queue_abort(decoder->p_queue);
  FrameQueueManager::frame_queue_signal(f_queue);
  SDL_WaitThread(decoder->decoder_thread, NULL);
  decoder->decoder_thread = NULL;
  PacketQueueManager::packet_queue_flush(decoder->p_queue);
}

int DecoderManager::decoder_start(Decoder* decoder, int (*fn)(void*),
                                  const char* thread_name, void* arg) {
  PacketQueueManager::packet_queue_start(decoder->p_queue);
  decoder->decoder_thread = SDL_CreateThread(fn, thread_name, arg);
  if (!decoder->decoder_thread) {
    av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  return 0;
}

// 用于获取当前时钟的时间值
double ClockManager::get_clock(Clock* clock) {
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
void ClockManager::set_clock_at(Clock* clock, double pts, int serial,
                                double time) {
  clock->pts = pts;
  clock->last_updated = time;
  clock->pts_drift = clock->pts - time;
  clock->serial = serial;
}

// 用于设置当前时钟的时间值。
void ClockManager::set_clock(Clock* clock, double pts, int serial) {
  double time = av_gettime_relative() / 1000000.0;
  set_clock_at(clock, pts, serial, time);
}

// 设置时钟 c 的播放速率为 speed。
// 主要用于同步音视频时钟,而不是用于实现倍速播放。
void ClockManager::set_clock_speed(Clock* clock, double speed) {
  set_clock(clock, get_clock(clock), clock->serial);
  clock->speed = speed;
}

void ClockManager::init_clock(Clock* clock, int* queue_serial) {
  clock->speed = 1.0;
  clock->paused = 0;
  clock->queue_serial = queue_serial;
  set_clock(clock, NAN, -1);
  // NAN: 表示无法用正常数值表示的情况，不代表任何数值
}

// 将时钟 c 与 slave 时钟同步。
// 用于在多路流播放时,将视频时钟与音频时钟进行同步。确保视频和音频保持良好的同步。
void ClockManager::sync_clock_to_slave(Clock* c, Clock* slave) {
  double clock = get_clock(c);
  double slave_clock = get_clock(slave);
  if (!isnan(slave_clock) &&
      (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD)) {
    set_clock(c, slave_clock, slave->serial);
  }
}

int ClockManager::get_master_sync_type(MediaState* ms) {
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
double ClockManager::get_master_clock(MediaState* ms) {
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
void ClockManager::check_external_clock_speed(MediaState* ms) {
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

SimplePlayer::SimplePlayer(const char* filename) : input_filename_(filename) {
  initialize();
}

SimplePlayer::~SimplePlayer() {
  if (ms)
    do_exit(ms);
  else
    do_exit(NULL);
}

void SimplePlayer::initialize() {
  avformat_network_init();

  if (display_disable_) {
    video_disable_ = 1;
  }

  int flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
  if (audio_disable_) {
    flags &= ~SDL_INIT_AUDIO;
  }
  if (display_disable_) {
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

  if (!display_disable_) {
    int flags = SDL_WINDOW_HIDDEN;
    if (alwaysontop_) {
      flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    }
    if (borderless_)
      flags |= SDL_WINDOW_BORDERLESS;
    else
      flags |= SDL_WINDOW_RESIZABLE;

    window_ = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED,
                               SDL_WINDOWPOS_UNDEFINED, default_width_,
                               default_height_, flags);

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (window_) {
      renderer_ = SDL_CreateRenderer(
          window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
      if (!renderer_) {
        av_log(NULL, AV_LOG_WARNING,
               "Failed to initialize a hardware accelerated renderer: %s\n",
               SDL_GetError());
        renderer_ = SDL_CreateRenderer(window_, -1, 0);
      }
      if (renderer_) {
        if (!SDL_GetRendererInfo(renderer_, &renderer_info_))
          av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n",
                 renderer_info_.name);
      }
    }

    if (!window_ || !renderer_ || !renderer_info_.num_texture_formats) {
      av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s",
             SDL_GetError());
      do_exit(NULL);
    }
  }
}

void SimplePlayer::Runable() {
  ms = stream_open(input_filename_, file_iformat_);
  if (!ms) {
    av_log(NULL, AV_LOG_FATAL, "Failed to initialize MediaState!\n");
    do_exit(NULL);
  }

  event_loop(ms);
}

int SimplePlayer::realloc_texture(SDL_Texture** texture, Uint32 new_format,
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
    if (!(*texture = SDL_CreateTexture(renderer_, new_format,
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
int SimplePlayer::upload_texture(SDL_Texture** texture, AVFrame* frame) {
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
void SimplePlayer::video_image_display(MediaState* ms) {
  Frame* video_pic;
  Frame* subtitle_pic = NULL;
  SDL_Rect rect;

  video_pic = FrameQueueManager::frame_queue_peek_last(&ms->pic_queue);
  if (ms->subtitle_stream) {
    if (FrameQueueManager::frame_queue_nb_remaining(&ms->sub_queue) > 0) {
      subtitle_pic = FrameQueueManager::frame_queue_peek(&ms->sub_queue);

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

  SDL_RenderCopyEx(renderer_, ms->video_texture, NULL, &rect, 0, NULL,
                   (SDL_RendererFlip)0);
  if (subtitle_pic) {
    // 字幕一次性渲染出来
    SDL_RenderCopy(renderer_, ms->sub_texture, NULL, &rect);
  }
}

void SimplePlayer::set_default_window_size(int width, int height,
                                           AVRational aspect_ratio) {
  SDL_Rect rect;
  int max_width = screen_width_ ? screen_width_ : INT_MAX;
  int max_height = screen_height_ ? screen_height_ : INT_MAX;
  if (max_width == INT_MAX && max_height == INT_MAX) {
    max_height = height;
  }
  calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height,
                         aspect_ratio);
  default_width_ = rect.w;
  default_height_ = rect.h;
}

int SimplePlayer::video_open(MediaState* ms) {
  int w, h;

  w = screen_width_ ? screen_width_ : default_width_;
  h = screen_height_ ? screen_height_ : default_height_;

  if (!window_title_) {
    window_title_ = input_filename_;
  }
  SDL_SetWindowTitle(window_, window_title_);

  SDL_SetWindowSize(window_, w, h);
  SDL_SetWindowPosition(window_, screen_left_, screen_top_);
  if (is_full_screen_)
    SDL_SetWindowFullscreen(window_, SDL_WINDOW_FULLSCREEN_DESKTOP);
  SDL_ShowWindow(window_);

  ms->width = w;
  ms->height = h;

  return 0;
}

/* display the current picture, if any */
void SimplePlayer::video_display(MediaState* ms) {
  if (!ms->width) {
    video_open(ms);
  }

  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
  SDL_RenderClear(renderer_);
  if (ms->audio_stream &&
      ms->show_mode != MediaState::ShowMode::SHOW_MODE_VIDEO) {
    // video_audio_display(is);  // 根据显示模式显示波形图或频谱分析图，先不处理
  } else if (ms->video_stream) {
    video_image_display(ms);
  }
  SDL_RenderPresent(renderer_);
}
/* ============= about VideoDisplay ============= */

void SimplePlayer::stream_component_close(MediaState* ms, int stream_index) {
  AVFormatContext* ic = ms->fmt_ctx;
  AVCodecParameters* codecpar;
  if (stream_index < 0 || stream_index >= ic->nb_streams) {
    return;
  }
  codecpar = ic->streams[stream_index]->codecpar;

  switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
      DecoderManager::decoder_abort(&ms->audio_dec, &ms->sample_queue);
      SDL_CloseAudioDevice(audio_deviceId_);
      DecoderManager::decoder_destroy(&ms->audio_dec);
      swr_free(&ms->swr_ctx);
      av_freep(&ms->audio_buf1);
      ms->audio_buf1_size = 0;
      ms->audio_buf = NULL;
      break;
    case AVMEDIA_TYPE_VIDEO:
      DecoderManager::decoder_abort(&ms->video_dec, &ms->pic_queue);
      DecoderManager::decoder_destroy(&ms->video_dec);
      break;
    case AVMEDIA_TYPE_SUBTITLE:
      DecoderManager::decoder_abort(&ms->sub_dec, &ms->sub_queue);
      DecoderManager::decoder_destroy(&ms->sub_dec);
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

void SimplePlayer::stream_close(MediaState* ms) {
  /* XXX: use a special url_shutdown call to abort parse cleanly */
  ms->abort_request = 1;
  SDL_WaitThread(ms->read_tid, NULL);

  /* close each stream */
  if (ms->audio_index >= 0) stream_component_close(ms, ms->audio_index);
  if (ms->video_index >= 0) stream_component_close(ms, ms->video_index);
  if (ms->subtitle_index >= 0) stream_component_close(ms, ms->subtitle_index);

  avformat_close_input(&ms->fmt_ctx);
  PacketQueueManager::packet_queue_destroy(&ms->video_queue);
  PacketQueueManager::packet_queue_destroy(&ms->audio_queue);
  PacketQueueManager::packet_queue_destroy(&ms->subtitle_queue);

  /* free all pictures */
  FrameQueueManager::frame_queue_destroy(&ms->pic_queue);
  FrameQueueManager::frame_queue_destroy(&ms->sample_queue);
  FrameQueueManager::frame_queue_destroy(&ms->sub_queue);
  SDL_DestroyCond(ms->continue_read_thread);
  sws_freeContext(ms->sub_convert_ctx);
  av_free(ms->filename);
  if (ms->video_texture) SDL_DestroyTexture(ms->video_texture);
  if (ms->sub_texture) SDL_DestroyTexture(ms->sub_texture);
  av_free(ms);
}

void SimplePlayer::do_exit(MediaState* ms) {
  if (ms) {
    stream_close(ms);
  }
  if (renderer_) SDL_DestroyRenderer(renderer_);
  if (window_) SDL_DestroyWindow(window_);
  av_freep(&input_filename_);
  avformat_network_deinit();
  if (show_status_) {
    printf("\n");
  }
  SDL_Quit();
  av_log(NULL, AV_LOG_QUIET, "%s", "");
  exit(0);
}

// 根据当前的同步状态,计算出视频帧需要的延迟时间
double SimplePlayer::compute_target_delay(double delay, MediaState* ms) {
  double sync_threshold, diff = 0;

  /* update delay to follow master synchronisation source */
  if (ClockManager::get_master_sync_type(ms) != AV_SYNC_VIDEO_MASTER) {
    /* if video is slave, we try to correct big delays by
            duplicating or deleting a frame */
    /* 根据diff决定是需要跳过还是重复播放某些帧 */
    diff = ClockManager::get_clock(&ms->video_clk) -
           ClockManager::get_master_clock(ms);

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
double SimplePlayer::vp_duration(MediaState* ms, Frame* vp, Frame* nextvp) {
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

void SimplePlayer::update_video_pts(MediaState* ms, double pts, int serial) {
  /* update current video pts */
  ClockManager::set_clock(&ms->video_clk, pts, serial);
  ClockManager::sync_clock_to_slave(&ms->external_clk, &ms->video_clk);
}

/* called to display each frame */
void SimplePlayer::video_refresh(void* opaque, double* remaining_time) {
  MediaState* ms = (MediaState*)opaque;
  double time;
  Frame* subtitle_pic;
  Frame* subtitle_pic2;

  if (!ms->paused &&
      ClockManager::get_master_sync_type(ms) == AV_SYNC_EXTERNAL_CLOCK &&
      ms->realtime) {
    ClockManager::check_external_clock_speed(ms);
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
      if (FrameQueueManager::frame_queue_nb_remaining(&ms->pic_queue) == 0) {
        // nothing to do, no picture to display in the queue
        break;
      } else {
        double last_duration, duration, delay;
        Frame* video_pic;
        Frame* last_video_pic;

        /* dequeue the picture */
        last_video_pic =
            FrameQueueManager::frame_queue_peek_last(&ms->pic_queue);
        video_pic = FrameQueueManager::frame_queue_peek(&ms->pic_queue);

        if (video_pic->serial != ms->video_queue.serial) {
          FrameQueueManager::frame_queue_next(&ms->pic_queue);
          continue;
        }

        if (last_video_pic->serial != video_pic->serial)
          ms->frame_timer = av_gettime_relative() / 1000000.0;

        if (ms->paused) {
          /* display picture */
          if (!display_disable_ && ms->force_refresh &&
              ms->show_mode == MediaState::ShowMode::SHOW_MODE_VIDEO &&
              ms->pic_queue.rindex_shown) {
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
          if (!display_disable_ && ms->force_refresh &&
              ms->show_mode == MediaState::ShowMode::SHOW_MODE_VIDEO &&
              ms->pic_queue.rindex_shown)
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

        if (FrameQueueManager::frame_queue_nb_remaining(&ms->pic_queue) > 1) {
          Frame* next_video_pic =
              FrameQueueManager::frame_queue_peek_next(&ms->pic_queue);
          duration = vp_duration(ms, video_pic, next_video_pic);
          if (!ms->step &&
              (framedrop_ > 0 ||
               (framedrop_ && ClockManager::get_master_sync_type(ms) !=
                                  AV_SYNC_VIDEO_MASTER)) &&
              time > ms->frame_timer + duration) {
            ms->frame_drops_late++;
            FrameQueueManager::frame_queue_next(&ms->pic_queue);
            continue;
          }
        }

        if (ms->subtitle_stream) {
          while (FrameQueueManager::frame_queue_nb_remaining(&ms->sub_queue) >
                 0) {
            subtitle_pic = FrameQueueManager::frame_queue_peek(&ms->sub_queue);

            if (FrameQueueManager::frame_queue_nb_remaining(&ms->sub_queue) > 1)
              subtitle_pic2 =
                  FrameQueueManager::frame_queue_peek_next(&ms->sub_queue);
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
              FrameQueueManager::frame_queue_next(&ms->sub_queue);
            } else {
              break;
            }
          }
        }

        FrameQueueManager::frame_queue_next(&ms->pic_queue);
        ms->force_refresh = 1;

        if (ms->step && !ms->paused) {
          stream_toggle_pause(ms);
        }
      }

      /* display picture */
      if (!display_disable_ && ms->force_refresh &&
          ms->show_mode == MediaState::ShowMode::SHOW_MODE_VIDEO &&
          ms->pic_queue.rindex_shown) {
        video_display(ms);
      }
      break;
    }
    ms->force_refresh = 0;
    if (show_status_) {
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
          av_diff = ClockManager::get_clock(&ms->audio_clk) -
                    ClockManager::get_clock(&ms->video_clk);
        else if (ms->video_stream)
          av_diff = ClockManager::get_master_clock(ms) -
                    ClockManager::get_clock(&ms->video_clk);
        else if (ms->audio_stream)
          av_diff = ClockManager::get_master_clock(ms) -
                    ClockManager::get_clock(&ms->audio_clk);

        av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
        av_bprintf(
            &buf, "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB \r",
            ClockManager::get_master_clock(ms),
            (ms->audio_stream && ms->video_stream)
                ? "A-V"
                : (ms->video_stream ? "M-V"
                                    : (ms->audio_stream ? "M-A" : "   ")),
            av_diff, ms->frame_drops_early + ms->frame_drops_late,
            aqsize / 1024, vqsize / 1024, sqsize);

        if (show_status_ == 1 && AV_LOG_INFO > av_log_get_level())
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
int SimplePlayer::queue_picture(MediaState* ms, AVFrame* src_frame, double pts,
                                double duration, int64_t pos, int serial) {
  Frame* video_pic;
  // 用于获取视频帧的帧类型
  //  printf("frame_type=%c pts=%0.3f\n",
  //         av_get_picture_type_char(src_frame->pict_type), pts);

  if (!(video_pic =
            FrameQueueManager::frame_queue_peek_writable(&ms->pic_queue))) {
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
  FrameQueueManager::frame_queue_push(&ms->pic_queue);
  return 0;
}

// 获取视频帧
int SimplePlayer::get_video_frame(MediaState* ms, AVFrame* frame) {
  int got_picture;

  if ((got_picture = DecoderManager::decoder_decode_frame(&ms->video_dec, frame,
                                                          NULL)) < 0)
    return -1;

  if (got_picture) {
    double dpts = NAN;

    if (frame->pts != AV_NOPTS_VALUE)
      dpts = av_q2d(ms->video_stream->time_base) * frame->pts;

    frame->sample_aspect_ratio =
        av_guess_sample_aspect_ratio(ms->fmt_ctx, ms->video_stream, frame);

    if (framedrop_ > 0 || (framedrop_ && ClockManager::get_master_sync_type(
                                             ms) != AV_SYNC_VIDEO_MASTER)) {
      if (frame->pts != AV_NOPTS_VALUE) {
        double diff = dpts - ClockManager::get_master_clock(ms);
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
  SimplePlayer* player = (SimplePlayer*)arg;
  return player->audio_thread_ex(player->getMediaState());
}

int SimplePlayer::audio_thread_ex(MediaState* ms) {
  AVFrame* frame = av_frame_alloc();
  Frame* audio_frame = NULL;
  int got_frame = 0;
  AVRational tb;
  int ret = 0;

  if (!frame) {
    return AVERROR(ENOMEM);
  }

  do {
    if ((got_frame = DecoderManager::decoder_decode_frame(&ms->audio_dec, frame,
                                                          NULL)) < 0) {
      av_frame_free(&frame);
      return ret;
    }

    if (got_frame) {
      tb = AVRATIONAL(1, frame->sample_rate);

      if (!(audio_frame = FrameQueueManager::frame_queue_peek_writable(
                &ms->sample_queue))) {
        av_frame_free(&frame);
        return ret;
      }

      audio_frame->pts =
          (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
      audio_frame->pos = frame->pkt_pos;
      audio_frame->serial = ms->audio_dec.pkt_serial;
      audio_frame->duration =
          av_q2d(AVRATIONAL(frame->nb_samples, frame->sample_rate));

      av_frame_move_ref(audio_frame->frame, frame);
      FrameQueueManager::frame_queue_push(&ms->sample_queue);
    }
  } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
  av_frame_free(&frame);
  return ret;
}

static int video_thread(void* arg) {
  SimplePlayer* player = (SimplePlayer*)arg;
  return player->video_thread_ex(player->getMediaState());
}

int SimplePlayer::video_thread_ex(MediaState* ms) {
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
                    ? av_q2d(AVRATIONAL(frame_rate.den, frame_rate.num))
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
  SimplePlayer* player = (SimplePlayer*)arg;
  return player->subtitle_thread_ex(player->getMediaState());
}

int SimplePlayer::subtitle_thread_ex(MediaState* ms) {
  Frame* sub_pic;
  int got_subtitle;
  double pts;

  while (1) {
    if (!(sub_pic =
              FrameQueueManager::frame_queue_peek_writable(&ms->sub_queue))) {
      return 0;
    }

    if ((got_subtitle = DecoderManager::decoder_decode_frame(
             &ms->sub_dec, NULL, &sub_pic->sub)) < 0) {
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
      FrameQueueManager::frame_queue_push(&ms->sub_queue);
    } else if (got_subtitle) {
      avsubtitle_free(&sub_pic->sub);
    }
  }
  return 0;
}

/* return the wanted number of samples to get better sync if sync_type is video
  * or external master clock */
int SimplePlayer::synchronize_audio(MediaState* ms, int nb_samples) {
  int wanted_nb_samples = nb_samples;

  /* if not master, then we try to remove or add samples to correct the clock */
  if (ClockManager::get_master_sync_type(ms) != AV_SYNC_AUDIO_MASTER) {
    double diff, avg_diff;
    int min_nb_samples, max_nb_samples;

    diff = ClockManager::get_clock(&ms->audio_clk) -
           ClockManager::get_master_clock(ms);
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
int SimplePlayer::audio_decode_frame(MediaState* ms) {
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
    while (FrameQueueManager::frame_queue_nb_remaining(&ms->sample_queue) ==
           0) {
      if ((av_gettime_relative() - audio_callback_time_) >
          1000000LL * ms->audio_hw_buf_size / ms->audio_target.bytes_per_sec /
              2) {
        return -1;
      }

      av_usleep(1000);
    }
#endif
    if (!(audio_frame = FrameQueueManager::frame_queue_peek_readable(
              &ms->sample_queue))) {
      return -1;
    }
    FrameQueueManager::frame_queue_next(&ms->sample_queue);
  } while (audio_frame->serial != ms->audio_queue.serial);

  // av_samples_get_buffer_size根据给定的音频参数(采样率、声道数、样本格式等)计算出所需的缓冲区大小(以字节为单位)
  data_size = av_samples_get_buffer_size(
      NULL, audio_frame->frame->ch_layout.nb_channels,
      audio_frame->frame->nb_samples,
      (AVSampleFormat)audio_frame->frame->format, 1);

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
    swr_alloc_set_opts2(&ms->swr_ctx, &ms->audio_target.ch_layout,
                        ms->audio_target.fmt, ms->audio_target.freq,
                        &audio_frame->frame->ch_layout,
                        (AVSampleFormat)audio_frame->frame->format,
                        audio_frame->frame->sample_rate, 0, NULL);
    if (!ms->swr_ctx || swr_init(ms->swr_ctx) < 0) {
      av_log(NULL, AV_LOG_ERROR,
             "Cannot create sample rate converter for conversion of %d Hz %s "
             "%d channels to %d Hz %s %d channels!\n",
             audio_frame->frame->sample_rate,
             av_get_sample_fmt_name((AVSampleFormat)audio_frame->frame->format),
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
    ms->audio_src.fmt = (AVSampleFormat)audio_frame->frame->format;
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
  SimplePlayer* player = (SimplePlayer*)opaque;
  MediaState* ms = player->getMediaState();
  int audio_size, rel_len;

  player->audio_callback_time_ = av_gettime_relative();

  while (len > 0) {
    if (ms->audio_buf_index >= ms->audio_buf_size) {
      audio_size = player->audio_decode_frame(ms);
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
    ClockManager::set_clock_at(
        &ms->audio_clk,
        ms->audio_clock -
            (double)(2 * ms->audio_hw_buf_size + ms->audio_write_buf_size) /
                ms->audio_target.bytes_per_sec,
        ms->audio_clock_serial, player->audio_callback_time_ / 1000000.0);
    ClockManager::sync_clock_to_slave(&ms->external_clk, &ms->audio_clk);
  }
}

// 配置和打开音频设备并设置音频参数，以便进行音频播放
int SimplePlayer::audio_open(void* opaque,
                             AVChannelLayout* wanted_channel_layout,
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
  while (!(audio_deviceId_ =
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
int SimplePlayer::stream_component_open(MediaState* ms, int stream_index) {
  AVFormatContext* ic = ms->fmt_ctx;
  AVCodecContext* avctx;
  const AVCodec* codec;
  AVDictionary* opts = NULL;
  const AVDictionaryEntry* t = NULL;
  int sample_rate;
  AVChannelLayout ch_layout;
  int ret = 0;
  int stream_lowres = lowres_;

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
      if ((ret = audio_open(this, &ch_layout, sample_rate, &ms->audio_target)) <
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

      if ((ret = DecoderManager::decoder_init(&ms->audio_dec, avctx,
                                              &ms->audio_queue,
                                              ms->continue_read_thread)) < 0) {
        goto fail;
      }
      if (ms->fmt_ctx->iformat->flags & AVFMT_NOTIMESTAMPS) {
        ms->audio_dec.start_pts = ms->audio_stream->start_time;
        ms->audio_dec.start_pts_tb = ms->audio_stream->time_base;
      }

      if ((ret = DecoderManager::decoder_start(&ms->audio_dec, audio_thread,
                                               "audio_decoder", this)) < 0) {
        goto out;
      }
      SDL_PauseAudioDevice(audio_deviceId_, 0);
      break;
    case AVMEDIA_TYPE_VIDEO:
      ms->video_index = stream_index;
      ms->video_stream = ic->streams[stream_index];
      if ((ret = DecoderManager::decoder_init(&ms->video_dec, avctx,
                                              &ms->video_queue,
                                              ms->continue_read_thread)) < 0) {
        goto fail;
      }
      if ((ret = DecoderManager::decoder_start(&ms->video_dec, video_thread,
                                               "video_decoder", this)) < 0) {
        goto out;
      }
      break;
    case AVMEDIA_TYPE_SUBTITLE:
      ms->subtitle_index = stream_index;
      ms->subtitle_stream = ic->streams[stream_index];
      if ((ret = DecoderManager::decoder_init(&ms->sub_dec, avctx,
                                              &ms->subtitle_queue,
                                              ms->continue_read_thread)) < 0) {
        goto fail;
      }
      if ((ret = DecoderManager::decoder_start(&ms->sub_dec, subtitle_thread,
                                               "subtitle_decoder", this)) < 0) {
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
  MediaState* ms = (MediaState*)arg;
  return ms->abort_request;
}

// AV_DISPOSITION_ATTACHED_PIC 所表示的意思，可查看下方文档
// https://segmentfault.com/a/1190000018373504
// 特殊的video_stream，如果一个流中含有这个标志的话，那么就是说这个流是 *.mp3 文件中的一个 Video Stream 。
// 并且该流只有一个 AVPacket ，也就是 attached_pic 。这个 AVPacket 中所存储的内容就是这个 *.mp3 文件的封面图片。
int SimplePlayer::stream_has_enough_packets(AVStream* stream, int stream_id,
                                            PacketQueue* p_queue) {
  return stream_id < 0 || p_queue->abort_request ||
         (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
         p_queue->nb_packets > MIN_FRAMES &&
             (!p_queue->duration ||
              av_q2d(stream->time_base) * p_queue->duration > 1.0);
}

int SimplePlayer::is_realtime(AVFormatContext* fmt_ctx) {
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
  SimplePlayer* player = (SimplePlayer*)arg;
  return player->demuxing(player->getMediaState());
}

int SimplePlayer::demuxing(MediaState* ms) {
  AVFormatContext* ic = NULL;
  int err, ret;
  int stream_index[AVMEDIA_TYPE_NB];
  int64_t stream_start_time;
  int pkt_in_play_range = 0;
  const AVDictionaryEntry* t;
  int64_t pkt_ts;
  AVPacket* pkt = NULL;

  SDL_mutex* wait_mutex = SDL_CreateMutex();
  if (!wait_mutex) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
    ret = AVERROR(ENOMEM);
    goto fail;
  }

  memset(stream_index, -1, sizeof(stream_index));
  ms->eof = 0;

  pkt = av_packet_alloc();
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
  if (seek_by_bytes_ < 0)
    seek_by_bytes_ = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
                     !!(ic->iformat->flags & AVFMT_TS_DISCONT) &&
                     strcmp("ogg", ic->iformat->name);

  ms->max_frame_duration =
      (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

  // 与 sprintf() 相比, av_asprintf() 的优势在于它能够自动分配足够的内存空间, 避免了缓冲区溢出的风险。同时, 返回的是一个动态分配的字符串指针, 使用更加灵活。
  // 动态内存分配和字符串构建函数，在格式化字符串的同时，分配内存，返回字符串指针
  if (!window_title_ && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
    window_title_ = av_asprintf("%s - %s", t->value, input_filename_);

  /* if seeking requested, we execute it */
  if (start_time_ != AV_NOPTS_VALUE) {
    int64_t timestamp;
    timestamp = start_time_;
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

  if (show_status_) {
    av_dump_format(ic, 0, ms->filename, 0);
  }

  // 获取流索引之前，先进行遍历，看播放前有没有选择想要播放对应索引的数据流，没有选择的话，wanted_stream_spec[type]为false，即无效
  // 如果没有设置选择想要播放的流索引的话，这块可以去掉处理，抄写时说明下把它注释掉
  for (int i = 0; i < ic->nb_streams; i++) {
    AVStream* st = ic->streams[i];
    enum AVMediaType type = st->codecpar->codec_type;
    st->discard = AVDISCARD_ALL;
    if (type >= 0 && wanted_stream_spec_[type] && stream_index[type] == -1)
      if (avformat_match_stream_specifier(ic, st, wanted_stream_spec_[type]) >
          0)
        stream_index[type] = i;
  }
  for (int i = 0; i < AVMEDIA_TYPE_NB; i++) {
    if (wanted_stream_spec_[i] && stream_index[i] == -1) {
      av_log(NULL, AV_LOG_ERROR,
             "Stream specifier %s does not match any %s stream\n",
             wanted_stream_spec_[i], av_get_media_type_string((AVMediaType)i));
      stream_index[i] = INT_MAX;
    }
  }

  if (!video_disable_)
    stream_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(
        ic, AVMEDIA_TYPE_VIDEO, stream_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
  if (!audio_disable_)
    stream_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(
        ic, AVMEDIA_TYPE_AUDIO, stream_index[AVMEDIA_TYPE_AUDIO],
        stream_index[AVMEDIA_TYPE_VIDEO], NULL, 0);
  if (!video_disable_ && !subtitle_disable_)
    stream_index[AVMEDIA_TYPE_SUBTITLE] = av_find_best_stream(
        ic, AVMEDIA_TYPE_SUBTITLE, stream_index[AVMEDIA_TYPE_SUBTITLE],
        (stream_index[AVMEDIA_TYPE_AUDIO] >= 0
             ? stream_index[AVMEDIA_TYPE_AUDIO]
             : stream_index[AVMEDIA_TYPE_VIDEO]),
        NULL, 0);

  ms->show_mode = show_mode_;
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
  if (infinite_buffer_ < 0 && ms->realtime) {
    infinite_buffer_ = 1;
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
        if (ms->audio_index >= 0)
          PacketQueueManager::packet_queue_flush(&ms->audio_queue);
        if (ms->subtitle_index >= 0)
          PacketQueueManager::packet_queue_flush(&ms->subtitle_queue);
        if (ms->video_index >= 0)
          PacketQueueManager::packet_queue_flush(&ms->video_queue);
        if (ms->seek_flags & AVSEEK_FLAG_BYTE)
          ClockManager::set_clock(&ms->external_clk, NAN, 0);
        else
          ClockManager::set_clock(&ms->external_clk,
                                  seek_target / (double)AV_TIME_BASE, 0);
      }
      ms->seek_req = 0;
      ms->eof = 0;
      if (ms->paused) {
        step_to_next_frame(ms);
      }
    }

    /* if the queue are full, no need to read more */
    /* 限制从流获取packet数据，实时流的话就不限制 */
    if (infinite_buffer_ < 1 &&
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
          FrameQueueManager::frame_queue_nb_remaining(&ms->sample_queue) ==
              0)) &&
        (!ms->video_stream ||
         (ms->video_dec.finished == ms->video_queue.serial &&
          FrameQueueManager::frame_queue_nb_remaining(&ms->pic_queue) == 0))) {
      if (loop_ != 1 && (!loop_ || --loop_)) {
        stream_seek(ms, start_time_ != AV_NOPTS_VALUE ? start_time_ : 0, 0, 0);
      } else if (autoexit_) {
        ret = AVERROR_EOF;
        goto fail;
      }
    }

    ret = av_read_frame(ic, pkt);
    if (ret < 0) {
      // 检查输入流是否到达文件末尾，到文件末尾，推入空包
      if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !ms->eof) {
        if (ms->video_index >= 0)
          PacketQueueManager::packet_queue_put_nullpacket(&ms->video_queue, pkt,
                                                          ms->video_index);
        if (ms->audio_index >= 0)
          PacketQueueManager::packet_queue_put_nullpacket(&ms->audio_queue, pkt,
                                                          ms->audio_index);
        if (ms->subtitle_index >= 0)
          PacketQueueManager::packet_queue_put_nullpacket(
              &ms->subtitle_queue, pkt, ms->subtitle_index);
        ms->eof = 1;
      }
      if (ic->pb && ic->pb->error) {
        if (autoexit_)
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
        duration_ == AV_NOPTS_VALUE ||
        (pkt_ts -
         (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                    av_q2d(ic->streams[pkt->stream_index]->time_base) -
                (double)(start_time_ != AV_NOPTS_VALUE ? start_time_ : 0) /
                    1000000 <=
            ((double)duration_ / 1000000);
    if (pkt->stream_index == ms->audio_index && pkt_in_play_range) {
      PacketQueueManager::packet_queue_put(&ms->audio_queue, pkt);
    } else if (pkt->stream_index == ms->video_index && pkt_in_play_range &&
               !(ms->video_stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
      PacketQueueManager::packet_queue_put(&ms->video_queue, pkt);
    } else if (pkt->stream_index == ms->subtitle_index && pkt_in_play_range) {
      PacketQueueManager::packet_queue_put(&ms->subtitle_queue, pkt);
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

MediaState* SimplePlayer::stream_open(const char* filename,
                                      const AVInputFormat* iformat) {
  MediaState* ms = NULL;

  ms = (MediaState*)av_mallocz(sizeof(MediaState));
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
  if (FrameQueueManager::frame_queue_init(&ms->pic_queue, &ms->video_queue,
                                          VIDEO_PICTURE_QUEUE_SIZE, 1) < 0 ||
      FrameQueueManager::frame_queue_init(&ms->sub_queue, &ms->subtitle_queue,
                                          SUBPICTURE_QUEUE_SIZE, 0) < 0 ||
      FrameQueueManager::frame_queue_init(&ms->sample_queue, &ms->audio_queue,
                                          SAMPLE_QUEUE_SIZE, 1) < 0) {
    stream_close(ms);
    return NULL;
  }

  if (PacketQueueManager::packet_queue_init(&ms->video_queue) < 0 ||
      PacketQueueManager::packet_queue_init(&ms->audio_queue) < 0 ||
      PacketQueueManager::packet_queue_init(&ms->subtitle_queue) < 0) {
    stream_close(ms);
    return NULL;
  }

  if (!(ms->continue_read_thread = SDL_CreateCond())) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
    stream_close(ms);
    return NULL;
  }

  ClockManager::init_clock(&ms->video_clk, &ms->video_queue.serial);
  ClockManager::init_clock(&ms->audio_clk, &ms->audio_queue.serial);
  ClockManager::init_clock(&ms->external_clk, &ms->external_clk.serial);
  ms->audio_clock_serial = -1;

  // 设置起始播放的音量
  if (startup_volume_ < 0) {
    av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n",
           startup_volume_);
  }
  if (startup_volume_ > 100) {
    av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n",
           startup_volume_);
  }
  startup_volume_ = av_clip(startup_volume_, 0, 100);
  startup_volume_ =
      av_clip(SDL_MIX_MAXVOLUME * startup_volume_ / 100, 0, SDL_MIX_MAXVOLUME);
  ms->audio_volume = startup_volume_;
  ms->muted = 0;
  ms->av_sync_type = av_sync_type_;
  ms->read_tid = SDL_CreateThread(read_thread, "read_thread", this);
  if (!ms->read_tid) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
    stream_close(ms);
    return NULL;
  }
  return ms;
}

// 用于处理视频刷新循环中的事件等待，负责控制视频帧的显示时机,确保视频与音频的同步,并处理各种用户输入和系统事件
void SimplePlayer::refresh_loop_wait_event(MediaState* ms, SDL_Event* event) {
  double remaining_time = 0.0;
  // SDL_PumpEvents作用是获取并处理系统中发生的所有事件。
  SDL_PumpEvents();
  // SDL_PeepEvents 用于从 SDL 的内部事件队列中获取、查看或删除事件
  // 成功读取到事件,循环会退出
  while (
      !SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
    if (!cursor_hidden_ &&
        av_gettime_relative() - cursor_last_shown_ > CURSOR_HIDE_DELAY) {
      SDL_ShowCursor(0);
      cursor_hidden_ = 1;
    }
    if (remaining_time > 0.0) {
      av_usleep((int64_t)(remaining_time * 1000000.0));
    }
    remaining_time = REFRESH_RATE;
    if (ms->show_mode != MediaState::ShowMode::SHOW_MODE_NONE &&
        (!ms->paused || ms->force_refresh))
      video_refresh(ms, &remaining_time);
    SDL_PumpEvents();
  }
}

/* ============= about Media Operation ============= */
/* seek in the stream */
void SimplePlayer::stream_seek(MediaState* ms, int64_t pos, int64_t rel,
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
void SimplePlayer::stream_toggle_pause(MediaState* ms) {
  if (ms->paused) {
    ms->frame_timer +=
        av_gettime_relative() / 1000000.0 - ms->video_clk.last_updated;
    if (ms->read_pause_return != AVERROR(ENOSYS)) {
      ms->video_clk.paused = 0;
    }
    ClockManager::set_clock(&ms->video_clk,
                            ClockManager::get_clock(&ms->video_clk),
                            ms->video_clk.serial);
  }
  ClockManager::set_clock(&ms->external_clk,
                          ClockManager::get_clock(&ms->external_clk),
                          ms->external_clk.serial);
  // 将 paused 标志应用到所有相关时钟
  ms->paused = !ms->paused;
  ms->audio_clk.paused = !ms->paused;
  ms->video_clk.paused = !ms->paused;
  ms->external_clk.paused = !ms->paused;
}

void SimplePlayer::toggle_pause(MediaState* ms) {
  stream_toggle_pause(ms);
  ms->step = 0;
}

void SimplePlayer::toggle_mute(MediaState* ms) { ms->muted = !ms->muted; }

void SimplePlayer::update_volume(MediaState* ms, int sign, double step) {
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

void SimplePlayer::step_to_next_frame(MediaState* ms) {
  /* if the stream is paused unpause it, then step */
  if (ms->paused) {
    stream_toggle_pause(ms);
  }
  ms->step = 1;
}

void SimplePlayer::toggle_full_screen(MediaState* ms) {
  is_full_screen_ = !is_full_screen_;
  SDL_SetWindowFullscreen(window_,
                          is_full_screen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}
/* ============= about Media Operation ============= */

/* handle an event sent by the GUI */
void SimplePlayer::event_loop(MediaState* ms) {
  SDL_Event event;
  double increase, pos;

  while (1) {
    refresh_loop_wait_event(ms, &event);
    switch (event.type) {
      case SDL_KEYDOWN:
        if (exit_on_keydown_ || event.key.keysym.sym == SDLK_ESCAPE ||
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
            if (seek_by_bytes_) {
              pos = -1;
              if (pos < 0 && ms->video_index >= 0)
                pos = FrameQueueManager::frame_queue_last_pos(&ms->pic_queue);
              if (pos < 0 && ms->audio_index >= 0)
                pos =
                    FrameQueueManager::frame_queue_last_pos(&ms->sample_queue);
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
              pos = ClockManager::get_master_clock(ms);
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
        if (exit_on_mousedown_) {
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
        if (cursor_hidden_) {
          SDL_ShowCursor(1);
          cursor_hidden_ = 0;
        }
        cursor_last_shown_ = av_gettime_relative();
        break;
      case SDL_WINDOWEVENT:
        switch (event.window.event) {
          case SDL_WINDOWEVENT_SIZE_CHANGED:
            screen_width_ = ms->width = event.window.data1;
            screen_height_ = ms->height = event.window.data2;
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
