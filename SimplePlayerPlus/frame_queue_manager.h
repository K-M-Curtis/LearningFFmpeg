#pragma once

#ifndef FRAME_QUEUE_MANAGER_H
#define FRAME_QUEUE_MANAGER_H

#include "common_param.h"
#include "packet_queue_manager.h"

/* 帧数据，为了⾳频、视频、字幕帧通⽤，部分成员只对特定类型 */
typedef struct Frame {
  AVFrame* frame;
  AVSubtitle sub;
  int serial;
  double pts;  // 当前帧的时间戳
  double duration;
  int64_t pos;
  int width;
  int height;
  int format;
  AVRational sar;  // 帧的宽高比，该值来⾃AVFrame结构体的sample_aspect_ratio变量
  int uploaded;  // 标志帧是否已上传到渲染缓冲区，即当前帧是否已显示
  // int flip_v;  // 帧是否需要垂直翻转
} Frame;

/* ========= FrameQueue =========
 帧队列，用数组实现的一个FIFO，一端写一端读
 引入了读取节点但节点不出队列的操作、读取下一节点也不出队列等等的操作
 缓存从解码器获取的视频/音频帧数据,以便于后续的播放。这样可以减少解码器和播放器之间的同步压力
 多线程环境，解码线程和播放线程可以并发地操作帧队列，提高整体的执行效率
*/
typedef struct FrameQueue {
  Frame queue[FRAME_QUEUE_SIZE];
  int rindex;  // 读索引。待播放时读取此帧进行播放，播放后此帧成为上一帧
  int windex;  // 写索引
  int size;    // 当前总帧数
  int max_size;
  int keep_last;     // 是否保持最后一帧
  int rindex_shown;  // 初始化为0，配合keep_last=1使用
  SDL_mutex* mutex;
  SDL_cond* cond;
  PacketQueue* p_queue;
} FrameQueue;

class FrameQueueManager {
 public:
  static int frame_queue_init(FrameQueue* f_queue, PacketQueue* p_queue,
                              int max_size, int keep_last);
  static void frame_queue_push(FrameQueue* f_queue);
  static void frame_queue_next(FrameQueue* f_queue);

  static void frame_queue_destroy(FrameQueue* f_queue);
  static void frame_queue_signal(FrameQueue* f_queue);
  static Frame* frame_queue_peek(FrameQueue* f_queue);
  static Frame* frame_queue_peek_next(FrameQueue* f_queue);
  static Frame* frame_queue_peek_last(FrameQueue* f_queue);
  static Frame* frame_queue_peek_writable(FrameQueue* f_queue);
  static Frame* frame_queue_peek_readable(FrameQueue* f_queue);
  static int frame_queue_nb_remaining(FrameQueue* f_queue);
  static int64_t frame_queue_last_pos(FrameQueue* f_queue);

 private:
  static void frame_queue_unref_item(Frame* frame);

  //FrameQueue* frame_queue;
};

#endif  // FRAME_QUEUE_MANAGER_H