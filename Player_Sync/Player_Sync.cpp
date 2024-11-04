// 同步处理：视频同步控制
#include <SDL.h>
#include <math.h>

#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)
#define AV_SYNC_THRESHOLD 0.01  // 前后两帧间的显示时间间隔的最小值0.01s
#define AV_NOSYNC_THRESHOLD 10.0  // 最小刷新间隔时间10ms

// this value should be greater than the value of len
// sample_rate(48000) * channels(2) * (AUDIO_S16SYS)16bits
const int MAX_AUDIO_FRAME_SIZE = 192000;
const int MAX_AUDIO_QUEUE_SIZE = 5 * 16 * 1024;
const int MAX_VIDEO_QUEUE_SIZE = 5 * 256 * 1024;
const int VIDEO_PICTURE_QUEUE_SIZE = 1;

typedef struct PacketQueue {
  AVPacketList* first_pkt = NULL;
  AVPacketList* last_pkt = NULL;
  int nb_packets;
  int size;
  SDL_mutex* mutex;  // 队列互斥量，保护队列数据
  SDL_cond* cond;    // 队列就绪条件变量
} PacketQueue;

typedef struct VideoPicture {
  AVFrame* pic_frame;
  int width;
  int height;
  int allocated;  // 是否分配内存空间
  double pts;     // 当前图像帧的绝对显示时间戳
} VideoPicture;

typedef struct MediaState {
  // for Multi-media file
  char filename[1024];
  AVFormatContext* fmt_ctx;
  int video_index;
  int audio_index;

  // for audio
  AVStream* audio_stream;
  AVCodecContext* audio_codec_ctx;
  PacketQueue audio_queue;
  uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
  unsigned int audio_buf_size;
  unsigned int audio_buf_index;
  AVFrame* audio_frame;
  AVPacket* audio_packet;
  uint8_t* audio_packet_data;
  int audio_packet_size;
  struct SwrContext* audio_swr_ctx;

  // for video
  AVStream* video_stream;
  AVCodecContext* video_codec_ctx;
  PacketQueue video_queue;
  struct SwsContext* sws_ctx;
  VideoPicture picture_queue[VIDEO_PICTURE_QUEUE_SIZE];
  int picture_queue_size;
  int picture_queue_rindex;
  int picture_queue_windex;

  // for sync
  // pts of last decoded frame / predicted pts of next decoded frame
  double audio_clock;
  double video_clock;
  // 视频播放到当前帧时的累计已播放时间
  double frame_timer;
  // 上一帧图像的显示时间戳，用于在video_refersh_timer中保存上一帧的pts值
  double frame_last_pts;
  // 上一帧图像的动态刷新延迟时间
  double frame_last_delay;

  // for thread
  SDL_mutex* picture_queue_mutex;
  SDL_cond* picture_queue_cond;
  SDL_Thread* parse_tid;
  SDL_Thread* video_tid;

  int quit;
} MediaState;

// global parameters
SDL_Window* window = NULL;
SDL_Renderer* renderer = NULL;
SDL_Texture* texture = NULL;
SDL_mutex* texture_mutex = NULL;

static int screen_width = 0;
static int screen_height = 0;
static int resize = 1;
MediaState* global_media_state = NULL;

void Packet_Queue_Init(PacketQueue* packet_queue) {
  std::cout << "==========" << __FUNCTION__ "==========" << std::endl;
  memset(packet_queue, 0, sizeof(PacketQueue));
  packet_queue->mutex = SDL_CreateMutex();
  packet_queue->cond = SDL_CreateCond();
}

// Put the packet to PacketQueue
int Packet_Queue_Put(PacketQueue* packet_queue, AVPacket* packet) {
  AVPacketList* packet_list = NULL;
  if (av_packet_make_refcounted(packet) < 0) {
    return -1;
  }
  packet_list = (AVPacketList*)av_malloc(sizeof(AVPacketList));
  if (!packet_list) {
    return -1;
  }
  packet_list->pkt = *packet;
  packet_list->next = NULL;

  SDL_LockMutex(packet_queue->mutex);

  if (!packet_queue->last_pkt) {
    packet_queue->first_pkt = packet_list;
  } else {
    packet_queue->last_pkt->next = packet_list;
  }

  packet_queue->last_pkt = packet_list;
  packet_queue->nb_packets++;
  packet_queue->size += packet_list->pkt.size;
  SDL_CondSignal(packet_queue->cond);

  SDL_UnlockMutex(packet_queue->mutex);
  return 0;
}

