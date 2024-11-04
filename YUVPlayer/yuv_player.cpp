#include <SDL.h>

#include <iostream>

#define BLOCK_SIZE 4096000

const char file_path[] = "../resource/sintel_640_360.yuv";

//Refresh Event
#define REFRESH_EVENT (SDL_USEREVENT + 1)
#define QUIT_EVENT (SDL_USEREVENT + 2)

int thread_exit = 0;

int refresh_video_timer(void* udata) {
  thread_exit = 0;
  while (!thread_exit) {
    SDL_Event event;
    event.type = REFRESH_EVENT;
    SDL_PushEvent(&event);
    // delay 40ms，即渲染一帧花40ms，一秒钟渲染25帧，符合人类视觉感受
    SDL_Delay(40);
  }
  thread_exit = 0;

  // push quit event
  SDL_Event event;
  event.type = QUIT_EVENT;
  SDL_PushEvent(&event);
  return 0;
}

void Close(SDL_Window* window, FILE* file_fd) {
  if (file_fd) {
    fclose(file_fd);
  }
  SDL_DestroyWindow(window);
  SDL_Quit();
}

int main(int argc, char* argv[]) {
  FILE* video_fd = NULL;
  int window_width = 640;
  int window_height = 400;
  // **YUV播放需要知道视频的宽高**
  int video_width = 640;
  int video_height = 360;

  if (SDL_Init(SDL_INIT_VIDEO)) {
    std::cout << "init SDL failed: " << SDL_GetError() << std::endl;
    return false;
  }

  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  SDL_Texture* texture = nullptr;

  window = SDL_CreateWindow(
      "YUV Player", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
      window_width, window_height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (!window) {
    std::cout << "create window failed: " << SDL_GetError() << std::endl;
    Close(window, video_fd);
    return -1;
  }

  renderer = SDL_CreateRenderer(window, -1, 0);
  if (!renderer) {
    std::cout << "create renderer failed: " << SDL_GetError() << std::endl;
    Close(window, video_fd);
    return -1;
  }

  //IYUV: Y + U + V  (3 planes)
  //YV12: Y + V + U  (3 planes)
  Uint32 pixformat = SDL_PIXELFORMAT_IYUV;
  texture = SDL_CreateTexture(renderer, pixformat, SDL_TEXTUREACCESS_STREAMING,
                              video_width, video_height);
  if (!texture) {
    std::cout << "create texture failed: " << SDL_GetError() << std::endl;
    Close(window, video_fd);
    return -1;
  }

  fopen_s(&video_fd, file_path, "rb+");
  if (!video_fd) {
    std::cout << "Failed to open yuv file..." << std::endl;
    Close(window, video_fd);
    return -1;
  }

  SDL_Thread* timer_thread = nullptr;
  timer_thread = SDL_CreateThread(refresh_video_timer, NULL, NULL);

  SDL_Event event;
  SDL_Rect rect;

  // 一个yuv图片的长度
  /*
        这个公式用于计算YUV格式视频每帧数据的长度（字节数）。
        YUV是一种基于颜色空间的视频格式，其中每个像素点不是一个RGB变量，而是由一个Luma（亮度）和两个色度（Chroma）组成的，
        通常用YUV 4:2:0的格式存储。在YUV 4:2:0格式中，Y分量的采样率是1，而U和V分量的采样率是1/2，即每四个Y像素只有一个U和V像素。因此，每个像素点需要1.5个字节的存储空间。

        根据这个公式，假设视频的宽度是video_width，高度是video_height，每个像素点需要1.5个字节的存储空间，那么一个YUV帧的大小可以表示为：

        yuv_frame_len = video_width * video_height * 1.5

        这个公式中12/8用于将1.5个字节转化为12位，然后再将结果转化为字节。
    *
    */
  const unsigned int temp_yuv_frame_len = video_width * video_height * 12 / 8;
  unsigned int yuv_frame_len = temp_yuv_frame_len;
  if (temp_yuv_frame_len & 0xF) {
    yuv_frame_len = (temp_yuv_frame_len & 0xFFF0) + 0x10;
  }

  //Uint8* video_buf[BLOCK_SIZE];  // 大内存的分配需要使用堆空间，使用栈分配大内存的话，会报错
  //Uint8* video_buf = (Uint8*)malloc(BLOCK_SIZE);
  Uint8* video_buf = (Uint8*)malloc(yuv_frame_len);

  while (true) {
    // 等待有效事件
    SDL_WaitEvent(&event);
    if (event.type == REFRESH_EVENT) {
      // 循环播放
      //if (fread(video_buf, 1, yuv_frame_len, video_fd) != yuv_frame_len) {
      //  // Loop
      //  fseek(video_fd, 0, SEEK_SET);
      //  fread(video_buf, 1, yuv_frame_len, video_fd);
      //}

      if (fread(video_buf, 1, yuv_frame_len, video_fd) <= 0) {
        std::cout << "eof, finish playing, exit thread!" << std::endl;
        thread_exit = 1;
        continue;
      }

      SDL_UpdateTexture(texture, NULL, video_buf, video_width);

      //FIX: If window is resize
      rect.x = 0;
      rect.y = 0;
      rect.w = window_width;
      rect.h = window_height;

      SDL_RenderClear(renderer);
      SDL_RenderCopy(renderer, texture, NULL, &rect);
      SDL_RenderPresent(renderer);
    } else if (event.type == SDL_WINDOWEVENT) {
      SDL_GetWindowSize(window, &window_width, &window_height);
    } else if (event.type == SDL_QUIT) {
      thread_exit = 1;
    } else if (event.type == QUIT_EVENT) {
      break;
    }
  }

  if (video_buf) {
    free(video_buf);
  }
  Close(window, video_fd);
  return 0;
}