#include <SDL.h>

#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// this value should be greater than the value of len
// sample_rate(48000) * channels(2) * (AUDIO_S16SYS)16bits
const int MAX_AUDIO_FRAME_SIZE = 192000;

struct SwrContext* audio_convert_ctx = NULL;
int quit = 0;

typedef struct PacketQueue {
  AVPacketList* first_pkt = NULL;
  AVPacketList* last_pkt = NULL;
  int nb_packets;
  int size;
  SDL_mutex* mutex;
  SDL_cond* cond;
} PacketQueue;
PacketQueue audio_queue;

void CloseAboutAVdata(AVFormatContext* fmt_ctx, AVCodecContext* video_codec_ctx,
                      AVCodecContext* audio_codec_ctx, AVFrame* frame) {
  std::cout << "==========" << __FUNCTION__ "==========" << std::endl;
  if (frame) av_frame_free(&frame);
  if (video_codec_ctx) avcodec_close(video_codec_ctx);
  if (audio_codec_ctx) avcodec_close(audio_codec_ctx);
  if (fmt_ctx) avformat_close_input(&fmt_ctx);
}

void CloseAboutWindow(SDL_Window* win, SDL_Renderer* renderer,
                      SDL_Texture* texture) {
  std::cout << "==========" << __FUNCTION__ "==========" << std::endl;
  if (win) SDL_DestroyWindow(win);
  if (renderer) SDL_DestroyRenderer(renderer);
  if (texture) SDL_DestroyTexture(texture);
  SDL_Quit();
}

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
    if (quit) {
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
      SDL_CondWait(packet_queue->cond, packet_queue->mutex);
    }
  }
  SDL_UnlockMutex(packet_queue->mutex);
  return ret;
}

int audio_decode_frame(AVCodecContext* audio_codec_ctx, uint8_t* audio_buf,
                       int buf_size) {
  //std::cout << "==========" << __FUNCTION__ "==========" << std::endl;
  static AVFrame* frame = NULL;
  static AVPacket* packet = NULL;
  static uint8_t* audio_pkt_data = NULL;
  static int audio_pkt_size = 0;
  int ret = 0;
  int data_size = 0;

  frame = av_frame_alloc();
  packet = av_packet_alloc();

  while (1) {
    if (Packet_Queue_Get(&audio_queue, packet, 1) < 0) {
      return -1;
    }
    if (packet && packet->data) {
      audio_pkt_data = packet->data;
      audio_pkt_size = packet->size;
    }
    // decode audio pkt
    while (audio_pkt_size > 0) {
      ret = avcodec_send_packet(audio_codec_ctx, packet);
      if (ret < 0) {
        std::cout << "Error submitting the packet to the decoder" << std::endl;
        audio_pkt_size = 0;
        return -1;
      }
      if (packet && packet->buf) {
        av_packet_unref(packet);
      }
      while (ret >= 0) {
        ret = avcodec_receive_frame(audio_codec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
          break;
        } else if (ret < 0) {
          std::cout << "Error decoding audio..." << std::endl;
          break;
        }

        int out_count =
            swr_get_out_samples(audio_convert_ctx, frame->nb_samples);
        uint8_t* out[4] = {0};
        out[0] = (uint8_t*)av_malloc(2 * 2 * out_count);

        int nb_samples =
            swr_convert(audio_convert_ctx, out, out_count,
                        (const uint8_t**)frame->data, frame->nb_samples);
        if (nb_samples < 0) {
          return -1;
        }
        data_size =
            nb_samples * av_get_bytes_per_sample(audio_codec_ctx->sample_fmt);
        memcpy(audio_buf, out[0], data_size);
        if (data_size > 0) {
          SDL_Delay(10);
        }
        return data_size;
      }
      audio_pkt_size = 0;
      /*return data_size;*/
    }
    if (packet && packet->buf) {
      av_packet_unref(packet);
    }
    if (quit) {
      return -1;
    }
  }
}

