#include <errno.h>
#include <stdio.h>  /* freopen */
#include <stdlib.h>  /* NULL */
#include <string.h>  /* memset */
#include <unistd.h>  /* fork, setsid */

#include "uv.h"

#include "master.h"
#include "common.h"
#include "config.h"
#include "error.h"
#include "logger.h"
#include "server.h"
#include "client.h"
#include "ipc.h"

#ifndef _WIN32
static int bud_daemonize(bud_error_t* err);
static bud_error_t bud_master_init_signals(bud_config_t* config);
static void bud_master_signal_close_cb(uv_handle_t* handle);
static void bud_master_signal_cb(uv_signal_t* handle, int signum);
#endif  /* !_WIN32 */
static bud_error_t bud_master_spawn_workers(bud_config_t* config);
static bud_error_t bud_master_spawn_worker(bud_worker_t* worker);
static void bud_master_kill_worker(bud_worker_t* worker,
                                   uint64_t delay,
                                   bud_worker_kill_cb cb);
static void bud_worker_timer_cb(uv_timer_t* handle);
static void bud_worker_close_cb(uv_handle_t* handle);
static void bud_master_respawn_worker(uv_process_t* proc,
                                      int64_t exit_status,
                                      int term_signal);
static void bud_master_ipc_close_cb(uv_handle_t* handle);


bud_error_t bud_master(bud_config_t* config) {
  int i;
  bud_error_t err;

  bud_log(config, kBudLogDebug, "master starting");

#ifndef _WIN32
  if (config->is_daemon)
    if (bud_daemonize(&err) != 0)
      goto fatal;
#endif  /* !_WIN32 */

  /* Create loop after forking */
  config->loop = uv_default_loop();
  if (config->loop == NULL) {
    err = bud_error_str(kBudErrNoMem, "config->loop");
    goto fatal;
  }

#ifndef _WIN32
  /* Initialize signal watchers */
  err = bud_master_init_signals(config);
  if (!bud_is_ok(err))
    goto fatal;
#endif  /* !_WIN32 */

  /* Create server and send it to all workers */
  err = bud_server_new(config);
  if (!bud_is_ok(err))
    goto fatal;

  err = bud_master_spawn_workers(config);

  if (bud_is_ok(err)) {
    bud_log(config,
            kBudLogInfo,
            "bud listening on [%s]:%d and...",
            config->frontend.host,
            config->frontend.port);
    for (i = 0; i < config->contexts[0].backend.count; i++) {
      bud_log(config,
              kBudLogInfo,
              "...forwarding to: [%s]:%d",
              config->contexts[0].backend.list[i].host,
              config->contexts[0].backend.list[i].port);
    }
  }

  /* Drop privileges */
  err = bud_config_drop_privileges(config);

fatal:
  return err;
}


bud_error_t bud_master_finalize(bud_config_t* config) {
  int i;

  for (i = 0; i < config->worker_count; i++)
    if (config->workers[i].state & kBudWorkerStateActive)
      bud_master_kill_worker(&config->workers[i], 0, NULL);

#ifndef _WIN32
  uv_close((uv_handle_t*) config->signal.sigterm, bud_master_signal_close_cb);
  uv_close((uv_handle_t*) config->signal.sigint, bud_master_signal_close_cb);
  uv_close((uv_handle_t*) config->signal.sighup, bud_master_signal_close_cb);
#endif  /* !_WIN32 */

  bud_server_free(config);

  return bud_ok();
}


