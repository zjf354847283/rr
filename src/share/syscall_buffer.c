/* -*- Mode: C; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#include "syscall_buffer.h"

/**
 * Buffer syscalls, so that rr can process the entire buffer with one
 * trap instead of a trap per call.
 *
 * This file is compiled into a dso that's PRELOADed in recorded
 * applications.  The dso replaces libc syscall wrappers with our own
 * implementation that saves nondetermistic outparams in a fixed-size
 * buffer.  When the buffer is full or the recorded application
 * invokes an un-buffered syscall or receives a signal, we trap to rr
 * and it records the state of the buffer.
 *
 * During replay, rr simply refills the buffer with the recorded data
 * when it reaches the "flush-buffer" events that were recorded.  Then
 * rr emulates each buffered syscall, and the code here restores the
 * client data from the refilled buffer.
 *
 * The crux of the implementation here is to selectively ptrace-trap
 * syscalls.  The normal (un-buffered) syscalls generate a ptrace
 * trap, and the buffered syscalls trap directly to the kernel.  This
 * is implemented with a seccomp-bpf which examines the syscall and
 * decides how to handle it (see seccomp-bpf.h).
 *
 * Because this code runs in the tracee's address space and overrides
 * libc symbols, the code is rather delicate.  The following rules
 * must be followed
 *
 * o No rr headers (other than seccomp-bpf.h) may be included
 * o All syscalls invoked by this code must be called directly, not
 *   through libc wrappers (which this file may itself wrap)
 */

#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <linux/futex.h>
#include <linux/net.h>
#include <linux/perf_event.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

/* NB: don't include any other local headers here. */
#include "seccomp-bpf.h"

typedef unsigned char byte;

/* Nonzero after we've installed the filter. */
static int is_seccomp_bpf_installed;

static __thread byte* buffer = NULL;
/* This tracks whether the buffer is currently in use for a system
 * call. This is helpful when a signal handler runs during a wrapped
 * system call; we don't want it to use the buffer for its system
 * calls. */
static __thread int buffer_locked = 0;
/* This is used to support the buffering of "may-block" system calls.
 * The problem that needs to be addressed can be introduced with a
 * simple example; assume that we're buffering the "read" and "write"
 * syscalls.
 *
 *  o (Tasks W and R set up a synchronous-IO pipe open between them; W
 *    "owns" the write end of the pipe; R owns the read end; the pipe
 *    buffer is full)
 *  o Task W invokes the write syscall on the pipe
 *  o Since write is a buffered syscall, the seccomp filter traps W
 *    directly to the kernel; there's no trace event for W delivered
 *    to rr.
 *  o The pipe is full, so W is descheduled by the kernel because W
 *    can't make progress.
 *  o rr thinks W is still running and doesn't schedule R.
 *
 * At this point, progress in the recorded application can only be
 * made by scheduling R, but no one tells rr to do that.  Oops!
 *
 * Thus enter the "desched counter".  It's a perf_event for the "sw
 * context switches" event (which, more precisely, is "sw deschedule";
 * it counts schedule-out, not schedule-in).  We program the counter
 * to deliver SIGIO to this task when there's new counter data
 * available.  And we set up the "sample period", how many descheds
 * are triggered before SIGIO is delivered, to be "1".  This means
 * that when the counter is armed, the next desched (i.e., the next
 * time the desched counter is bumped up) of this task will deliver
 * SIGIO to it.  And signal delivery always generates a ptrace trap,
 * so rr can deduce that this task was descheduled and schedule
 * another.
 *
 * One implementation note is that the tracer always sees *two* SIGIOs
 * per desched notification.  The current theory of what's happening
 * is
 * 
 *  o child gets descheduled, bumps counter to i and schedules SIGIO
 *  o SIGIO notification "schedules" child, but it doesn't actually
 *    run any application code
 *  o child is being ptraced, so we "deschedule" child to notify
 *    parent and bump counter to i+1.  (The parent hasn't had a chance
 *    to clear the counter yet.)
 *  o another counter signal is generated, but SIGIO is already
 *    pending so this one is queued
 *  o parent is notified and sees counter value i+1
 *  o parent stops delivery of first signal and disarms counter
 *  o second SIGIO dequeued and delivered, notififying parent (counter
 *    is disarmed now, so no pseudo-desched possible here)
 *  o parent notifiedand sees counter value i+1 again
 *  o parent stops delivery of second SIGIO and we continue on
 *
 * So we "work around" this by the tracer expecting two SIGIO
 * notifications, and silently discarding both. */
static __thread int desched_counter_fd;

/**
 * Return a pointer to the buffer header, which happens to occupy the
 * initial bytes in the mapped region.
 */
static struct syscallbuf_hdr* buffer_hdr()
{
	return (struct syscallbuf_hdr*)buffer;
}

/**
 * Return a pointer to the byte just after the last valid syscall record in
 * the buffer.
 */
static byte* buffer_last()
{
	return buffer + sizeof(*buffer_hdr()) + buffer_hdr()->num_rec_bytes;
}

/**
 * Return a pointer to the byte just after the very end of the mapped
 * region.
 */
static byte* buffer_end()
{
	return buffer + SYSCALLBUF_BUFFER_SIZE;
}

/* The following are wrappers for the syscalls invoked by this library
 * itself.  These syscalls will generate ptrace traps. */

static void traced__exit(int status)
{
	syscall(SYS_exit_group, status);
}

static int traced_fcntl(int fd, int cmd, ...)
{
	va_list ap;
	void *arg;

	va_start(ap, cmd);
	arg = va_arg(ap, void*);
	va_end(ap);

	return syscall(SYS_fcntl64, fd, cmd, arg);
}

static pid_t traced_getpid()
{
	return syscall(SYS_getpid);
}

static pid_t traced_gettid()
{
	return syscall(SYS_gettid);
}

