/*
Test executor, ssh plugin.
It is used to send tests to real machines or VMs using SSH protocol.


Copyright (C) 2014-2015 SUSE

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, version 2.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <libssh/libssh.h>
#include <libssh/callbacks.h>

#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <libgen.h>

#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <signal.h>
#include <assert.h>

#include "twopence.h"

#define BUFFER_SIZE 16384              // Size in bytes of the work buffer for receiving data from the remote host


typedef struct twopence_ssh_transaction twopence_ssh_transaction_t;

// This structure encapsulates in an opaque way the behaviour of the library
// It is not 100 % opaque, because it is publicly known that the first field is the plugin type
struct twopence_ssh_target
{
  struct twopence_target base;

  ssh_session template;

  /* Current command being executed.
   * Down the road, we can have one foreground command (which will
   * receive Ctrl-C interrupts), and any number of backgrounded commands.
   */
  struct {
    twopence_ssh_transaction_t *foreground;
  } transactions;
};

struct twopence_ssh_transaction {
  struct twopence_ssh_target *handle;

  ssh_session		session;
  ssh_channel		channel;

  /* This is used by the lower-level routines to report exceptions
   * while processing the transaction. The value of the exception
   * is a twopence error code.
   */
  int			exception;

  /* If non-NULL, this is where we store the command's status */
  twopence_status_t *	status_ret;

  struct {
    twopence_iostream_t *stream;
    struct pollfd	pfd;
    bool		eof;
    int			was_blocking;
  } stdin;

  struct twopence_ssh_output {
    int			index;		/* For ssh_channel_read() and friends */
    twopence_iostream_t *stream;
    bool		eof;
  } stdout, stderr;

  struct timeval	command_timeout;

  bool			eof_sent;
  bool			use_tty;
  bool			interrupted;
  int			exit_signal;

  /* Right now, we need the callbacks for exactly one reason -
   * to catch the exit signal of the remote process.
   * When a command dies from a signal, libssh will always report
   * an exit code of -1 (SSH_ERROR), and the only way to catch what
   * really happens is by hooking up this callback.
   */
  struct ssh_channel_callbacks_struct callbacks;
};

typedef struct twopence_scp_transaction twopence_scp_transaction_t;
struct twopence_scp_transaction {
  struct twopence_ssh_target *handle;

  ssh_session		session;
  ssh_scp		scp;

  twopence_iostream_t *	local_stream;
  long			remaining;
};

#if 0
# define SSH_TRACE(fmt...)	fprintf(stderr, fmt)
#else
# define SSH_TRACE(fmt...)	do { } while (0)
#endif

/* Note to self: if you need to find out what libssh is doing,
 * consider enabling tracing:
 *  ssh_set_log_level(SSH_LOG_TRACE);
 */

extern const struct twopence_plugin twopence_ssh_ops;

static ssh_session	__twopence_ssh_open_session(const struct twopence_ssh_target *, const char *);
static int		__twopence_ssh_interrupt_ssh(struct twopence_ssh_target *);

///////////////////////////// Lower layer ///////////////////////////////////////

// Output a "stdout" character through one of the available methods
//
// Returns 0 if everything went fine, a negative error code otherwise
static inline int
__twopence_ssh_output(struct twopence_ssh_target *handle, char c)
{
  return twopence_target_putc(&handle->base, TWOPENCE_STDOUT, c);
}

// Output a "stderr" character through one of the available methods
//
// Returns 0 if everything went fine, a negative error code otherwise
static inline int
__twopence_ssh_error(struct twopence_ssh_target *handle, char c)
{
  return twopence_target_putc(&handle->base, TWOPENCE_STDERR, c);
}

static int
__twopence_ssh_transaction_send_eof(twopence_ssh_transaction_t *trans)
{
  int rc = SSH_OK;

  if (trans->channel == NULL || trans->eof_sent)
    return SSH_OK;
  if (trans->use_tty)
    rc = ssh_channel_write(trans->channel, "\004", 1);
  if (rc == SSH_OK)
    rc = ssh_channel_send_eof(trans->channel);
  if (rc == SSH_OK)
    trans->eof_sent = true;
  return rc;
}

static void
__twopence_ssh_transaction_close_channel(twopence_ssh_transaction_t *trans)
{
  if (trans->channel) {
    ssh_channel_close(trans->channel);
    ssh_channel_free(trans->channel);
    trans->channel = NULL;
  }

  if (trans->session) {
    ssh_disconnect(trans->session);
    ssh_free(trans->session);
    trans->session = NULL;
  }
}