#ifndef _WIN32
int bud_daemonize(bud_error_t* err) {
  pid_t p;

  p = fork();
  if (p > 0) {
    /* Make parent exit */
    exit(0);
  } else if (p == -1) {
    *err = bud_error_num(kBudErrForkFailed, errno);
    return -1;
  }

  /* Child starts new life here */
  if (chdir("/") != 0) {
    *err = bud_error_num(kBudErrChdirFailed, errno);
    return -1;
  }

  p = setsid();
  if (p == -1) {
    *err = bud_error_num(kBudErrSetsidFailed, errno);
    return -1;
  }

  freopen("/dev/null", "r", stdin);
  freopen("/dev/null", "w", stdout);
  freopen("/dev/null", "w", stderr);
  if (stdin == NULL || stdout == NULL || stderr == NULL) {
    *err = bud_error(kBudErrNoMem);
    return -1;
  }

  return 0;
}


bud_error_t bud_master_init_signals(bud_config_t* config) {
  int r;
  bud_error_t err;

  config->signal.sigterm = malloc(sizeof(*config->signal.sigterm));
  config->signal.sigint = malloc(sizeof(*config->signal.sigint));
  config->signal.sighup = malloc(sizeof(*config->signal.sighup));
  if (config->signal.sigterm == NULL ||
      config->signal.sigint == NULL ||
      config->signal.sighup == NULL) {
    err = bud_error_str(kBudErrNoMem, "master uv_signal_t");
    goto fatal;
  }

  config->signal.sigterm->data = config;
  config->signal.sigint->data = config;
  config->signal.sighup->data = config;

  r = uv_signal_init(config->loop, config->signal.sigterm);
  if (r != 0) {
    err = bud_error_num(kBudErrSignalInit, r);
    goto fatal;
  }
  r = uv_signal_init(config->loop, config->signal.sigint);
  if (r != 0) {
    err = bud_error_num(kBudErrSignalInit, r);
    goto failed_sigint_init;
  }
  r = uv_signal_init(config->loop, config->signal.sighup);
  if (r != 0) {
    err = bud_error_num(kBudErrSignalInit, r);
    goto failed_sighup_init;
  }

  r = uv_signal_start(config->signal.sigterm, bud_master_signal_cb, SIGTERM);
  if (r == 0)
    r = uv_signal_start(config->signal.sigint, bud_master_signal_cb, SIGINT);
  if (r == 0)
    r = uv_signal_start(config->signal.sighup, bud_master_signal_cb, SIGHUP);
  if (r != 0) {
    err = bud_error_num(kBudErrSignalStart, r);
    goto failed_signal_start;
  }

  /* Signals should not keep loop running */
  uv_unref((uv_handle_t*) config->signal.sigint);
  uv_unref((uv_handle_t*) config->signal.sigterm);
  uv_unref((uv_handle_t*) config->signal.sighup);

  return bud_ok();

failed_signal_start:
  uv_close((uv_handle_t*) config->signal.sighup, bud_master_signal_close_cb);

failed_sighup_init:
  uv_close((uv_handle_t*) config->signal.sigint, bud_master_signal_close_cb);

failed_sigint_init:
  uv_close((uv_handle_t*) config->signal.sigterm, bud_master_signal_close_cb);
  goto cleanup;

fatal:
  free(config->signal.sigterm);
  free(config->signal.sigint);
  free(config->signal.sighup);

cleanup:
  config->signal.sigterm = NULL;
  config->signal.sigint = NULL;
  config->signal.sighup = NULL;
  return err;
}


void bud_master_signal_close_cb(uv_handle_t* handle) {
  free(handle);
}