void audio_callback(void* userdata, Uint8* stream, int len) {
  AVCodecContext* audio_codec_ctx = (AVCodecContext*)userdata;
  static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
  static unsigned int audio_buf_size = 0;
  static unsigned int audio_buf_index = 0;
  int real_len, audio_size;

  while (len > 0) {
    if (audio_buf_index >= audio_buf_size) {
      // already sent all data, get more. 解码音频帧的数据
      audio_size =
          audio_decode_frame(audio_codec_ctx, audio_buf, sizeof(audio_buf));
      if (audio_size < 0) {
        // If error, output silence. 解码失败或者没有解码到音频数据，就输出一段静音的内容
        audio_buf_size = 1024;  // ??? arbitrary?
        memset(audio_buf, 0, audio_buf_size);
      } else {
        audio_buf_size = audio_size;
      }
      audio_buf_index = 0;
    }
    // 这里audio_buf_size 大于等于 audio_buf_index
    real_len = audio_buf_size - audio_buf_index;
    if (real_len > len) {
      real_len = len;
    }
    std::cout << __FUNCTION__ << "  index: " << audio_buf_index
              << " real_len: " << real_len << " len: " << len << std::endl;
    memcpy(stream, audio_buf + audio_buf_index, real_len);
    len -= real_len;
    stream += real_len;
    audio_buf_index += real_len;
  }
  SDL_Delay(1);
}

