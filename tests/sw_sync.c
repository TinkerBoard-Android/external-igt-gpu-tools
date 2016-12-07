/*
 * Copyright © 2016 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Robert Foss <robert.foss@collabora.com>
 */

#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <unistd.h>

#include "igt.h"
#include "igt_aux.h"
#include "igt_primes.h"

#include "sw_sync.h"


IGT_TEST_DESCRIPTION("Test SW Sync Framework");

typedef struct {
	int timeline;
	uint32_t thread_id;
	volatile uint32_t * volatile counter;
	sem_t *sem;
} data_t;

static void test_alloc_timeline(void)
{
	int timeline;

	timeline = sw_sync_timeline_create();
	close(timeline);
}

static void test_alloc_fence(void)
{
	int in_fence;
	int timeline;

	timeline = sw_sync_timeline_create();
	in_fence = sw_sync_fence_create(timeline, 0);

	close(in_fence);
	close(timeline);
}

static void test_alloc_fence_invalid_timeline(void)
{
	igt_assert_f(__sw_sync_fence_create(-1, 0) < 0,
	    "Did not fail to create fence on invalid timeline\n");
}

static void test_alloc_merge_fence(void)
{
	int in_fence[2];
	int fence_merge;
	int timeline[2];

	timeline[0] = sw_sync_timeline_create();
	timeline[1] = sw_sync_timeline_create();

	in_fence[0] = sw_sync_fence_create(timeline[0], 1);
	in_fence[1] = sw_sync_fence_create(timeline[1], 1);
	fence_merge = sync_merge(in_fence[1], in_fence[0]);

	close(in_fence[0]);
	close(in_fence[1]);
	close(fence_merge);
	close(timeline[0]);
	close(timeline[1]);
}

static void test_sync_busy(void)
{
	int fence, ret;
	int timeline;
	int seqno;

	timeline = sw_sync_timeline_create();
	fence = sw_sync_fence_create(timeline, 5);

	/* Make sure that fence has not been signaled yet */
	ret = sync_wait(fence, 0);
	igt_assert_f(ret == -1 && errno == ETIME,
	    "Fence signaled early (timeline value 0, fence seqno 5)\n");

	/* Advance timeline from 0 -> 1 */
	sw_sync_timeline_inc(timeline, 1);

	/* Make sure that fence has not been signaled yet */
	ret = sync_wait(fence, 0);
	igt_assert_f(ret == -1 && errno == ETIME,
	    "Fence signaled early (timeline value 1, fence seqno 5)\n");

	/* Advance timeline from 1 -> 5: signaling the fence (seqno 5)*/
	sw_sync_timeline_inc(timeline, 4);
	ret = sync_wait(fence, 0);
	igt_assert_f(ret == 0,
	    "Fence not signaled (timeline value 5, fence seqno 5)\n");

	/* Go even further, and confirm wait still succeeds */
	sw_sync_timeline_inc(timeline, 5);
	ret = sync_wait(fence, 0);
	igt_assert_f(ret == 0,
	    "Fence not signaled (timeline value 10, fence seqno 5)\n");

	seqno = 10;
	for_each_prime_number(prime, 100) {
		int fence_prime;
		seqno += prime;

		fence_prime = sw_sync_fence_create(timeline, seqno);
		sw_sync_timeline_inc(timeline, prime);

		ret = sync_wait(fence_prime, 0);
		igt_assert_f(ret == 0,
		    "Fence not signaled during test of prime timeline increments\n");
		close(fence_prime);
	}

	close(fence);
	close(timeline);
}

