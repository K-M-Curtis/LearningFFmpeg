#include "media_manager.h"

MediaManager::MediaManager() {}

MediaManager::~MediaManager() {
  if (simple_player_) {
    delete simple_player_;
  }
}

void MediaManager::initialize(const char* filename) {
  filename_ = filename;
  simple_player_ = new SimplePlayer(filename_);
}

void MediaManager::set_width(double width) {
  if (width > 0 && simple_player_) simple_player_->set_width(width);
}

void MediaManager::set_height(double height) {
  if (height > 0 && simple_player_) simple_player_->set_height(height);
}

void MediaManager::set_format(const char* input_name) {
  if (simple_player_) simple_player_->set_format(input_name);
}

void MediaManager::set_sync_type(const char* arg) {
  if (simple_player_) simple_player_->set_sync_type(arg);
}

void MediaManager::set_play_start_time(double play_start_time) {
  if (simple_player_) simple_player_->set_play_start_time(play_start_time);
}

void MediaManager::set_window_always_on_top(int always_on_top) {
  if (simple_player_) simple_player_->set_window_always_on_top(always_on_top);
}

void MediaManager::set_full_screen(int full_screen) {
  if (simple_player_) simple_player_->set_full_screen(full_screen);
}

void MediaManager::set_loop_count(int loop_count) {
  if (simple_player_) simple_player_->set_loop_count(loop_count);
}

void MediaManager::set_disable_to_show_video(int no_display) {
  if (simple_player_) simple_player_->set_disable_to_show_video(no_display);
}

void MediaManager::set_audio_disable(int disable_audio) {
  if (simple_player_) simple_player_->set_audio_disable(disable_audio);
}

void MediaManager::set_video_disable(int disable_video) {
  if (simple_player_) simple_player_->set_video_disable(disable_video);
}

void MediaManager::set_disable_subtitling(int disable_subtitling) {
  if (simple_player_)
    simple_player_->set_disable_subtitling(disable_subtitling);
}

// 这个duration在read_thread里面使用到
void MediaManager::set_play_duration(double play_duration) {
  if (simple_player_) simple_player_->set_play_duration(play_duration);
}

void MediaManager::set_is_need_seek_by_bytes(int is_need_seek_by_bytes) {
  if (simple_player_)
    simple_player_->set_is_need_seek_by_bytes(is_need_seek_by_bytes);
}

void MediaManager::set_window_bordless(int noborder) {
  if (simple_player_) simple_player_->set_window_bordless(noborder);
}

void MediaManager::set_start_volume(int volume) {
  if (simple_player_) simple_player_->set_start_volume(volume);
}

void MediaManager::set_auto_exit_at_end(int auto_exit) {
  if (simple_player_) simple_player_->set_auto_exit_at_end(auto_exit);
}

void MediaManager::set_exit_on_keydown(int keydown) {
  if (simple_player_) simple_player_->set_exit_on_keydown(keydown);
}

void MediaManager::set_exit_on_mousedown(int mousedown) {
  if (simple_player_) simple_player_->set_exit_on_mousedown(mousedown);
}