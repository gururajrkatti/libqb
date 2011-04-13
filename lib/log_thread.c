/*
 * Copyright (C) 2011 Red Hat, Inc.
 *
 * All rights reserved.
 *
 * Author: Angus Salkeld <asalkeld@redhat.com>
 *
 * libqb is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * libqb is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libqb.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "os_base.h"

#include <pthread.h>
#include <semaphore.h>

#include <qb/qbdefs.h>
#include <qb/qblist.h>
#include <qb/qbutil.h>
#include "log_int.h"

static int wthread_active = 0;

static int wthread_should_exit = 0;

static void wthread_create(void);

static qb_thread_lock_t *logt_wthread_lock = NULL;

static QB_LIST_DECLARE(logt_print_finished_records);

static int logt_memory_used = 0;

static int logt_dropped_messages = 0;

static sem_t logt_thread_start;

static sem_t logt_print_finished;

static int logt_sched_param_queued = 0;

static int logt_sched_policy;

static struct sched_param logt_sched_param;

static int logt_after_log_ops_yield = 10;

static pthread_t logt_thread_id = 0;

static void *qb_logt_worker_thread(void *data) __attribute__ ((noreturn));
static void *qb_logt_worker_thread(void *data)
{
	struct qb_log_record *rec;
	int dropped = 0;
	int res;

	/*
	 * Signal wthread_create that the initialization process may continue
	 */
	sem_post(&logt_thread_start);
	for (;;) {
		dropped = 0;
retry_sem_wait:
		res = sem_wait(&logt_print_finished);
		if (res == -1 && errno == EINTR) {
			goto retry_sem_wait;
		} else if (res == -1) {
			/*
			 * This case shouldn't happen
			 */
			pthread_exit(NULL);
		}

		(void)qb_thread_lock(logt_wthread_lock);
		if (wthread_should_exit) {
			int value;

			res = sem_getvalue(&logt_print_finished, &value);
			if (value == 0) {
				(void)qb_thread_unlock(logt_wthread_lock);
				pthread_exit(NULL);
			}
		}

		rec =
		    qb_list_entry(logt_print_finished_records.next,
				  struct qb_log_record, list);
		qb_list_del(&rec->list);
		logt_memory_used = logt_memory_used - strlen(rec->buffer) -
		    sizeof(struct qb_log_record) - 1;
		dropped = logt_dropped_messages;
		logt_dropped_messages = 0;
		(void)qb_thread_unlock(logt_wthread_lock);
		if (dropped) {
			printf("%d messages lost\n", dropped);
		}

		qb_log_thread_log_write(rec->cs, rec->timestamp, rec->buffer);
		free(rec->buffer);
		free(rec);
	}
}

static int logt_thread_priority_set(int policy,
				    const struct sched_param *param,
				    unsigned int after_log_ops_yield)
{
	int res = 0;
	if (param == NULL) {
		return 0;
	}
#if defined(HAVE_PTHREAD_SETSCHEDPARAM) && defined(HAVE_SCHED_GET_PRIORITY_MAX)
	if (wthread_active == 0) {
		logsys_sched_policy = policy;
		memcpy(&logsys_sched_param, param, sizeof(struct sched_param));
		logsys_sched_param_queued = 1;
	} else {
		res = pthread_setschedparam(logsys_thread_id, policy, param);
	}
#endif

	if (after_log_ops_yield > 0) {
		logt_after_log_ops_yield = after_log_ops_yield;
	}

	return res;
}

static void wthread_create(void)
{
	int res;

	if (wthread_active) {
		return;
	}

	wthread_active = 1;

	/*
	 * TODO: propagate pthread_create errors back to the caller
	 */
	res = pthread_create(&logt_thread_id, NULL,
			     qb_logt_worker_thread, NULL);
	sem_wait(&logt_thread_start);

	if (res == 0) {
		if (logt_sched_param_queued == 1) {
			/*
			 * TODO: propagate qb_logt_thread_priority_set errors back to
			 * the caller
			 */
			res = logt_thread_priority_set(logt_sched_policy,
						       &logt_sched_param,
						       logt_after_log_ops_yield);
			logt_sched_param_queued = 0;
		}
	} else {
		wthread_active = 0;
	}
}

void qb_log_thread_start(void)
{
	wthread_create();
	logt_wthread_lock = qb_thread_lock_create(QB_THREAD_LOCK_SHORT);
}

void qb_log_thread_log_post(struct qb_log_callsite *cs,
			    time_t timestamp, const char *buffer)
{
	struct qb_log_record *rec;
	size_t buf_size;
	size_t total_size;

	rec = malloc(sizeof(struct qb_log_record));
	if (rec == NULL) {
		return;
	}

	buf_size = strlen(buffer) + 1;
	total_size = sizeof(struct qb_log_record) + buf_size;

	rec->cs = cs;
	rec->buffer = malloc(buf_size);
	if (rec->buffer == NULL) {
		goto free_record;
	}
	memcpy(rec->buffer, buffer, buf_size);

	rec->timestamp = timestamp;

	qb_list_init(&rec->list);
	(void)qb_thread_lock(logt_wthread_lock);
	logt_memory_used += total_size;
	if (logt_memory_used > 512000) {
		free(rec->buffer);
		free(rec);
		logt_memory_used = logt_memory_used - total_size;
		logt_dropped_messages += 1;
		(void)qb_thread_unlock(logt_wthread_lock);
		return;

	} else {
		qb_list_add_tail(&rec->list, &logt_print_finished_records);
	}
	(void)qb_thread_unlock(logt_wthread_lock);

	sem_post(&logt_print_finished);
	return;

free_record:
	free(rec);
}

void qb_log_thread_stop(void)
{
	int res;
	int value;
	struct qb_log_record *rec;

	if (wthread_active == 0 && logt_wthread_lock == NULL) {
		return;
	}
	if (wthread_active == 0) {
		for (;;) {
			(void)qb_thread_lock(logt_wthread_lock);

			res = sem_getvalue(&logt_print_finished, &value);
			if (res != 0 || value == 0) {
				(void)qb_thread_unlock(logt_wthread_lock);
				return;
			}
			sem_wait(&logt_print_finished);

			rec = qb_list_entry(logt_print_finished_records.next, struct qb_log_record, list);
			qb_list_del(&rec->list);
			logt_memory_used = logt_memory_used - strlen(rec->buffer) -
			    sizeof(struct qb_log_record) - 1;
			(void)qb_thread_unlock(logt_wthread_lock);

			qb_log_thread_log_write(rec->cs, rec->timestamp, rec->buffer);
			free(rec->buffer);
			free(rec);
		}
	} else {
		wthread_should_exit = 1;
		sem_post(&logt_print_finished);
		pthread_join(logt_thread_id, NULL);
	}
}