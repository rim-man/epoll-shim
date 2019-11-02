#include "timerfd_ctx.h"

#include <sys/event.h>
#include <sys/param.h>

#include <assert.h>
#include <signal.h>

#include <pthread_np.h>

static void *
worker_function(void *arg)
{
	TimerFDCtx *ctx = arg;

	uint64_t total_expirations = 0;

	siginfo_t info;
	sigset_t rt_set;
	sigset_t block_set;

	sigemptyset(&rt_set);
	sigaddset(&rt_set, SIGRTMIN);
	sigaddset(&rt_set, SIGRTMIN + 1);

	sigfillset(&block_set);

	(void)pthread_sigmask(SIG_BLOCK, &block_set, NULL);

	struct kevent kev;
	EV_SET(&kev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0,
	    (void *)(intptr_t)pthread_getthreadid_np());
	(void)kevent(ctx->kq, &kev, 1, NULL, 0, NULL);

	for (;;) {
		if (sigwaitinfo(&rt_set, &info) != SIGRTMIN) {
			break;
		}
		total_expirations += 1 + timer_getoverrun(ctx->complex.timer);
		EV_SET(&kev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0,
		    (void *)(uintptr_t)total_expirations);
		(void)kevent(ctx->kq, &kev, 1, NULL, 0, NULL);
	}

	return NULL;
}

static errno_t
upgrade_to_complex_timer(TimerFDCtx *ctx, int clockid)
{
	errno_t err;

	if (ctx->kind == TIMERFD_KIND_COMPLEX) {
		return 0;
	}

	if (ctx->kind == TIMERFD_KIND_SIMPLE) {
		struct kevent kev[1];
		EV_SET(&kev[0], 0, EVFILT_TIMER, EV_DELETE, 0, 0, 0);
		(void)kevent(ctx->kq, kev, nitems(kev), NULL, 0, NULL);

		ctx->kind = TIMERFD_KIND_UNDETERMINED;
	}

	assert(ctx->kind == TIMERFD_KIND_UNDETERMINED);

	struct kevent kev;
	EV_SET(&kev, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, 0);
	if (kevent(ctx->kq, &kev, 1, NULL, 0, NULL) < 0) {
		assert(errno != 0);
		return errno;
	}

	if ((err = pthread_create(&ctx->complex.worker, /**/
		 NULL, worker_function, ctx)) != 0) {
		return err;
	}

	if (kevent(ctx->kq, NULL, 0, &kev, 1, NULL) < 0) {
		goto out;
	}

	int tid = (int)(intptr_t)kev.udata;

	struct sigevent sigev = {.sigev_notify = SIGEV_THREAD_ID,
	    .sigev_signo = SIGRTMIN,
	    .sigev_notify_thread_id = tid};

	if (timer_create(clockid, &sigev, &ctx->complex.timer) < 0) {
		goto out;
	}

	ctx->complex.current_expirations = 0;
	ctx->kind = TIMERFD_KIND_COMPLEX;
	return 0;

out:
	err = errno;
	assert(err != 0);
	pthread_kill(ctx->complex.worker, SIGRTMIN + 1);
	pthread_join(ctx->complex.worker, NULL);
	return err;
}

errno_t
timerfd_ctx_init(TimerFDCtx *timerfd, int kq, int clockid)
{
	errno_t err;

	if (clockid != CLOCK_MONOTONIC && clockid != CLOCK_REALTIME) {
		return EINVAL;
	}

	*timerfd = (TimerFDCtx){.kq = kq, .kind = TIMERFD_KIND_UNDETERMINED};

	if (clockid == CLOCK_REALTIME) {
		if ((err = upgrade_to_complex_timer(timerfd, /**/
			 CLOCK_REALTIME)) != 0) {
			return err;
		}
	}

	return 0;
}