// Get the packet from PacketQueue
int Packet_Queue_Get(PacketQueue* packet_queue, AVPacket* packet, int block) {
  AVPacketList* packet_list = NULL;
  int ret = 0;

  SDL_LockMutex(packet_queue->mutex);

  while (1) {
    if (global_media_state->quit) {
      std::cout << __FUNCTION__ << " quit from queue get..." << std::endl;
      ret = -1;
      break;
    }

    packet_list = packet_queue->first_pkt;
    if (packet_list) {
      packet_queue->first_pkt = packet_list->next;
      if (!packet_queue->first_pkt) {
        packet_queue->last_pkt = NULL;
      }
      packet_queue->nb_packets--;
      packet_queue->size -= packet_list->pkt.size;
      *packet = packet_list->pkt;
      av_free(packet_list);
      ret = 1;
      break;
    } else if (!block) {
      ret = 0;
      break;
    } else {
      std::cout << __FUNCTION__ << " queue is empty, so wait a cond signal..."
                << std::endl;
      SDL_CondWait(packet_queue->cond, packet_queue->mutex);
    }
  }
  SDL_UnlockMutex(packet_queue->mutex);
  return ret;
}

/*-----------取得音频时钟----------
------取得当前播放音频数据的pts，以音频时钟作为音视频同步基准------
* 音视频同步的原理是根据音频的pts来控制视频的播放
* 也就是说在视频解码一帧后，是否显示以及显示多长时间，是通过该帧的PTS与同时正在播放的音频的PTS比较而来的
* 如果音频的PTS较大，则视频准备完毕立即刷新，否则等待
* 因为pcm数据采用audio_callback回调方式进行播放
* 对于音频播放我们只能得到写入回调函数前缓存音频帧的pts，而无法得到当前播放帧的pts(需要采用当前播放音频帧的pts作为参考时钟)
* 考虑到音频的大小与播放时间成正比(相同采样率)，那么当前时刻正在播放的音频帧pts(位于回调函数缓存中)
* 就可以根据已送入声卡的pcm数据长度、缓存中剩余pcm数据长度，缓存长度及采样率进行推算了
--------------------------------*/
double get_audio_clock(MediaState* media_state) {
  double pts;
  int hw_buf_size = 0;
  int bytes_per_second = 0;

  // maintained in the audio thread
  pts = media_state->audio_clock;
  // 计算当前音频解码数据缓存索引位置
  hw_buf_size = media_state->audio_buf_size - media_state->audio_buf_index;
  if (media_state->audio_stream) {
    // 计算每秒的原始音频字节数
    // 采样率*声道数*每声道数据字节数（AV_SAMPLE_FMT_S16 -- 2个字节）
    bytes_per_second = media_state->audio_codec_ctx->sample_rate *
                       media_state->audio_codec_ctx->ch_layout.nb_channels * 2;
  }
  if (bytes_per_second) {
    // 根据送入声卡缓存的索引位置，往前倒推计算当前时刻的音频播放时间戳pts
    pts -= (double)hw_buf_size / bytes_per_second;
  }
  // 返回当前正在播放的音频时间戳
  return pts;
}

int audio_decode_frame(MediaState* audio_state, uint8_t* audio_buf,
                       int buf_size, double* pts_ptr) {
  int ret = 0;
  int data_size = 0;
  AVPacket* packet = audio_state->audio_packet;
  audio_state->audio_frame = av_frame_alloc();
  double pts;  // 音频播放时间戳

  while (1) {
    if (Packet_Queue_Get(&audio_state->audio_queue, packet, 1) < 0) {
      return -1;
    }
    if (packet && packet->data) {
      audio_state->audio_packet_data = packet->data;
      audio_state->audio_packet_size = packet->size;
    }
    // if update, update the audio clock w/pts
    // 检查音频播放时间戳
    if (packet->pts != AV_NOPTS_VALUE) {
      // 获得一个新的packet的时候，更新audio_clock，用packet中的pts更新audio_clock(一个pkt对应一个pts)
      // 更新音频已经播的时间
      audio_state->audio_clock =
          av_q2d(audio_state->audio_stream->time_base) * packet->pts;
    }
    // decode audio pkt
    while (audio_state->audio_packet_size > 0) {
      ret = avcodec_send_packet(audio_state->audio_codec_ctx, packet);
      if (ret < 0) {
        std::cout << "Error submitting the packet to the decoder" << std::endl;
        audio_state->audio_packet_size = 0;
        break;
      }
      if (packet && packet->buf) {
        av_packet_unref(packet);
      }
      while (ret >= 0) {
        ret = avcodec_receive_frame(audio_state->audio_codec_ctx,
                                    audio_state->audio_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
          break;
        } else if (ret < 0) {
          std::cout << "Error decoding audio..." << std::endl;
          break;
        }

        int out_count = av_samples_get_buffer_size(
            NULL, audio_state->audio_codec_ctx->ch_layout.nb_channels,
            audio_state->audio_frame->nb_samples, AV_SAMPLE_FMT_S16, 1);
        uint8_t* out = (uint8_t*)av_malloc(out_count);

        // 这里是将输入的格式 audio_codec_ctx->sample_fmt 转换为 AV_SAMPLE_FMT_S16 格式输出
        // 我这里使用的文件的格式 audio_codec_ctx->sample_fmt 是 fltp 浮点型，32bits
        int nb_samples =
            swr_convert(audio_state->audio_swr_ctx, &out, out_count,
                        (const uint8_t**)audio_state->audio_frame->data,
                        audio_state->audio_frame->nb_samples);
        if (nb_samples < 0) {
          av_free(out);
          return -1;
        }
        data_size = nb_samples * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16) *
                    audio_state->audio_codec_ctx->ch_layout.nb_channels;
        std::cout << "out_count: " << out_count << " nb_samples: " << nb_samples
                  << " per_sample: "
                  << av_get_bytes_per_sample(AV_SAMPLE_FMT_S16)
                  << " audio_size: " << data_size << std::endl;

        if (data_size <= buf_size) {
          memcpy(audio_buf, out, data_size);
        } else {
          av_free(out);
          return -1;
        }
        av_free(out);
        audio_state->audio_packet_data += audio_state->audio_frame->pkt_size;
        audio_state->audio_packet_size -= audio_state->audio_frame->pkt_size;
        if (data_size <= 0) {
          continue;
        }
        // 用每次更新的音频播放时间更新音频PTS
        pts = audio_state->audio_clock;
        *pts_ptr = pts;
        /*---------------------
        * 当一个packet中包含多个音频帧时
        * 通过[解码后音频原始数据长度]及[采样率]来推算一个packet中其他音频帧的播放时间戳pts
        * 采样频率44.1kHz，量化位数16位，意味着每秒采集数据44.1k个，每个数据占2字节
        --------------------*/
        //计算每组音频采样数据的字节数=每个声道音频采样字节数*声道数
        /*pcm_bytes =2 *audio_state->audio_codec_ctx->ch_layout.nb_channels;  */
        /*----更新audio_clock---
         * 一个pkt包含多个音频frame，同时一个pkt对应一个pts(pkt->pts)
         * 因此，该pkt中包含的多个音频帧的时间戳由以下公式推断得出
         * bytes_per_sec = pcm_bytes * audio_state->audio_codec_ctx->sample_rate
         * 从pkt中不断的解码，推断(一个pkt中)每帧数据的pts并累加到音频播放时钟
        --------------------*/
        audio_state->audio_clock +=
            (double)data_size /
            (double)(2 * audio_state->audio_codec_ctx->ch_layout.nb_channels *
                     audio_state->audio_codec_ctx->sample_rate);
        return data_size;
      }
      audio_state->audio_packet_size = 0;
    }
    if (packet && packet->buf) {
      av_packet_unref(packet);
    }
    if (audio_state->quit) {
      std::cout << __FUNCTION__ << " will quit program..." << std::endl;
      return -1;
    }
  }
}