static void test_sync_merge(void)
{
	int in_fence[3];
	int fence_merge;
	int timeline;
	int active, signaled;

	timeline = sw_sync_timeline_create();
	in_fence[0] = sw_sync_fence_create(timeline, 1);
	in_fence[1] = sw_sync_fence_create(timeline, 2);
	in_fence[2] = sw_sync_fence_create(timeline, 3);

	fence_merge = sync_merge(in_fence[0], in_fence[1]);
	fence_merge = sync_merge(in_fence[2], fence_merge);

	/* confirm all fences have one active point (even d) */
	active = sync_fence_count_status(in_fence[0],
					    SW_SYNC_FENCE_STATUS_ACTIVE);
	igt_assert_f(active == 1, "in_fence[0] has too many active fences\n");
	active = sync_fence_count_status(in_fence[1],
					    SW_SYNC_FENCE_STATUS_ACTIVE);
	igt_assert_f(active == 1, "in_fence[1] has too many active fences\n");
	active = sync_fence_count_status(in_fence[2],
					    SW_SYNC_FENCE_STATUS_ACTIVE);
	igt_assert_f(active == 1, "in_fence[2] has too many active fences\n");
	active = sync_fence_count_status(fence_merge,
					    SW_SYNC_FENCE_STATUS_ACTIVE);
	igt_assert_f(active == 1, "fence_merge has too many active fences\n");

	/* confirm that fence_merge is not signaled until the max of fence 0,1,2 */
	sw_sync_timeline_inc(timeline, 1);
	signaled = sync_fence_count_status(in_fence[0],
					      SW_SYNC_FENCE_STATUS_SIGNALED);
	active = sync_fence_count_status(fence_merge,
					    SW_SYNC_FENCE_STATUS_ACTIVE);
	igt_assert_f(signaled == 1, "in_fence[0] did not signal\n");
	igt_assert_f(active == 1, "fence_merge signaled too early\n");

	sw_sync_timeline_inc(timeline, 1);
	signaled = sync_fence_count_status(in_fence[1],
					      SW_SYNC_FENCE_STATUS_SIGNALED);
	active = sync_fence_count_status(fence_merge,
					    SW_SYNC_FENCE_STATUS_ACTIVE);
	igt_assert_f(signaled == 1, "in_fence[1] did not signal\n");
	igt_assert_f(active == 1, "fence_merge signaled too early\n");

	sw_sync_timeline_inc(timeline, 1);
	signaled = sync_fence_count_status(in_fence[2],
					      SW_SYNC_FENCE_STATUS_SIGNALED);
	igt_assert_f(signaled == 1, "in_fence[2] did not signal\n");
	signaled = sync_fence_count_status(fence_merge,
					       SW_SYNC_FENCE_STATUS_SIGNALED);
	active = sync_fence_count_status(fence_merge,
					    SW_SYNC_FENCE_STATUS_ACTIVE);
	igt_assert_f(active == 0 && signaled == 1,
		     "fence_merge did not signal\n");

	close(in_fence[0]);
	close(in_fence[1]);
	close(in_fence[2]);
	close(fence_merge);
	close(timeline);
}

static void test_sync_merge_same(void)
{
	int in_fence[2];
	int timeline;
	int signaled;

	timeline = sw_sync_timeline_create();
	in_fence[0] = sw_sync_fence_create(timeline, 1);
	in_fence[1] = sync_merge(in_fence[0], in_fence[0]);

	signaled = sync_fence_count_status(in_fence[0],
					      SW_SYNC_FENCE_STATUS_SIGNALED);
	igt_assert_f(signaled == 0, "Fence signaled too early\n");

	sw_sync_timeline_inc(timeline, 1);
	signaled = sync_fence_count_status(in_fence[0],
					      SW_SYNC_FENCE_STATUS_SIGNALED);
	igt_assert_f(signaled == 1, "Fence did not signal\n");

	close(in_fence[0]);
	close(in_fence[1]);
	close(timeline);
}

#define MULTI_CONSUMER_THREADS 8
#define MULTI_CONSUMER_ITERATIONS (1 << 14)
static void * test_sync_multi_consumer_thread(void *arg)
{
	data_t *data = arg;
	int thread_id = data->thread_id;
	int timeline = data->timeline;
	int ret, i;

	for (i = 0; i < MULTI_CONSUMER_ITERATIONS; i++) {
		int next_point = i * MULTI_CONSUMER_THREADS + thread_id;
		int fence = sw_sync_fence_create(timeline, next_point);

		ret = sync_wait(fence, 1000);
		if (ret == -1)
		{
			return (void *) 1;
		}

		if (*(data->counter) != next_point)
		{
			return (void *) 1;
		}

		sem_post(data->sem);
		close(fence);
	}
	return NULL;
}