static twopence_ssh_transaction_t *
__twopence_ssh_transaction_new(struct twopence_ssh_target *handle, unsigned long timeout)
{
  twopence_ssh_transaction_t *trans;

  trans = calloc(1, sizeof(*trans));
  if (trans == NULL)
    return NULL;

  trans->handle = handle;
  trans->channel = NULL;
  trans->stdin.pfd.fd = -1;

  gettimeofday(&trans->command_timeout, NULL);
  trans->command_timeout.tv_sec += timeout;

  return trans;
}

void
__twopence_ssh_transaction_free(struct twopence_ssh_transaction *trans)
{
  twopence_iostream_t *stream;

  /* Reset stdin to previous behavior */
  if ((stream = trans->stdin.stream) != NULL)
    if (trans->stdin.was_blocking >= 0)
      twopence_iostream_set_blocking(stream, trans->stdin.was_blocking);

  __twopence_ssh_transaction_close_channel(trans);
  free(trans);
}

static inline void
__twopence_ssh_transaction_fail(twopence_ssh_transaction_t *trans, int error)
{
  if (!trans->exception)
    trans->exception = error;
}

static void
__twopence_ssh_transaction_setup_stdio(twopence_ssh_transaction_t *trans,
		twopence_iostream_t *stdin_stream,
		twopence_iostream_t *stdout_stream,
		twopence_iostream_t *stderr_stream)
{
  if (stdin_stream) {
    /* Set stdin to non-blocking IO */
    trans->stdin.was_blocking = twopence_iostream_set_blocking(stdin_stream, false);
    trans->stdin.stream = stdin_stream;
    trans->stdin.pfd.fd = -1;
    trans->stdin.pfd.revents = 0;
  }

  trans->stdout.index = 0;
  trans->stdout.stream = stdout_stream;

  trans->stderr.index = 1;
  trans->stderr.stream = stderr_stream;
}

static void
__twopence_ssh_exit_signal_callback(ssh_session session, ssh_channel channel, const char *signal, int core, const char *errmsg, const char *lang, void *userdata)
{
  twopence_ssh_transaction_t *trans = (twopence_ssh_transaction_t *) userdata;

  static const char *signames[NSIG] = {
	[SIGHUP] = "HUP",
	[SIGINT] = "INT",
	[SIGQUIT] = "QUIT",
	[SIGILL] = "ILL",
	[SIGTRAP] = "TRAP",
	[SIGABRT] = "ABRT",
	[SIGIOT] = "IOT",
	[SIGBUS] = "BUS",
	[SIGFPE] = "FPE",
	[SIGKILL] = "KILL",
	[SIGUSR1] = "USR1",
	[SIGSEGV] = "SEGV",
	[SIGUSR2] = "USR2",
	[SIGPIPE] = "PIPE",
	[SIGALRM] = "ALRM",
	[SIGTERM] = "TERM",
	[SIGSTKFLT] = "STKFLT",
	[SIGCHLD] = "CHLD",
	[SIGCONT] = "CONT",
	[SIGSTOP] = "STOP",
	[SIGTSTP] = "TSTP",
	[SIGTTIN] = "TTIN",
	[SIGTTOU] = "TTOU",
	[SIGURG] = "URG",
	[SIGXCPU] = "XCPU",
	[SIGXFSZ] = "XFSZ",
	[SIGVTALRM] = "VTALRM",
	[SIGPROF] = "PROF",
	[SIGWINCH] = "WINCH",
	[SIGIO] = "IO",
	[SIGPWR] = "PWR",
	[SIGSYS] = "SYS",
  };
  int signo;

  SSH_TRACE("%s(%s)\n", __func__, signal);
  for (signo = 0; signo < NSIG; ++signo) {
    const char *name = signames[signo];

    if (name && !strcmp(name, signal)) {
      trans->exit_signal = signo;
      return;
    }
  }

  trans->exit_signal = -1;
}

static void
__twopence_ssh_init_callbacks(twopence_ssh_transaction_t *trans)
{
  struct ssh_channel_callbacks_struct *cb = &trans->callbacks;

  if (cb->size == 0) {
    cb->channel_exit_signal_function = __twopence_ssh_exit_signal_callback;
    ssh_callbacks_init(cb);
  }

  if (trans->channel == NULL)
    return;

  cb->userdata = trans;
  ssh_set_channel_callbacks(trans->channel, cb);
}

static int
__twopence_ssh_transaction_open_session(twopence_ssh_transaction_t *trans, const char *username)
{
  if (!trans->handle)
    return TWOPENCE_OPEN_SESSION_ERROR;

  trans->session = __twopence_ssh_open_session(trans->handle, username);
  if (trans->session == NULL)
    return TWOPENCE_OPEN_SESSION_ERROR;

  trans->channel = ssh_channel_new(trans->session);
  if (trans->channel == NULL)
    return TWOPENCE_OPEN_SESSION_ERROR;

  if (ssh_channel_open_session(trans->channel) != SSH_OK)
    return TWOPENCE_OPEN_SESSION_ERROR;

  return 0;
}