/*
 音频输出回调函数，sdl通过该回调函数将解码后的pcm数据送入声卡播放,
 sdl通常一次会准备一组缓存pcm数据，通过该回调送入声卡，声卡根据音频pts依次播放pcm数据
 待送入缓存的pcm数据完成播放后，再载入一组新的pcm缓存数据(每次音频输出缓存为空时，sdl就调用此函数填充音频输出缓存，并送入声卡播放)
*/
void audio_callback(void* userdata, Uint8* stream, int len) {
  MediaState* audio_state = (MediaState*)userdata;
  int real_len, audio_size;  // 每次写入stream的数据长度，解码后的数据长度
  double pts;

  SDL_memset(stream, 0, len);
  while (len > 0) {
    if (audio_state->audio_buf_index >= audio_state->audio_buf_size) {
      // already sent all data, get more. 解码音频帧的数据
      audio_size = audio_decode_frame(audio_state, audio_state->audio_buf,
                                      sizeof(audio_state->audio_buf), &pts);
      if (audio_size < 0) {
        // If error, output silence. 解码失败或者没有解码到音频数据，就输出一段静音的内容
        audio_state->audio_buf_size = 1024 * 2 * 2;
        memset(audio_state->audio_buf, 0, audio_state->audio_buf_size);
      } else {
        audio_state->audio_buf_size = audio_size;
      }
      audio_state->audio_buf_index = 0;
    }
    // 这里audio_buf_size 大于等于 audio_buf_index
    real_len = audio_state->audio_buf_size - audio_state->audio_buf_index;
    if (real_len > len) {
      real_len = len;
    }
    std::cout << __FUNCTION__ << "  index: " << audio_state->audio_buf_index
              << " real_len: " << real_len << " len: " << len << std::endl;
    // 这个可以不进行清零处理
    /*memcpy(stream, audio_state->audio_buf + audio_state->audio_buf_index,
           real_len);*/
    // 使用这个SDL_MixAudio，注意下，在前面需要进行清零处理，否则播放的音频有问题
    //SDL_MixAudio(stream, audio_state->audio_buf, real_len, SDL_MIX_MAXVOLUME);
    SDL_MixAudio(stream, audio_state->audio_buf + audio_state->audio_buf_index,
                 real_len, SDL_MIX_MAXVOLUME);
    len -= real_len;
    stream += real_len;
    audio_state->audio_buf_index += real_len;
  }
}

// 定时器触发的回调函数
static Uint32 sdl_refresh_timer_cb(Uint32 interval, void* opaque) {
  SDL_Event event;
  event.type = FF_REFRESH_EVENT;
  event.user.data1 = opaque;
  SDL_PushEvent(&event);
  return 0;  // 0 means stop timer
}

