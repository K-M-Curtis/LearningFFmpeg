#include "frame_queue_manager.h"

int FrameQueueManager::frame_queue_init(FrameQueue* f_queue,
                                        PacketQueue* p_queue, int max_size,
                                        int keep_last) {
  memset(f_queue, 0, sizeof(FrameQueue));
  if (!(f_queue->mutex = SDL_CreateMutex())) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }

  if (!(f_queue->cond = SDL_CreateCond())) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  f_queue->p_queue = p_queue;
  f_queue->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
  f_queue->keep_last = !!keep_last;
  for (int i = 0; i < f_queue->max_size; i++) {
    if (!(f_queue->queue[i].frame = av_frame_alloc())) {
      return AVERROR(ENOMEM);
    }
  }
  return 0;
}

void FrameQueueManager::frame_queue_push(FrameQueue* f_queue) {
  if (++f_queue->windex == f_queue->max_size) {
    f_queue->windex = 0;
  }
  SDL_LockMutex(f_queue->mutex);
  f_queue->size++;
  SDL_CondSignal(f_queue->cond);
  SDL_UnlockMutex(f_queue->mutex);
}

// 将读取位置推进到下一个位置，减少队列中的帧数，并发送条件信号。
// 更新读索引(同时删除旧frame)
void FrameQueueManager::frame_queue_next(FrameQueue* f_queue) {
  if (f_queue->keep_last && !f_queue->rindex_shown) {
    f_queue->rindex_shown = 1;
    return;
  }
  frame_queue_unref_item(&f_queue->queue[f_queue->rindex]);
  if (++f_queue->rindex == f_queue->max_size) {
    f_queue->rindex = 0;
  }
  SDL_LockMutex(f_queue->mutex);
  f_queue->size--;
  SDL_CondSignal(f_queue->cond);
  SDL_UnlockMutex(f_queue->mutex);
}

void FrameQueueManager::frame_queue_destroy(FrameQueue* f_queue) {
  for (int i = 0; i < f_queue->max_size; i++) {
    Frame* vp = &f_queue->queue[i];
    frame_queue_unref_item(vp);
    av_frame_free(&vp->frame);
  }
  SDL_DestroyMutex(f_queue->mutex);
  SDL_DestroyCond(f_queue->cond);
}

// 发送条件信号，通知等待的线程
void FrameQueueManager::frame_queue_signal(FrameQueue* f_queue) {
  SDL_LockMutex(f_queue->mutex);
  SDL_CondSignal(f_queue->cond);
  SDL_UnlockMutex(f_queue->mutex);
}

// 返回当前读取位置的帧（不移除）
Frame* FrameQueueManager::frame_queue_peek(FrameQueue* f_queue) {
  return &f_queue->queue[(f_queue->rindex + f_queue->rindex_shown) %
                         f_queue->max_size];
}

// 返回下一读取位置的帧（不移除）
Frame* FrameQueueManager::frame_queue_peek_next(FrameQueue* f_queue) {
  return &f_queue->queue[(f_queue->rindex + f_queue->rindex_shown + 1) %
                         f_queue->max_size];
}

/* ========= 获取last Frame，返回最后读取位置的帧 =========
 * 当rindex_shown=0时，和frame_queue_peek效果一样
 * 当rindex_shown=1时，读取的是已经显示过的frame
*/
Frame* FrameQueueManager::frame_queue_peek_last(FrameQueue* f_queue) {
  return &f_queue->queue[f_queue->rindex];
}

// 等待直到有可写入的新帧空间，返回可写入帧的指针
Frame* FrameQueueManager::frame_queue_peek_writable(FrameQueue* f_queue) {
  SDL_LockMutex(f_queue->mutex);
  while (f_queue->size >= f_queue->max_size &&
         !f_queue->p_queue->abort_request) {
    SDL_CondWait(f_queue->cond, f_queue->mutex);
  }
  SDL_UnlockMutex(f_queue->mutex);

  if (f_queue->p_queue->abort_request) {
    return NULL;
  }
  return &f_queue->queue[f_queue->windex];
}

Frame* FrameQueueManager::frame_queue_peek_readable(FrameQueue* f_queue) {
  SDL_LockMutex(f_queue->mutex);
  while (f_queue->size - f_queue->rindex_shown <= 0 &&
         !f_queue->p_queue->abort_request) {
    SDL_CondWait(f_queue->cond, f_queue->mutex);
  }
  SDL_UnlockMutex(f_queue->mutex);

  if (f_queue->p_queue->abort_request) {
    return NULL;
  }
  return &f_queue->queue[(f_queue->rindex + f_queue->rindex_shown) %
                         f_queue->max_size];
}

/* return the number of undisplayed frames in the queue */
int FrameQueueManager::frame_queue_nb_remaining(FrameQueue* f_queue) {
  return f_queue->size - f_queue->rindex_shown;
}

// 获取最近播放Frame对应数据在媒体⽂件的位置，主要在seek时使⽤
/* return last shown position */
int64_t FrameQueueManager::frame_queue_last_pos(FrameQueue* f_queue) {
  Frame* fp = &f_queue->queue[f_queue->rindex];
  if (f_queue->rindex_shown && fp->serial == f_queue->p_queue->serial)
    return fp->pos;
  else
    return -1;
}

void FrameQueueManager::frame_queue_unref_item(Frame* frame) {
  av_frame_unref(frame->frame);
  avsubtitle_free(&frame->sub);
}