static int
__twopence_ssh_transaction_execute_command(twopence_ssh_transaction_t *trans, twopence_command_t *cmd, twopence_status_t *status_ret)
{
  if (trans->channel == NULL)
    return TWOPENCE_OPEN_SESSION_ERROR;

  __twopence_ssh_init_callbacks(trans);

  // Request that the command be run inside a tty
  if (cmd->request_tty) {
    if (ssh_channel_request_pty(trans->channel) != SSH_OK) {
      __twopence_ssh_transaction_free(trans);
      return TWOPENCE_OPEN_SESSION_ERROR;
    }
    trans->use_tty = true;
  }

  __twopence_ssh_transaction_setup_stdio(trans,
		  &cmd->iostream[TWOPENCE_STDIN],
		  &cmd->iostream[TWOPENCE_STDOUT],
		  &cmd->iostream[TWOPENCE_STDERR]);

  // Execute the command
  if (ssh_channel_request_exec(trans->channel, cmd->command) != SSH_OK)
  {
    __twopence_ssh_transaction_free(trans);
    return TWOPENCE_SEND_COMMAND_ERROR;
  }

  trans->status_ret = status_ret;

  return 0;
}


static int
__twopence_ssh_transaction_get_exit_status(twopence_ssh_transaction_t *trans)
{
  twopence_status_t *status_ret;

  if ((status_ret = trans->status_ret) == NULL)
    return 0;

  if (trans->channel == NULL)
    return 0;

  /* If we haven't done so, send the EOF now. */
  if (__twopence_ssh_transaction_send_eof(trans) == SSH_ERROR) {
    __twopence_ssh_transaction_fail(trans, TWOPENCE_RECEIVE_RESULTS_ERROR);
    return -1;
  }

  /* Get the exit status as reported by the SSH server.
   * If the command exited with a signal, this will be SSH_ERROR;
   * but the exit_signal_callback will be invoked, which allows us
   * to snarf the exit_signal
   */
  status_ret->minor = ssh_channel_get_exit_status(trans->channel);

  if (status_ret->minor == SSH_ERROR && trans->exit_signal) {
    // mimic the behavior of the test server for now.
    // in the long run, better reporting would be great.
    status_ret->major = EFAULT;
    status_ret->minor = trans->exit_signal;
  }

  /* We successfully reported the exit status; do not try again */
  trans->status_ret = NULL;
  return 0;
}

///////////////////////////// Middle layer //////////////////////////////////////

static int
__twopence_ssh_transaction_mark_stdin_eof(twopence_ssh_transaction_t *trans)
{
  SSH_TRACE("%s: stdin is at EOF\n", __func__);
  trans->stdin.eof = true;

  if (__twopence_ssh_transaction_send_eof(trans) == SSH_ERROR)
    return -1;
  trans->stdin.pfd.fd = -1;
  return 0;
}

/*
 * Read data from stdin and forward it to the remote command
 */
static int
__twopence_ssh_transaction_forward_stdin(twopence_ssh_transaction_t *trans)
{
  twopence_iostream_t *stream;
  char buffer[BUFFER_SIZE];
  int size, written;

  stream = trans->stdin.stream;
  if (stream == NULL || twopence_iostream_eof(stream))
    return __twopence_ssh_transaction_mark_stdin_eof(trans);

  // Read from stdin
  size = twopence_iostream_read(stream, buffer, BUFFER_SIZE);
  if (size < 0) {
    if (errno != EAGAIN)               // Error
      return -1;
    return 0;
  }
  if (size == 0) {
    /* EOF from local file */
    return __twopence_ssh_transaction_mark_stdin_eof(trans);
  }

  SSH_TRACE("%s: writing %d bytes to command\n", __func__, size);
  written = ssh_channel_write(trans->channel, buffer, size);
  if (written != size)
    return -1;
  return 0;
}

/*
 * Read data from remote command and forward it to local stream
 */
static int
__twopence_ssh_transaction_forward_output(twopence_ssh_transaction_t *trans, struct twopence_ssh_output *out, const char *name)
{
  char buffer[BUFFER_SIZE];
  int size;

  if (ssh_channel_poll(trans->channel, out->index) != 0) {
    SSH_TRACE("%s: trying to read some data from %s\n", __func__, name);

    size = ssh_channel_read_nonblocking(trans->channel, buffer, sizeof(buffer), out->index);
    if (size == SSH_ERROR) {
      __twopence_ssh_transaction_fail(trans, TWOPENCE_RECEIVE_RESULTS_ERROR);
      return -1;
    }

    if (size == SSH_EOF) {
      SSH_TRACE("%s: %s is at EOF\n", __func__, name);
      out->eof = true;
    }

    /* If there is no local stream to write it to, simply drop it on the floor */
    if (size > 0 && out->stream) {
      if (twopence_iostream_write(out->stream, buffer, size) < 0) {
        __twopence_ssh_transaction_fail(trans, TWOPENCE_RECEIVE_RESULTS_ERROR);
        return -1;
      }
    }
  }

  return 0;
}