/* schedule a video refresh in 'delay' ms */
//delay用于在图像帧的解码顺序与渲染顺序不一致的情况下，调节下一帧的渲染时机，
//从而尽可能的使所有图像帧按照固定的帧率渲染刷新
static void schedule_refresh(MediaState* media_state, int delay) {
  /*
  添加一个定时器，该定时器每delay毫秒触发一次 sdl_refresh_timer_cb
  函数。在 sdl_refresh_timer_cb 函数中，我们通过返回
  interval 来重新设置定时器，从而实现周期性触发。收到返回0时停止定时器
  */
  SDL_AddTimer(delay, sdl_refresh_timer_cb, media_state);
}

// 视频帧渲染
void video_display(MediaState* video_state) {
  SDL_Rect rect;
  VideoPicture* video_picture = NULL;

  if (screen_width && resize) {
    SDL_SetWindowSize(window, screen_width / 2, screen_height / 2);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED,
                          SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    Uint32 pixformat = SDL_PIXELFORMAT_IYUV;
    texture =
        SDL_CreateTexture(renderer, pixformat, SDL_TEXTUREACCESS_STREAMING,
                          screen_width, screen_height);
    if (!texture) {
      std::cout << "failed to create texture by SDL..." << std::endl;
      return;
    }
    resize = 0;
  }
  // 从图像帧队列（数组）中获取图像帧
  video_picture =
      &video_state->picture_queue[video_state->picture_queue_rindex];
  if (video_picture->pic_frame) {
    SDL_UpdateYUVTexture(texture, NULL, video_picture->pic_frame->data[0],
                         video_picture->pic_frame->linesize[0],
                         video_picture->pic_frame->data[1],
                         video_picture->pic_frame->linesize[1],
                         video_picture->pic_frame->data[2],
                         video_picture->pic_frame->linesize[2]);

    rect.x = 0;
    rect.y = 0;
    rect.w = video_state->video_codec_ctx->width / 2;
    rect.h = video_state->video_codec_ctx->height / 2;

    SDL_LockMutex(texture_mutex);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, &rect);
    SDL_RenderPresent(renderer);
    SDL_UnlockMutex(texture_mutex);
  }
}

/*---------------------------
* 显示刷新函数(FF_REFRESH_EVENT事件响应函数)
* 将视频同步到音频上(声音是连续播放的，因此用画面去同步声音)，计算下一帧的延迟时间
* 使用当前帧的PTS和上一帧的PTS差来估计播放下一帧的延迟时间，并根据video的播放速度来调整这个延迟时间
---------------------------*/
void video_refresh_timer(void* userdata) {
  MediaState* video_state = (MediaState*)userdata;
  VideoPicture* video_picture = NULL;
  // 当前帧与下一帧的显示时间间隔(动态时间、真实时间、绝对时间)
  double actual_delay;
  double delay;           // 前后帧间的显示时间间隔
  double sync_threshold;  // 前后帧间的最小时间差
  double ref_clock;       // 音频时间戳，作为视频同步的参考时间
  double diff;            // 图像帧显示与音频帧播放间的时间差

  if (video_state->video_stream) {
    // 检查图像帧队列是否有待显示图像
    if (video_state->picture_queue_size == 0) {
      schedule_refresh(video_state, 1);
    } else {
      // 从显示队列中取得等待显示的图像帧
      video_picture =
          &video_state->picture_queue[video_state->picture_queue_rindex];

      // the pts from last time
      // 计算当前帧和前一帧显示(pts)的间隔时间(显示时间戳的差值)
      delay = video_picture->pts - video_state->frame_last_pts;
      if (delay <= 0 || delay >= 1.0) {
        // if incorrect delay, use previous one
        // 沿用之前的动态刷新间隔时间
        delay = video_state->frame_last_delay;
      }
      // save for next time
      // 保存上一帧图像的动态刷新延迟时间
      video_state->frame_last_delay = delay;
      // 保存上一帧图像的显示时间戳
      video_state->frame_last_pts = video_picture->pts;

      // update delay to sync to audio
      ref_clock = get_audio_clock(video_state);
      // 就是说在diff这段时间中声音是匀速发生的，但是在delay这段时间frame的显示可能就会有快慢的区别
      // 计算图像帧显示与音频帧播放间的时间差
      diff = video_picture->pts - ref_clock;

      // Skip or repeat the frame. Take delay into account
      /*FFPlay still doesn't "know if this is the best guess.*/
      // 根据时间差调整播放下一帧的延迟时间，以实现同步
      // 比较前后两帧间的显示时间间隔与最小时间间隔
      sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;

      // 判断音视频不同步条件，即音视频间的时间差 & 前后帧间的时间差<10ms阈值，若>该阈值则为快进模式，不存在音视频同步问题
      if (fabs(diff) < AV_NOSYNC_THRESHOLD) {
        if (diff <= -sync_threshold) {
          // 比较前一帧&当前帧[画面-声音]间的时间间隔与前一帧 & 当前帧[画面-画面]间的时间间隔，慢了，delay设为0
          // 下一帧画面显示的时间和当前的声音很近的话加快显示下一帧，即后面video_display显示完当前帧后开启定时器很快去显示下一帧
          delay = 0;
        } else if (diff >= sync_threshold) {
          // 比较两帧画面间的显示时间与两帧画面间声音的播放时间，快了，加倍delay
          delay = 2 * delay;
        }
      }
      // 如果diff(明显)大于AV_NOSYNC_THRESHOLD，即快进的模式了，画面跳动太大，不存在音视频同步的问题了

      // 更新视频播放到当前帧时的已播放时间值(所有图像帧动态播放累计时间值-真实值)，
      // frame_timer一直累加在播放过程中我们计算的延时
      video_state->frame_timer += delay;
      // compute the real delay
      // 每次计算frame_timer与系统时间的差值(以系统时间为基准时间)，将frame_timer与系统时间(绝对时间)相关联的目的
      actual_delay = video_state->frame_timer - (av_gettime() / 1000000.0);
      if (actual_delay < 0.010) {
        // Really it should skip the picture instead
        actual_delay = 0.010;
      }

      // 用绝对时间开定时器去动态显示刷新下一帧
      schedule_refresh(video_state, (int)(actual_delay * 1000 + 0.5));
      // show the pic_frame
      video_display(video_state);

      /*update queue for next frame*/
      // 更新并检查图像帧队列读位置索引
      if (++video_state->picture_queue_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
        video_state->picture_queue_rindex = 0;  // 重置读位置索引
      }

      SDL_LockMutex(video_state->picture_queue_mutex);
      video_state->picture_queue_size--;
      SDL_CondSignal(video_state->picture_queue_cond);
      SDL_UnlockMutex(video_state->picture_queue_mutex);
    }
  } else {
    schedule_refresh(video_state, 100);
  }
}

