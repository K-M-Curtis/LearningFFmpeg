#include <SDL.h>

#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

void CloseAboutAVdata(AVFormatContext* fmt_ctx, AVCodecContext* codec_ctx,
                      AVFrame* frame) {
  if (frame) av_frame_free(&frame);
  if (codec_ctx) avcodec_close(codec_ctx);
  if (fmt_ctx) avformat_close_input(&fmt_ctx);
}

void CloseAboutWindow(SDL_Window* win, SDL_Renderer* renderer,
                      SDL_Texture* texture) {
  if (win) SDL_DestroyWindow(win);
  if (renderer) SDL_DestroyRenderer(renderer);
  if (texture) SDL_DestroyTexture(texture);
  SDL_Quit();
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
  AVCodecContext* codec_ctx = NULL;
  struct SwsContext* sws_ctx = NULL;
  const AVCodec* video_codec = NULL;
  AVFrame* frame = NULL;
  AVPacket* packet = NULL;

  if (avformat_open_input(&fmt_ctx, argv[1], NULL, NULL)) {
    std::cout << "failed to open video file..." << std::endl;
    CloseAboutAVdata(fmt_ctx, codec_ctx, frame);
    return ret;
  }

  if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
    std::cout << "failed to find stream information..." << std::endl;
    CloseAboutAVdata(fmt_ctx, codec_ctx, frame);
    return ret;
  }

  std::cout << "=========================" << std::endl;
  av_dump_format(fmt_ctx, 0, argv[1], 0);
  std::cout << "=========================" << std::endl;

  int video_stream = -1;
  for (int i = 0; i < fmt_ctx->nb_streams; i++) {
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream = i;
      break;
    }
  }

  if (video_stream == -1) {
    std::cout << "Cannot find the video stream..." << std::endl;
    CloseAboutAVdata(fmt_ctx, codec_ctx, frame);
    return ret;
  }

  video_codec =
      avcodec_find_decoder(fmt_ctx->streams[video_stream]->codecpar->codec_id);
  if (!video_codec) {
    std::cout << "Cannot supported the codec..." << std::endl;
    CloseAboutAVdata(fmt_ctx, codec_ctx, frame);
    return ret;
  }

  codec_ctx = avcodec_alloc_context3(video_codec);
  if (avcodec_parameters_to_context(
          codec_ctx, fmt_ctx->streams[video_stream]->codecpar) < 0) {
    std::cout << "failed to copy the in streams codecpar to codec context..."
              << std::endl;
    CloseAboutAVdata(fmt_ctx, codec_ctx, frame);
    return ret;
  }

  if (avcodec_open2(codec_ctx, video_codec, NULL) < 0) {
    std::cout << "failed to open decoder..." << std::endl;
    CloseAboutAVdata(fmt_ctx, codec_ctx, frame);
    return ret;
  }

  frame = av_frame_alloc();
  packet = av_packet_alloc();
  window_width = codec_ctx->width;
  window_height = codec_ctx->height;

  SDL_Window* window = NULL;
  SDL_Renderer* renderer = NULL;
  SDL_Texture* texture = NULL;
  SDL_Rect rect;

  window = SDL_CreateWindow(
      "Media Player", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
      window_width, window_height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (!window) {
    std::cout << "failed to create window by SDL..." << std::endl;
    CloseAboutAVdata(fmt_ctx, codec_ctx, frame);
    CloseAboutWindow(window, renderer, texture);
    return ret;
  }

  renderer = SDL_CreateRenderer(window, -1, 0);
  if (!renderer) {
    std::cout << "failed to create renderer by SDL..." << std::endl;
    CloseAboutAVdata(fmt_ctx, codec_ctx, frame);
    CloseAboutWindow(window, renderer, texture);
    return ret;
  }

  Uint32 pixformat = SDL_PIXELFORMAT_IYUV;
  texture = SDL_CreateTexture(renderer, pixformat, SDL_TEXTUREACCESS_STREAMING,
                              window_width, window_height);
  if (!texture) {
    std::cout << "failed to create texture by SDL..." << std::endl;
    CloseAboutAVdata(fmt_ctx, codec_ctx, frame);
    CloseAboutWindow(window, renderer, texture);
    return ret;
  }

  sws_ctx = sws_getContext(
      codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt, codec_ctx->width,
      codec_ctx->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);

  // AVPicture 在新的ffmpeg版本中已不再推荐使用，并且已移除
  // 替代方案时使用AVFrame结构，这是一个更通用，更安全的处理帧数据的方式
  // 1. 分配AVFrame结构
  AVFrame* pic_frame = av_frame_alloc();
  // 2. 设置帧参数
  pic_frame->format = AV_PIX_FMT_YUV420P;
  pic_frame->width = codec_ctx->width;
  pic_frame->height = codec_ctx->height;
  // 3. 分配帧的缓冲区
  /* allocate the buffers for the frame data */
  if (av_frame_get_buffer(pic_frame, 0) < 0) {
    av_frame_free(&pic_frame);
    CloseAboutAVdata(fmt_ctx, codec_ctx, frame);
    CloseAboutWindow(window, renderer, texture);
    return ret;
  }

  while (av_read_frame(fmt_ctx, packet) >= 0) {
    // Is this the packet from the video stream?
    if (packet->stream_index == video_stream) {
      // decode video frame
      ret = avcodec_send_packet(codec_ctx, packet);  // 返回0代表success
      if (ret < 0) {
        std::cout << "Error sending the packet to the decoder..." << std::endl;
        continue;
      }

      while (ret >= 0) {
        // 获取解码出的frame，一个packet可能会解码出很多的frame
        ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR(EOF)) {
          break;
        } else if (ret < 0) {
          std::cout << "Error during decoding..." << std::endl;
          break;
        }
        fflush(stdout);

        sws_scale(sws_ctx, (const uint8_t* const*)frame->data, frame->linesize,
                  0, codec_ctx->height, pic_frame->data, pic_frame->linesize);
        // 4. 使用帧数据
        SDL_UpdateYUVTexture(texture, NULL, pic_frame->data[0],
                             pic_frame->linesize[0], pic_frame->data[1],
                             pic_frame->linesize[1], pic_frame->data[2],
                             pic_frame->linesize[2]);

        rect.x = 0;
        rect.y = 0;
        rect.w = codec_ctx->width;
        rect.h = codec_ctx->height;

        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, &rect);
        SDL_RenderPresent(renderer);
        SDL_Delay(25);
      }
    }
    av_packet_unref(packet);
  }

  av_packet_free(&packet);
  // 5. 释放帧资源
  av_frame_unref(pic_frame);
  av_frame_free(&pic_frame);

  ret = 0;
  CloseAboutAVdata(fmt_ctx, codec_ctx, frame);
  CloseAboutWindow(window, renderer, texture);
  return ret;
}