static int
__twopence_ssh_stdin_cb(socket_t fd, int revents, void *userdata)
{
  struct pollfd *pfd = (struct pollfd *) userdata;

  SSH_TRACE("%s: revents=%d\n", __func__, revents);
  pfd->revents = revents;
  return 0;
}

static int
__twopence_ssh_transaction_poll_stdin(twopence_ssh_transaction_t *trans)
{
  trans->stdin.pfd.events = 0;
  trans->stdin.pfd.fd = -1;

  while (!trans->stdin.eof) {
    twopence_iostream_t *stream;
    int n;

    if ((stream = trans->stdin.stream) == NULL) {
      __twopence_ssh_transaction_mark_stdin_eof(trans);
      break;
    }

    n = twopence_iostream_poll(stream, &trans->stdin.pfd, POLLIN);
    if (n == 0) {
      /* twopence_iostream_poll returns 0; which means it's either a buffer object
       * that has no open fd, or it's at EOF already.
       * In either case, we should try reading from this stream right away. */
      SSH_TRACE("%s: writing stdin synchronously to peer\n", __func__);
      if (__twopence_ssh_transaction_forward_stdin(trans) < 0) {
        __twopence_ssh_transaction_fail(trans, TWOPENCE_FORWARD_INPUT_ERROR);
        return -1;
      }
    }
    if (n < 0) {
      __twopence_ssh_transaction_mark_stdin_eof(trans);
      break;
    }
    if (n > 0) {
      SSH_TRACE("%s: set up stdin for polling from fd %d\n", __func__, trans->stdin.pfd.fd);
      assert(trans->stdin.pfd.fd >= 0);
      break;
    }
  }

  return 0;
}

static void
__twopence_ssh_poll_add_transaction(ssh_event event, twopence_ssh_transaction_t *trans)
{
  twopence_iostream_t *stream;

  ssh_event_add_session(event, ssh_channel_get_session(trans->channel));

  if ((stream = trans->stdin.stream) == NULL)
    __twopence_ssh_transaction_mark_stdin_eof(trans);

  if (trans->stdin.pfd.fd >= 0) {
    SSH_TRACE("poll on fd %d\n", trans->stdin.pfd.fd);
    ssh_event_add_fd(event, trans->stdin.pfd.fd, POLLIN, __twopence_ssh_stdin_cb, &trans->stdin.pfd);
  }
}

static bool
__twopence_ssh_check_timeout(const struct timeval *now, const struct timeval *expires, int *msec)
{
    struct timeval until;
    long until_ms;

    if (timercmp(expires, now, <))
      return false;

    timersub(expires, now, &until);
    until_ms = 1000 * until.tv_sec + until.tv_usec / 1000;
    if (*msec < 0 || until_ms < *msec)
      *msec = until_ms;

    return true;
}

static int
__twopence_ssh_poll(struct twopence_ssh_transaction *trans)
{
  ssh_event event;

  fflush(stdout);

  if (__twopence_ssh_transaction_poll_stdin(trans) < 0)
    return -1;

  do {
    struct timeval now;
    int timeout;
    int rc;

    if (trans->stdin.pfd.revents & (POLLIN|POLLHUP)) {
      SSH_TRACE("%s: trying to read some data from stdin\n", __func__);
      if (__twopence_ssh_transaction_forward_stdin(trans) < 0) {
	__twopence_ssh_transaction_fail(trans, TWOPENCE_FORWARD_INPUT_ERROR);
        return -1;
      }
    }
    trans->stdin.pfd.revents = 0;

    if (__twopence_ssh_transaction_forward_output(trans, &trans->stdout, "stdout") < 0)
      return -1;

    if (__twopence_ssh_transaction_forward_output(trans, &trans->stderr, "stderr") < 0)
      return -1;

    SSH_TRACE("eof=%d/%d/%d\n", trans->stdin.eof, trans->stdout.eof, trans->stderr.eof);
    if (trans->stdout.eof && trans->stderr.eof)
      return __twopence_ssh_transaction_get_exit_status(trans);

    gettimeofday(&now, NULL);
    timeout = -1;

    if (!__twopence_ssh_check_timeout(&now, &trans->command_timeout, &timeout)) {
      __twopence_ssh_transaction_fail(trans, TWOPENCE_COMMAND_TIMEOUT_ERROR);
      return -1;
    }

    event = ssh_event_new();

    __twopence_ssh_poll_add_transaction(event, trans);

    SSH_TRACE("polling for events; timeout=%d\n", timeout);
    rc = ssh_event_dopoll(event, timeout);
    ssh_event_free(event);

    if (rc == SSH_ERROR) {
      __twopence_ssh_transaction_fail(trans, TWOPENCE_RECEIVE_RESULTS_ERROR);
      return -1;
    }

    SSH_TRACE("ssh_event_dopoll() = %d\n", rc);
  } while (true);

  return 0;
}

