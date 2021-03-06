/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <pthread.h>
#include <errno.h>
#include <sys/resource.h>

#include "pipewire/log.h"
#include "pipewire/data-loop.h"
#include "pipewire/private.h"

#define NAME "data-loop"

SPA_EXPORT
int pw_data_loop_wait(struct pw_data_loop *this, int timeout)
{
	int res;

	while (true) {
		if (!this->running)
			return -ECANCELED;
		if ((res = pw_loop_iterate(this->loop, timeout)) < 0) {
			if (errno == EINTR)
				continue;
		}
		break;
	}
	return res;
}

SPA_EXPORT
void pw_data_loop_exit(struct pw_data_loop *this)
{
	this->running = false;
}

static void *do_loop(void *user_data)
{
	struct pw_data_loop *this = user_data;
	int res;

	pw_log_debug(NAME" %p: enter thread", this);
	pw_loop_enter(this->loop);

	while (this->running) {
		if ((res = pw_loop_iterate(this->loop, -1)) < 0) {
			if (errno == EINTR)
				continue;
			pw_log_error(NAME" %p: iterate error %d (%s)",
					this, res, spa_strerror(res));
		}
	}

	pw_log_debug(NAME" %p: leave thread", this);
	pw_loop_leave(this->loop);

	return NULL;
}


static void do_stop(void *data, uint64_t count)
{
	struct pw_data_loop *this = data;
	pw_log_debug(NAME" %p: stopping", this);
	this->running = false;
}

/** Create a new \ref pw_data_loop.
 * \return a newly allocated data loop
 *
 * \memberof pw_data_loop
 */
SPA_EXPORT
struct pw_data_loop *pw_data_loop_new(struct pw_properties *properties)
{
	struct pw_data_loop *this;
	int res;

	this = calloc(1, sizeof(struct pw_data_loop));
	if (this == NULL) {
		res = -errno;
		goto error_cleanup;
	}

	pw_log_debug(NAME" %p: new", this);

	this->loop = pw_loop_new(properties);
	properties = NULL;
	if (this->loop == NULL) {
		res = -errno;
		pw_log_error(NAME" %p: can't create loop: %m", this);
		goto error_free;
	}

	this->event = pw_loop_add_event(this->loop, do_stop, this);
	if (this->event == NULL) {
		res = -errno;
		pw_log_error(NAME" %p: can't add event: %m", this);
		goto error_loop_destroy;
	}

	spa_hook_list_init(&this->listener_list);

	return this;

error_loop_destroy:
	pw_loop_destroy(this->loop);
error_free:
	free(this);
error_cleanup:
	if (properties)
		pw_properties_free(properties);
	errno = -res;
	return NULL;
}

/** Destroy a data loop
 * \param loop the data loop to destroy
 * \memberof pw_data_loop
 */
SPA_EXPORT
void pw_data_loop_destroy(struct pw_data_loop *loop)
{
	pw_log_debug(NAME" %p: destroy", loop);

	pw_data_loop_emit_destroy(loop);

	pw_data_loop_stop(loop);

	pw_loop_destroy_source(loop->loop, loop->event);
	pw_loop_destroy(loop->loop);
	free(loop);
}

SPA_EXPORT
void pw_data_loop_add_listener(struct pw_data_loop *loop,
			       struct spa_hook *listener,
			       const struct pw_data_loop_events *events,
			       void *data)
{
	spa_hook_list_append(&loop->listener_list, listener, events, data);
}

struct pw_loop *
pw_data_loop_get_loop(struct pw_data_loop *loop)
{
	return loop->loop;
}

/** Start a data loop
 * \param loop the data loop to start
 * \return 0 if ok, -1 on error
 *
 * This will start the realtime thread that manages the loop.
 *
 * \memberof pw_data_loop
 */
SPA_EXPORT
int pw_data_loop_start(struct pw_data_loop *loop)
{
	if (!loop->running) {
		int err;

		loop->running = true;
		if ((err = pthread_create(&loop->thread, NULL, do_loop, loop)) != 0) {
			pw_log_error(NAME" %p: can't create thread: %s", loop, strerror(err));
			loop->running = false;
			return -err;
		}
	}
	return 0;
}

/** Stop a data loop
 * \param loop the data loop to Stop
 * \return 0
 *
 * This will stop and join the realtime thread that manages the loop.
 *
 * \memberof pw_data_loop
 */
SPA_EXPORT
int pw_data_loop_stop(struct pw_data_loop *loop)
{
	if (loop->running) {
		pw_loop_signal_event(loop->loop, loop->event);

		pthread_join(loop->thread, NULL);
	}
	return 0;
}

/** Check if we are inside the data loop
 * \param loop the data loop to check
 * \return true is the current thread is the data loop thread
 *
 * \memberof pw_data_loop
 */
SPA_EXPORT
bool pw_data_loop_in_thread(struct pw_data_loop * loop)
{
	return pthread_equal(loop->thread, pthread_self());
}