// 创建 or 重置图像帧，为图像帧分配内存空间
void alloc_picture(void* userdata) {
  MediaState* video_state = (MediaState*)userdata;
  VideoPicture* video_picture = NULL;

  video_picture =
      &video_state->picture_queue[video_state->picture_queue_windex];
  if (video_picture->pic_frame) {
    av_frame_free(&(video_picture->pic_frame));
  }

  SDL_LockMutex(texture_mutex);
  // 1. 分配AVFrame结构
  video_picture->pic_frame = av_frame_alloc();
  // 2. 设置帧参数
  if (video_picture->pic_frame) {
    video_picture->pic_frame->format = AV_PIX_FMT_YUV420P;
    video_picture->pic_frame->width = video_state->video_codec_ctx->width;
    video_picture->pic_frame->height = video_state->video_codec_ctx->height;
  }
  // 3. 分配帧缓冲区
  if (av_frame_get_buffer(video_picture->pic_frame, 0) < 0) {
    av_frame_free(&(video_picture->pic_frame));
  }
  SDL_UnlockMutex(texture_mutex);

  video_picture->width = video_state->video_codec_ctx->width;
  video_picture->height = video_state->video_codec_ctx->height;
  video_picture->allocated = 1;
}

// 图像帧插入队列等待渲染
int queue_picture(MediaState* video_state, AVFrame* frame, double pts) {
  VideoPicture* video_picture = NULL;

  // 其实这里是在限制视频刷新的帧率
  /* wait until we have space for a new pic */
  SDL_LockMutex(video_state->picture_queue_mutex);
  while (video_state->picture_queue_size >= VIDEO_PICTURE_QUEUE_SIZE &&
         !video_state->quit) {
    // 播放一帧，减掉一帧后再push一帧，等待video_display显示完一帧后的信号
    SDL_CondWait(video_state->picture_queue_cond,
                 video_state->picture_queue_mutex);
  }
  SDL_UnlockMutex(video_state->picture_queue_mutex);

  if (video_state->quit) {
    std::cout << "quit from queue picture..." << std::endl;
    return -1;
  }

  // windex is set to 0 initially
  video_picture =
      &video_state->picture_queue[video_state->picture_queue_windex];

  /* allocate or resize the buffer! */
  if (!video_picture->pic_frame ||
      video_picture->width != video_state->video_codec_ctx->width ||
      video_picture->height != video_state->video_codec_ctx->height) {
    video_picture->allocated = 0;
    alloc_picture(video_state);
    if (video_state->quit) {
      std::cout << "quit from queue picture after..." << std::endl;
      return -1;
    }
  }

  /* We have a place to put our video_picture on the queue */
  if (video_picture->pic_frame) {
    video_picture->pts = pts;
    // convert the image into YUV format that SDL use.
    sws_scale(video_state->sws_ctx, (const uint8_t* const*)frame->data,
              frame->linesize, 0, video_state->video_codec_ctx->height,
              video_picture->pic_frame->data,
              video_picture->pic_frame->linesize);

    // 更新并检查当前图像帧队列写入位置
    /* now we inform our display thread that we have a pic ready */
    if (++video_state->picture_queue_windex == VIDEO_PICTURE_QUEUE_SIZE) {
      video_state->picture_queue_windex = 0;  // 重置图像帧队列写入位置
    }
    SDL_LockMutex(video_state->picture_queue_mutex);
    video_state->picture_queue_size++;
    SDL_UnlockMutex(video_state->picture_queue_mutex);
  }
  return 0;
}