// Send a file in chunks through SCP
//
// Returns 0 if everything went fine, or a negative error code if failed
static int
__twopence_ssh_send_file(twopence_scp_transaction_t *trans, twopence_status_t *status)
{
  char buffer[BUFFER_SIZE];
  int size, received;

  while (trans->remaining > 0) {
    size = trans->remaining;
    if (size > BUFFER_SIZE)
      size = BUFFER_SIZE;

    received = twopence_iostream_read(trans->local_stream, buffer, size);
    if (received != size)
    {
      __twopence_ssh_output(trans->handle, '\n');
      return TWOPENCE_LOCAL_FILE_ERROR;
    }

    if (ssh_scp_write (trans->scp, buffer, size) != SSH_OK)
    {
      status->major = ssh_get_error_code(trans->session);
      __twopence_ssh_output(trans->handle, '\n');
      return TWOPENCE_SEND_FILE_ERROR;
    }

    __twopence_ssh_output(trans->handle, '.');     // Progression dots
    trans->remaining -= size;                 // That much we don't need to send anymore
  }
  __twopence_ssh_output(trans->handle, '\n');
  return 0;
}

// Receive a file in chunks through SCP
//
// Returns 0 if everything went fine, or a negative error code if failed
static int
__twopence_ssh_receive_file(twopence_scp_transaction_t *trans, twopence_status_t *status)
{
  char buffer[BUFFER_SIZE];
  int size, received, written;

  while (trans->remaining > 0) {
    size = trans->remaining;
    if (size > BUFFER_SIZE)
      size = BUFFER_SIZE;

    received = ssh_scp_read(trans->scp, buffer, size);
    if (received != size)
    {
      status->major = ssh_get_error_code(trans->session);
      __twopence_ssh_output(trans->handle, '\n');
      return TWOPENCE_RECEIVE_FILE_ERROR;
    }

    written = twopence_iostream_write(trans->local_stream, buffer, size);
    if (written != size)
    {
      __twopence_ssh_output(trans->handle, '\n');
      return TWOPENCE_LOCAL_FILE_ERROR;
    }

    __twopence_ssh_output(trans->handle, '.');     // Progression dots
    trans->remaining -= size;                 // That's that much less to receive
  }
  __twopence_ssh_output(trans->handle, '\n');
  return 0;
}

///////////////////////////// Top layer /////////////////////////////////////////

// Open a SSH session as some user
//
// Returns 0 if everything went fine, a negative error code otherwise
static ssh_session
__twopence_ssh_open_session(const struct twopence_ssh_target *handle, const char *username)
{
  ssh_session session;

  if (username == NULL)
    username = "root";

  // Create a new session based on the session template
  session = ssh_new();                 // FIXME: according to the documentation, we should not allocate 'session' ourselves (?)
  if (session == NULL)
    return NULL;
  if (ssh_options_copy(handle->template, &session) < 0)
  {
    ssh_free(session);
    return NULL;
  }

  // Store the username
  if (ssh_options_set(session, SSH_OPTIONS_USER, username) < 0)
  {
    ssh_free(session);
    return NULL;
  }

  // Connect to the server
  if (ssh_connect(session) != SSH_OK)
  {
    ssh_free(session);
    return NULL;
  }

  // Authenticate with our private key, with no passphrase
  // That's the only available method, given that we are in the context of testing
  // For safety reasons, do not use such private keys with no passphrases to access production systems
  if (ssh_userauth_autopubkey(session, NULL) != SSH_AUTH_SUCCESS)
  {
    ssh_disconnect(session);
    ssh_free(session);
    return NULL;
  }

  return session;
}