static void test_sync_multi_consumer(void)
{

	data_t data_arr[MULTI_CONSUMER_THREADS];
	pthread_t thread_arr[MULTI_CONSUMER_THREADS];
	sem_t sem;
	int timeline;
	volatile uint32_t counter = 0;
	uintptr_t thread_ret = 0;
	data_t data;
	int i, ret;

	sem_init(&sem, 0, 0);
	timeline = sw_sync_timeline_create();

	data.counter = &counter;
	data.timeline = timeline;
	data.sem = &sem;

	/* Start sync threads. */
	for (i = 0; i < MULTI_CONSUMER_THREADS; i++)
	{
		data_arr[i] = data;
		data_arr[i].thread_id = i;
		ret = pthread_create(&thread_arr[i], NULL,
				     test_sync_multi_consumer_thread,
				     (void *) &(data_arr[i]));
		igt_assert_eq(ret, 0);
	}

	/* Produce 'content'. */
	for (i = 0; i < MULTI_CONSUMER_THREADS * MULTI_CONSUMER_ITERATIONS; i++)
	{
		sem_wait(&sem);

		counter++;
		sw_sync_timeline_inc(timeline, 1);
	}

	/* Wait for threads to complete. */
	for (i = 0; i < MULTI_CONSUMER_THREADS; i++)
	{
		uintptr_t local_thread_ret;
		pthread_join(thread_arr[i], (void **)&local_thread_ret);
		thread_ret |= local_thread_ret;
	}

	close(timeline);
	sem_destroy(&sem);

	igt_assert_f(counter == MULTI_CONSUMER_THREADS * MULTI_CONSUMER_ITERATIONS,
		     "Counter has unexpected value.\n");

	igt_assert_f(thread_ret == 0, "A sync thread reported failure.\n");
}

#define MULTI_CONSUMER_PRODUCER_THREADS 8
#define MULTI_CONSUMER_PRODUCER_ITERATIONS (1 << 14)
static void * test_sync_multi_consumer_producer_thread(void *arg)
{
	data_t *data = arg;
	int thread_id = data->thread_id;
	int timeline = data->timeline;
	int ret, i;

	for (i = 0; i < MULTI_CONSUMER_PRODUCER_ITERATIONS; i++) {
		int next_point = i * MULTI_CONSUMER_PRODUCER_THREADS + thread_id;
		int fence = sw_sync_fence_create(timeline, next_point);

		ret = sync_wait(fence, 1000);
		if (ret == -1)
		{
			return (void *) 1;
		}

		if (*(data->counter) != next_point)
		{
			return (void *) 1;
		}

		(*data->counter)++;

		/* Kick off the next thread. */
		sw_sync_timeline_inc(timeline, 1);

		close(fence);
	}
	return NULL;
}

static void test_sync_multi_consumer_producer(void)
{
	data_t data_arr[MULTI_CONSUMER_PRODUCER_THREADS];
	pthread_t thread_arr[MULTI_CONSUMER_PRODUCER_THREADS];
	int timeline;
	volatile uint32_t counter = 0;
	uintptr_t thread_ret = 0;
	data_t data;
	int i, ret;

	timeline = sw_sync_timeline_create();

	data.counter = &counter;
	data.timeline = timeline;

	/* Start consumer threads. */
	for (i = 0; i < MULTI_CONSUMER_PRODUCER_THREADS; i++)
	{
		data_arr[i] = data;
		data_arr[i].thread_id = i;
		ret = pthread_create(&thread_arr[i], NULL,
				     test_sync_multi_consumer_producer_thread,
				     (void *) &(data_arr[i]));
		igt_assert_eq(ret, 0);
	}

	/* Wait for threads to complete. */
	for (i = 0; i < MULTI_CONSUMER_PRODUCER_THREADS; i++)
	{
		uintptr_t local_thread_ret;
		pthread_join(thread_arr[i], (void **)&local_thread_ret);
		thread_ret |= local_thread_ret;
	}

	close(timeline);

	igt_assert_f(counter == MULTI_CONSUMER_PRODUCER_THREADS *
	                        MULTI_CONSUMER_PRODUCER_ITERATIONS,
		     "Counter has unexpected value.\n");

	igt_assert_f(thread_ret == 0, "A sync thread reported failure.\n");
}

igt_main
{
	igt_subtest("alloc_timeline")
		test_alloc_timeline();

	igt_subtest("alloc_fence")
		test_alloc_fence();

	igt_subtest("alloc_fence_invalid_timeline")
		test_alloc_fence_invalid_timeline();

	igt_subtest("alloc_merge_fence")
		test_alloc_merge_fence();

	igt_subtest("sync_busy")
		test_sync_busy();

	igt_subtest("sync_merge")
		test_sync_merge();

	igt_subtest("sync_merge_same")
		test_sync_merge_same();

	igt_subtest("sync_multi_consumer")
		test_sync_multi_consumer();

	igt_subtest("sync_multi_consumer_producer")
		test_sync_multi_consumer_producer();
}

