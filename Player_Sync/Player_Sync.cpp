// ͬ��������Ƶͬ������
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
#define AV_SYNC_THRESHOLD 0.01  // ǰ����֡�����ʾʱ��������Сֵ0.01s
#define AV_NOSYNC_THRESHOLD 10.0  // ��Сˢ�¼��ʱ��10ms

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
  SDL_mutex* mutex;  // ���л�������������������
  SDL_cond* cond;    // ���о�����������
} PacketQueue;

typedef struct VideoPicture {
  AVFrame* pic_frame;
  int width;
  int height;
  int allocated;  // �Ƿ�����ڴ�ռ�
  double pts;     // ��ǰͼ��֡�ľ�����ʾʱ���
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
  // ��Ƶ���ŵ���ǰ֡ʱ���ۼ��Ѳ���ʱ��
  double frame_timer;
  // ��һ֡ͼ�����ʾʱ�����������video_refersh_timer�б�����һ֡��ptsֵ
  double frame_last_pts;
  // ��һ֡ͼ��Ķ�̬ˢ���ӳ�ʱ��
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

/*-----------ȡ����Ƶʱ��----------
------ȡ�õ�ǰ������Ƶ���ݵ�pts������Ƶʱ����Ϊ����Ƶͬ����׼------
* ����Ƶͬ����ԭ���Ǹ�����Ƶ��pts��������Ƶ�Ĳ���
* Ҳ����˵����Ƶ����һ֡���Ƿ���ʾ�Լ���ʾ�೤ʱ�䣬��ͨ����֡��PTS��ͬʱ���ڲ��ŵ���Ƶ��PTS�Ƚ϶�����
* �����Ƶ��PTS�ϴ�����Ƶ׼���������ˢ�£�����ȴ�
* ��Ϊpcm���ݲ���audio_callback�ص���ʽ���в���
* ������Ƶ��������ֻ�ܵõ�д��ص�����ǰ������Ƶ֡��pts�����޷��õ���ǰ����֡��pts(��Ҫ���õ�ǰ������Ƶ֡��pts��Ϊ�ο�ʱ��)
* ���ǵ���Ƶ�Ĵ�С�벥��ʱ�������(��ͬ������)����ô��ǰʱ�����ڲ��ŵ���Ƶ֡pts(λ�ڻص�����������)
* �Ϳ��Ը���������������pcm���ݳ��ȡ�������ʣ��pcm���ݳ��ȣ����泤�ȼ������ʽ���������
--------------------------------*/
double get_audio_clock(MediaState* media_state) {
  double pts;
  int hw_buf_size = 0;
  int bytes_per_second = 0;

  // maintained in the audio thread
  pts = media_state->audio_clock;
  // ���㵱ǰ��Ƶ�������ݻ�������λ��
  hw_buf_size = media_state->audio_buf_size - media_state->audio_buf_index;
  if (media_state->audio_stream) {
    // ����ÿ���ԭʼ��Ƶ�ֽ���
    // ������*������*ÿ���������ֽ�����AV_SAMPLE_FMT_S16 -- 2���ֽڣ�
    bytes_per_second = media_state->audio_codec_ctx->sample_rate *
                       media_state->audio_codec_ctx->ch_layout.nb_channels * 2;
  }
  if (bytes_per_second) {
    // ���������������������λ�ã���ǰ���Ƽ��㵱ǰʱ�̵���Ƶ����ʱ���pts
    pts -= (double)hw_buf_size / bytes_per_second;
  }
  // ���ص�ǰ���ڲ��ŵ���Ƶʱ���
  return pts;
}

int audio_decode_frame(MediaState* audio_state, uint8_t* audio_buf,
                       int buf_size, double* pts_ptr) {
  int ret = 0;
  int data_size = 0;
  AVPacket* packet = audio_state->audio_packet;
  audio_state->audio_frame = av_frame_alloc();
  double pts;  // ��Ƶ����ʱ���

  while (1) {
    if (Packet_Queue_Get(&audio_state->audio_queue, packet, 1) < 0) {
      return -1;
    }
    if (packet && packet->data) {
      audio_state->audio_packet_data = packet->data;
      audio_state->audio_packet_size = packet->size;
    }
    // if update, update the audio clock w/pts
    // �����Ƶ����ʱ���
    if (packet->pts != AV_NOPTS_VALUE) {
      // ���һ���µ�packet��ʱ�򣬸���audio_clock����packet�е�pts����audio_clock(һ��pkt��Ӧһ��pts)
      // ������Ƶ�Ѿ�����ʱ��
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

        // �����ǽ�����ĸ�ʽ audio_codec_ctx->sample_fmt ת��Ϊ AV_SAMPLE_FMT_S16 ��ʽ���
        // ������ʹ�õ��ļ��ĸ�ʽ audio_codec_ctx->sample_fmt �� fltp �����ͣ�32bits
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
        // ��ÿ�θ��µ���Ƶ����ʱ�������ƵPTS
        pts = audio_state->audio_clock;
        *pts_ptr = pts;
        /*---------------------
        * ��һ��packet�а��������Ƶ֡ʱ
        * ͨ��[�������Ƶԭʼ���ݳ���]��[������]������һ��packet��������Ƶ֡�Ĳ���ʱ���pts
        * ����Ƶ��44.1kHz������λ��16λ����ζ��ÿ��ɼ�����44.1k����ÿ������ռ2�ֽ�
        --------------------*/
        //����ÿ����Ƶ�������ݵ��ֽ���=ÿ��������Ƶ�����ֽ���*������
        /*pcm_bytes =2 *audio_state->audio_codec_ctx->ch_layout.nb_channels;  */
        /*----����audio_clock---
         * һ��pkt���������Ƶframe��ͬʱһ��pkt��Ӧһ��pts(pkt->pts)
         * ��ˣ���pkt�а����Ķ����Ƶ֡��ʱ��������¹�ʽ�ƶϵó�
         * bytes_per_sec = pcm_bytes * audio_state->audio_codec_ctx->sample_rate
         * ��pkt�в��ϵĽ��룬�ƶ�(һ��pkt��)ÿ֡���ݵ�pts���ۼӵ���Ƶ����ʱ��
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
 ��Ƶ����ص�������sdlͨ���ûص�������������pcm����������������,
 sdlͨ��һ�λ�׼��һ�黺��pcm���ݣ�ͨ���ûص���������������������Ƶpts���β���pcm����
 �����뻺���pcm������ɲ��ź�������һ���µ�pcm��������(ÿ����Ƶ�������Ϊ��ʱ��sdl�͵��ô˺��������Ƶ������棬��������������)
*/
void audio_callback(void* userdata, Uint8* stream, int len) {
  MediaState* audio_state = (MediaState*)userdata;
  int real_len, audio_size;  // ÿ��д��stream�����ݳ��ȣ����������ݳ���
  double pts;

  SDL_memset(stream, 0, len);
  while (len > 0) {
    if (audio_state->audio_buf_index >= audio_state->audio_buf_size) {
      // already sent all data, get more. ������Ƶ֡������
      audio_size = audio_decode_frame(audio_state, audio_state->audio_buf,
                                      sizeof(audio_state->audio_buf), &pts);
      if (audio_size < 0) {
        // If error, output silence. ����ʧ�ܻ���û�н��뵽��Ƶ���ݣ������һ�ξ���������
        audio_state->audio_buf_size = 1024 * 2 * 2;
        memset(audio_state->audio_buf, 0, audio_state->audio_buf_size);
      } else {
        audio_state->audio_buf_size = audio_size;
      }
      audio_state->audio_buf_index = 0;
    }
    // ����audio_buf_size ���ڵ��� audio_buf_index
    real_len = audio_state->audio_buf_size - audio_state->audio_buf_index;
    if (real_len > len) {
      real_len = len;
    }
    std::cout << __FUNCTION__ << "  index: " << audio_state->audio_buf_index
              << " real_len: " << real_len << " len: " << len << std::endl;
    // ������Բ��������㴦��
    /*memcpy(stream, audio_state->audio_buf + audio_state->audio_buf_index,
           real_len);*/
    // ʹ�����SDL_MixAudio��ע���£���ǰ����Ҫ�������㴦�����򲥷ŵ���Ƶ������
    //SDL_MixAudio(stream, audio_state->audio_buf, real_len, SDL_MIX_MAXVOLUME);
    SDL_MixAudio(stream, audio_state->audio_buf + audio_state->audio_buf_index,
                 real_len, SDL_MIX_MAXVOLUME);
    len -= real_len;
    stream += real_len;
    audio_state->audio_buf_index += real_len;
  }
}

// ��ʱ�������Ļص�����
static Uint32 sdl_refresh_timer_cb(Uint32 interval, void* opaque) {
  SDL_Event event;
  event.type = FF_REFRESH_EVENT;
  event.user.data1 = opaque;
  SDL_PushEvent(&event);
  return 0;  // 0 means stop timer
}

/* schedule a video refresh in 'delay' ms */
//delay������ͼ��֡�Ľ���˳������Ⱦ˳��һ�µ�����£�������һ֡����Ⱦʱ����
//�Ӷ������ܵ�ʹ����ͼ��֡���չ̶���֡����Ⱦˢ��
static void schedule_refresh(MediaState* media_state, int delay) {
  /*
  ���һ����ʱ�����ö�ʱ��ÿdelay���봥��һ�� sdl_refresh_timer_cb
  �������� sdl_refresh_timer_cb �����У�����ͨ������
  interval ���������ö�ʱ�����Ӷ�ʵ�������Դ������յ�����0ʱֹͣ��ʱ��
  */
  SDL_AddTimer(delay, sdl_refresh_timer_cb, media_state);
}

// ��Ƶ֡��Ⱦ
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
  // ��ͼ��֡���У����飩�л�ȡͼ��֡
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
* ��ʾˢ�º���(FF_REFRESH_EVENT�¼���Ӧ����)
* ����Ƶͬ������Ƶ��(�������������ŵģ�����û���ȥͬ������)��������һ֡���ӳ�ʱ��
* ʹ�õ�ǰ֡��PTS����һ֡��PTS�������Ʋ�����һ֡���ӳ�ʱ�䣬������video�Ĳ����ٶ�����������ӳ�ʱ��
---------------------------*/
void video_refresh_timer(void* userdata) {
  MediaState* video_state = (MediaState*)userdata;
  VideoPicture* video_picture = NULL;
  // ��ǰ֡����һ֡����ʾʱ����(��̬ʱ�䡢��ʵʱ�䡢����ʱ��)
  double actual_delay;
  double delay;           // ǰ��֡�����ʾʱ����
  double sync_threshold;  // ǰ��֡�����Сʱ���
  double ref_clock;       // ��Ƶʱ�������Ϊ��Ƶͬ���Ĳο�ʱ��
  double diff;            // ͼ��֡��ʾ����Ƶ֡���ż��ʱ���

  if (video_state->video_stream) {
    // ���ͼ��֡�����Ƿ��д���ʾͼ��
    if (video_state->picture_queue_size == 0) {
      schedule_refresh(video_state, 1);
    } else {
      // ����ʾ������ȡ�õȴ���ʾ��ͼ��֡
      video_picture =
          &video_state->picture_queue[video_state->picture_queue_rindex];

      // the pts from last time
      // ���㵱ǰ֡��ǰһ֡��ʾ(pts)�ļ��ʱ��(��ʾʱ����Ĳ�ֵ)
      delay = video_picture->pts - video_state->frame_last_pts;
      if (delay <= 0 || delay >= 1.0) {
        // if incorrect delay, use previous one
        // ����֮ǰ�Ķ�̬ˢ�¼��ʱ��
        delay = video_state->frame_last_delay;
      }
      // save for next time
      // ������һ֡ͼ��Ķ�̬ˢ���ӳ�ʱ��
      video_state->frame_last_delay = delay;
      // ������һ֡ͼ�����ʾʱ���
      video_state->frame_last_pts = video_picture->pts;

      // update delay to sync to audio
      ref_clock = get_audio_clock(video_state);
      // ����˵��diff���ʱ�������������ٷ����ģ�������delay���ʱ��frame����ʾ���ܾͻ��п���������
      // ����ͼ��֡��ʾ����Ƶ֡���ż��ʱ���
      diff = video_picture->pts - ref_clock;

      // Skip or repeat the frame. Take delay into account
      /*FFPlay still doesn't "know if this is the best guess.*/
      // ����ʱ������������һ֡���ӳ�ʱ�䣬��ʵ��ͬ��
      // �Ƚ�ǰ����֡�����ʾʱ��������Сʱ����
      sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;

      // �ж�����Ƶ��ͬ��������������Ƶ���ʱ��� & ǰ��֡���ʱ���<10ms��ֵ����>����ֵ��Ϊ���ģʽ������������Ƶͬ������
      if (fabs(diff) < AV_NOSYNC_THRESHOLD) {
        if (diff <= -sync_threshold) {
          // �Ƚ�ǰһ֡&��ǰ֡[����-����]���ʱ������ǰһ֡ & ��ǰ֡[����-����]���ʱ���������ˣ�delay��Ϊ0
          // ��һ֡������ʾ��ʱ��͵�ǰ�������ܽ��Ļ��ӿ���ʾ��һ֡��������video_display��ʾ�굱ǰ֡������ʱ���ܿ�ȥ��ʾ��һ֡
          delay = 0;
        } else if (diff >= sync_threshold) {
          // �Ƚ���֡��������ʾʱ������֡����������Ĳ���ʱ�䣬���ˣ��ӱ�delay
          delay = 2 * delay;
        }
      }
      // ���diff(����)����AV_NOSYNC_THRESHOLD���������ģʽ�ˣ���������̫�󣬲���������Ƶͬ����������

      // ������Ƶ���ŵ���ǰ֡ʱ���Ѳ���ʱ��ֵ(����ͼ��֡��̬�����ۼ�ʱ��ֵ-��ʵֵ)��
      // frame_timerһֱ�ۼ��ڲ��Ź��������Ǽ������ʱ
      video_state->frame_timer += delay;
      // compute the real delay
      // ÿ�μ���frame_timer��ϵͳʱ��Ĳ�ֵ(��ϵͳʱ��Ϊ��׼ʱ��)����frame_timer��ϵͳʱ��(����ʱ��)�������Ŀ��
      actual_delay = video_state->frame_timer - (av_gettime() / 1000000.0);
      if (actual_delay < 0.010) {
        // Really it should skip the picture instead
        actual_delay = 0.010;
      }

      // �þ���ʱ�俪��ʱ��ȥ��̬��ʾˢ����һ֡
      schedule_refresh(video_state, (int)(actual_delay * 1000 + 0.5));
      // show the pic_frame
      video_display(video_state);

      /*update queue for next frame*/
      // ���²����ͼ��֡���ж�λ������
      if (++video_state->picture_queue_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
        video_state->picture_queue_rindex = 0;  // ���ö�λ������
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

// ���� or ����ͼ��֡��Ϊͼ��֡�����ڴ�ռ�
void alloc_picture(void* userdata) {
  MediaState* video_state = (MediaState*)userdata;
  VideoPicture* video_picture = NULL;

  video_picture =
      &video_state->picture_queue[video_state->picture_queue_windex];
  if (video_picture->pic_frame) {
    av_frame_free(&(video_picture->pic_frame));
  }

  SDL_LockMutex(texture_mutex);
  // 1. ����AVFrame�ṹ
  video_picture->pic_frame = av_frame_alloc();
  // 2. ����֡����
  if (video_picture->pic_frame) {
    video_picture->pic_frame->format = AV_PIX_FMT_YUV420P;
    video_picture->pic_frame->width = video_state->video_codec_ctx->width;
    video_picture->pic_frame->height = video_state->video_codec_ctx->height;
  }
  // 3. ����֡������
  if (av_frame_get_buffer(video_picture->pic_frame, 0) < 0) {
    av_frame_free(&(video_picture->pic_frame));
  }
  SDL_UnlockMutex(texture_mutex);

  video_picture->width = video_state->video_codec_ctx->width;
  video_picture->height = video_state->video_codec_ctx->height;
  video_picture->allocated = 1;
}

// ͼ��֡������еȴ���Ⱦ
int queue_picture(MediaState* video_state, AVFrame* frame, double pts) {
  VideoPicture* video_picture = NULL;

  // ��ʵ��������������Ƶˢ�µ�֡��
  /* wait until we have space for a new pic */
  SDL_LockMutex(video_state->picture_queue_mutex);
  while (video_state->picture_queue_size >= VIDEO_PICTURE_QUEUE_SIZE &&
         !video_state->quit) {
    // ����һ֡������һ֡����pushһ֡���ȴ�video_display��ʾ��һ֡����ź�
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

    // ���²���鵱ǰͼ��֡����д��λ��
    /* now we inform our display thread that we have a pic ready */
    if (++video_state->picture_queue_windex == VIDEO_PICTURE_QUEUE_SIZE) {
      video_state->picture_queue_windex = 0;  // ����ͼ��֡����д��λ��
    }
    SDL_LockMutex(video_state->picture_queue_mutex);
    video_state->picture_queue_size++;
    SDL_UnlockMutex(video_state->picture_queue_mutex);
  }
  return 0;
}

/*---------------------------
* �����ڲ���Ƶ���ż�ʱ��(��¼��Ƶ�Ѿ���ʱ��(video_clock)
* update the PTS to be in sync
---------------------------*/
double synchronize_video(MediaState* video_state, AVFrame* frame, double pts) {
  double frame_delay;
  /*----------�����ʾʱ����Ƿ���Ч----------*/
  if (pts != 0) {
    // if we have pts, set video clock to it
    // ����ʾʱ��������Ѳ���ʱ��
    video_state->video_clock = pts;
  } else {
    // if we aren't given a pts, set it to the clock
    // ����ȡ������ʾʱ���, ���Ѳ���ʱ�������ʾʱ���
    pts = video_state->video_clock;
  }
  /*----------������Ƶ�Ѿ���ʱ��----------*/
  // update the video clock
  // ����֡Ҫ�ظ���ʾ(ȡ����repeat_pict)����ȫ����Ƶ����ʱ��video_clockӦ�����ظ���ʾ������*֡��
  // ��֡��ʾ�꽫Ҫ���ѵ�ʱ��
  frame_delay = av_q2d(video_state->video_codec_ctx->time_base);
  // if we are repeating a frame, adjust clock accordingly
  // �������ظ�֡�������������ŵ�ǰ����֡ͼ��䰲����Ⱦ�ظ�֡
  // ������Ⱦ�ظ�֡��ʱֵ(����������ʱֵ)
  frame_delay += frame->repeat_pict * (frame_delay * 0.5);
  // ������Ƶ����ʱ��
  video_state->video_clock += frame_delay;
  // ��ʱ���ص�ֵ��Ϊ��һ֡��Ҫ��ʼ��ʾ��ʱ���
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
      av_packet_unref(packet);  // �ͷ�packet�б���ı�������
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
      * �ڽ����̺߳����м��㵱ǰͼ��֡����ʾʱ���
      * 1��ȡ�ñ������ݰ��е�ͼ��֡��ʾ���PTS(int64_t),����ʱ������pts(double)��
      * 2������PTS*time_base�����㵱ǰ����������Ƶ�е���ʾʱ�������PTS*(1/framerate)
      * av_q2d��AVRatioal�ṹת����double�ĺ�����
      * ���ڼ�����ƵԴÿ��ͼ��֡��ʾ�ļ��ʱ��(1/framerate),������(time_base->num/time_base->den)
      -------------------------*/
      //����pts=PTS*time_base={numerator=1,denominator=25}���㵱ǰ����������Ƶ�е���ʾʱ���
      // time_baseΪAVRational�������ṹ��{num=1,den=25}����¼����ƵԴÿ��ͼ��֡��ʾ�ļ��ʱ��
      pts *= av_q2d(media_state->video_stream->time_base);
      // ��鵱ǰ֡����ʾʱ���pts�������ڲ���Ƶ���ż�ʱ��(��¼��Ƶ�Ѿ���ʱ��(video_clock)��
      pts = synchronize_video(media_state, frame, pts);

      // ��ͼ��֡��ӵ�ͼ��֡����
      if (queue_picture(media_state, frame, pts) < 0) {
        break;
      }
    }
  }
  av_packet_free(&packet);
  av_frame_free(&frame);
  return 0;
}

// ����ָ�����ʹ������ҵ���Ӧ�Ľ�������������Ӧ����Ƶ���á�����ؼ���Ϣ��MediaState��������Ƶ����Ƶ�߳�
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
    wanted_spec.freq = codec_ctx->sample_rate;                // ������
    wanted_spec.format = AUDIO_S16SYS;                        // ������ʽ
    wanted_spec.channels = codec_ctx->ch_layout.nb_channels;  // ������
    wanted_spec.silence = 0;  // �����ʱ�Ƿ���
    // Ĭ��ÿ�ζ���Ƶ����Ĵ�С���Ƽ�ֵΪ 512~8192��ffplayʹ�õ���1024
    wanted_spec.samples = 1024;
    wanted_spec.callback = audio_callback;  // ��ȡ��Ƶ���ݵĻص��ӿں���
    wanted_spec.userdata = media_state;  // �ص��д��ݵ����ݲ���
    if (SDL_OpenAudio(&wanted_spec, NULL) < 0) {
      std::cout << "failed to open audio device: " << SDL_GetError()
                << std::endl;
      return -1;
    }
  }

  // ������������
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
      // ��ϵͳʱ��Ϊ��׼����ʼ�����ŵ���ǰ֡���Ѳ���ʱ��ֵ����ֵΪ��ʵʱ��ֵ����̬ʱ��ֵ������ʱ��ֵ
      media_state->frame_timer = (double)av_gettime() / 1000000.0;
      // ��ʼ����һ֡ͼ��Ķ�̬ˢ���ӳ�ʱ��
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
* decode_thread���������ݰ������̺߳���(����Ƶ�ļ��н���������Ƶ�������ݵ�Ԫ��һ��AVPacket��dataͨ����Ӧһ��NAL)
* 1��ֱ��ʶ���ļ���ʽ�ͼ��ʶ��ý���ʽ
* 2���򿪽����������������߳�
* 3����������Ƶý������ҽӵ���Ӧ����
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

  // �����ļ��а�����������ý������(��Ƶ������Ƶ������Ļ����)
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
    // �������Ƶ�������ݰ����г����Ƿ����
    if (media_state->audio_queue.size > MAX_AUDIO_QUEUE_SIZE ||
        media_state->video_queue.size > MAX_VIDEO_QUEUE_SIZE) {
      SDL_Delay(10);
      continue;
    }

    packet = av_packet_alloc();
    /*��Ƶ�Ƿ񲥷Ž����������жϣ�*/
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

  // �����������ݰ������߳�
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
      // �˳������¼�
      case FF_QUIT_EVENT:
      case SDL_QUIT:
        std::cout << "receive a Quit Event: " << event.type << std::endl;
        media_state->quit = 1;
        break;
      // ��Ƶ��ʾˢ���¼�
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