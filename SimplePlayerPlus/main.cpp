#include <signal.h>

#include "media_manager.h"
#include "simple_player.h"

static void sigterm_handler(int sig) {
  /*
   终止当前进程的函数调用。
   当调用 exit() 函数时,进程会以传入的参数值(这里是 1001)作为退出状态码退出。
   退出状态码是一个整数值,通常用于表示进程的退出状态。零表示正常退出,非零表示异常退出。
   在 Unix/Linux 系统中,退出状态码可以被父进程或操作系统读取,用于判断子进程的执行结果。
  */
  exit(1001);
}

static void show_usage(void) {
  av_log(NULL, AV_LOG_INFO, "Simple media player\n");
  av_log(NULL, AV_LOG_INFO, "usage: %s [options] input_file\n", program_name);
  av_log(NULL, AV_LOG_INFO, "\n");
  printf(
      "\nWhile playing:\n"
      "q, ESC              quit\n"
      "f                   toggle full screen\n"
      "SPC                 pause\n"
      "m                   toggle mute\n"
      "9, 0                decrease and increase volume respectively\n"
      "/, *                decrease and increase volume respectively\n"
      "s                   activate frame-step mode\n"
      "left/right          seek backward/forward 10 seconds or to custom "
      "interval if -seek_interval is set\n"
      "down/up             seek backward/forward 1 minute\n"
      "page down/page up   seek backward/forward 10 minutes\n"
      "right mouse click   seek to percentage in file corresponding to "
      "fraction of width\n"
      "left double-click   toggle full screen\n");
}

int main(int argc, char** argv) {
  if (argc < 2 || !argv[1]) {
    show_usage();
    exit(1);
  }

  signal(SIGINT, sigterm_handler);  /* Interrupt (ANSI).    */
  signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */

  MediaManager::getInstance()->initialize(argv[1]);
  SimplePlayer* player = MediaManager::getInstance()->getPlayer();
  if (player) {
    player->Runable();
  } else {
    printf("Create player failed...\n");
  }

  return 0;
}