int main(int argc, char* argv[]) {
  int ret = -1;

  // default size of window
  int window_width = 640;
  int window_height = 480;

  if (argc < 2) {
    std::cout << "Usage: command <file> " << std::endl;
    return ret;
  }

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
    std::cout << "initialize SDL failed: " << SDL_GetError() << std::endl;
    return ret;
  }

  AVFormatContext* fmt_ctx = NULL;
  // for video decode
  AVCodecContext* video_codec_ctx = NULL;
  const AVCodec* video_codec = NULL;

  // for audio decode
  AVCodecContext* audio_codec_ctx = NULL;
  const AVCodec* audio_codec = NULL;

  //struct SwrContext* audio_convert_ctx = NULL;
  struct SwsContext* sws_ctx = NULL;
  AVFrame* frame = NULL;
  AVPacket* packet = NULL;

  if (avformat_open_input(&fmt_ctx, argv[1], NULL, NULL) != 0) {
    std::cout << "failed to open video file..." << std::endl;
    return ret;
  }
  // retrieve stream information
  if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
    std::cout << "failed to find stream information..." << std::endl;
    return ret;
  }

  std::cout << "=========================" << std::endl;
  av_dump_format(fmt_ctx, 0, argv[1], 0);
  std::cout << "=========================" << std::endl;

  int video_stream = -1;
  int audio_stream = -1;
  for (int i = 0; i < fmt_ctx->nb_streams; i++) {
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
        video_stream < 0) {
      video_stream = i;
    }
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
        audio_stream < 0) {
      audio_stream = i;
    }
  }

  if (video_stream == -1) {
    std::cout << "Cannot find the video stream..." << std::endl;
    CloseAboutAVdata(fmt_ctx, video_codec_ctx, audio_codec_ctx, frame);
    return ret;
  }
  if (audio_stream == -1) {
    std::cout << "Cannot find the audio stream..." << std::endl;
    CloseAboutAVdata(fmt_ctx, video_codec_ctx, audio_codec_ctx, frame);
    return ret;
  }

  // codec_context for the audio stream
  audio_codec =
      avcodec_find_decoder(fmt_ctx->streams[audio_stream]->codecpar->codec_id);
  if (!audio_codec) {
    std::cout << "Cannot supported the audio codec..." << std::endl;
    CloseAboutAVdata(fmt_ctx, video_codec_ctx, audio_codec_ctx, frame);
    return ret;
  }

  audio_codec_ctx = avcodec_alloc_context3(audio_codec);
  if (avcodec_parameters_to_context(
          audio_codec_ctx, fmt_ctx->streams[audio_stream]->codecpar) < 0) {
    std::cout << "failed to copy the audio stream codecpar to codec context..."
              << std::endl;
    CloseAboutAVdata(fmt_ctx, video_codec_ctx, audio_codec_ctx, frame);
    return ret;
  }

  // open the decoder, finish initialize
  if (avcodec_open2(audio_codec_ctx, audio_codec, NULL) < 0) {
    std::cout << "failed to open decoder..." << std::endl;
    CloseAboutAVdata(fmt_ctx, video_codec_ctx, audio_codec_ctx, frame);
    return ret;
  }

  // set audio settings from codec info
  SDL_AudioSpec wanted_spec;
  wanted_spec.freq = audio_codec_ctx->sample_rate;
  wanted_spec.format = AUDIO_S16SYS;
  wanted_spec.channels = audio_codec_ctx->ch_layout.nb_channels;
  wanted_spec.silence = 0;
  wanted_spec.samples = 1024;
  wanted_spec.callback = audio_callback;
  wanted_spec.userdata = audio_codec_ctx;
  if (SDL_OpenAudio(&wanted_spec, NULL) < 0) {
    std::cout << "failed to open audio device: " << SDL_GetError() << std::endl;
    CloseAboutAVdata(fmt_ctx, video_codec_ctx, audio_codec_ctx, frame);
    return ret;
  }
  Packet_Queue_Init(&audio_queue);

  //int64_t in_channel_layout =
  //    av_get_default_channel_layout(audio_codec_ctx->channels);
  AVChannelLayout in_channel_layout;
  av_channel_layout_default(&in_channel_layout,
                            audio_codec_ctx->ch_layout.nb_channels);
  AVChannelLayout out_channel_layout = in_channel_layout;

  audio_convert_ctx = swr_alloc();
  if (audio_convert_ctx) {
    swr_alloc_set_opts2(&audio_convert_ctx, &out_channel_layout,
                        AV_SAMPLE_FMT_S16, audio_codec_ctx->sample_rate,
                        &in_channel_layout, audio_codec_ctx->sample_fmt,
                        audio_codec_ctx->sample_rate, 0, NULL);
  }
  swr_init(audio_convert_ctx);

  SDL_PauseAudio(0);

  // codec_context for the video stream
  video_codec =
      avcodec_find_decoder(fmt_ctx->streams[video_stream]->codecpar->codec_id);
  if (!video_codec) {
    std::cout << "Cannot supported the video codec..." << std::endl;
    CloseAboutAVdata(fmt_ctx, video_codec_ctx, audio_codec_ctx, frame);
    return ret;
  }

  video_codec_ctx = avcodec_alloc_context3(video_codec);
  if (avcodec_parameters_to_context(
          video_codec_ctx, fmt_ctx->streams[video_stream]->codecpar) < 0) {
    std::cout << "failed to copy the video stream codecpar to codec context..."
              << std::endl;
    CloseAboutAVdata(fmt_ctx, video_codec_ctx, audio_codec_ctx, frame);
    return ret;
  }

  if (avcodec_open2(video_codec_ctx, video_codec, NULL) < 0) {
    std::cout << "failed to open decoder..." << std::endl;
    CloseAboutAVdata(fmt_ctx, video_codec_ctx, audio_codec_ctx, frame);
    return ret;
  }

  frame = av_frame_alloc();
  packet = av_packet_alloc();
  window_width = video_codec_ctx->width;
  window_height = video_codec_ctx->height;
  std::cout << "width: " << window_width << " height: " << window_height
            << std::endl;

  SDL_Window* window = NULL;
  SDL_Renderer* renderer = NULL;
  SDL_Texture* texture = NULL;
  SDL_Event event;
  SDL_Rect rect;

  window = SDL_CreateWindow(
      "Media Player", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
      window_width, window_height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (!window) {
    std::cout << "failed to create window by SDL..." << std::endl;
    CloseAboutAVdata(fmt_ctx, video_codec_ctx, audio_codec_ctx, frame);
    CloseAboutWindow(window, renderer, texture);
    return ret;
  }

  renderer = SDL_CreateRenderer(window, -1, 0);
  if (!renderer) {
    std::cout << "failed to create renderer by SDL..." << std::endl;
    CloseAboutAVdata(fmt_ctx, video_codec_ctx, audio_codec_ctx, frame);
    CloseAboutWindow(window, renderer, texture);
    return ret;
  }

  Uint32 pixformat = SDL_PIXELFORMAT_IYUV;
  texture = SDL_CreateTexture(renderer, pixformat, SDL_TEXTUREACCESS_STREAMING,
                              window_width, window_height);
  if (!texture) {
    std::cout << "failed to create texture by SDL..." << std::endl;
    CloseAboutAVdata(fmt_ctx, video_codec_ctx, audio_codec_ctx, frame);
    CloseAboutWindow(window, renderer, texture);
    return ret;
  }

  // initialize SWS context for software scaling
  sws_ctx = sws_getContext(video_codec_ctx->width, video_codec_ctx->height,
                           video_codec_ctx->pix_fmt, video_codec_ctx->width,
                           video_codec_ctx->height, AV_PIX_FMT_YUV420P,
                           SWS_BILINEAR, NULL, NULL, NULL);

  // 1. 分配AVFrame结构
  AVFrame* pic_frame = av_frame_alloc();
  // 2. 设置帧参数
  pic_frame->format = AV_PIX_FMT_YUV420P;
  pic_frame->width = video_codec_ctx->width;
  pic_frame->height = video_codec_ctx->height;
  // 3. 分配帧缓冲区
  if (av_frame_get_buffer(pic_frame, 0) < 0) {
    av_frame_free(&pic_frame);
    CloseAboutAVdata(fmt_ctx, video_codec_ctx, audio_codec_ctx, frame);
    CloseAboutWindow(window, renderer, texture);
    return ret;
  }

  while (av_read_frame(fmt_ctx, packet) >= 0) {
    if (packet->stream_index == video_stream) {
      // video stream
      // decode video frame
      ret = avcodec_send_packet(video_codec_ctx, packet);
      if (ret < 0) {
        std::cout << "Error sending the packet to the decoder..." << std::endl;
        continue;
      }
      while (ret >= 0) {
        ret = avcodec_receive_frame(video_codec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
          break;
        } else if (ret < 0) {
          std::cout << "Error during decoding..." << std::endl;
          break;
        }
        fflush(stdout);

        // convert the image into YUV format that SDL use.
        sws_scale(sws_ctx, (const uint8_t* const*)frame->data, frame->linesize,
                  0, video_codec_ctx->height, pic_frame->data,
                  pic_frame->linesize);

        // 4. 使用帧数据
        SDL_UpdateYUVTexture(texture, NULL, pic_frame->data[0],
                             pic_frame->linesize[0], pic_frame->data[1],
                             pic_frame->linesize[1], pic_frame->data[2],
                             pic_frame->linesize[2]);

        rect.x = 0;
        rect.y = 0;
        rect.w = video_codec_ctx->width;
        rect.h = video_codec_ctx->height;

        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, &rect);
        SDL_RenderPresent(renderer);
      }
      if (packet && packet->buf) av_packet_unref(packet);
    } else if (packet->stream_index == audio_stream) {
      Packet_Queue_Put(&audio_queue, packet);
    } else {
      if (packet && packet->buf) av_packet_unref(packet);
    }
    // 轮询的方式，检测event
    SDL_PollEvent(&event);
    switch (event.type) {
      case SDL_QUIT:
        quit = 1;
        CloseAboutAVdata(fmt_ctx, video_codec_ctx, audio_codec_ctx, frame);
        CloseAboutWindow(window, renderer, texture);
        return 0;
      default:
        break;
    }
  }

  SDL_CloseAudio();
  sws_freeContext(sws_ctx);
  swr_free(&audio_convert_ctx);
  av_packet_free(&packet);
  av_frame_unref(frame);
  av_frame_free(&frame);
  ret = 0;
  CloseAboutAVdata(fmt_ctx, video_codec_ctx, audio_codec_ctx, frame);
  CloseAboutWindow(window, renderer, texture);

  return ret;
}