errno_t
timerfd_ctx_terminate(TimerFDCtx *timerfd)
{
	if (timerfd->kind == TIMERFD_KIND_COMPLEX) {
		timer_delete(timerfd->complex.timer);
		pthread_kill(timerfd->complex.worker, SIGRTMIN + 1);
		pthread_join(timerfd->complex.worker, NULL);
	}

	return (0);
}

errno_t
timerfd_ctx_settime(TimerFDCtx *timerfd, int flags,
    const struct itimerspec *new, struct itimerspec *old)
{
	errno_t err;

	if (flags & ~(TIMER_ABSTIME)) {
		return EINVAL;
	}

	if ((flags & TIMER_ABSTIME) ||
	    ((new->it_interval.tv_sec != 0 || new->it_interval.tv_nsec != 0) &&
		(new->it_interval.tv_sec != new->it_value.tv_sec ||
		    new->it_interval.tv_nsec != new->it_value.tv_nsec))) {
		if ((err = upgrade_to_complex_timer(timerfd, /**/
			 CLOCK_MONOTONIC)) != 0) {
			return err;
		}
	}

	if (timerfd->kind == TIMERFD_KIND_COMPLEX) {
		if (timer_settime(timerfd->complex.timer, /**/
			flags, new, old) < 0) {
			return errno;
		}
	} else {
		struct kevent kev[1];
		int oneshot_flag;
		int64_t micros;

		if (old) {
			*old = timerfd->simple.current_itimerspec;
		}

		if (new->it_value.tv_sec == 0 && new->it_value.tv_nsec == 0) {
			struct kevent kev[1];
			EV_SET(&kev[0], 0, EVFILT_TIMER, EV_DELETE, 0, 0, 0);
			(void)kevent(timerfd->kq, kev, nitems(kev), NULL, 0,
			    NULL);
		} else {
			if (__builtin_mul_overflow(new->it_value.tv_sec,
				1000000, &micros) ||
			    __builtin_add_overflow(micros,
				new->it_value.tv_nsec / 1000, &micros)) {
				return EOVERFLOW;
			}

			if ((new->it_value.tv_nsec % 1000) &&
			    __builtin_add_overflow(micros, 1, &micros)) {
				return EOVERFLOW;
			}

			if (new->it_interval.tv_sec == 0 &&
			    new->it_interval.tv_nsec == 0) {
				oneshot_flag = EV_ONESHOT;
			} else {
				oneshot_flag = 0;
			}

			EV_SET(&kev[0], 0, EVFILT_TIMER, EV_ADD | oneshot_flag,
			    NOTE_USECONDS, micros, 0);

			if (kevent(timerfd->kq, kev, nitems(kev), /**/
				NULL, 0, NULL) < 0) {
				return errno;
			}
		}

		timerfd->simple.current_itimerspec = *new;
		timerfd->kind = TIMERFD_KIND_SIMPLE;
	}

	return 0;
}

errno_t
timerfd_ctx_read(TimerFDCtx *timerfd, uint64_t *value)
{
	uint64_t nr_expired;

	for (;;) {
		struct timespec timeout = {0, 0};
		struct kevent kev;
		int ret = kevent(timerfd->kq, NULL, 0, &kev, 1, &timeout);
		if (ret < 0) {
			return errno;
		}

		if (ret == 0) {
			return EAGAIN;
		}

		if (timerfd->kind == TIMERFD_KIND_COMPLEX) {
			uint64_t expired_new = (uint64_t)kev.udata;

			/* TODO(jan): Should replace this with a
			 * per-timerfd_context mutex. */
			// pthread_mutex_lock(&timerfd_context_mtx);
			if (expired_new >
			    timerfd->complex.current_expirations) {
				nr_expired = expired_new -
				    timerfd->complex.current_expirations;
				timerfd->complex.current_expirations =
				    expired_new;
			} else {
				nr_expired = 0;
			}
			// pthread_mutex_unlock(&timerfd_context_mtx);
		} else {
			nr_expired = kev.data;
		}

		if (nr_expired != 0) {
			break;
		}
	}

	*value = nr_expired;
	return 0;
}