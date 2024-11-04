#pragma once

#ifndef MEDIA_MANAGER_H
#define MEDIA_MANAGER_H

#include "simple_player.h"

class MediaManager {
 public:
  virtual ~MediaManager();

  static MediaManager* getInstance() {
    static MediaManager* instance = nullptr;
    if (instance == nullptr) {
      instance = new MediaManager();
    }
    return instance;
  }

  void initialize(const char* filename);
  SimplePlayer* getPlayer() { return simple_player_; }

  void set_width(double width);
  void set_height(double height);
  void set_format(const char* input_name);
  void set_sync_type(const char* arg);
  void set_play_start_time(double play_start_time);
  void set_window_always_on_top(int always_on_top);
  void set_full_screen(int full_screen);
  void set_loop_count(int loop_count);
  void set_disable_to_show_video(int no_display);
  void set_audio_disable(int disable_audio);
  void set_video_disable(int disable_video);
  void set_disable_subtitling(int disable_subtitling);
  // 这个duration在read_thread里面使用到
  void set_play_duration(double play_duration);
  void set_is_need_seek_by_bytes(int is_need_seek_by_bytes);
  void set_window_bordless(int noborder);
  void set_start_volume(int volume);
  void set_auto_exit_at_end(int auto_exit);
  void set_exit_on_keydown(int keydown);
  void set_exit_on_mousedown(int mousedown);

 private:
  MediaManager();
  MediaManager(const MediaManager& other) = delete;
  MediaManager operator=(const MediaManager& other) = delete;

  const char* filename_;
  SimplePlayer* simple_player_;
};

#endif  // MEDIA_MANAGER_H