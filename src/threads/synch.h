#ifndef THREADS_SYNCH_H
#define THREADS_SYNCH_H

#include <list.h>
#include <stdbool.h>
#include <debug.h>

/* A counting semaphore. */
struct semaphore 
  {
    unsigned value;             /* Current value. */
    struct list waiters;        /* List of waiting threads. */
    int priority_max;           /* Maximum priority value in list_waiters */
  };

void sema_init (struct semaphore *, unsigned value);
void sema_down (struct semaphore *);
bool sema_try_down (struct semaphore *);
void sema_up (struct semaphore *);
void sema_self_test (void);
/* Less functions for max priority of semaphores */
bool sema_less_priority_max (const struct list_elem* e1,
                             const struct list_elem* e2,
                             void* AUX UNUSED);

/* Lock. */
struct lock 
  {
    struct thread *holder;      /* Thread holding lock (for debugging). */
    struct semaphore semaphore; /* Binary semaphore controlling access. */
    struct list_elem elem;      /* List element for locks in thread (see thread.h) */
  };

void lock_init (struct lock *);
void lock_acquire (struct lock *);
bool lock_try_acquire (struct lock *);
void lock_release (struct lock *);
bool lock_held_by_current_thread (const struct lock *);

/* Condition variable. */
struct condition 
  {
    struct list waiters;        /* List of waiting threads. */
  };

void cond_init (struct condition *);
void cond_wait (struct condition *, struct lock *);
void cond_signal (struct condition *, struct lock *);
void cond_broadcast (struct condition *, struct lock *);
/* Less function that determine which semaphore has less maximum priority */
bool cond_less_sema_priority (const struct list_elem* e1,
                              const struct list_elem* e2,
                              void* AUX UNUSED);


/* Lock for reader & writer */
struct rw_lock
{
  struct condition cond_read;   /* Condition for readers */
  struct condition cond_write;  /* Condition for writers */
  struct condition cond_evict;  /* Condition for eviction */
  bool is_evict;                /* Is it to evicted or not ?? */
  bool write_first;             /* Is write is prefered or not? */
  int r_wait, r_active;         /* Read waiters cnt and read active cnt */
  int w_wait, w_active;         /* Write waiters cnt and write active cnt */
  struct lock lock;             /* Lock for mutual exclusion */
};

void rw_init (struct rw_lock *);
bool rw_rd_lock (struct rw_lock *);
bool rw_wr_lock (struct rw_lock *);
void rw_evict_lock (struct rw_lock *);
void rw_rd_unlock (struct rw_lock *);
void rw_wr_unlock (struct rw_lock *);
void rw_evict_unlock (struct rw_lock *);

/* Optimization barrier.

   The compiler will not reorder operations across an
   optimization barrier.  See "Optimization Barriers" in the
   reference guide for more information.*/
#define barrier() asm volatile ("" : : : "memory")

#endif /* threads/synch.h */