// Submit a command to the remote host
//
// Returns 0 if everything went fine, a negative error code otherwise
static int
__twopence_ssh_command_ssh
    (struct twopence_ssh_target *handle, twopence_command_t *cmd, twopence_status_t *status_ret)
{
  twopence_ssh_transaction_t *trans = NULL;
  int rc;

  trans = __twopence_ssh_transaction_new(handle, cmd->timeout);
  if (trans == NULL)
    return TWOPENCE_OPEN_SESSION_ERROR;

  handle->transactions.foreground = trans;

  rc = __twopence_ssh_transaction_open_session(trans, cmd->user);
  if (rc != 0) {
    __twopence_ssh_transaction_free(trans);
    return rc;
  }

  status_ret->minor = 0;
  rc = __twopence_ssh_transaction_execute_command(trans, cmd, status_ret);
  if (rc != 0) {
    __twopence_ssh_transaction_free(trans);
    return rc;
  }

  // Read "standard output", "standard error", and remote error code
  rc = __twopence_ssh_poll(trans);

  if (rc < 0) {
    assert(trans->exception < 0);
    rc = trans->exception;
  }

  // Terminate the channel
  __twopence_ssh_transaction_free(trans);
  handle->transactions.foreground = NULL;

  return rc;
}

/*
 * SCP transaction functions
 */
static void
twopence_scp_transfer_init(twopence_scp_transaction_t *state, struct twopence_ssh_target *handle)
{
  memset(state, 0, sizeof(*state));
  state->handle = handle;
}

static void
twopence_scp_transfer_destroy(twopence_scp_transaction_t *trans)
{
  if (trans->scp) {
    ssh_scp_close(trans->scp);
    ssh_scp_free(trans->scp);
    trans->scp = NULL;
  }
  if (trans->session) {
    ssh_disconnect(trans->session);
    ssh_free(trans->session);
    trans->session = NULL;
  }
}

static int
twopence_scp_transfer_open_session(twopence_scp_transaction_t *trans, const char *username)
{
  trans->session = __twopence_ssh_open_session(trans->handle, username);
  if (trans->session == NULL)
    return TWOPENCE_OPEN_SESSION_ERROR;

  return 0;
}

static int
twopence_scp_transfer_init_copy(twopence_scp_transaction_t *trans, int direction, const char *remote_name)
{
  trans->scp = ssh_scp_new(trans->session, direction, remote_name);
  if (trans->scp == NULL)
    return TWOPENCE_OPEN_SESSION_ERROR;
  if (ssh_scp_init(trans->scp) != SSH_OK)
    return TWOPENCE_OPEN_SESSION_ERROR;

  return 0;
}

static bool
__twopence_ssh_check_remote_dir(ssh_session session, const char *remote_dirname)
{
  ssh_scp scp = NULL;
  bool exists = false;

  scp = ssh_scp_new(session, SSH_SCP_READ|SSH_SCP_RECURSIVE, remote_dirname);
  if (scp != NULL
   && ssh_scp_init(scp) == SSH_OK
   && ssh_scp_pull_request(scp) == SSH_SCP_REQUEST_NEWDIR)
    exists = true;

  if (scp) {
    ssh_scp_close(scp);
    ssh_scp_free(scp);
  }

  return exists;
}

// Inject a file into the remote host through SSH
//
// Returns 0 if everything went fine
static int
__twopence_ssh_inject_ssh(twopence_scp_transaction_t *trans, twopence_file_xfer_t *xfer,
		const char *remote_dirname, const char *remote_basename,
		twopence_status_t *status)
{
  long filesize;
  int rc;

  filesize = twopence_iostream_filesize(xfer->local_stream);
  assert(filesize >= 0);

  /* Unfortunately, we have to make sure the remote directory exists.
   * In openssh-6.2p2 (and maybe others), if you try to create file
   * "foo" inside non-existant directory "/bar" will result in the
   * creation of regular file "/bar" and upload the content there.
   */
  if (!__twopence_ssh_check_remote_dir(trans->session, remote_dirname))
    return TWOPENCE_SEND_FILE_ERROR;

  if ((rc = twopence_scp_transfer_init_copy(trans, SSH_SCP_WRITE, remote_dirname)) < 0)
    return rc;

  // Tell the remote host about the file size
  if (ssh_scp_push_file(trans->scp, remote_basename, filesize, xfer->remote.mode) != SSH_OK)
  {
    status->major = ssh_get_error_code(trans->session);
    return TWOPENCE_SEND_FILE_ERROR;
  }

  trans->local_stream = xfer->local_stream;
  trans->remaining = filesize;

  // Send the file
  return __twopence_ssh_send_file(trans, status);
}