void bud_master_signal_cb(uv_signal_t* handle, int signum) {
  bud_config_t* config;
  bud_worker_t* stale;
  bud_error_t err;
  int i;

  config = handle->data;

  /* Stop the loop and let finalize to be called */
  if (config->signal.sighup != handle)
    return uv_stop(handle->loop);

  /* SIGHUP: 0 workers, handle it */
  if (config->worker_count == 0) {
    bud_log(config, kBudLogWarning, "Can't reload config in 0-workers setup");
    return;
  }

  /* SIGHUP - gracefully restart workers */
  bud_log(config,
          kBudLogInfo,
          "master got SIGHUP, broadcasting to workers");
  stale = config->workers;

  /* Allocate new workers array and start them */
  config->workers = calloc(config->worker_count, sizeof(*config->workers));
  if (config->workers == NULL) {
    err = bud_error_str(kBudErrNoMem, "new workers");
    goto restore;
  }

  err = bud_master_spawn_workers(config);
  if (!bud_is_ok(err))
    goto restore;

  /* Send SIGHUP to stale workers and mark them as stale */
  for (i = 0; i < config->worker_count; i++) {
    if (stale[i].state & kBudWorkerStateActive) {
      stale[i].state |= kBudWorkerStateStale;
      uv_process_kill(&stale[i].proc, SIGHUP);
    }
  }
  return;

restore:
  /* Failure, restore previous workers */
  free(config->workers);
  config->workers = stale;

  bud_error_print(stderr, err);
}
#endif  /* !_WIN32 */


bud_error_t bud_master_spawn_workers(bud_config_t* config) {
  bud_error_t err;
  int i;

  /* In case, if worker_count == 0 */
  err = bud_ok();

  /* Spawn workers */
  for (i = 0; i < config->worker_count; i++) {
    config->workers[i].config = config;
    config->workers[i].index = i;
    err = bud_master_spawn_worker(&config->workers[i]);

    if (!bud_is_ok(err))
      while (i-- > 0)
        bud_master_kill_worker(&config->workers[i], 0, NULL);
  }

  return err;
}


bud_error_t bud_master_spawn_worker(bud_worker_t* worker) {
  bud_error_t err;
  bud_config_t* config;
  int i;
  int r;
  uv_process_options_t options;

  config = worker->config;
  ASSERT(config != NULL, "Worker config absent");

  /* Config reload requested, this worker should not be restarted */
  if (worker->state & kBudWorkerStateStale) {
    bud_worker_t* start;

    start = &worker[-worker->index];
    worker->state |= kBudWorkerStateDead;

    /* However check if all workers are gone, and if so - release the storage */
    for (i = 0; i < config->worker_count; i++)
      if (!(start[i].state & kBudWorkerStateDead))
        return bud_ok();

    /* All gone :( Good bye! */
    free(start);
    return bud_ok();
  }

  memset(&options, 0, sizeof(options));
  options.exit_cb = bud_master_respawn_worker;
  options.file = config->exepath;
  options.stdio_count = 3;
  options.stdio = calloc(options.stdio_count, sizeof(*options.stdio));
  options.args = calloc(config->argc + 2, sizeof(*options.args));
  if (options.stdio == NULL || options.args == NULL) {
    err = bud_error(kBudErrNoMem);
    goto done;
  }

  /* args = { config.argv, "--worker" } */
  for (i = 0; i < config->argc; i++)
    options.args[i] = config->argv[i];
  options.args[i] = "--worker";
  options.args[i + 1] = NULL;

  r = uv_timer_init(config->loop, &worker->restart_timer);
  if (r != 0) {
    err = bud_error_num(kBudErrRestartTimer, r);
    goto done;
  }

  err = bud_ipc_init(&worker->ipc, config);
  if (!bud_is_ok(err))
    goto failed_ipc_init;

  /* stdio = { pipe, inherit, inherit } */
  options.stdio[0].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
  options.stdio[0].data.stream = bud_ipc_get_stream(&worker->ipc);
  options.stdio[1].flags = UV_INHERIT_FD;
  options.stdio[1].data.fd = 1;
  options.stdio[2].flags = UV_INHERIT_FD;
  options.stdio[2].data.fd = 2;

  r = uv_spawn(config->loop, &worker->proc, &options);

  if (r != 0) {
    err = bud_error_num(kBudErrSpawn, r);
    goto failed_uv_spawn;
  } else {
    worker->state |= kBudWorkerStateActive;
    err = bud_ok();
    bud_log(worker->config,
            kBudLogInfo,
            "spawned bud worker<%d>",
            worker->proc.pid);

    /* Pending accept - try balancing */
    if (config->pending_accept) {
      config->pending_accept = 0;
      bud_master_balance(config->server);
    }
  }

  goto done;

failed_uv_spawn:
  bud_ipc_close(&worker->ipc);

failed_ipc_init:
  uv_close((uv_handle_t*) &worker->restart_timer, bud_master_ipc_close_cb);

done:
  free(options.stdio);
  free(options.args);
  options.stdio = NULL;
  options.args = NULL;
  return err;
}