static int traced_perf_event_open(struct perf_event_attr *attr,
				  pid_t pid, int cpu, int group_fd,
				  unsigned long flags)
{
    return syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static int traced_prctl(int option, unsigned long arg2, unsigned long arg3,
		     unsigned long arg4, unsigned long arg5)
{
	return syscall(SYS_prctl, option, arg2, arg3, arg4, arg4);
}

static int traced_raise(int sig)
{
	return syscall(SYS_kill, traced_getpid(), sig);
}

static int traced_sigprocmask(int how, const sigset_t* set, sigset_t* oldset)
{
	/* Warning: expecting this to only change the mask of the
	 * current task is a linux-ism; POSIX leaves the behavior
	 * undefined. */
	return syscall(SYS_rt_sigprocmask, set, oldset);
}

static ssize_t traced_write(int fd, const void* buf, size_t count)
{
	return syscall(SYS_write, fd, buf, count);
}

/* Helpers for invoking untraced syscalls, which do *not* generate
 * ptrace traps.
 *
 * XXX make a nice assembly helper like libc's |syscall()|? */
static int untraced_syscall(int syscall, long arg0, long arg1, long arg2,
			    long arg3, long arg4)
{
	int ret;
	__asm__ __volatile__("call _untraced_syscall_entry_point"
			     : "=a"(ret)
			     : "0"(syscall), "b"(arg0), "c"(arg1), "d"(arg2),
			       "S"(arg3), "D"(arg4));
	return ret;
}
#define untraced_syscall5(no, a0, a1, a2, a3, a4)	\
	untraced_syscall(no, a0, a1, a2, a3, a4)
#define untraced_syscall4(no, a0, a1, a2, a3)		\
	untraced_syscall5(no, a0, a1, a2, a3, 0)
#define untraced_syscall3(no, a0, a1, a2) untraced_syscall4(no, a0, a1, a2, 0)
#define untraced_syscall2(no, a0, a1) untraced_syscall3(no, a0, a1, 0)
#define untraced_syscall1(no, a0) untraced_syscall2(no, a0, 0)
#define untraced_syscall0(no) untraced_syscall1(no, 0)

/**
 * The seccomp filter is set up so that system calls made through
 * _untraced_syscall_entry_point are always allowed without triggering
 * ptrace. This gives us a convenient way to make non-traced system calls.
 */
__asm__(".text\n\t"
	"_untraced_syscall_entry_point:\n\t"
	"int $0x80\n\t"
	"_untraced_syscall_entry_point_ip:\n\t"
	"ret");

static void* get_untraced_syscall_entry_point()
{
    void *ret;
    __asm__ __volatile__(
	    "call _get_untraced_syscall_entry_point__pic_helper\n\t"
	    "_get_untraced_syscall_entry_point__pic_helper: pop %0\n\t"
	    "addl $(_untraced_syscall_entry_point_ip - _get_untraced_syscall_entry_point__pic_helper),%0"
	    : "=a"(ret));
    return ret;
}

/**
 * Do what's necessary to map the shared syscall buffer region in the
 * caller's address space and return the mapped region.
 * |untraced_syscall_ip| lets rr know where our untraced syscalls will
 * originate from.  |addr| is the address of the control socket the
 * child expects to connect to.  |msg| is a pre-prepared IPC that can
 * be used to share fds; |fdptr| is a pointer to the control-message
 * data buffer where the fd number being shared will be stored.
 * |args_vec| provides the tracer with preallocated space to make
 * socketcall syscalls.
 *
 * This is a "magic" syscall implemented by rr.
 */
static void* rrcall_init_syscall_buffer(void* untraced_syscall_ip,
					struct sockaddr_un* addr,
					struct msghdr* msg, int* fdptr,
					struct socketcall_args* args_vec)
{
	return (void*)syscall(RRCALL_init_syscall_buffer, untraced_syscall_ip,
			      addr, msg, fdptr, args_vec);
}

/* We can't use the rr logging helpers because they rely on libc
 * syscall-invoking functions, so roll our own here.
 *
 * XXX just use these for all logging? */

__attribute__((format(printf, 1, 2)))
static void logmsg(const char* msg, ...)
{
  va_list args;
  char buf[1024];
  int len;

  va_start(args, msg);
  len = vsnprintf(buf, sizeof(buf) - 1, msg, args);
  va_end(args);

  traced_write(STDERR_FILENO, buf, len);
}

#ifndef NDEBUG
# define assert(cond)							\
	do {								\
		if (!(cond)) {						\
			logmsg("%s:%d: Assertion " #cond "failed.",	\
			       __FILE__, __LINE__);			\
			traced_raise(SIGABRT);				\
		}							\
	} while (0)
#else
# define assert(cond) ((void)0)
#endif

#define fatal(msg, ...)							\
	do {								\
		logmsg("[FATAL] (%s:%d: errno: %s) " msg "\n",		\
		       __FILE__, __LINE__, strerror(errno), ##__VA_ARGS__); \
		traced__exit(1);					\
	} while (0)

#define log_info(msg, ...)					\
	logmsg("[INFO] (%s:%d) " msg "\n", __FILE__, __LINE__, ##__VA_ARGS__)


/**
 * This installs the actual filter which examines the callsite and
 * determines whether it will be ptraced or handled by the
 * intercepting library
 */
static void install_syscall_filter()
{
	void* protected_call_start = get_untraced_syscall_entry_point();
	struct sock_filter filter[] = {
		/* Allow all system calls from our protected_call
		 * callsite */
		ALLOW_SYSCALLS_FROM_CALLSITE((uintptr_t)protected_call_start),
		/* Grab the system call number. */
		EXAMINE_SYSCALL,
		/* Note: if these are traced, we get a SIGSTOP after
		 * child creation We don't need to trace them as they
		 * will be captured by their own ptrace event */
		ALLOW_SYSCALL(clone),
		ALLOW_SYSCALL(fork),
		/* There is really no need for us to ptrace
		 * restart_syscall. In fact, this will cause an error
		 * in case the restarted syscall is in the wrapper */
		ALLOW_SYSCALL(restart_syscall),
		/* All the rest are handled in rr */
		TRACE_PROCESS,
	};
	struct sock_fprog prog = {
		.len = (unsigned short)(sizeof(filter)/sizeof(filter[0])),
		.filter = filter,
	};

	log_info("Initializing syscall buffer: protected_call_start = %p",
		protected_call_start);

	if (traced_prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
		fatal("prctl(NO_NEW_PRIVS) failed, SECCOMP_FILTER is not available.");
	}

	/* Note: the filter is installed only for record. This call
	 * will be emulated in the replay */
	if (traced_prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER,
			 (uintptr_t)&prog, 0, 0)) {
		fatal("prctl(SECCOMP) failed, SECCOMP_FILTER is not available.");
	}
	/* anything that happens from this point on gets filtered! */
}

/**
 * Return a counter that generates a SIGIO targeted at this task every
 * time the task is descheduled |nr_descheds| times.
 */
static int open_desched_event_counter(size_t nr_descheds)
{
	struct perf_event_attr attr;
	int fd;
	struct f_owner_ex own;

	memset(&attr, 0, sizeof(attr));
	attr.size = sizeof(attr);
	attr.type = PERF_TYPE_SOFTWARE;
	attr.config = PERF_COUNT_SW_CONTEXT_SWITCHES;
	attr.disabled = 1;
	attr.sample_period = nr_descheds;

	fd = traced_perf_event_open(&attr, 0/*self*/, -1/*any cpu*/, -1, 0);
	if (0 > fd) {
		fatal("Failed to perf_event_open(cs, period=%u)", nr_descheds);
	}
	if (traced_fcntl(fd, F_SETFL, O_ASYNC)) {
		fatal("Failed to fcntl(O_ASYNC) the desched counter");
	}
	own.type = F_OWNER_TID;
	own.pid = traced_gettid();
	if (traced_fcntl(fd, F_SETOWN_EX, &own)) {
		fatal("Failed to fcntl(SETOWN_EX) the desched counter to this");
	}
	if (traced_fcntl(fd, F_SETSIG, SIGIO)) {
		fatal("Failed to fcntl(SETSIG, SIGIO) the desched counter");
	}

	return fd;
}

static void set_up_buffer()
{
	struct sockaddr_un addr;
	struct msghdr msg;
	struct iovec data;
	int msgbuf;
	struct cmsghdr* cmsg;
	int* msg_fdptr;
	int* cmsg_fdptr;
	char cmsgbuf[CMSG_SPACE(sizeof(*cmsg_fdptr))];
	struct socketcall_args args_vec;

	assert(!buffer);

	/* NB: we want this setup emulated during replay. */
	desched_counter_fd = open_desched_event_counter(1);

	/* Prepare arguments for rrcall.  We do this in the tracee
	 * just to avoid some hairy IPC to set up the arguments
	 * remotely from the traceer; this isn't strictly
	 * necessary. */
	prepare_syscallbuf_socket_addr(&addr, traced_gettid());

	memset(&msg, 0, sizeof(msg));
	msg_fdptr = &msgbuf;
	data.iov_base = msg_fdptr;
	data.iov_len = sizeof(msgbuf);
	msg.msg_iov = &data;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_len = CMSG_LEN(sizeof(*cmsg_fdptr));
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg_fdptr = (int*)CMSG_DATA(cmsg);

	/* Set the "fd parameter" in the message buffer, which we send
	 * to let the other side know the local fd number we shared to
	 * it. */
	*msg_fdptr = desched_counter_fd;
	/* Set the "fd parameter" in the cmsg buffer, which is the one
	 * the kernel parses, dups, then sets to the fd number
	 * allocated in the other process. */
	*cmsg_fdptr = desched_counter_fd;
	{
		sigset_t mask, oldmask;
		/* Create a "critical section" that can't be
		 * interrupted by signals.  rr doesn't want to deal
		 * with signals while injecting syscalls into us. */
		sigfillset(&mask);
		traced_sigprocmask(SIG_BLOCK, &mask, &oldmask);

		/* Trap to rr: let the magic begin!  We've prepared
		 * the buffer so that it's immediately ready to be
		 * sendmsg()'d to rr to share the desched counter to
		 * it (under rr's control).  rr can further use the
		 * buffer to share more fd's to us. */
		buffer = rrcall_init_syscall_buffer(
			get_untraced_syscall_entry_point(),
			&addr, &msg, cmsg_fdptr, &args_vec);
		/* rr initializes the buffer header. */

		/* End "critical section". */
		traced_sigprocmask(SIG_SETMASK, &oldmask, NULL);
	}
}

/**
 * After a fork(), the new child will still share the buffer mapping
 * with its parent.  That's obviously very bad.  Pretend that we don't
 * know about the old buffer, so that the next time a buffered syscall
 * is hit, we map a new buffer.
 */
static void drop_buffer()
{
	buffer = NULL;
	buffer_locked = 0;
}

/**
 * Initialize the library:
 * 1. Install filter-by-callsite (once for all threads)
 * 2. Make subsequent threads call init()
 * 3. Open and mmap the recording cache, shared with rr (once for
 *    every thread)
 *
 * Remember: init() will only be called if the process uses at least
 * one of the library's intercepted functions.
 */
static void init()
{
	if (!is_seccomp_bpf_installed) {
		install_syscall_filter();
		is_seccomp_bpf_installed = 1;
		pthread_atfork(NULL, NULL, drop_buffer);
	}
	set_up_buffer();
}

/**
 * Wrappers start here.
 *
 * How wrappers operate:
 *
 * 1. The syscall is intercepted by the wrapper function.
 * 2. A new record is prepared on the buffer. A record is composed of:
 * 		[the syscall number]
 * 		[the overall size in bytes of the record]
 * 		[the return value]
 * 		[other syscall output, if such exists]
 *    If the buffer runs out of space, we turn this into a
 *    non-intercepted system call which is handled by rr directly,
 *    flushing the buffer and aborting these steps.  Note: these
 *    records will be written AS-IS to the raw file, and a succinct
 *    line will be written to the trace file (without register
 *    content, etc.)
 * 3. Then, the syscall wrapper code redirects all potential output
 *    for the syscall to the record (and corrects the overall size of
 *    the record while it does so).
 * 4. The syscall is invoked directly via assembly.
 * 5. The syscall output, written on the buffer, is copied to the
 *    original pointers provided by the user.  Take notice that this
 *    part saves us the injection of the data on replay, as we only
 *    need to push the data to the buffer and the wrapper code will
 *    copy it to the user address for us.
 * 6. The first 3 parameters of the record are put in (return value
 *    and overall size are known now)
 * 7. buffer[0] is updated.
 * 8. errno is set.
 */

/**
 * Call this and save the result at the start of every system call we
 * want to buffer. The result is a pointer into the record space. You
 * can add to this pointer to allocate space in the trace record.
 * However, do not read or write through this pointer until
 * can_buffer_syscall has been called.  And you *must* call
 * can_buffer_syscall after this is called, otherwise buffering state
 * will be inconsistent between syscalls.  Usage should look something
 * like
 *
 *   // If there's something at runtime that should stop buffering the
 *   // syscall, like an unknown parameter, bail.
 *   if (!try_to_buffer()) {
 *       goto fallback_trace;
 *   }
 *
 *   // Reserve buffer space for the recorded syscall and any other
 *   // required internal bookkeeping data.
 *   void* ptr = prep_syscall(WILL_ARM_DESCHED_EVENT);
 *
 *   // If the syscall requires recording any extra data, reserve
 *   // space for it too.
 *   ptr += sizeof(extra_syscall_data);
 *
 *   // If there's not enough space, bail.
 *   if (!can_buffer_syscall(ptr)) {
 *       goto fallback_trace;
 *   }
 *
 *   // Since this syscall may block, arm/disarm the desched
 *   // notification.
 *   arm_desched_notification();
 *   untraced_syscall(...);
 *   disarm_desched_notification();
 *
 *   // Store the extra_syscall_data that space was reserved for
 *   // above.
 *
 *   // Update the buffer.
 *   return commit_syscall(..., DID_DISARM_DESCHED_EVENT);
 *
 * fallback_trace:
 *   return syscall(...);  // traced
 */
enum { WILL_ARM_DESCHED_EVENT, DISARMED_DESCHED_EVENT, NO_DESCHED };
static void* prep_syscall(int notify_desched)
{
	if (!buffer) {
		init();
	}
	if (buffer_locked) {
		/* We may be reentering via a signal handler. Return
		 * an invalid pointer.
		 */
		return NULL;
	}
	/* We don't need to worry about a race between testing
	 * buffer_locked and setting it here. rr recording is
	 * responsible for ensuring signals are not delivered during
	 * syscall_buffer prologue and epilogue code.
	 *
	 * XXX except for synchronous signals generated in the syscall
	 * buffer code, while reading/writing user pointers */
	buffer_locked = 1;
	/* "Allocate" space for a new syscall record, not including
	 * syscall outparam data. */
	return buffer_last() + sizeof(struct syscallbuf_record);
}

/**
 * Return 1 if it's ok to proceed with buffering this system call.
 * Return 0 if we should trace the system call.
 * This must be checked before proceeding with the buffered system call.
 */
static int can_buffer_syscall(void* record_end)
{
	void* record_start = buffer_last();
	void* stored_end =
		record_start + stored_record_size(record_end - record_start);

	if (stored_end < record_start + sizeof(struct syscallbuf_record)) {
		/* Either a catastrophic buffer overflow or
		 * we failed to lock the buffer. Just bail out. */
		return 0;
	}
	if (stored_end > (void*)buffer_end() - sizeof(struct syscallbuf_record)) {
		/* Buffer overflow.
		 * Unlock the buffer and then execute the system call
		 * with a trap to rr.  Note that we reserve enough
		 * space in the buffer for the next prep_syscall(). */
		buffer_locked = 0;
		return 0;
	}
	return 1;
}

inline static void arm_desched_event()
{
	/* Don't trace the ioctl; doing so would trigger a flushing
	 * ptrace trap, which is exactly what this code is trying to
	 * avoid! :) Although we don't allocate extra space for these
	 * ioctl's, we do record that we called them; the replayer
	 * knows how to skip over them. */
	if (untraced_syscall3(SYS_ioctl, desched_counter_fd,
			      PERF_EVENT_IOC_ENABLE, 0)) {
		fatal("Failed to ENABLE counter %d", desched_counter_fd);
	}
}

inline static void disarm_desched_event()
{
	/* See above. */
	if (untraced_syscall3(SYS_ioctl, desched_counter_fd,
			      PERF_EVENT_IOC_DISABLE, 0)) {
		fatal("Failed to ENABLE counter %d", desched_counter_fd);
	}
}

static int update_errno_ret(int ret)
{
	/* EHWPOISON is the last known errno as of linux 3.9.5. */
	if (0 > ret && ret >= -EHWPOISON) {
		errno = -ret;
		ret = -1;
	}
	return ret;
}

/**
 * Commit the record for a buffered system call.
 * record_end can be adjusted downward from what was passed to
 * can_buffer_syscall, if not all of the initially requested space is needed.
 * The result of this function should be returned directly by the
 * wrapper function.
 */
static int commit_syscall(int syscallno, void* record_end, int ret,
			  int disarmed_desched)
{
	void* record_start = buffer_last();
	struct syscallbuf_record* rec = record_start;
	struct syscallbuf_hdr* hdr = buffer_hdr();

	if (hdr->abort_commit) {
		/* We were descheduled in the middle of a may-block
		 * syscall, and it was recorded as a normal entry/exit
		 * pair.  So don't record the syscall in the buffer or
		 * replay will go haywire. */
		hdr->abort_commit = 0;
	} else {
		rec->ret = ret;
		rec->syscallno = syscallno;
		rec->desched = NO_DESCHED != disarmed_desched;
		rec->size = record_end - record_start;
		hdr->num_rec_bytes += stored_record_size(rec->size);
	}
	buffer_locked = 0;

	return update_errno_ret(ret);
}

int clock_gettime(clockid_t clk_id, struct timespec* tp)
{
	void* ptr = prep_syscall(NO_DESCHED);
	struct timespec *tp2 = NULL;
	long ret;

	/* Set it up so the syscall writes to the record cache. */
	if (tp) {
		tp2 = ptr;
		ptr += sizeof(struct timespec);
	}
	if (!can_buffer_syscall(ptr)) {
		return syscall(SYS_clock_gettime, clk_id, tp);
 	}
	ret = untraced_syscall2(SYS_clock_gettime, clk_id, (uintptr_t)tp2);
	/* Now in the replay we can simply refill the recorded buffer
	 * data, emulate the syscalls, and this code will restore the
	 * recorded data to the outparams. */
	if (tp) {
		memcpy(tp, tp2, sizeof(struct timespec));
	}
	return commit_syscall(SYS_clock_gettime, ptr, ret, NO_DESCHED);
}

int gettimeofday(struct timeval* tp, struct timezone* tzp)
{
	void *ptr = prep_syscall(NO_DESCHED);
	struct timeval *tp2 = NULL;
	struct timezone *tzp2 = NULL;
	long ret;

	if (tp) {
		tp2 = ptr;
		ptr += sizeof(struct timeval);
	}
	if (tzp) {
		tzp2 = ptr;
		ptr += sizeof(struct timezone);
	}
	if (!can_buffer_syscall(ptr)) {
		return syscall(SYS_gettimeofday, tp, tzp);
	}
	ret = untraced_syscall2(SYS_gettimeofday,
				    (uintptr_t)tp2, (uintptr_t)tzp2);
	if (tp) {
		memcpy(tp, tp2, sizeof(struct timeval));
	}
	if (tzp) {
		memcpy(tzp, tzp2, sizeof(struct timezone));
	}
	return commit_syscall(SYS_gettimeofday, ptr, ret, NO_DESCHED);
}

#if 0

/* Since nanosleep (almost) always blocks, it's basically pointless to
 * wrap it.  Testing only code.*/
int nanosleep(const struct timespec* req, struct timespec* rem)
{
	void* ptr = prep_syscall(WILL_ARM_DESCHED_EVENT);
	struct timespec* rem2 = NULL;
	long ret;

	if (rem) {
		rem2 = ptr;
		ptr += sizeof(*rem2);
	}
	if (!can_buffer_syscall(ptr)) {
		return syscall(SYS_nanosleep, req, rem);
	}

	arm_desched_event();
	ret = untraced_syscall2(SYS_nanosleep,
				(uintptr_t)req, (uintptr_t)rem2);
	disarm_desched_event();

	if (rem) {
		memcpy(rem, rem2, sizeof(*rem));
	}
	return commit_syscall(SYS_nanosleep, ptr, ret, DISARMED_DESCHED_EVENT);
}

/* This is useful for testing, but not much else. */
int sched_yield(void)
{
	void* ptr = prep_syscall(WILL_ARM_DESCHED_EVENT);
	if (!can_buffer_syscall(ptr)) {
		return syscall(SYS_sched_yield);
	}

	arm_desched_event();
	int ret = untraced_syscall0(SYS_sched_yield);
	disarm_desched_event();

	return commit_syscall(SYS_sched_yield, ptr, ret, DISARMED_DESCHED_EVENT);
}

/* XXX this code is effectively dead at the moment, because glibc
 * directly invokes the futex syscall.  Also, we don't have the
 * helper yet for making a six-argument syscall. */
int futex(int* uaddr, int op, int val, const struct timespec* timeout,
	  int* uaddr2, int val3)
{
	int* save_uaddr2 = NULL;
	void* ptr;
	void** save_uaddr;
	int* save_uaddr_deref;
	int ret;

	switch (op & FUTEX_CMD_MASK) {
        case FUTEX_FD:
        case FUTEX_REQUEUE:
        case FUTEX_WAKE:
        case FUTEX_WAKE_BITSET:
		/* Nothing special to do, we'll just run the system call.
		 * FUTEX_WAKE_OP writes to user space but that's OK since
		 * it doesn't block. */
		break;

        case FUTEX_CMP_REQUEUE:
        case FUTEX_CMP_REQUEUE_PI:
        case FUTEX_WAKE_OP:
		/* Record uaddr2 if it's nonnull. */
		save_uaddr2 = uaddr2;
		break;

        case FUTEX_LOCK_PI:
        case FUTEX_UNLOCK_PI:
        case FUTEX_TRYLOCK_PI:
		/* XXX we previously buffered these; was that OK?
		 * Fall through for now to be on the safe side. */

        case FUTEX_WAIT:
        case FUTEX_WAIT_BITSET:
        case FUTEX_WAIT_REQUEUE_PI:
		/* These could perhaps be emulated here, but for now,
		 * let's just fall back. */
	default:
		/* Non-accelerated op. Just perform the syscall
		 * normally. */
		goto fallback_trace;
	}

	/* Allocate space for the record, |uaddr|, |*uaddr|, and
	 * |*uaddr2| if necessary. */
	ptr = prep_syscall();
	/* XXX why do we save this? */
	save_uaddr = ptr;
	ptr += sizeof(uaddr);
	save_uaddr_deref = ptr;
	ptr += sizeof(*uaddr);
	if (save_uaddr2) {
		save_uaddr2 = ptr;
		ptr += sizeof(*save_uaddr2);
		*save_uaddr2 = *uaddr2;
	}
	if (!can_buffer_syscall(ptr)) {
		goto fallback_trace;
	}

	ret = untraced_syscall6(SYS_futex, (uintptr_t)uaddr, op, val,
				(uintptr_t)timeout,
				(uintptr_t)save_uaddr2, val3);
	*save_uaddr = uaddr;
	*save_uaddr_deref = *uaddr;
	if (save_uaddr2) {
		*uaddr2 = *save_uaddr2;
	}

	return commit_syscall(__NR_futex, ptr, ret);

fallback_trace:
	return syscall(SYS_futex, uaddr, op, val, timeout, uaddr2, val3);
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
	prep_syscall((events && maxevents > 0) ? (maxevents * sizeof(struct epoll_event)) : 0)
	void *events2 = NULL;
	if (events && maxevents > 0) {
		events2 = ptr;
		ptr +=  (maxevents * sizeof(struct epoll_event));
	}
	_syscall4(epoll_wait,epfd,events2,maxevents,timeout,ret)
	if (ret > 0) {
		record_size_in_bytes += ret * sizeof(struct epoll_event);
		memcpy(events,events2,ret * sizeof(struct epoll_event));
	}
	commit_syscall(epoll_wait)
}

/* TODO: the socketcall API can block, and we need to handle that better. */

#define _copy_socketcall_args(arg0,arg1,arg2,arg3,arg4,arg5) \
volatile long args[6] = { (long)arg0, (long)arg1, (long)arg2, (long)arg3, (long)arg4, (long)arg5 };

int accept4_(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
	/* stuff gets recorded only if addr is not null */
	prep_syscall(addr ? *addrlen + sizeof(socklen_t) : 0)
	_copy_socketcall_args(sockfd,addr,addrlen,flags, 0, 0)
	void *addr2 = NULL;
	socklen_t *addrlen2 = NULL;
	if (addr) {
		record_size_in_bytes += *addrlen;
		addr2 = ptr;
		ptr += *addrlen;
		args[1] = (long)addr2;
		record_size_in_bytes += sizeof(socklen_t);
		addrlen2 = ptr;
		ptr += sizeof(socklen_t);
		args[2] = (long)addrlen2;
	}
	_syscall2(socketcall,SYS_ACCEPT,args,ret)
	if (addr) {
		memcpy(addr, addr2, *addrlen2);
		*addrlen = *addrlen2;
	}
	commit_syscall(socketcall)
}

int accept_(int socket,struct sockaddr *addr, socklen_t *length_ptr) {
	return accept4(socket,addr,length_ptr,0);
}

int getpeername_(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
	/* stuff gets recorded only if addr is not null */
	prep_syscall(addr ? *addrlen + sizeof(socklen_t) : 0)
	_copy_socketcall_args(sockfd,addr,addrlen,0, 0, 0)
	void *addr2 = NULL;
	socklen_t *addrlen2 = NULL;
	if (addr) {
		record_size_in_bytes += *addrlen;
		addr2 = ptr;
		memcpy(addr2,addr,*addrlen);
		ptr += *addrlen;
		args[1] = (long)addr2;
		record_size_in_bytes += sizeof(socklen_t);
		addrlen2 = ptr;
		*addrlen2 = *addrlen;
		ptr += sizeof(socklen_t);
		args[2] = (long)addrlen2;
	}
	_syscall2(socketcall,SYS_GETPEERNAME,args,ret)
	if (addr) {
		memcpy(addr, addr2, *addrlen2);
		*addrlen = *addrlen2;
	}
	commit_syscall(socketcall)
}

int getsockname_(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
	/* stuff gets recorded only if addr is not null */
	prep_syscall(addr ? *addrlen + sizeof(socklen_t) : 0)
	_copy_socketcall_args(sockfd,addr,addrlen,0, 0, 0)
	void *addr2 = NULL;
	socklen_t *addrlen2 = NULL;
	if (addr) {
		record_size_in_bytes += *addrlen;
		addr2 = ptr;
		memcpy(addr2,addr,*addrlen);
		ptr += *addrlen;
		args[1] = (long)addr2;
		record_size_in_bytes += sizeof(socklen_t);
		addrlen2 = ptr;
		*addrlen2 = *addrlen;
		ptr += sizeof(socklen_t);
		args[2] = (long)addrlen2;
	}
	_syscall2(socketcall,SYS_GETSOCKNAME,args,ret)
	if (addr) {
		memcpy(addr, addr2, *addrlen2);
		*addrlen = *addrlen2;
	}
	commit_syscall(socketcall)
}

int getsockopt_(int sockfd, int level, int optname, void *optval, socklen_t* optlen) {
	/* stuff gets recorded only if optval is not null */
	prep_syscall(optval ? *optlen + sizeof(socklen_t) : 0)
	_copy_socketcall_args(sockfd,level,optname,optval, optlen, 0)
	void *optval2 = NULL;
	socklen_t *optlen2 = NULL;
	if (optval && optlen) {
		record_size_in_bytes += *optlen;
		optval2 = ptr;
		memcpy(optval2,optval,*optlen);
		ptr += *optlen;
		args[3] = (long)optval2;
		record_size_in_bytes += sizeof(socklen_t);
		optlen2 = ptr;
		*optlen2 = *optlen;
		ptr += sizeof(socklen_t);
		args[4] = (long)optlen2;
	}
	_syscall2(socketcall,SYS_GETSOCKOPT,args,ret);
	if (optval) {
		memcpy(optval, optval2, *optlen2);
		*optlen = *optlen2;
	}
	commit_syscall(socketcall)
}


ssize_t recv_(int sockfd, void *buf, size_t len, int flags) {
	/* stuff gets recorded only if buf is not null */
	prep_syscall((buf && len > 0) ? len : 0)
	_copy_socketcall_args(sockfd,buf,len,flags,0,0)
	void *buf2 = NULL;
	if (buf && len > 0) {
		record_size_in_bytes += len;
		buf2 = ptr;
		ptr += len;
		args[1] = (long)buf2;
	}
	_syscall2(socketcall,SYS_RECV,args,ret)
	if (buf && ret > 0) {
		memcpy(buf, buf2, ret);
	}
	commit_syscall(socketcall)
}

ssize_t recvfrom_(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen) {
	/* stuff gets recorded only if buf, etc. is not null */
	prep_syscall((buf ? len : 0) + (src_addr ? *addrlen +  sizeof(socklen_t) : 0))
	_copy_socketcall_args(sockfd,buf,len,flags, src_addr, addrlen)
	void *buf2 = NULL;
	if (buf) {
		record_size_in_bytes += len;
		buf2 = ptr;
		ptr += len;
		args[1] = (long)buf2;
	}
	struct sockaddr *src_addr2 = NULL;
	socklen_t *addrlen2 = NULL;
	if (src_addr) {
		record_size_in_bytes += *addrlen;
		src_addr2 = ptr;
		memcpy(src_addr2,src_addr,*addrlen);
		ptr += *addrlen;
		args[4] = (long)src_addr2;
		record_size_in_bytes += sizeof(socklen_t);
		addrlen2 = ptr;
		*addrlen2 = *addrlen;
		ptr += sizeof(socklen_t);
		args[5] = (long)addrlen2;
	}
	_syscall2(socketcall,SYS_RECVFROM,args,ret)
	if (buf)
		memcpy(buf, buf2, len);
	if (src_addr) {
		memcpy(src_addr, src_addr2, *addrlen2);
		*addrlen = *addrlen2;
	}
	commit_syscall(socketcall)
}

#define _socketcall_no_output(call,arg0,arg1,arg2,arg3,arg4,arg5) 	\
	prep_syscall(0)							\
	(void)ptr;							\
	_copy_socketcall_args(arg0,arg1,arg2,arg3,arg4,arg5)		\
	_syscall2(socketcall,call,args,ret)				\
	commit_syscall(socketcall)

int socket_(int domain, int type, int protocol) {
	_socketcall_no_output(SYS_SOCKET,domain,type,protocol,0,0,0)
}

int bind_(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
	_socketcall_no_output(SYS_BIND,sockfd,addr,addrlen,0,0,0)
}

int connect_(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
	_socketcall_no_output(SYS_CONNECT,sockfd,addr,addrlen,0,0,0)
}

int listen_(int sockfd, int backlog) {
	_socketcall_no_output(SYS_LISTEN,sockfd,backlog,0,0,0,0)
}

ssize_t sendmsg_(int sockfd, const struct msghdr *msg, int flags) {
	_socketcall_no_output(SYS_SENDMSG,sockfd,msg,flags,0,0,0)
}

ssize_t send_(int sockfd, const void *buf, size_t len, int flags) {
	_socketcall_no_output(SYS_SEND,sockfd,buf,len,flags,0,0)
}

ssize_t sendto_(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen) {
	_socketcall_no_output(SYS_SENDTO,sockfd,buf,len,flags,dest_addr,addrlen)
}

int setsockopt_(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
	_socketcall_no_output(SYS_SETSOCKOPT,sockfd,level,optname,optval,optlen,0)
}

int shutdown_(int socket, int how) {
	_socketcall_no_output(SYS_SHUTDOWN,socket,how,0,0,0,0)
}


#define _stat(call,file,buf)				\
	prep_syscall( buf ? sizeof(struct stat) : 0 )			\
	struct stat* buf2 = NULL;					\
	if (buf) {							\
		buf2 = ptr;						\
		record_size_in_bytes += sizeof(struct stat);		\
		ptr += sizeof(struct stat);				\
	}								\
	_syscall2(call,file,buf2,ret)					\
	if (ret == 0) {							\
		memcpy(buf,buf2,ret);					\
	}								\
	commit_syscall(call)

/* TODO: the stat API can block, and we need to handle that better. */

int fstat_(int fd, struct stat *buf){
	_stat(fstat64,fd,buf);
}

int lstat_(const char *path, struct stat *buf) {
	_stat(lstat64,path,buf);
}

int stat_(const char *path, struct stat *buf){
	_stat(stat64,path,buf);
}

/* TODO: fix the complex logic here */
ssize_t recvmsg_(int sockfd, struct msghdr *msg, int flags) {
	/* stuff gets recorded only if msg is not null */
	prep_syscall(msg ? (sizeof(struct msghdr)
						/*+ (msg->msg_name ? msg->msg_namelen : 0)*/
					    + (msg->msg_iov ? sizeof(struct iovec) + msg->msg_iovlen : 0)
					    + (msg->msg_control ? msg->msg_controllen : 0) )
					    : 0)
	_copy_socketcall_args(sockfd,msg,flags, 0, 0, 0)
	struct msghdr* msg2 = NULL;
	struct iovec* msg_iov2 = NULL; /* scatter/gather array */
	void* msg_iov_base2;
	void* msg_control2 = NULL; /* ancillary data, see below */
	if (msg) {
		record_size_in_bytes += sizeof(struct msghdr);
		msg2 = ptr;
		memcpy(msg2,msg,sizeof(struct msghdr));
		ptr += sizeof(struct msghdr);
		args[1] = (long)msg2;
		if (msg->msg_iov) {
			assert(msg->msg_iovlen == 1);
			record_size_in_bytes += sizeof(struct iovec) + msg->msg_iovlen;
			msg2->msg_iov = msg_iov2 = ptr;
			memcpy(msg_iov2,msg->msg_iov,sizeof(struct iovec));
			ptr += sizeof(struct iovec);
			msg2->msg_iov->iov_base = msg_iov_base2 = ptr;
			memcpy(msg_iov_base2,msg->msg_iov->iov_base,msg->msg_iov->iov_len);
			ptr += msg->msg_iov->iov_len;
		}
		if (msg->msg_control) {
			record_size_in_bytes += msg->msg_controllen;
			msg2->msg_control = msg_control2 = ptr;
			memcpy(msg_control2,msg->msg_control,msg->msg_controllen);
			ptr += msg->msg_controllen;
		}
	}
	_syscall2(socketcall,SYS_RECVMSG,args,ret)
	if (msg) {
		if (msg->msg_iov) {
			msg->msg_iov->iov_len = msg_iov2->iov_len;
			memcpy(msg->msg_iov->iov_base,msg_iov2->iov_base,msg->msg_iov->iov_len);
		}
		if (msg->msg_control) {
			msg->msg_controllen = msg2->msg_controllen;
			memcpy(msg->msg_control,msg_control2,msg->msg_controllen);
		}
		msg->msg_flags = msg2->msg_flags;
	}
	commit_syscall(socketcall)
}

/* TODO: not working */
int socketpair_(int domain, int type, int protocol, int sv[2]) {
	prep_syscall(sizeof(sv))
	_copy_socketcall_args(domain,type,protocol,sv, 0, 0)
	int * sv2;
	sv2 = ptr;
	ptr += sizeof(sv);
	record_size_in_bytes += sizeof(sv);
	args[3] = (long)sv2;
	_syscall2(socketcall,SYS_SOCKETPAIR,args,ret)
	memcpy(sv, sv2, sizeof(sv));
	commit_syscall(socketcall)
}

/* TODO: recv, recvfrom create a compilation error
int socketcall(int call, unsigned long *args){
	switch (call) {
	case SYS_SOCKET:
		return socket(args[0],args[1],args[2]);
	case SYS_BIND:
		return bind(args[0],(const struct sockaddr *)args[1],args[2]);
	case SYS_CONNECT:
		return connect(args[0],(const struct sockaddr *)args[1],args[2]);
	case SYS_LISTEN:
		return listen(args[0],args[1]);
	case SYS_ACCEPT:
		return accept(args[0],(struct sockaddr *)args[1],(socklen_t *)args[2]);
	case SYS_GETSOCKNAME:
		return getsockname(args[0],(struct sockaddr *)args[1],(socklen_t *)args[2]);
	case SYS_GETPEERNAME:
		return getpeername(args[0],(struct sockaddr *)args[1],(socklen_t *)args[2]);
	case SYS_SOCKETPAIR:
		return socketpair(args[0],args[1],args[2],(int *)args[3]);
	case SYS_SEND:
		return send(args[0],(void*)args[1],args[2],args[3]);
	case SYS_SENDTO:
		return sendto(args[0], (void *)args[1], args[2], args[3],(struct sockaddr *)args[4], args[5]);
	case SYS_RECV:
		return recv(args[0], (void *)args[1], args[2], args[3]);
	case SYS_RECVFROM:
		return recvfrom(args[0], (void *)args[1], args[2], args[3], (struct sockaddr *)args[4], (int *)args[5]);
	case SYS_SHUTDOWN:
		return shutdown(args[0], args[1]);
	case SYS_SETSOCKOPT:
		return setsockopt(args[0], args[1], args[2], (char *)args[3], args[4]);
	case SYS_GETSOCKOPT:
		return getsockopt(args[0], args[1], args[2], (char *)args[3], (int *)args[4]);
	case SYS_SENDMSG:
		return sendmsg(args[0], (struct msghdr *)args[1], args[2]);
	case SYS_SENDMMSG:
		return sendmmsg(args[0], (struct mmsghdr *)args[1], args[2], args[3]);
	case SYS_RECVMSG:
		return recvmsg(args[0], (struct msghdr *)args[1], args[2]);
	case SYS_RECVMMSG:
		return recvmmsg(args[0], (struct mmsghdr *)args[1], args[2], args[3],(struct timespec *)args[4]);
	case SYS_ACCEPT4:
		return accept4(args[0], (struct sockaddr *)args[1], (int *)args[2], args[3]);
	default:
		assert(0);
	}
}
*/


/* TODO: causes strange signal IOT */
int madvise_(void *addr, size_t length, int advice) {
	prep_syscall(0)
	(void)ptr;
	_syscall3(madvise,addr,length,advice,ret)
	commit_syscall(madvise)
}

/* TODO: makes the process die */
ssize_t read_(int fd, void *buf, size_t count) {
	prep_syscall(buf ? count : 0)
	void *buf2 = NULL;
	if (buf) {
		buf2 = ptr;
		record_size_in_bytes += count;
		ptr += count;
	}
	_syscall3(read,fd,buf2,count,ret)
	if (buf && ret > 0) {
		memcpy(buf,buf2,ret);
	}
	commit_syscall(read)
}

/* FIXME: write does not work, this has to do with the fact that it runs before stuff gets initialized in the main thread (??) */
ssize_t write_(int fd, const void *buf, size_t count) {
	prep_syscall(0)
	(void)ptr;
	_syscall3(write,fd,buf,count,ret)
	commit_syscall(write)
}

ssize_t writev_(int fd, const struct iovec *iov, int iovcnt) {
	prep_syscall(0)
	(void)ptr;
	_syscall3(writev,fd,iov,iovcnt,ret)
	commit_syscall(writev)
}

/* TODO: hangs */
int poll_(struct pollfd *fds, nfds_t nfds, int timeout) {
	size_t size = nfds * sizeof(struct pollfd);
	prep_syscall(fds ? size : 0)
	struct pollfd *fds2 = NULL;
	if (fds) {
		fds2 = ptr;
		record_size_in_bytes += size;
		memcpy(fds2, fds, size);
		ptr += size;
	}
	_syscall3(poll,fds2,nfds,timeout,ret)
	if (fds)
		memcpy(fds, fds2, size);
	commit_syscall(poll)
}

/* TODO: somehow, this slows us down. */
pid_t waitpid_(pid_t pid, int *status, int options) {
	prep_syscall(status ? sizeof(int) : 0)
	int * status2 = NULL;
	if (status) {
		status2 = ptr;
		record_size_in_bytes += sizeof(int);
		ptr += sizeof(int);
	}
	_syscall3(waitpid,pid,status2,options,ret)
	if (status) {
		*status = *status2;
	}
	commit_syscall(waitpid)
}

#endif