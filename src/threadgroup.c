/*
Copyright (c) 2014, Intel Corporation

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
  threading infrastructure
  . threadgroup abstraction
  . fork/join/barrier
*/


#include "options.h"

#include <immintrin.h>
#include <stdlib.h>
#include <string.h>
#include "ia_misc.h"
#include "threadgroup.h"


int ti_threadgroup_create(uint8_t num_sockets, uint8_t num_cores,
                          uint8_t num_threads_per_core,
                          ti_threadgroup_t **newtg)
{
    int i;
    ti_threadgroup_t *tg;
    int num_threads = num_sockets * num_cores * num_threads_per_core;
    char *cp;

    if (num_threads > TI_MAX_THREADS)
        return -1;

    tg = (ti_threadgroup_t *)_mm_malloc(sizeof (ti_threadgroup_t), 64);
    for (i = 0;  i < num_threads;  ++i)
        tg->tid_map[i] = -1;
    tg->num_sockets = num_sockets;
    tg->num_cores = num_cores;
    tg->num_threads_per_core = num_threads_per_core;
    tg->num_threads = num_threads;
    tg->added_threads = 0;
    tg->thread_sense = (ti_thread_sense_t **)
            _mm_malloc(num_threads * sizeof (ti_thread_sense_t *), 64);
    for (i = 0;  i < num_threads;  i++)
        tg->thread_sense[i] = NULL;
    tg->group_sense = 0;
    tg->forked = 0;

    pthread_mutex_init(&tg->alarm_lock, NULL);
    pthread_cond_init(&tg->alarm, NULL);

    tg->sleep_threshold = THREAD_SLEEP_THRESHOLD_DEFAULT;
    cp = getenv(THREAD_SLEEP_THRESHOLD_NAME);
    if (cp) {
	if (!strncasecmp(cp, "infinite", 8))
	    tg->sleep_threshold = 0;
	else
	    tg->sleep_threshold = (uint64_t)strtol(cp, NULL, 10);
    }

    *newtg = tg;
    return 0;
}


int ti_threadgroup_addthread(ti_threadgroup_t *tg, int16_t ext_tid,
                             int16_t *tgtid)
{
    if (ext_tid < 0 || ext_tid >= TI_MAX_THREADS)
        return -1;
    if (tg->tid_map[ext_tid] != -1)
        return -2;
    if (tg->added_threads == tg->num_threads)
        return -3;

    tg->tid_map[ext_tid] = tg->added_threads++;
    if (tgtid) *tgtid = tg->tid_map[ext_tid];

    return 0;
}


int ti_threadgroup_initthread(ti_threadgroup_t *tg, int16_t ext_tid)
{
    ti_thread_sense_t *ts;

    if (ext_tid < 0 || ext_tid >= TI_MAX_THREADS)
        return -1;
    if (tg->thread_sense[tg->tid_map[ext_tid]] != NULL)
        return -2;
    if (tg->num_threads == 0)
        return -3;

    ts = (ti_thread_sense_t *)_mm_malloc(sizeof (ti_thread_sense_t), 64);
    ts->sense = 1;
    tg->thread_sense[tg->tid_map[ext_tid]] = ts;

    return 0;
}


int ti_threadgroup_member(ti_threadgroup_t *tg, int16_t ext_tid,
                          int16_t *tgtid)
{
    if (ext_tid < 0 || ext_tid >= TI_MAX_THREADS)
        return -1;
    if (tg == NULL) {
        if (tgtid) *tgtid = -1;
        return -2;
    }
    if (tg->tid_map[ext_tid] == -1) {
        if (tgtid) *tgtid = -1;
        return -3;
    }
    if (tgtid) *tgtid = tg->tid_map[ext_tid];

    return 0;
}


int ti_threadgroup_size(ti_threadgroup_t *tg, int16_t *tgsize)
{
    *tgsize = tg->num_threads;
    return 0;
}


int ti_threadgroup_fork(ti_threadgroup_t *tg, int16_t ext_tid,
                        void **bcast_val)
{
    if (tg->tid_map[ext_tid] == 0) {
        tg->envelope = bcast_val ? *bcast_val : NULL;
        cpu_sfence();
	tg->forked = 1;
        tg->group_sense = tg->thread_sense[0]->sense;

	// if it's possible that threads are sleeping, signal them
	if (tg->sleep_threshold) {
	    pthread_mutex_lock(&tg->alarm_lock);
	    pthread_cond_broadcast(&tg->alarm);
	    pthread_mutex_unlock(&tg->alarm_lock);
	}
    }
    else {
	// spin up to threshold cycles (count sheep), then sleep
	uint64_t spin_cycles, spin_start = rdtsc();
        while (tg->group_sense !=
               tg->thread_sense[tg->tid_map[ext_tid]]->sense) {
	    if (tg->sleep_threshold) {
		spin_cycles = rdtsc() - spin_start;
		if (spin_cycles >= tg->sleep_threshold) {
		    pthread_mutex_lock(&tg->alarm_lock);
                    if (tg->group_sense !=
                        tg->thread_sense[tg->tid_map[ext_tid]]->sense) {
                        pthread_cond_wait(&tg->alarm, &tg->alarm_lock);
                    }
                    pthread_mutex_unlock(&tg->alarm_lock);
		    spin_start = rdtsc();
		    continue;
		}
	    }
            cpu_pause();
	}
        cpu_lfence();
        if (bcast_val)
            *bcast_val = tg->envelope;
    }

    return 0;
}


int ti_threadgroup_join(ti_threadgroup_t *tg, int16_t ext_tid)
{
    int i;

    tg->thread_sense[tg->tid_map[ext_tid]]->sense
        = !tg->thread_sense[tg->tid_map[ext_tid]]->sense;
    if (tg->tid_map[ext_tid] == 0) {
        for (i = 1;  i < tg->num_threads;  ++i) {
            while (tg->thread_sense[i]->sense == tg->group_sense)
                cpu_pause();
        }
	tg->forked = 0;
    }

    return 0;
}


void ti_threadgroup_barrier(ti_threadgroup_t *tg, int16_t ext_tid)
{
    if (tg->tid_map[ext_tid] == 0  &&  !tg->forked)
	return;

    ti_threadgroup_join(tg, ext_tid);
    ti_threadgroup_fork(tg, ext_tid, NULL);
}


int ti_threadgroup_destroy(ti_threadgroup_t *tg)
{
    int i;

    pthread_mutex_destroy(&tg->alarm_lock);
    pthread_cond_destroy(&tg->alarm);

    for (i = 0;  i < tg->num_threads;  i++)
        _mm_free(tg->thread_sense[i]);
    _mm_free(tg->thread_sense);
    _mm_free(tg);

    return 0;
}