/*---------------------------
* 更新内部视频播放计时器(记录视频已经播时间(video_clock)
* update the PTS to be in sync
---------------------------*/
double synchronize_video(MediaState* video_state, AVFrame* frame, double pts) {
  double frame_delay;
  /*----------检查显示时间戳是否有效----------*/
  if (pts != 0) {
    // if we have pts, set video clock to it
    // 用显示时间戳更新已播放时间
    video_state->video_clock = pts;
  } else {
    // if we aren't given a pts, set it to the clock
    // 若获取不到显示时间戳, 用已播放时间更新显示时间戳
    pts = video_state->video_clock;
  }
  /*----------更新视频已经播时间----------*/
  // update the video clock
  // 若该帧要重复显示(取决于repeat_pict)，则全局视频播放时序video_clock应加上重复显示的数量*帧率
  // 该帧显示完将要花费的时间
  frame_delay = av_q2d(video_state->video_codec_ctx->time_base);
  // if we are repeating a frame, adjust clock accordingly
  // 若存在重复帧，则在正常播放的前后两帧图像间安排渲染重复帧
  // 计算渲染重复帧的时值(类似于音符时值)
  frame_delay += frame->repeat_pict * (frame_delay * 0.5);
  // 更新视频播放时间
  video_state->video_clock += frame_delay;
  // 此时返回的值即为下一帧将要开始显示的时间戳
  return pts;
}

int video_thread(void* arg) {
  MediaState* media_state = (MediaState*)arg;
  AVFrame* frame = NULL;
  AVPacket* packet = NULL;
  double pts;
  int ret = 0;

  frame = av_frame_alloc();
  packet = av_packet_alloc();

  while (1) {
    if (Packet_Queue_Get(&media_state->video_queue, packet, 1) < 0) {
      break;
    }
    pts = 0;
    std::cout << "get video packet size: "
              << media_state->video_queue.nb_packets << std::endl;
    ret = avcodec_send_packet(media_state->video_codec_ctx, packet);
    if (ret < 0) {
      std::cout << "Error sending the packet to the decoder..." << std::endl;
      continue;
    }
    if (packet && packet->buf)
      av_packet_unref(packet);  // 释放packet中保存的编码数据
    while (ret >= 0) {
      ret = avcodec_receive_frame(media_state->video_codec_ctx, frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        break;
      } else if (ret < 0) {
        std::cout << "Error during decoding..." << std::endl;
        break;
      }
      /*fflush(stdout);*/

      if ((pts = frame->best_effort_timestamp) != AV_NOPTS_VALUE) {
      } else {
        pts = 0;
      }
      /*-------------------------
      * 在解码线程函数中计算当前图像帧的显示时间戳
      * 1、取得编码数据包中的图像帧显示序号PTS(int64_t),并暂时保存在pts(double)中
      * 2、根据PTS*time_base来计算当前桢在整个视频中的显示时间戳，即PTS*(1/framerate)
      * av_q2d把AVRatioal结构转换成double的函数，
      * 用于计算视频源每个图像帧显示的间隔时间(1/framerate),即返回(time_base->num/time_base->den)
      -------------------------*/
      //根据pts=PTS*time_base={numerator=1,denominator=25}计算当前桢在整个视频中的显示时间戳
      // time_base为AVRational有理数结构体{num=1,den=25}，记录了视频源每个图像帧显示的间隔时间
      pts *= av_q2d(media_state->video_stream->time_base);
      // 检查当前帧的显示时间戳pts并更新内部视频播放计时器(记录视频已经播时间(video_clock)）
      pts = synchronize_video(media_state, frame, pts);

      // 将图像帧添加到图像帧队列
      if (queue_picture(media_state, frame, pts) < 0) {
        break;
      }
    }
  }
  av_packet_free(&packet);
  av_frame_free(&frame);
  return 0;
}