// Extract a file from the remote host through SSH
//
// Returns 0 if everything went fine
static int
__twopence_ssh_extract_ssh(twopence_scp_transaction_t *trans, twopence_file_xfer_t *xfer, twopence_status_t *status)
{
  int size, rc;

  if ((rc = twopence_scp_transfer_init_copy(trans, SSH_SCP_READ, xfer->remote.name)) < 0)
    return rc;

  // Get the file size from the remote host
  if (ssh_scp_pull_request(trans->scp) != SSH_SCP_REQUEST_NEWFILE)
    goto receive_file_error;
  size = ssh_scp_request_get_size(trans->scp);
  if (!size)
    return 0;

  // Accept the transfer request
  if (ssh_scp_accept_request(trans->scp) != SSH_OK)
    goto receive_file_error;

  trans->local_stream = xfer->local_stream;
  trans->remaining = size;

  // Receive the file
  rc = __twopence_ssh_receive_file(trans, status);
  if (rc < 0)
    return rc;

  // Check for proper termination
  if (ssh_scp_pull_request(trans->scp) != SSH_SCP_REQUEST_EOF)
    goto receive_file_error;

  return 0;

receive_file_error:
  status->major = ssh_get_error_code(trans->session);
  return TWOPENCE_RECEIVE_FILE_ERROR;
}

// Interrupt current command
//
// Returns 0 if everything went fine, or a negative error code if failed
static int
__twopence_ssh_interrupt_ssh(struct twopence_ssh_target *handle)
{
  twopence_ssh_transaction_t *trans;
  ssh_channel channel = NULL;

  if ((trans = handle->transactions.foreground) == NULL
   || (channel = trans->channel) == NULL)
    return TWOPENCE_OPEN_SESSION_ERROR;

#if 0
  // This is currently completly useless with OpenSSH
  // (see https://bugzilla.mindrot.org/show_bug.cgi?id=1424)
  if (ssh_channel_request_send_signal(channel, "INT") != SSH_OK)
    return TWOPENCE_INTERRUPT_COMMAND_ERROR;
#else
  if (trans->use_tty) {
    if (trans->eof_sent) {
      printf("Cannot send Ctrl-C, channel already closed for writing\n");
      return TWOPENCE_INTERRUPT_COMMAND_ERROR;
    }

    if (ssh_channel_write(channel, "\003", 1) != 1)
      return TWOPENCE_INTERRUPT_COMMAND_ERROR;
  } else {
    printf("Command not being run in tty, cannot interrupt it\n");
    trans->interrupted = true;
  }
#endif

  return 0;
}

///////////////////////////// Public interface //////////////////////////////////

// Initialize the library
//
// This specific plugin takes an IP address or an hostname as argument
//
// Returns a "handle" that must be passed to subsequent function calls,
// or NULL in case of a problem
static struct twopence_target *
__twopence_ssh_init(const char *hostname, unsigned int port)
{
  struct twopence_ssh_target *handle;
  ssh_session template;

  // Allocate the opaque handle
  handle = calloc(1, sizeof(struct twopence_ssh_target));
  if (handle == NULL) return NULL;

  // Store the plugin type
  handle->base.plugin_type = TWOPENCE_PLUGIN_SSH;
  handle->base.ops = &twopence_ssh_ops;

  // Create the SSH session template
  template = ssh_new();
  if (template == NULL)
  {
    free(handle);
    return NULL;
  }

  // Store the hostname and the port number
  if (ssh_options_set(template, SSH_OPTIONS_HOST, hostname) < 0 ||
      ssh_options_set(template, SSH_OPTIONS_PORT, &port) < 0
     )
  {
    ssh_free(template);
    free(handle);
    return NULL;
  }

  // Register the SSH session template and return the handle
  handle->template = template;
  return (struct twopence_target *) handle;
};

//////////////////////////////////////////////////////////////////
// This is the new way of initializing the library.
// This function expects just the part of the target spec following
// the "ssh:" plugin type.
//////////////////////////////////////////////////////////////////
static struct twopence_target *
twopence_ssh_init(const char *arg)
{
  char *copy_spec, *s, *hostname;
  struct twopence_target *target = NULL;
  unsigned long port;

  /* The arg can have a trailing ":<portnum>" portion. Split
   * that off. */
  if (strrchr(arg, ':') == NULL) {
    /* Just a hostname */
    return __twopence_ssh_init(arg, 22);
  }

  copy_spec = strdup(arg);
  s = strrchr(copy_spec, ':');
  *s++ = '\0';
 
  port = strtoul(s, &s, 10);
  if (*s != '\0' || port >= 65535) {
    /* FIXME: we should complain about an invalid port number.
     * Right now, we just fail silently - as we do with every
     * other invalid piece of input. 
     */
    free(copy_spec);
    return NULL;
  }

  /* The hostname portion may actually be an IPv6 like [::1].
   * Strip off the outer brackets */
  hostname = copy_spec;
  if (*hostname == '[') {
    int n = strlen(hostname);

    if (hostname[n-1] == ']') {
      hostname[n-1] = '\0';
      ++hostname;
    }
  }

  target = __twopence_ssh_init(hostname, port);

  free(copy_spec);
  return target;
}

/*
 * Run a test
 */
