/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

//#define DEBUGTAG "Diverter"

#include "diverter.h"

#include <sys/syscall.h>

#include "debugger_gdb.h"
#include "log.h"
#include "remote_syscalls.h"
#include "replayer.h"
#include "session.h"
#include "task.h"

// The global diversion session, of which there can only be one at a
// time currently.  See long comment at the top of diverter.h.
static ReplaySession::shr_ptr session;

static void finish_emulated_syscall_with_ret(Task* t, long ret) {
  Registers r = t->regs();
  r.set_syscall_result(ret);
  t->set_regs(r);
  t->finish_emulated_syscall();
}

/**
 * Execute the syscall contained in |t|'s current register set.  The
 * return value of the syscall is set for |t|'s registers, to be
 * returned to the tracee task.
 */
static void execute_syscall(Task* t) {
  t->finish_emulated_syscall();

  AutoRemoteSyscalls remote(t);
  remote.syscall(remote.regs().original_syscallno(), remote.regs().arg1(),
                 remote.regs().arg2(), remote.regs().arg3(),
                 remote.regs().arg4(), remote.regs().arg5(),
                 remote.regs().arg6());
  remote.regs().set_syscall_result(t->regs().syscall_result());
}

static void process_syscall(Task* t, int syscallno) {
  LOG(debug) << "Processing " << t->syscallname(syscallno);

  switch (syscallno) {
    // The arm/disarm-desched ioctls are emulated as no-ops.
    // However, because the rr preload library expects these
    // syscalls to succeed and aborts if they don't, we fudge a
    // "0" return value.
    case SYS_ioctl:
      if (!t->is_desched_event_syscall()) {
        break;
      }
      finish_emulated_syscall_with_ret(t, 0);
      return;

    // We blacklist these syscalls because the params include
    // namespaced identifiers that are different in replay than
    // recording, and during replay they may refer to different,
    // live resources.  For example, if a recorded tracees kills
    // one of its threads, then during replay that killed pid
    // might refer to a live process outside the tracee tree.  We
    // don't want diversion tracees randomly shooting down other
    // processes!
    //
    // We optimistically assume that filesystem operations were
    // intended by the user.
    //
    // There's a potential problem with "fd confusion": in the
    // diversion tasks, fds returned from open() during replay are
    // emulated.  But those fds may accidentally refer to live fds
    // in the task fd table.  So write()s etc may not be writing
    // to the file the tracee expects.  However, the only real fds
    // that leak into tracees are the stdio fds, and there's not
    // much harm that can be caused by accidental writes to them.
    case SYS_ipc:
    case SYS_kill:
    case SYS_rt_sigqueueinfo:
    case SYS_rt_tgsigqueueinfo:
    case SYS_tgkill:
    case SYS_tkill:
      return;
  }

  return execute_syscall(t);
}

/**
 * Advance execution of |t| according to |req| until either a signal
 * is received (including a SIGTRAP generated by a single-step) or a
 * syscall is made.  Return false when "interrupted" by a signal, true
 * when a syscall is made.
 */
static bool advance(Task* t, const struct dbg_request& req) {
  assert(!t->child_sig);

  switch (req.type) {
    case DREQ_CONTINUE:
      LOG(debug) << "Continuing to next syscall";
      t->cont_sysemu();
      break;
    case DREQ_STEP:
      t->cont_sysemu_singlestep();
      LOG(debug) << "Stepping to next insn/syscall";
      break;
    default:
      FATAL() << "Illegal debug request " << req.type;
  }
  if (t->pending_sig()) {
    return false;
  }
  process_syscall(t, t->regs().original_syscallno());
  return true;
}

/**
 * Process debugger requests made through |dbg| until action needs to
 * be taken by the caller (a resume-execution request is received).
 * The returned Task* is the target of the resume-execution request.
 *
 * The received request is returned through |req|.
 */
static Task* process_debugger_requests(struct dbg_context* dbg, Task* t,
                                       struct dbg_request* req) {
  while (true) {
    *req = dbg_get_request(dbg);

    if (dbg_is_resume_request(req)) {
      if (session->diversion_dying()) {
        return nullptr;
      }
      return t;
    }

    switch (req->type) {
      case DREQ_RESTART:
        return nullptr;

      case DREQ_READ_SIGINFO: {
        LOG(debug) << "Adding ref to diversion session";
        session->diversion_ref();
        // TODO: maybe share with replayer.cc?
        byte si_bytes[req->mem.len];
        memset(si_bytes, 0, sizeof(si_bytes));
        dbg_reply_read_siginfo(dbg, si_bytes, sizeof(si_bytes));
        continue;
      }
      case DREQ_SET_QUERY_THREAD: {
        Task* next_task = t->session().find_task(req->target.tid);
        t = next_task ? next_task : t;
        break;
      }
      case DREQ_WRITE_SIGINFO:
        LOG(debug) << "Removing reference to diversion session ...";
        session->diversion_unref();
        if (session->diversion_dying()) {
          LOG(debug) << "  ... dying at next continue request";
        }
        dbg_reply_write_siginfo(dbg);
        continue;

      case DREQ_REMOVE_SW_BREAK:
      case DREQ_REMOVE_HW_BREAK:
      case DREQ_REMOVE_RD_WATCH:
      case DREQ_REMOVE_WR_WATCH:
      case DREQ_REMOVE_RDWR_WATCH:
      case DREQ_SET_SW_BREAK:
      case DREQ_SET_HW_BREAK:
      case DREQ_SET_RD_WATCH:
      case DREQ_SET_WR_WATCH:
      case DREQ_SET_RDWR_WATCH: {
        // Setting breakpoints in a dying diversion is assumed
        // to be a user action intended for the replay
        // session, so return to it now.
        if (session->diversion_dying()) {
          return nullptr;
        }
        break;
      }

      default:
        break;
    }

    dispatch_debugger_request(*session, dbg, t, *req);
  }
}

void divert(ReplaySession& replay, struct dbg_context* dbg, pid_t task,
            struct dbg_request* req) {
  LOG(debug) << "Starting debugging diversion for " << HEX(&replay);

  session = replay.clone_diversion();
  Task* t = session->find_task(task);
  while (true) {
    if (!(t = process_debugger_requests(dbg, t, req))) {
      break;
    }
    if (!advance(t, *req)) {
      dbg_threadid_t thread;
      thread.pid = t->tgid();
      thread.tid = t->rec_tid;

      int sig = t->pending_sig();
      LOG(debug) << "Tracee raised " << signalname(sig);
      if (SIGTRAP != sig &&
          (TRAP_BKPT_USER == t->vm()->get_breakpoint_type_at_addr(t->ip()))) {
        // See comment in replayer.cc near
        // breakpoint-dispatch code.
        sig = SIGTRAP;
      }
      LOG(debug) << "  notifying debugger of " << signalname(sig);
      dbg_notify_stop(dbg, thread, sig);
    }
  }

  LOG(debug) << "... ending debugging diversion";
  assert(session->diversion_dying());
  session->kill_all_tasks();
  session = nullptr;
}