// 根据指定类型打开流，找到对应的解码器、创建对应的音频配置、保存关键信息到MediaState、启动音频和视频线程
int stream_component_open(MediaState* media_state, int stream_index) {
  AVFormatContext* fmt_ctx = media_state->fmt_ctx;
  // for decode
  AVCodecContext* codec_ctx = NULL;
  const AVCodec* codec = NULL;

  if (stream_index < 0 || stream_index >= fmt_ctx->nb_streams) {
    return -1;
  }

  // codec_context for the stream
  codec =
      avcodec_find_decoder(fmt_ctx->streams[stream_index]->codecpar->codec_id);
  if (!codec) {
    std::cout << "Cannot supported the audio codec..." << std::endl;
    return -1;
  }

  codec_ctx = avcodec_alloc_context3(codec);
  if (avcodec_parameters_to_context(
          codec_ctx, fmt_ctx->streams[stream_index]->codecpar) < 0) {
    std::cout << "failed to copy the audio stream codecpar to codec context..."
              << std::endl;
    return -1;
  }

  // open the decoder, finish initialize
  if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
    std::cout << "failed to open decoder..." << std::endl;
    return -1;
  }

  if (codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
    // set audio settings from codec info
    SDL_AudioSpec wanted_spec;
    wanted_spec.freq = codec_ctx->sample_rate;                // 采样率
    wanted_spec.format = AUDIO_S16SYS;                        // 采样格式
    wanted_spec.channels = codec_ctx->ch_layout.nb_channels;  // 声道数
    wanted_spec.silence = 0;  // 无输出时是否静音
    // 默认每次读音频缓存的大小，推荐值为 512~8192，ffplay使用的是1024
    wanted_spec.samples = 1024;
    wanted_spec.callback = audio_callback;  // 读取音频数据的回调接口函数
    wanted_spec.userdata = media_state;  // 回调中传递的数据参数
    if (SDL_OpenAudio(&wanted_spec, NULL) < 0) {
      std::cout << "failed to open audio device: " << SDL_GetError()
                << std::endl;
      return -1;
    }
  }

  // 检查解码器类型
  switch (codec_ctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
      media_state->audio_index = stream_index;
      media_state->audio_stream = fmt_ctx->streams[stream_index];
      media_state->audio_codec_ctx = codec_ctx;
      media_state->audio_buf_size = 0;
      media_state->audio_buf_index = 0;
      media_state->audio_packet = av_packet_alloc();
      memset(media_state->audio_packet, 0, sizeof(AVPacket));
      Packet_Queue_Init(&media_state->audio_queue);

      AVChannelLayout in_channel_layout;
      av_channel_layout_default(
          &in_channel_layout,
          media_state->audio_codec_ctx->ch_layout.nb_channels);
      AVChannelLayout out_channel_layout = in_channel_layout;
      media_state->audio_swr_ctx = swr_alloc();
      if (media_state->audio_swr_ctx) {
        swr_alloc_set_opts2(
            &media_state->audio_swr_ctx, &out_channel_layout, AV_SAMPLE_FMT_S16,
            media_state->audio_codec_ctx->sample_rate, &in_channel_layout,
            media_state->audio_codec_ctx->sample_fmt,
            media_state->audio_codec_ctx->sample_rate, 0, NULL);
      }
      swr_init(media_state->audio_swr_ctx);
      SDL_PauseAudio(0);
      break;
    case AVMEDIA_TYPE_VIDEO:
      media_state->video_index = stream_index;
      media_state->video_stream = fmt_ctx->streams[stream_index];
      media_state->video_codec_ctx = codec_ctx;
      // 以系统时间为基准，初始化播放到当前帧的已播放时间值，该值为真实时间值、动态时间值、绝对时间值
      media_state->frame_timer = (double)av_gettime() / 1000000.0;
      // 初始化上一帧图像的动态刷新延迟时间
      media_state->frame_last_delay = 40e-3;
      Packet_Queue_Init(&media_state->video_queue);
      media_state->sws_ctx =
          sws_getContext(media_state->video_codec_ctx->width,
                         media_state->video_codec_ctx->height,
                         media_state->video_codec_ctx->pix_fmt,
                         media_state->video_codec_ctx->width,
                         media_state->video_codec_ctx->height,
                         AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
      media_state->video_tid =
          SDL_CreateThread(video_thread, "video_thread", media_state);
      break;
    default:
      break;
  }
  return 0;
}

void quit_event(void* arg) {
  MediaState* media_state = (MediaState*)arg;
  SDL_Event event;
  event.type = FF_QUIT_EVENT;
  event.user.data1 = media_state;
  SDL_PushEvent(&event);
}

