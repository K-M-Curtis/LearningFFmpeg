// pcm���ݿ�ʹ��ffmpeg�����еķ�ʽ��mp3��ʽ����Ƶת��Ϊpcm��ת������Ϊ��
// ffmpeg -i audio.mp3 -f s16le audio.pcm
#include <SDL.h>

#include <iostream>

// �����С��ø������õĲ����ʣ��������Ͳ������ʽ������ȷ��
// ÿ�ζ�ȡ2֡����, ��2048��������һ֡ 2ͨ�� 16bit������Ϊ��
#define BLOCK_SIZE (2 * 2048 * 2 * 2)
//#define BLOCK_SIZE 4096000

const char* file_path = "../resource/thunder_sound.pcm";

static Uint8* audio_pos = nullptr;
static size_t buffer_len = 0;

// �ص��ĺ�������Ƶ�豸��Ҫ�������ݵ�ʱ�����øûص�����
// userdata��SDL_AudioSpec�ṹ�е��û��Զ������ݣ�һ������¿��Բ��á�
// stream����ָ��ָ����Ҫ������Ƶ��������
// len����Ƶ�������Ĵ�С�����ֽ�Ϊ��λ����
void read_audio_data(void* userdata, Uint8* stream, int len) {
  if (buffer_len == 0) {
    return;
  }
  SDL_memset(stream, 0, len);

  // �����������ÿ�β������ݴ�С��len������ÿ�ο��������ݳ��Ȳ��ܳ���len
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
  // samples * channels * format = 8196 bytes���ص�ÿ�ζ�ȡ�����ݣ�
  // ÿ�ζ�ȡ�Ĳ�������
  spec.samples = 2048;
  // ��õ���һ�λص���samples�й�ϵ���ص���� 2048 / 44100��46.4ms, ������ʱ�������������ֶ�������
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
  //      audio_pos�ڻص����������ı䡣
  //      ÿ��һ��ʱ��(10ms)�жϵ�ǰ���ŵĵط��Ƿ��Ѿ�����audio_buf��β����
  //      ���û��β������ѭ���ȴ����������β���ͱ�ʾaudio_buf�Ѿ��������ˡ�
  //      Ȼ���ٴζ�ȡBLOCK_SIZE��С�����ݵ�audio_buf�У����ѭ��֪��������Ϊֹ
  //      ��ע�����ַ�ʽ��buffer_lenҪ�Ȼص������е�len������һЩ����BLOCK_SIZEҪ���ô�һ��
  //  */
  //  while (audio_pos < (audio_buf + buffer_len)) {
  //    SDL_Delay(10);
  //  }
  //} while (buffer_len != 0);

  // ���BLOCK_SIZE����Ϊ < len��ֵ�Ļ�����ʹ�����淽ʽ����
  while (1) {
    if (fread(audio_buf, 1, BLOCK_SIZE, audio_fd) != BLOCK_SIZE) {
      std::cout << "eof! play finish! " << std::endl;
      break;
    }
    buffer_len = BLOCK_SIZE;
    audio_pos = audio_buf;

    // �ȴ�������Ƶ����
    while (buffer_len > 0) {
      SDL_Delay(10);
    }
  }

  SDL_CloseAudio();

  Close(audio_buf, audio_fd);
  return 0;
}