void bud_master_respawn_worker(uv_process_t* proc,
                               int64_t exit_status,
                               int term_signal) {
  bud_worker_t* worker;

  worker = container_of(proc, bud_worker_t, proc);
  ASSERT(worker != NULL, "Proc has no worker");

  bud_log(worker->config,
          kBudLogWarning,
          "bud worker<%d> died, signal: %d",
          proc->pid,
          term_signal);

  bud_master_kill_worker(worker,
                         (uint64_t) worker->config->restart_timeout,
                         bud_master_spawn_worker);
}


void bud_master_kill_worker(bud_worker_t* worker,
                            uint64_t delay,
                            bud_worker_kill_cb cb) {
  ASSERT(worker->state & kBudWorkerStateActive,
         "Tried to kill inactive worker");

  uv_process_kill(&worker->proc, SIGKILL);
  worker->state &= ~kBudWorkerStateActive;
  worker->kill_cb = cb;
  worker->proc.data = worker;
  worker->restart_timer.data = worker;
  worker->close_waiting = 2;
  uv_close((uv_handle_t*) &worker->proc, bud_worker_close_cb);
  bud_ipc_close(&worker->ipc);
  if (delay == 0) {
    uv_close((uv_handle_t*) &worker->restart_timer, bud_worker_close_cb);
  } else {
    int r;

    r = uv_timer_start(&worker->restart_timer, bud_worker_timer_cb, delay, 0);
    if (r != 0)
      uv_close((uv_handle_t*) &worker->restart_timer, bud_worker_close_cb);
  }
}


void bud_worker_timer_cb(uv_timer_t* handle) {
  bud_worker_t* worker;

  worker = handle->data;
  ASSERT(worker != NULL, "Timers\'s worker absent");
  uv_close((uv_handle_t*) &worker->restart_timer, bud_worker_close_cb);
}


void bud_worker_close_cb(uv_handle_t* handle) {
  bud_worker_t* worker;

  worker = handle->data;
  ASSERT(worker != NULL, "Handle\'s worker absent");

  if (--worker->close_waiting == 0 && worker->kill_cb != NULL)
    worker->kill_cb(worker);
}


void bud_master_ipc_close_cb(uv_handle_t* handle) {
  /* No-op */
}


void bud_master_balance(struct bud_server_s* server) {
  bud_error_t err;
  bud_config_t* config;
  bud_worker_t* worker;
  int last_index;

  config = server->config;

  if (config->worker_count == 0) {
    bud_log(config, kBudLogDebug, "master self accept");

    /* Master = worker */
    return bud_client_create(config, (uv_stream_t*) &server->tcp);
  }

  bud_log(config,
          kBudLogDebug,
          "master balance");

  /* Round-robin worker selection */
  last_index = (config->last_worker + 1) % config->worker_count;
  do {
    config->last_worker++;
    config->last_worker %= config->worker_count;
    worker = &config->workers[config->last_worker];
  } while (!(worker->state & kBudWorkerStateActive) &&
           config->last_worker != last_index);

  /* All workers are down... wait */
  if (!(worker->state & kBudWorkerStateActive)) {
    config->pending_accept = 1;
    return;
  }

  err = bud_ipc_balance(&worker->ipc, (uv_stream_t*) &server->tcp);
  if (!bud_is_ok(err))
    bud_error_log(config, kBudLogWarning, err);
}
