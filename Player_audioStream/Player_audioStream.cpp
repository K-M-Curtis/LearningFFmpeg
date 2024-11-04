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
// audio of 48khz 32bit sample is 16bits
const int MAX_AUDIO_FRAME_SIZE = 192000;
static int audio_size = 0;
uint8_t* audio_buf = nullptr;
int quit = 0;

void CloseAboutAVdata(AVFormatContext* fmt_ctx, AVCodecContext* audio_codec_ctx,
                      AVFrame* frame) {
  std::cout << "==========" << __FUNCTION__ "==========" << std::endl;
  if (frame) av_frame_free(&frame);
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

/*
  这个回调函数是由 SDL
  音频子系统在需要新的音频数据时自动调用的。 具体来说，当你设置好 SDL_AudioSpec 并通过 SDL_OpenAudio 打开音频设备后，
  SDL 会在内部周期性地调用这个回调函数，以流的形式播放音频。
*/
void audio_callback(void* userdata, Uint8* stream, int len) {
  static unsigned int audio_buf_size = 0;
  static unsigned int audio_buf_index = 0;
  int real_len;

  while (len > 0) {
    if (audio_buf_index >= audio_buf_size) {
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
  //SDL_Delay(1);
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

  // for audio decode
  AVCodecContext* audio_codec_ctx = NULL;
  const AVCodec* audio_codec = NULL;

  struct SwrContext* audio_convert_ctx = NULL;
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
    CloseAboutAVdata(fmt_ctx, audio_codec_ctx, frame);
    return ret;
  }
  if (audio_stream == -1) {
    std::cout << "Cannot find the audio stream..." << std::endl;
    CloseAboutAVdata(fmt_ctx, audio_codec_ctx, frame);
    return ret;
  }

  // codec_context for the audio stream
  audio_codec =
      avcodec_find_decoder(fmt_ctx->streams[audio_stream]->codecpar->codec_id);
  if (!audio_codec) {
    std::cout << "Cannot supported the audio codec..." << std::endl;
    CloseAboutAVdata(fmt_ctx, audio_codec_ctx, frame);
    return ret;
  }

  audio_codec_ctx = avcodec_alloc_context3(audio_codec);
  if (avcodec_parameters_to_context(
          audio_codec_ctx, fmt_ctx->streams[audio_stream]->codecpar) < 0) {
    std::cout << "failed to copy the audio stream codecpar to codec context..."
              << std::endl;
    CloseAboutAVdata(fmt_ctx, audio_codec_ctx, frame);
    return ret;
  }

  // open the decoder, finish initialize
  if (avcodec_open2(audio_codec_ctx, audio_codec, NULL) < 0) {
    std::cout << "failed to open decoder..." << std::endl;
    CloseAboutAVdata(fmt_ctx, audio_codec_ctx, frame);
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
    CloseAboutAVdata(fmt_ctx, audio_codec_ctx, frame);
    return ret;
  }

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

  frame = av_frame_alloc();
  packet = av_packet_alloc();
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
    CloseAboutAVdata(fmt_ctx, audio_codec_ctx, frame);
    CloseAboutWindow(window, renderer, texture);
    return ret;
  }

  renderer = SDL_CreateRenderer(window, -1, 0);
  if (!renderer) {
    std::cout << "failed to create renderer by SDL..." << std::endl;
    CloseAboutAVdata(fmt_ctx, audio_codec_ctx, frame);
    CloseAboutWindow(window, renderer, texture);
    return ret;
  }

  Uint32 pixformat = SDL_PIXELFORMAT_IYUV;
  texture = SDL_CreateTexture(renderer, pixformat, SDL_TEXTUREACCESS_STREAMING,
                              window_width, window_height);
  if (!texture) {
    std::cout << "failed to create texture by SDL..." << std::endl;
    CloseAboutAVdata(fmt_ctx, audio_codec_ctx, frame);
    CloseAboutWindow(window, renderer, texture);
    return ret;
  }

  audio_buf = (uint8_t*)av_malloc((MAX_AUDIO_FRAME_SIZE * 3) / 2);
  while (av_read_frame(fmt_ctx, packet) >= 0) {
    if (packet->stream_index == audio_stream) {
      while (packet->size > 0) {
        ret = avcodec_send_packet(audio_codec_ctx, packet);
        if (ret < 0) {
          std::cout << "Error submitting the packet to the decoder"
                    << std::endl;
          packet->size = 0;
          break;
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

          //uint8_t* outbuf[1];
          //int outbuf_size = av_samples_get_buffer_size(
          //    nullptr, audio_codec_ctx->ch_layout.nb_channels,
          //    frame->nb_samples, AV_SAMPLE_FMT_S16, 1);
          //av_samples_alloc(outbuf, nullptr,
          //                 audio_codec_ctx->ch_layout.nb_channels,
          //                 frame->nb_samples, AV_SAMPLE_FMT_S16, 1);
          //swr_convert(audio_convert_ctx, outbuf, frame->nb_samples,
          //            (const uint8_t**)frame->data, frame->nb_samples);
          //av_freep(&outbuf);

          /*
          推荐使用这个，这个接口它会根据fmt去判断是否是planner格式，
          然后根据align参数进行缓冲区大小对齐，虽然我不知道是什么作用，但是有这个操作
          audio_codec_ctx->ch_layout.nb_channels * frame->nb_samples * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16)(AV_SAMPLE_FMT_S16的字节数)
          */
          /*int data_size = av_samples_get_buffer_size(
              NULL, audio_codec_ctx->ch_layout.nb_channels, frame->nb_samples,
              AV_SAMPLE_FMT_S16, 1);*/
          /*av_get_bytes_per_sample(audio_codec_ctx->sample_fmt) 是 fltp, float浮点类型，32bits = 4bytes*/

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
          audio_size =
              nb_samples * av_get_bytes_per_sample(audio_codec_ctx->sample_fmt);
          memcpy(audio_buf, out[0], audio_size);
          std::cout << "out_count: " << out_count
                    << " nb_samples: " << nb_samples << " per_sample: "
                    << av_get_bytes_per_sample(AV_SAMPLE_FMT_S16)
                    << " audio_size: " << audio_size << std::endl;
          if (audio_size > 0) {
            SDL_Delay(10);
          }
        }
      }
    } else {
      if (packet && packet->buf) av_packet_unref(packet);
    }

    SDL_PollEvent(&event);
    switch (event.type) {
      case SDL_QUIT:
        quit = 1;
        CloseAboutAVdata(fmt_ctx, audio_codec_ctx, frame);
        CloseAboutWindow(window, renderer, texture);
        return 0;
      default:
        break;
    }
  }

  if (audio_buf) {
    av_free(audio_buf);
  }
  SDL_CloseAudio();
  swr_free(&audio_convert_ctx);
  av_packet_free(&packet);
  av_frame_unref(frame);
  av_frame_free(&frame);
  ret = 0;
  CloseAboutAVdata(fmt_ctx, audio_codec_ctx, frame);
  CloseAboutWindow(window, renderer, texture);

  return ret;
}