/*---------------------------
* decode_thread：编码数据包解析线程函数(从视频文件中解析出音视频编码数据单元，一个AVPacket的data通常对应一个NAL)
* 1、直接识别文件格式和间接识别媒体格式
* 2、打开解码器并启动解码线程
* 3、分离音视频媒体包并挂接到相应队列
---------------------------*/
int decode_thread(void* arg) {
  MediaState* media_state = (MediaState*)arg;
  AVFormatContext* fmt_ctx = NULL;
  AVPacket* packet = NULL;

  media_state->video_index = -1;
  media_state->audio_index = -1;
  global_media_state = media_state;

  int video_index = -1;
  int audio_index = -1;

  if (avformat_open_input(&fmt_ctx, media_state->filename, NULL, NULL) != 0) {
    std::cout << "failed to open video file..." << std::endl;
    return -1;
  }
  media_state->fmt_ctx = fmt_ctx;
  // retrieve stream information
  if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
    std::cout << "failed to find stream information..." << std::endl;
    return -1;
  }

  std::cout << "=========================" << std::endl;
  av_dump_format(fmt_ctx, 0, media_state->filename, 0);
  std::cout << "=========================" << std::endl;

  // 遍历文件中包含的所有流媒体类型(视频流、音频流、字幕流等)
  for (int i = 0; i < fmt_ctx->nb_streams; i++) {
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
        video_index < 0) {
      video_index = i;
    }
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
        audio_index < 0) {
      audio_index = i;
    }
  }

  if (audio_index >= 0) {
    stream_component_open(media_state, audio_index);
  }
  if (video_index >= 0) {
    stream_component_open(media_state, video_index);
  }

  if (media_state->video_index < 0 || media_state->audio_index < 0) {
    std::cout << "could not open codec..." << std::endl;
    quit_event(media_state);
    return -1;
  }

  std::cout << "video context width: " << media_state->video_codec_ctx->width
            << " height: " << media_state->video_codec_ctx->height << std::endl;
  screen_width = media_state->video_codec_ctx->width;
  screen_height = media_state->video_codec_ctx->height;

  // main decode loop
  while (1) {
    if (media_state->quit) {
      SDL_CondSignal(media_state->video_queue.cond);
      SDL_CondSignal(media_state->audio_queue.cond);
      break;
    }
    // 检查音视频编码数据包队列长度是否溢出
    if (media_state->audio_queue.size > MAX_AUDIO_QUEUE_SIZE ||
        media_state->video_queue.size > MAX_VIDEO_QUEUE_SIZE) {
      SDL_Delay(10);
      continue;
    }

    packet = av_packet_alloc();
    /*视频是否播放结束在这里判断？*/
    if (av_read_frame(media_state->fmt_ctx, packet) < 0) {
      if (media_state->fmt_ctx->pb->error == 0) {
        //  no error, wait for user input
        SDL_Delay(100);
        continue;
      } else {
        break;
      }
    }

    if (packet->stream_index == media_state->video_index) {
      Packet_Queue_Put(&(media_state->video_queue), packet);
      std::cout << "put video queue size: "
                << media_state->video_queue.nb_packets
                << "  packet_size: " << packet->size << std::endl;
    } else if (packet->stream_index == media_state->audio_index) {
      Packet_Queue_Put(&(media_state->audio_queue), packet);
      std::cout << "put audio queue size: "
                << media_state->audio_queue.nb_packets
                << "  packet_size: " << packet->size << std::endl;
    } else {
      if (packet && packet->buf) av_packet_unref(packet);
      av_packet_free(&packet);
    }
  }

  /*all done - wait for it !*/
  while (!media_state->quit) {
    SDL_Delay(100);
  }

  quit_event(media_state);
  return 0;
}

void Close(SDL_Window* win, SDL_Renderer* renderer, SDL_Texture* texture) {
  std::cout << "==========" << __FUNCTION__ "==========" << std::endl;
  if (window) SDL_DestroyWindow(window);
  if (renderer) SDL_DestroyRenderer(renderer);
  if (texture) SDL_DestroyTexture(texture);
  SDL_Quit();
}

int main(int argc, char* argv[]) {
  int ret = -1;
  MediaState* media_state = NULL;
  SDL_Event event;

  if (argc < 2) {
    std::cout << "Usage: command <file> " << std::endl;
    return ret;
  }

  media_state = (MediaState*)av_mallocz(sizeof(MediaState));

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
    std::cout << "initialize SDL failed: " << SDL_GetError() << std::endl;
    return ret;
  }
  window = SDL_CreateWindow("Media Player", SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED, 640, 480,
                            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (!window) {
    std::cout << "failed to create window by SDL..." << std::endl;
    return -1;
  }

  renderer = SDL_CreateRenderer(window, -1, 0);
  if (!renderer) {
    std::cout << "failed to create renderer by SDL..." << std::endl;
    return -1;
  }

  texture_mutex = SDL_CreateMutex();
  media_state->picture_queue_mutex = SDL_CreateMutex();
  media_state->picture_queue_cond = SDL_CreateCond();

  SDL_strlcpy(media_state->filename, argv[1], sizeof(media_state->filename));

  // set timer
  schedule_refresh(media_state, 40);

  // 创建编码数据包解析线程
  media_state->parse_tid =
      SDL_CreateThread(decode_thread, "decode_thread", media_state);
  if (!media_state->parse_tid) {
    av_free(media_state);
    Close(window, renderer, texture);
    return ret;
  }

  while (media_state->quit != 1) {
    SDL_WaitEvent(&event);
    switch (event.type) {
      // 退出进程事件
      case FF_QUIT_EVENT:
      case SDL_QUIT:
        std::cout << "receive a Quit Event: " << event.type << std::endl;
        media_state->quit = 1;
        break;
      // 视频显示刷新事件
      case FF_REFRESH_EVENT:
        video_refresh_timer(event.user.data1);
        break;
      default:
        break;
    }
  }

  ret = 0;
  SDL_CloseAudio();
  sws_freeContext(media_state->sws_ctx);
  swr_free(&media_state->audio_swr_ctx);
  if (media_state->picture_queue_mutex)
    SDL_DestroyMutex(media_state->picture_queue_mutex);
  if (media_state->picture_queue_cond)
    SDL_DestroyCond(media_state->picture_queue_cond);
  if (media_state) av_free(media_state);
  if (texture_mutex) SDL_DestroyMutex(texture_mutex);
  Close(window, renderer, texture);
  return ret;
}