static int
twopence_ssh_run_test
  (struct twopence_target *opaque_handle, twopence_command_t *cmd, twopence_status_t *status_ret)
{
  struct twopence_ssh_target *handle = (struct twopence_ssh_target *) opaque_handle;

  if (cmd->command == NULL)
    return TWOPENCE_PARAMETER_ERROR;

  /* 'major' makes no sense for SSH and 'minor' defaults to 0 */
  memset(status_ret, 0, sizeof(*status_ret));

  /* We no longer use the handle's current.io, but move that to the
   * transaction object. This is a precondition to having multiple
   * concurrent commands.
   */
  handle->base.current.io = NULL;

  // Execute the command
  return __twopence_ssh_command_ssh(handle, cmd, status_ret);
}


// Inject a file into the remote host
//
// Returns 0 if everything went fine
static int
twopence_ssh_inject_file(struct twopence_target *opaque_handle,
		twopence_file_xfer_t *xfer, twopence_status_t *status)
{
  struct twopence_ssh_target *handle = (struct twopence_ssh_target *) opaque_handle;
  twopence_scp_transaction_t state;
  char *dirname, *basename;
  long filesize;
  int rc;

  // Connect to the remote host
  twopence_scp_transfer_init(&state, handle);
  if ((rc = twopence_scp_transfer_open_session(&state, xfer->user)) < 0)
    return rc;

  dirname = ssh_dirname(xfer->remote.name);
  basename = ssh_basename(xfer->remote.name);

  /* Unfortunately, the SCP protocol requires the size of the file to be
   * transmitted :-(
   *
   * If we've been asked to read from eg a pipe or some other special
   * iostream, just buffer everything and then send it as a whole.
   */
  filesize = twopence_iostream_filesize(xfer->local_stream);
  if (filesize < 0) {
    twopence_file_xfer_t tmp_xfer = *xfer;
    twopence_buf_t *bp;

    bp = twopence_iostream_read_all(xfer->local_stream);
    if (bp == NULL)
      return TWOPENCE_LOCAL_FILE_ERROR;

    tmp_xfer.local_stream = NULL;
    twopence_iostream_wrap_buffer(bp, false, &tmp_xfer.local_stream);
    rc = __twopence_ssh_inject_ssh(&state, &tmp_xfer, dirname, basename, status);
    twopence_iostream_free(tmp_xfer.local_stream);
  } else {
    rc = __twopence_ssh_inject_ssh(&state, xfer, dirname, basename, status);
  }

  if (rc == 0 && (status->major != 0 || status->minor != 0))
    rc = TWOPENCE_REMOTE_FILE_ERROR;

  /* Destroy all state, and disconnect from remote host */
  twopence_scp_transfer_destroy(&state);

  /* Clean up */
  free(basename);
  free(dirname);

  return rc;
}

// Extract a file from the remote host
//
// Returns 0 if everything went fine
static int
twopence_ssh_extract_file(struct twopence_target *opaque_handle,
		twopence_file_xfer_t *xfer, twopence_status_t *status)
{
  struct twopence_ssh_target *handle = (struct twopence_ssh_target *) opaque_handle;
  twopence_scp_transaction_t state;
  int rc;

  // Connect to the remote host
  twopence_scp_transfer_init(&state, handle);
  if ((rc = twopence_scp_transfer_open_session(&state, xfer->user)) < 0)
    return rc;

  // Extract the file
  rc = __twopence_ssh_extract_ssh(&state, xfer, status);
  if (rc == 0 && (status->major != 0 || status->minor != 0))
    rc = TWOPENCE_REMOTE_FILE_ERROR;

  return rc;
}

// Interrupt current command
//
// Returns 0 if everything went fine
static int
twopence_ssh_interrupt_command(struct twopence_target *opaque_handle)
{
  struct twopence_ssh_target *handle = (struct twopence_ssh_target *) opaque_handle;

  return __twopence_ssh_interrupt_ssh(handle);
}

// Tell the remote test server to exit
//
// Returns 0 if everything went fine
static int
twopence_ssh_exit_remote(struct twopence_target *opaque_handle)
{
  return -1;                           // Makes no sense with SSH
}

// Close the library
static void
twopence_ssh_end(struct twopence_target *opaque_handle)
{
  struct twopence_ssh_target *handle = (struct twopence_ssh_target *) opaque_handle;

  ssh_free(handle->template);
  free(handle);
}

/*
 * Define the plugin ops vector
 */
const struct twopence_plugin twopence_ssh_ops = {
	.name		= "ssh",

	.init = twopence_ssh_init,
	.run_test = twopence_ssh_run_test,
	.inject_file = twopence_ssh_inject_file,
	.extract_file = twopence_ssh_extract_file,
	.exit_remote = twopence_ssh_exit_remote,
	.interrupt_command = twopence_ssh_interrupt_command,
	.end = twopence_ssh_end,
};
