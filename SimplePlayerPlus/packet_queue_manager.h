#pragma once
// 防止头文件或其他代码片段被多次包含（include）而导致的重复定义问题。
#ifndef PACKET_QUEUE_MANAGER_H
#define PACKET_QUEUE_MANAGER_H

extern "C" {
#include <SDL.h>
#include <libavcodec/avcodec.h>
#include <libavutil/fifo.h>
}

typedef struct MyAVPacketList {
  AVPacket* pkt;
  int serial;
} MyAVPacketList;

typedef struct PacketQueue {
  AVFifo* pkt_list;
  int nb_packets;
  int size;
  int64_t duration;
  int abort_request;
  int serial;
  SDL_mutex* mutex;
  SDL_cond* cond;
} PacketQueue;

class PacketQueueManager {
 public:
  static int packet_queue_init(PacketQueue* p_queue);
  static void packet_queue_start(PacketQueue* p_queue);
  static int packet_queue_put(PacketQueue* p_queue, AVPacket* pkt);
  static int packet_queue_put_nullpacket(PacketQueue* p_queue, AVPacket* pkt,
                                         int stream_index);
  static int packet_queue_get(PacketQueue* p_queue, AVPacket* pkt, int block,
                              int* serial);
  static void packet_queue_flush(PacketQueue* p_queue);
  static void packet_queue_abort(PacketQueue* p_queue);
  static void packet_queue_destroy(PacketQueue* p_queue);

 private:
  static int packet_queue_put_private(PacketQueue* p_queue, AVPacket* pkt);

  //PacketQueue* packet_queue;
};

#endif  // PACKET_QUEUE_MANAGER_H