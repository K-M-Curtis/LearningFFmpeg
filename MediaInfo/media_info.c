#include <libavformat/avformat.h>

int main(int argc, char* argv[]) {
  if (argc < 2) {
    fprintf(stderr, "[Usage]you should input media file!\n");
    return -2;
  }
  char* src_filename = argv[1];
  int err_code;
  char errors[1024];
  AVFormatContext* fmt_ctx = NULL;

  //av_register_all();

  //open input file, and allocate format context
  if ((err_code = avformat_open_input(&fmt_ctx, src_filename, NULL, NULL)) < 0) {
    av_strerror(err_code, errors, 1024);
    fprintf(stderr, "Could not open source file %s, %d(%s)\n", src_filename, err_code, errors);
    exit(1);
  }

  //retreive stream information
  if ((err_code = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
    av_strerror(err_code, errors, 1024);
    fprintf(stderr, "Could not find stream info %d(%s)\n", err_code, errors);
    exit(1);
  }

  //dump input information to stderr
  av_dump_format(fmt_ctx, 0, src_filename, 0);

  //close input file
  avformat_close_input(&fmt_ctx);

  return 0;
}


//#include <stdio.h>
//
//#include <libavformat/avformat.h>
//#include <libavutil/dict.h>
//
//int main(int argc, char** argv) {
//  AVFormatContext* fmt_ctx = NULL;
//  const AVDictionaryEntry* tag = NULL;
//  int ret;
//
//  if (argc != 2) {
//    printf("usage: %s <input_file>\n"
//      "example program to demonstrate the use of the libavformat metadata API.\n"
//      "\n", argv[0]);
//    return 1;
//  }
//
//  if ((ret = avformat_open_input(&fmt_ctx, argv[1], NULL, NULL)))
//    return ret;
//
//  if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
//    av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
//    return ret;
//  }
//
//  while ((tag = av_dict_get(fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
//    printf("%s=%s\n", tag->key, tag->value);
//
//  avformat_close_input(&fmt_ctx);
//  return 0;
//}