/* Deterministic reproduction of the 0041 job-cleanup race, as a model of Mesa's util_queue.
 *
 * u_queue.c (queue_thread, ~l.296-300) runs   execute(); fence_signal(); cleanup();   and
 * util_queue_drop_job() returns early once the fence is signalled (~l.662). kk_shader_destroy() frees the
 * kk_async_pipeline right after that return, so a cleanup callback that still reads async->status races
 * with the free. The code between the "kk_shader.c" markers is the driver's, with the `ran` flag the fix
 * adds; compile with -DOLD_CLEANUP to get the code before the fix.
 *
 *   clang -fsanitize=address -O1 -pthread async-cleanup-model.c -o m && ./m
 *   -> RESULT: PASS (fixed) / AddressSanitizer: heap-use-after-free (-DOLD_CLEANUP)
 */
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

enum { KK_ASYNC_PENDING, KK_ASYNC_READY, KK_ASYNC_FAILED };
struct kk_async_pipeline { volatile unsigned status; };
struct kk_async_job { struct kk_async_pipeline *async; bool ran; };

/* ---- kk_shader.c ---- */
static void kk_async_pipeline_worker(void *job_data)
{
   struct kk_async_job *job = job_data;
   __atomic_store_n(&job->async->status, KK_ASYNC_READY, __ATOMIC_SEQ_CST);
   job->ran = true; /* the fix: last access to async by this job */
}
static void kk_async_pipeline_cleanup(void *job_data)
{
   struct kk_async_job *job = job_data;
#ifdef OLD_CLEANUP
   struct kk_async_pipeline *async = job->async;
   if (__atomic_load_n(&async->status, __ATOMIC_SEQ_CST) == KK_ASYNC_PENDING)
      __atomic_store_n(&async->status, KK_ASYNC_FAILED, __ATOMIC_SEQ_CST);
#else
   if (!job->ran) {
      struct kk_async_pipeline *async = job->async;
      if (__atomic_load_n(&async->status, __ATOMIC_SEQ_CST) == KK_ASYNC_PENDING)
         __atomic_store_n(&async->status, KK_ASYNC_FAILED, __ATOMIC_SEQ_CST);
   }
#endif
   free(job);
}
/* ---- end kk_shader.c ---- */

/* util_queue worker for one job: execute, signal the fence, then cleanup. `gap` is the scheduling
 * delay the worker can suffer between the signal and the cleanup callback. */
struct qjob { struct kk_async_job *job; volatile int fence; };
static void *queue_thread(void *p)
{
   struct qjob *q = p;
   kk_async_pipeline_worker(q->job);
   struct kk_async_job *job = q->job;
   __atomic_store_n(&q->fence, 1, __ATOMIC_SEQ_CST);   /* util_queue_fence_signal */
   usleep(2000);                                       /* preempted before cleanup */
   kk_async_pipeline_cleanup(job);
   return NULL;
}

int main(void)
{
   for (int i = 0; i < 20; i++) {
      struct kk_async_pipeline *async = calloc(1, sizeof(*async));
      struct kk_async_job *job = calloc(1, sizeof(*job));
      job->async = async;
      struct qjob q = {.job = job};
      pthread_t t; pthread_create(&t, NULL, queue_thread, &q);
      /* kk_shader_destroy: util_queue_drop_job returns as soon as the fence is signalled, then frees async */
      while (!__atomic_load_n(&q.fence, __ATOMIC_SEQ_CST)) sched_yield();
      free(async);
      pthread_join(t, NULL);
   }
   printf("RESULT: PASS cleanup never touched async after the fence signal\n");
   return 0;
}
