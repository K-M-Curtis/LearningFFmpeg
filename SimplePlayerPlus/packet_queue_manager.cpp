#include "packet_queue_manager.h"

int PacketQueueManager::packet_queue_init(PacketQueue* p_queue) {
  memset(p_queue, 0, sizeof(PacketQueue));
  p_queue->pkt_list =
      av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
  if (!p_queue->pkt_list) return AVERROR(ENOMEM);
  p_queue->mutex = SDL_CreateMutex();
  if (!p_queue->mutex) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  p_queue->cond = SDL_CreateCond();
  if (!p_queue->cond) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  p_queue->abort_request = 1;
  return 0;
}

void PacketQueueManager::packet_queue_start(PacketQueue* p_queue) {
  SDL_LockMutex(p_queue->mutex);
  p_queue->abort_request = 0;
  p_queue->serial++;
  SDL_UnlockMutex(p_queue->mutex);
}

int PacketQueueManager::packet_queue_put(PacketQueue* p_queue, AVPacket* pkt) {
  AVPacket* pkt1;
  int ret;

  pkt1 = av_packet_alloc();
  if (!pkt1) {
    av_packet_unref(pkt);
    return -1;
  }
  av_packet_move_ref(pkt1, pkt);

  SDL_LockMutex(p_queue->mutex);
  ret = packet_queue_put_private(p_queue, pkt1);
  SDL_UnlockMutex(p_queue->mutex);
  if (ret < 0) {
    av_packet_free(&pkt1);
  }
  return ret;
}

// 放⼊空包意味着流的结束，⼀般在媒体数据读取完成的时候放⼊空包。放⼊空包，⽬的是为了冲刷解码器，将编码器⾥⾯所有frame都读取出来
int PacketQueueManager::packet_queue_put_nullpacket(PacketQueue* p_queue,
                                                    AVPacket* pkt,
                                                    int stream_index) {
  pkt->stream_index = stream_index;
  return packet_queue_put(p_queue, pkt);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
int PacketQueueManager::packet_queue_get(PacketQueue* p_queue, AVPacket* pkt,
                                         int block, int* serial) {
  MyAVPacketList pkt1;
  int ret;

  SDL_LockMutex(p_queue->mutex);

  while (1) {
    if (p_queue->abort_request) {
      ret = -1;
      break;
    }

    if (av_fifo_read(p_queue->pkt_list, &pkt1, 1) >= 0) {
      p_queue->nb_packets--;
      p_queue->size -= pkt1.pkt->size + sizeof(pkt1);
      p_queue->duration -= pkt1.pkt->duration;
      av_packet_move_ref(pkt, pkt1.pkt);
      if (serial) {
        *serial = pkt1.serial;
      }
      av_packet_free(&pkt1.pkt);
      ret = 1;
      break;
    } else if (!block) {
      ret = 0;
      break;
    } else {
      SDL_CondWait(p_queue->cond, p_queue->mutex);
    }
  }
  SDL_UnlockMutex(p_queue->mutex);
  return ret;
}

// ⽤于将packet队列中的所有节点清除，包括节点对应的AVPacket。⽐如⽤于退出播放和seek播放
void PacketQueueManager::packet_queue_flush(PacketQueue* p_queue) {
  MyAVPacketList pkt1;

  SDL_LockMutex(p_queue->mutex);
  while (av_fifo_read(p_queue->pkt_list, &pkt1, 1) >= 0) {
    av_packet_free(&pkt1.pkt);
  }
  p_queue->nb_packets = 0;
  p_queue->size = 0;
  p_queue->duration = 0;
  p_queue->serial++;
  SDL_UnlockMutex(p_queue->mutex);
}

void PacketQueueManager::packet_queue_abort(PacketQueue* p_queue) {
  SDL_LockMutex(p_queue->mutex);
  p_queue->abort_request = 1;
  SDL_CondSignal(p_queue->cond);
  SDL_UnlockMutex(p_queue->mutex);
}

void PacketQueueManager::packet_queue_destroy(PacketQueue* p_queue) {
  packet_queue_flush(p_queue);
  av_fifo_freep2(&p_queue->pkt_list);
  SDL_DestroyMutex(p_queue->mutex);
  SDL_DestroyCond(p_queue->cond);
}

int PacketQueueManager::packet_queue_put_private(PacketQueue* p_queue,
                                                 AVPacket* pkt) {
  MyAVPacketList tmp_pkt;
  int ret;

  if (p_queue->abort_request) return -1;

  tmp_pkt.pkt = pkt;
  tmp_pkt.serial = p_queue->serial;

  // av_fifo_write确保数据包按它们到达的顺序被存储和处理。
  ret = av_fifo_write(p_queue->pkt_list, &tmp_pkt, 1);
  if (ret < 0) return ret;
  p_queue->nb_packets++;
  p_queue->size += tmp_pkt.pkt->size + sizeof(tmp_pkt);
  p_queue->duration += tmp_pkt.pkt->duration;
  SDL_CondSignal(p_queue->cond);
  return 0;
}
