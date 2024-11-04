// pcm数据可使用ffmpeg命令行的方式将mp3格式的音频转换为pcm，转换命令为：
// ffmpeg -i audio.mp3 -f s16le audio.pcm
#include <SDL.h>

#include <iostream>

// 这个大小最好根据设置的采样率，声道数和采样点格式来进行确定
// 每次读取2帧数据, 以2048个采样点一帧 2通道 16bit采样点为例
#define BLOCK_SIZE (2 * 2048 * 2 * 2)
//#define BLOCK_SIZE 4096000

const char* file_path = "../resource/thunder_sound.pcm";

static Uint8* audio_pos = nullptr;
static size_t buffer_len = 0;

// 回调的函数，音频设备需要更多数据的时候会调用该回调函数
// userdata：SDL_AudioSpec结构中的用户自定义数据，一般情况下可以不用。
// stream：该指针指向需要填充的音频缓冲区。
// len：音频缓冲区的大小（以字节为单位）。
void read_audio_data(void* userdata, Uint8* stream, int len) {
  if (buffer_len == 0) {
    return;
  }
  SDL_memset(stream, 0, len);

  // 声卡计算出来每次播放数据大小是len，所以每次拷贝的数据长度不能超过len
  len = (len < buffer_len) ? len : buffer_len;
  SDL_MixAudio(stream, audio_pos, len, SDL_MIX_MAXVOLUME);

  audio_pos += len;
  buffer_len -= len;
  std::cout << __FUNCTION__ << " buffer_len: " << buffer_len << " len: " << len
            << std::endl;
}

void Close(Uint8* audio_buf, FILE* audio_fd) {
  if (audio_buf) {
    free(audio_buf);
  }
  if (audio_fd) {
    fclose(audio_fd);
  }
  SDL_Quit();
}

int main(int argc, char* argv[]) {
  Uint8* audio_buf = (Uint8*)malloc(BLOCK_SIZE);
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
    std::cout << "SDL init failed: " << SDL_GetError() << std::endl;
    return -1;
  }

  FILE* audio_fd = nullptr;
  fopen_s(&audio_fd, file_path, "rb+");
  if (!audio_fd) {
    std::cout << "open pcm file failed..." << std::endl;
    Close(audio_buf, audio_fd);
    return -1;
  }

  SDL_AudioSpec spec;
  spec.freq = 44100;
  spec.format = AUDIO_S16SYS;  // 16 bits = 2 bytes
  spec.channels = 2;
  spec.silence = 0;
  // samples * channels * format = 8196 bytes（回调每次读取的数据）
  // 每次读取的采样数量
  spec.samples = 2048;
  // 多久调用一次回调和samples有关系（回调间隔 2048 / 44100）46.4ms, 设置延时超过这个数会出现断音现象
  spec.callback = read_audio_data;
  spec.userdata = NULL;

  if (SDL_OpenAudio(&spec, NULL)) {
    std::cout << "open audio device failed: " << SDL_GetError() << std::endl;
    Close(audio_buf, audio_fd);
    return -1;
  }

  SDL_PauseAudio(0);
  //do {
  //  buffer_len = fread(audio_buf, 1, BLOCK_SIZE, audio_fd);
  //  audio_pos = audio_buf;
  //  /*
  //      audio_pos在回调函数里面会改变。
  //      每隔一段时间(10ms)判断当前播放的地方是否已经到了audio_buf的尾部。
  //      如果没到尾部继续循环等待，如果到了尾部就表示audio_buf已经播放完了。
  //      然后再次读取BLOCK_SIZE大小的数据到audio_buf中，如此循环知道播放完为止
  //      备注：这种方式的buffer_len要比回调函数中的len尽量大一些，即BLOCK_SIZE要设置大一点
  //  */
  //  while (audio_pos < (audio_buf + buffer_len)) {
  //    SDL_Delay(10);
  //  }
  //} while (buffer_len != 0);

  // 如果BLOCK_SIZE设置为 < len的值的话，可使用下面方式处理
  while (1) {
    if (fread(audio_buf, 1, BLOCK_SIZE, audio_fd) != BLOCK_SIZE) {
      std::cout << "eof! play finish! " << std::endl;
      break;
    }
    buffer_len = BLOCK_SIZE;
    audio_pos = audio_buf;

    // 等待消耗音频数据
    while (buffer_len > 0) {
      SDL_Delay(10);
    }
  }

  SDL_CloseAudio();

  Close(audio_buf, audio_fd);
  return 0;
}