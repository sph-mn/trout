#ifndef sph_thread_pool_c_included
#define sph_thread_pool_c_included

#include <errno.h>
#include <stdlib.h>
#include <trout/sph/queue.c>
#include <trout/sph/thread-pool.h>

int sph_thread_pool_destroy(sph_thread_pool_t* a) {
  if (a->size || !a->shutdown) {
    return EBUSY;
  };
  pthread_cond_destroy(&a->queue_not_empty);
  pthread_mutex_destroy(&a->queue_mutex);
  return 0;
}

int sph_thread_pool_enqueue(sph_thread_pool_t* a, sph_thread_pool_task_t* task) {
  pthread_mutex_lock(&a->queue_mutex);
  if (!a->accepting) {
    pthread_mutex_unlock(&a->queue_mutex);
    return EBUSY;
  };
  sph_queue_enq(&a->queue, &task->q);
  pthread_cond_signal(&a->queue_not_empty);
  pthread_mutex_unlock(&a->queue_mutex);
  return 0;
}

void* sph_thread_pool_worker(void* data) {
  sph_thread_pool_worker_data_t* worker;
  sph_thread_pool_t* a;
  sph_thread_pool_task_t* task;
  worker = data;
  a = worker->pool;
  while (1) {
    pthread_mutex_lock(&a->queue_mutex);
    while (!a->queue.size && !a->shutdown && !a->retire[worker->index]) {
      pthread_cond_wait(&a->queue_not_empty, &a->queue_mutex);
    };
    if ((!a->shutdown && a->retire[worker->index]) ||
        (!a->queue.size && (a->shutdown || a->retire[worker->index]))) {
      pthread_mutex_unlock(&a->queue_mutex);
      return 0;
    };
    task = sph_queue_get(sph_queue_deq(&a->queue), sph_thread_pool_task_t, q);
    pthread_mutex_unlock(&a->queue_mutex);
    task->f(task);
  };
}

void sph_thread_finish(sph_thread_pool_task_t* task) {
  free(task);
  pthread_exit(0);
}

int sph_thread_pool_init(sph_thread_pool_size_t size, sph_thread_pool_t* a) {
  sph_thread_pool_size_t i;
  int error;
  sph_queue_init(&a->queue);
  error = pthread_mutex_init(&a->queue_mutex, 0);
  if (error) {
    return error;
  };
  error = pthread_cond_init(&a->queue_not_empty, 0);
  if (error) {
    pthread_mutex_destroy(&a->queue_mutex);
    return error;
  };
  a->size = 0;
  a->requested_size = 0;
  a->resizing = 0;
  a->shutdown = 0;
  a->accepting = 1;
  for (i = 0; i < size; i += 1) {
    a->retire[i] = 0;
    a->workers[i].pool = a;
    a->workers[i].index = i;
    error = pthread_create(a->threads + i, 0, sph_thread_pool_worker, a->workers + i);
    if (error) {
      pthread_mutex_lock(&a->queue_mutex);
      a->accepting = 0;
      a->shutdown = 1;
      pthread_cond_broadcast(&a->queue_not_empty);
      pthread_mutex_unlock(&a->queue_mutex);
      while (i) {
        i -= 1;
        pthread_join(a->threads[i], 0);
      };
      a->size = 0;
      sph_thread_pool_destroy(a);
      return error;
    };
    a->size = i + 1;
  };
  return 0;
}

int sph_thread_pool_resize(sph_thread_pool_t* a, sph_thread_pool_size_t size, uint8_t no_wait, uint8_t discard_queue) {
  sph_thread_pool_size_t i;
  int error;
  if (size > sph_thread_pool_thread_limit) {
    return EINVAL;
  };
  if (a->resizing) {
    if (no_wait) {
      return 0;
    };
  } else if (size > a->size) {
    for (i = a->size; i < size; i += 1) {
      a->retire[i] = 0;
      a->workers[i].pool = a;
      a->workers[i].index = i;
      error = pthread_create(a->threads + i, 0, sph_thread_pool_worker, a->workers + i);
      if (error) {
        pthread_mutex_lock(&a->queue_mutex);
        for (sph_thread_pool_size_t j = a->size; j < i; j += 1) {
          a->retire[j] = 1;
        };
        pthread_cond_broadcast(&a->queue_not_empty);
        pthread_mutex_unlock(&a->queue_mutex);
        while (i > a->size) {
          i -= 1;
          pthread_join(a->threads[i], 0);
        };
        return error;
      };
    };
    a->size = size;
    return 0;
  } else if (size < a->size) {
    a->requested_size = size;
    a->resizing = 1;
    pthread_mutex_lock(&a->queue_mutex);
    if (!size) {
      a->accepting = 0;
      a->shutdown = 1;
    };
    if (discard_queue) {
      sph_queue_init(&a->queue);
    };
    for (i = size; i < a->size; i += 1) {
      a->retire[i] = 1;
    };
    pthread_cond_broadcast(&a->queue_not_empty);
    pthread_mutex_unlock(&a->queue_mutex);
    if (no_wait) {
      return 0;
    };
  } else {
    if (!size) {
      a->accepting = 0;
      a->shutdown = 1;
      return sph_thread_pool_destroy(a);
    };
    return 0;
  };
  for (i = a->requested_size; i < a->size; i += 1) {
    pthread_join(a->threads[i], 0);
  };
  a->size = a->requested_size;
  a->resizing = 0;
  if (!a->size) {
    return sph_thread_pool_destroy(a);
  };
  return 0;
}

int sph_thread_pool_finish(sph_thread_pool_t* a, uint8_t no_wait, uint8_t discard_queue) {
  return sph_thread_pool_resize(a, 0, no_wait, discard_queue);
}
#endif
