/*
 * ProFTPD - mod_proxy
 * Copyright (c) 2012-2015 TJ Saunders
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA.
 *
 * As a special exemption, TJ Saunders and other respective copyright holders
 * give permission to link this program with OpenSSL, and distribute the
 * resulting executable, without including the source code for OpenSSL in the
 * source distribution.
 *
 * -----DO NOT EDIT BELOW THIS LINE-----
 * $Archive: mod_proxy.a $
 * $Libraries: -lsqlite3 $
 */

#include "mod_proxy.h"
#include "proxy/random.h"
#include "proxy/db.h"
#include "proxy/session.h"
#include "proxy/conn.h"
#include "proxy/netio.h"
#include "proxy/inet.h"
#include "proxy/tls.h"
#include "proxy/forward.h"
#include "proxy/reverse.h"
#include "proxy/ftp/conn.h"
#include "proxy/ftp/ctrl.h"
#include "proxy/ftp/data.h"
#include "proxy/ftp/msg.h"
#include "proxy/ftp/xfer.h"

/* Proxy role */
#define PROXY_ROLE_REVERSE		1
#define PROXY_ROLE_FORWARD		2

/* How long (in secs) to wait to connect to real server? */
#define PROXY_CONNECT_DEFAULT_TIMEOUT	5

extern module xfer_module;

/* From response.c */
extern pr_response_t *resp_list, *resp_err_list;

module proxy_module;

int proxy_logfd = -1;
pool *proxy_pool = NULL;
unsigned long proxy_opts = 0UL;
unsigned int proxy_sess_state = 0U;

static int proxy_engine = FALSE;
static int proxy_role = PROXY_ROLE_REVERSE;
static const char *proxy_tables_dir = NULL;

static const char *trace_channel = "proxy";

MODRET proxy_cmd(cmd_rec *cmd, struct proxy_session *proxy_sess,
    pr_response_t **rp) {
  int res, xerrno;
  pr_response_t *resp;
  unsigned int resp_nlines = 0;

  res = proxy_ftp_ctrl_send_cmd(cmd->tmp_pool, proxy_sess->backend_ctrl_conn,
    cmd);
  if (res < 0) {
    xerrno = errno;
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "error sending %s to backend: %s", (char *) cmd->argv[0],
      strerror(xerrno));

    pr_response_add_err(R_500, _("%s: %s"), (char *) cmd->argv[0],
      strerror(xerrno));
    pr_response_flush(&resp_err_list);

    errno = xerrno;
    return PR_ERROR(cmd);
  }

  resp = proxy_ftp_ctrl_recv_resp(cmd->tmp_pool, proxy_sess->backend_ctrl_conn,
    &resp_nlines);
  if (resp == NULL) {
    xerrno = errno;

    /* For a certain number of conditions, if we cannot read the response
     * from the backend, then we should just close the frontend, otherwise
     * we might "leak" to the client the fact that we are fronting some
     * backend server rather than being the server.
     */
    if (xerrno == ECONNRESET ||
        xerrno == ECONNABORTED ||
        xerrno == ENOENT ||
        xerrno == EPIPE) {
      pr_session_disconnect(&proxy_module, PR_SESS_DISCONNECT_BY_APPLICATION,
        "Backend control connection lost");
    }

    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "error receiving %s response from backend: %s", (char *) cmd->argv[0],
      strerror(xerrno));

    pr_response_add_err(R_500, _("%s: %s"), (char *) cmd->argv[0],
      strerror(xerrno));
    pr_response_flush(&resp_err_list);

    errno = xerrno;
    return PR_ERROR(cmd);
  }

  res = proxy_ftp_ctrl_send_resp(cmd->tmp_pool, proxy_sess->frontend_ctrl_conn,
    resp, resp_nlines);
  if (res < 0) {
    xerrno = errno;

    pr_response_block(TRUE);
    errno = xerrno;
    return PR_ERROR(cmd);
  }

  pr_response_block(TRUE);

  if (rp != NULL) {
    *rp = resp;
  }

  return PR_HANDLED(cmd);
}

static int proxy_stalled_timeout_cb(CALLBACK_FRAME) {
  int timeout_stalled;

  timeout_stalled = pr_data_get_timeout(PR_DATA_TIMEOUT_STALLED);

  pr_event_generate("core.timeout-stalled", NULL);
  pr_log_pri(PR_LOG_NOTICE, "Data transfer stall timeout: %d %s",
    timeout_stalled, timeout_stalled != 1 ? "seconds" : "second");
  pr_session_disconnect(NULL, PR_SESS_DISCONNECT_TIMEOUT,
    "TimeoutStalled during data transfer");

  /* Do not restart the timer (should never be reached). */
  return 0;
}

static void proxy_log_xfer(cmd_rec *cmd, char abort_flag) {
  struct timeval end_time;
  char direction, *path = NULL;

  switch (cmd->cmd_id) {
    case PR_CMD_APPE_ID:
    case PR_CMD_STOR_ID:
    case PR_CMD_STOU_ID:
      direction = 'i';
      break;

    case PR_CMD_RETR_ID:
      direction = 'o';
      break;
  }

  memset(&end_time, '\0', sizeof(end_time));

  if (session.xfer.start_time.tv_sec != 0) {
    gettimeofday(&end_time, NULL);
    end_time.tv_sec -= session.xfer.start_time.tv_sec;

    if (end_time.tv_usec >= session.xfer.start_time.tv_usec) {
      end_time.tv_usec -= session.xfer.start_time.tv_usec;

    } else {
      end_time.tv_usec = 1000000L - (session.xfer.start_time.tv_usec -
        end_time.tv_usec);
      end_time.tv_sec--;
    }
  }

  path = cmd->arg;

  xferlog_write(end_time.tv_sec, pr_netaddr_get_sess_remote_name(),
    session.xfer.total_bytes, path,
    (session.sf_flags & SF_ASCII ? 'a' : 'b'), direction,
    'r', session.user, abort_flag, "_");

  pr_log_debug(DEBUG1, "Transfer %s %" PR_LU " bytes in %ld.%02lu seconds",
    abort_flag == 'c' ? "completed:" : "aborted after",
    (pr_off_t) session.xfer.total_bytes, (long) end_time.tv_sec,
    (unsigned long)(end_time.tv_usec / 10000));
}

static int proxy_mkdir(const char *dir, uid_t uid, gid_t gid, mode_t mode) {
  mode_t prev_mask;
  struct stat st;
  int res = -1;

  res = stat(dir, &st);
  if (res < 0 &&
      errno != ENOENT) {
    return -1;
  }

  /* The directory already exists. */
  if (res == 0) {
    return 0;
  }

  /* The given mode is absolute, not subject to any Umask setting. */
  prev_mask = umask(0);

  if (mkdir(dir, mode) < 0) {
    int xerrno = errno;

    (void) umask(prev_mask);
    errno = xerrno;
    return -1;
  }

  umask(prev_mask);

  if (chown(dir, uid, gid) < 0) {
    return -1;
  }

  return 0;
}

static int proxy_mkpath(pool *p, const char *path, uid_t uid, gid_t gid,
    mode_t mode) {
  char *currpath = NULL, *tmppath = NULL;
  struct stat st;

  if (stat(path, &st) == 0) {
    /* Path already exists, nothing to be done. */
    errno = EEXIST;
    return -1;
  }

  tmppath = pstrdup(p, path);

  currpath = "/";
  while (tmppath && *tmppath) {
    char *currdir = strsep(&tmppath, "/");
    currpath = pdircat(p, currpath, currdir, NULL);

    if (proxy_mkdir(currpath, uid, gid, mode) < 0) {
      return -1;
    }

    pr_signals_handle();
  }

  return 0;
}

static int proxy_rmpath(pool *p, const char *path) {
  DIR *dirh;
  struct dirent *dent;
  int res, xerrno = 0;

  if (path == NULL) {
    errno = EINVAL;
    return -1;
  }

  dirh = opendir(path);
  if (dirh == NULL) {
    xerrno = errno;

    /* Change the permissions in the directory, and try again. */
    if (chmod(path, (mode_t) 0755) == 0) {
      dirh = opendir(path);
    }

    if (dirh == NULL) {
      pr_trace_msg(trace_channel, 9,
        "error opening '%s': %s", path, strerror(xerrno));
      errno = xerrno;
      return -1;
    }
  }

  while ((dent = readdir(dirh)) != NULL) {
    struct stat st;
    char *file;

    pr_signals_handle();

    if (strncmp(dent->d_name, ".", 2) == 0 ||
        strncmp(dent->d_name, "..", 3) == 0) {
      continue;
    }

    file = pdircat(p, path, dent->d_name, NULL);

    if (stat(file, &st) < 0) {
      pr_trace_msg(trace_channel, 9,
        "unable to stat '%s': %s", file, strerror(errno));
      continue;
    }

    if (S_ISDIR(st.st_mode)) {
      res = proxy_rmpath(p, file);
      if (res < 0) {
        pr_trace_msg(trace_channel, 9,
          "error removing directory '%s': %s", file, strerror(errno));
      }

    } else {
      res = unlink(file);
      if (res < 0) {
        pr_trace_msg(trace_channel, 9,
          "error removing file '%s': %s", file, strerror(errno));
      }
    }
  }

  closedir(dirh);

  res = rmdir(path);
  if (res < 0) {
    xerrno = errno;
    pr_trace_msg(trace_channel, 9,
      "error removing directory '%s': %s", path, strerror(xerrno));
    errno = xerrno;
  }

  return res;
}

static void proxy_remove_symbols(void) {
  int res;

  /* Remove mod_xfer's PRE_CMD handlers; they will only interfere
   * with proxied data transfers (see GitHub issue #3).
   */

  res = pr_stash_remove_cmd(C_APPE, &xfer_module, PRE_CMD, NULL, -1);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1,
      "error removing PRE_CMD APPE mod_xfer handler: %s", strerror(errno));

  } else {
    pr_trace_msg(trace_channel, 9, "removed PRE_CMD APPE mod_xfer handlers");
  }
 
  res = pr_stash_remove_cmd(C_RETR, &xfer_module, PRE_CMD, NULL, -1);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1,
      "error removing PRE_CMD RETR mod_xfer handler: %s", strerror(errno));

  } else {
    pr_trace_msg(trace_channel, 9, "removed PRE_CMD RETR mod_xfer handlers");
  }

  res = pr_stash_remove_cmd(C_STOR, &xfer_module, PRE_CMD, NULL, -1);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1,
      "error removing PRE_CMD STOR mod_xfer handler: %s", strerror(errno));

  } else {
    pr_trace_msg(trace_channel, 9, "removed PRE_CMD STOR mod_xfer handlers");
  }

  res = pr_stash_remove_cmd(C_STOU, &xfer_module, PRE_CMD, NULL, -1);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1,
      "error removing PRE_CMD STOU mod_xfer handler: %s", strerror(errno));

  } else {
    pr_trace_msg(trace_channel, 9, "removed PRE_CMD STOU mod_xfer handlers");
  }
}

static void proxy_restrict_session(void) {
  const char *proxy_chroot = NULL;

  PRIVS_ROOT

  if (getuid() == PR_ROOT_UID) {
    int res;

    /* Chroot to the ProxyTables/empty/ directory before dropping root privs. */
    proxy_chroot = pdircat(proxy_pool, proxy_tables_dir, "empty", NULL);
    res = chroot(proxy_chroot);
    if (res < 0) {
      int xerrno = errno;

      PRIVS_RELINQUISH

      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "unable to chroot to ProxyTables/empty/ directory '%s': %s",
        proxy_chroot, strerror(xerrno));
      pr_session_disconnect(&proxy_module, PR_SESS_DISCONNECT_MODULE_ACL,
       "Unable to chroot proxy session");
    }

    if (chdir("/") < 0) {
      int xerrno = errno;

      PRIVS_RELINQUISH

      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "unable to chdir to root directory within chroot: %s",
        strerror(xerrno));
      pr_session_disconnect(&proxy_module, PR_SESS_DISCONNECT_MODULE_ACL,
       "Unable to chroot proxy session");
    }
  }

  /* Make the proxy session process have the identity of the configured daemon
   * User/Group.
   */
  PRIVS_REVOKE

  if (proxy_chroot != NULL) {
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "proxy session running as UID %lu, GID %lu, restricted to '%s'",
      (unsigned long) getuid(), (unsigned long) getgid(), proxy_chroot);

  } else {
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "proxy session running as UID %lu, GID %lu, located in '%s'",
      (unsigned long) getuid(), (unsigned long) getgid(), getcwd(NULL, 0));
  }
}

/* Configuration handlers
 */

/* usage: ProxyDataTransferPolicy "active"|"passive"|"pasv"|"epsv"|"port"|
 *          "eprt"|"client"
 */
MODRET set_proxydatatransferpolicy(cmd_rec *cmd) {
  config_rec *c;
  int cmd_id = -1;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  cmd_id = pr_cmd_get_id(cmd->argv[1]);
  if (cmd_id < 0) {
    if (strncasecmp(cmd->argv[1], "active", 7) == 0) {
      /* Try to use EPRT over PORT. */
      cmd_id = PR_CMD_EPRT_ID;

    } else if (strncasecmp(cmd->argv[1], "passive", 8) == 0) {
      /* Try to use EPSV over PASV. */
      cmd_id = PR_CMD_EPSV_ID;

    } else if (strncasecmp(cmd->argv[1], "client", 7) == 0) {
      cmd_id = 0;

    } else {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unsupported DataTransferPolicy: ",
        (char *) cmd->argv[1], NULL));
    }
  }

  if (cmd_id != PR_CMD_PASV_ID &&
      cmd_id != PR_CMD_EPSV_ID &&
      cmd_id != PR_CMD_PORT_ID &&
      cmd_id != PR_CMD_EPRT_ID &&
      cmd_id != 0) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unsupported DataTransferPolicy: ",
      (char *) cmd->argv[1], NULL));
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = cmd_id;

  return PR_HANDLED(cmd);
}

/* usage: ProxyEngine on|off */
MODRET set_proxyengine(cmd_rec *cmd) {
  int engine = 1;
  config_rec *c;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  engine = get_boolean(cmd, 1);
  if (engine == -1) {
    CONF_ERROR(cmd, "expected Boolean parameter");
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = engine;

  return PR_HANDLED(cmd);
}

/* usage: ProxyForwardEnabled on|off */
MODRET set_proxyforwardenabled(cmd_rec *cmd) {
  int enabled = -1, res;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_CLASS);

  enabled = get_boolean(cmd, 1);
  if (enabled < 0) {
    CONF_ERROR(cmd, "expected Boolean parameter");
  }

  /* Stash this setting in the notes for this class. */
  res = pr_class_add_note(PROXY_FORWARD_ENABLED_NOTE, &enabled, sizeof(int));
  if (res < 0) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "error storing parameter: ",
      strerror(errno), NULL));
  }

  return PR_HANDLED(cmd);
}

/* usage: ProxyForwardMethod method */
MODRET set_proxyforwardmethod(cmd_rec *cmd) {
  config_rec *c;
  int forward_method = -1;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  forward_method = proxy_forward_get_method(cmd->argv[1]);
  if (forward_method < 0) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
      "unknown/unsupported forward method: ", (char *) cmd->argv[1], NULL));
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = forward_method;

  return PR_HANDLED(cmd);
}

/* usage: ProxyForwardTo [!]pattern [flags] */
MODRET set_proxyforwardto(cmd_rec *cmd) {
#ifdef PR_USE_REGEX
  config_rec *c;
  pr_regex_t *pre = NULL;
  int negated = FALSE, regex_flags = REG_EXTENDED|REG_NOSUB, res = 0;
  char *pattern;

  if (cmd->argc-1 < 1 ||
      cmd->argc-1 > 2) {
    CONF_ERROR(cmd, "bad number of parameters");
  }

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  /* Make sure that, if present, the flags parameter is correctly formatted. */
  if (cmd->argc-1 == 2) {
    int flags = 0;

    /* We need to parse the flags parameter here, to see if any flags which
     * affect the compilation of the regex (e.g. NC) are present.
     */

    flags = pr_filter_parse_flags(cmd->tmp_pool, cmd->argv[2]);
    if (flags < 0) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
        ": badly formatted flags parameter: '", (char *) cmd->argv[2], "'",
        NULL));
    }

    if (flags == 0) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
        ": unknown filter flags '", (char *) cmd->argv[2], "'", NULL));
    }

    regex_flags |= flags;
  }

  pre = pr_regexp_alloc(&proxy_module);

  pattern = cmd->argv[1];
  if (*pattern == '!') {
    negated = TRUE;
    pattern++;
  }

  res = pr_regexp_compile(pre, pattern, regex_flags);
  if (res != 0) {
    char errstr[200] = {'\0'};

    pr_regexp_error(res, pre, errstr, sizeof(errstr));
    pr_regexp_free(NULL, pre);

    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", pattern, "' failed regex "
      "compilation: ", errstr, NULL));
  }

  c = add_config_param(cmd->argv[0], 2, pre, NULL);
  c->argv[1] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[1]) = negated;
  return PR_HANDLED(cmd);

#else /* no regular expression support at the moment */
  CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "The ", param, " directive cannot be "
    "used on this system, as you do not have POSIX compliant regex support",
    NULL));
#endif
}

/* usage: ProxyLog path|"none" */
MODRET set_proxylog(cmd_rec *cmd) {
  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  (void) add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
  return PR_HANDLED(cmd);
}

/* usage: ProxyOptions opt1 ... optN */
MODRET set_proxyoptions(cmd_rec *cmd) {
  register unsigned int i;
  config_rec *c;
  unsigned long opts = 0UL;

  if (cmd->argc-1 == 0) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  c = add_config_param(cmd->argv[0], 1, NULL);

  for (i = 1; i < cmd->argc; i++) {
    if (strncmp(cmd->argv[i], "UseProxyProtocol", 17) == 0) {
      opts |= PROXY_OPT_USE_PROXY_PROTOCOL;

    } else if (strncmp(cmd->argv[i], "ShowFeatures", 13) == 0) {
      opts |= PROXY_OPT_SHOW_FEATURES;

    } else if (strncmp(cmd->argv[i], "UseReverseProxyAuth", 20) == 0) {
      opts |= PROXY_OPT_USE_REVERSE_PROXY_AUTH;

    } else {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, ": unknown ProxyOption '",
        (char *) cmd->argv[i], "'", NULL));
    }
  }

  c->argv[0] = pcalloc(c->pool, sizeof(unsigned long));
  *((unsigned long *) c->argv[0]) = opts;

  return PR_HANDLED(cmd);
}

/* usage: ProxyRetryCount count */
MODRET set_proxyretrycount(cmd_rec *cmd) {
  config_rec *c;
  int retry_count = -1;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  retry_count = atoi(cmd->argv[1]);
  if (retry_count < 1) {
    CONF_ERROR(cmd, "retry count must be one or more");
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = retry_count;

  return PR_HANDLED(cmd);
}

/* usage: ProxyReverseConnectPolicy [policy] */
MODRET set_proxyreverseconnectpolicy(cmd_rec *cmd) {
  config_rec *c;
  int connect_policy = -1;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  connect_policy = proxy_reverse_connect_get_policy(cmd->argv[1]);
  if (connect_policy < 0) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
      "unknown/unsupported connect policy: ", (char *) cmd->argv[1], NULL));
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = connect_policy;

  return PR_HANDLED(cmd);
}

/* usage: ProxyReverseServers server1 ... server N
 *                            file:/path/to/server/list.txt
 *                            sql:/SQLNamedQuery
 */
MODRET set_proxyreverseservers(cmd_rec *cmd) {
  config_rec *c;
  array_header *backend_servers;
  char *uri = NULL;

  if (cmd->argc-1 < 1) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  c = add_config_param(cmd->argv[0], 2, NULL, NULL);
  backend_servers = make_array(c->pool, 1, sizeof(struct proxy_conn *));

  if (cmd->argc-1 == 1) {
    /* We are dealing with one of the following possibilities:
     *
     *  file:/path/to/file.txt
     *  sql://SQLNamedQuery/...
     *  <server>
     */

    if (strncmp(cmd->argv[1], "file:", 5) == 0) {
      char *param, *path;

      param = cmd->argv[1]; 
      path = param + 5;

      /* If the path contains the %U variable, then defer loading of
       * this file until the USER name is known.
       */
      if (strstr(path, "%U") == NULL) {    
        int xerrno;

        /* For now, load the list of servers at sess init time.  In
         * the future, we will want to load it at postparse time, mapped
         * to the appropriate server_rec, and clear/reload on 'core.restart'.
         */

        PRIVS_ROOT
        backend_servers = proxy_reverse_json_parse_uris(cmd->server->pool,
          path);
        xerrno = errno;
        PRIVS_RELINQUISH

        if (backend_servers == NULL) {
          CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
            "error reading ProxyReverseServers file '", path, "': ",
            strerror(xerrno), NULL));
        }

        if (backend_servers->nelts == 0) {
          CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
            "no usable URLs found in file '", path, NULL));
        }
      }

      uri = cmd->argv[1];

    } else if (strncmp(cmd->argv[1], "sql:/", 5) == 0) {

      /* Unfortunately there's not very much we can do to validate these
       * SQL URIs at the moment.  They point to a SQLNamedQuery, which
       * may not have been parsed yet from the config file, or which may be
       * in a <Global> scope.  Thus we simply store them for now, and
       * let the lookup routines do the necessary validation.
       */

      uri = cmd->argv[1];

    } else {
      /* Treat it as a server-spec (i.e. a URI) */
      struct proxy_conn *pconn;

      pconn = proxy_conn_create(c->pool, cmd->argv[1]);
      if (pconn == NULL) {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "error parsing '",
          (char *) cmd->argv[1], "': ", strerror(errno), NULL));
      }

      *((struct proxy_conn **) push_array(backend_servers)) = pconn;
    }

  } else {
    register unsigned int i;

    /* More than one parameter, which means they are all URIs. */

    for (i = 1; i < cmd->argc; i++) {
      struct proxy_conn *pconn;

      pconn = proxy_conn_create(c->pool, cmd->argv[i]);
      if (pconn == NULL) {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "error parsing '",
          (char *) cmd->argv[i], "': ", strerror(errno), NULL));
      }

      *((struct proxy_conn **) push_array(backend_servers)) = pconn;
    }
  }

  c->argv[0] = backend_servers;
  if (uri != NULL) {
    c->argv[1] = pstrdup(c->pool, uri);
  }

  return PR_HANDLED(cmd);
}

/* usage: ProxyRole "forward"|"reverse" */
MODRET set_proxyrole(cmd_rec *cmd) {
  int role = 0;
  config_rec *c;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  if (strcasecmp(cmd->argv[1], "forward") == 0) {
    role = PROXY_ROLE_FORWARD;

  } else if (strcasecmp(cmd->argv[1], "reverse") == 0) {
    role = PROXY_ROLE_REVERSE;

  } else {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unknown proxy role '",
      (char *) cmd->argv[1], "'", NULL));
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = role;

  return PR_HANDLED(cmd);
}

/* usage: ProxySourceAddress address */
MODRET set_proxysourceaddress(cmd_rec *cmd) {
  config_rec *c = NULL;
  pr_netaddr_t *src_addr = NULL;
  unsigned int addr_flags = PR_NETADDR_GET_ADDR_FL_INCL_DEVICE;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  src_addr = pr_netaddr_get_addr2(cmd->server->pool, cmd->argv[1], NULL,
    addr_flags);
  if (src_addr == NULL) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to resolve '",
      (char *) cmd->argv[1], "'", NULL));
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = src_addr;

  return PR_HANDLED(cmd);
}

/* usage: ProxyTables path */
MODRET set_proxytables(cmd_rec *cmd) {
  int res;
  struct stat st;
  char *path;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT);

  path = cmd->argv[1];
  if (*path != '/') {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "must be a full path: '", path,
      "'", NULL));
  }

  res = stat(path, &st);
  if (res < 0) {
    char *proxy_chroot;

    if (errno != ENOENT) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to stat '", path,
        "': ", strerror(errno), NULL));
    }

    pr_log_debug(DEBUG0, MOD_PROXY_VERSION
      ": ProxyTables directory '%s' does not exist, creating it", path);

    /* Create the directory. */
    res = proxy_mkpath(cmd->tmp_pool, path, geteuid(), getegid(), 0755);
    if (res < 0) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to create directory '",
        path, "': ", strerror(errno), NULL));
    }

    /* Also create the empty/ directory underneath, for the chroot. */
    proxy_chroot = pdircat(cmd->tmp_pool, path, "empty", NULL);

    res = proxy_mkpath(cmd->tmp_pool, proxy_chroot, geteuid(), getegid(), 0111);
    if (res < 0) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to create directory '",
        proxy_chroot, "': ", strerror(errno), NULL));
    }

    pr_log_debug(DEBUG2, MOD_PROXY_VERSION
      ": created ProxyTables directory '%s'", path);

  } else {
    char *proxy_chroot;

    if (!S_ISDIR(st.st_mode)) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to use '", path,
        "': Not a directory", NULL));
    }

    /* See if the chroot directory empty/ already exists as well.  And enforce
     * the permissions on that directory.
     */
    proxy_chroot = pdircat(cmd->tmp_pool, path, "empty", NULL);

    res = stat(proxy_chroot, &st);
    if (res < 0) {
      if (errno != ENOENT) {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to stat '", proxy_chroot,
          "': ", strerror(errno), NULL));
      }

      res = proxy_mkpath(cmd->tmp_pool, proxy_chroot, geteuid(), getegid(),
        0111);
      if (res < 0) {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to create directory '",
          proxy_chroot, "': ", strerror(errno), NULL));
      }

    } else {
      mode_t dir_mode, expected_mode;

      if (!S_ISDIR(st.st_mode)) {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "path '", proxy_chroot,
          "' is not a directory as expected", NULL));
      }

      dir_mode = st.st_mode;
      dir_mode &= ~S_IFMT;
      expected_mode = (S_IXUSR|S_IXGRP|S_IXOTH);

      if (dir_mode != expected_mode) {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "directory '", proxy_chroot,
          "' has incorrect permissions (not 0111 as required)", NULL));
      }
    }
  }

  add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
  return PR_HANDLED(cmd);
}

/* usage: ProxyTimeoutConnect secs */
MODRET set_proxytimeoutconnect(cmd_rec *cmd) {
  int timeout = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  if (pr_str_get_duration(cmd->argv[1], &timeout) < 0) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "error parsing timeout value '",
      (char *) cmd->argv[1], "': ", strerror(errno), NULL));
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = timeout;

  return PR_HANDLED(cmd);
}

/* usage: ProxyTLSCACertificateFile path */
MODRET set_proxytlscacertfile(cmd_rec *cmd) {
#ifdef PR_USE_OPENSSL
  int res;
  char *path;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];

  PRIVS_ROOT
  res = file_exists(path);
  PRIVS_RELINQUISH

  if (!res) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", path, "' does not exist",
      NULL));
  }

  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  add_config_param_str(cmd->argv[0], 1, path);
  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, "Missing required OpenSSL support (see --enable-openssl configure option)");
#endif /* PR_USE_OPENSSL */
}

/* usage: ProxyTLSCACertificatePath path */
MODRET set_proxytlscacertpath(cmd_rec *cmd) {
#ifdef PR_USE_OPENSSL
  int res;
  char *path;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];

  PRIVS_ROOT
  res = dir_exists(path);
  PRIVS_RELINQUISH

  if (!res) {
    CONF_ERROR(cmd, "parameter must be a directory path");
  }

  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  add_config_param_str(cmd->argv[0], 1, path);
  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, "Missing required OpenSSL support (see --enable-openssl configure option)");
#endif /* PR_USE_OPENSSL */
}

/* usage: ProxyTLSCARevocationFile path */
MODRET set_proxytlscacrlfile(cmd_rec *cmd) {
#ifdef PR_USE_OPENSSL
  int res;
  char *path;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];

  PRIVS_ROOT
  res = file_exists(path);
  PRIVS_RELINQUISH

  if (!res) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", path, "' does not exist",
      NULL));
  }

  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  add_config_param_str(cmd->argv[0], 1, path);
  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, "Missing required OpenSSL support (see --enable-openssl configure option)");
#endif /* PR_USE_OPENSSL */
}

/* usage: ProxyTLSCARevocationPath path */
MODRET set_proxytlscacrlpath(cmd_rec *cmd) {
#ifdef PR_USE_OPENSSL
  int res;
  char *path;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];

  PRIVS_ROOT
  res = dir_exists(path);
  PRIVS_RELINQUISH

  if (!res) {
    CONF_ERROR(cmd, "parameter must be a directory path");
  }

  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  add_config_param_str(cmd->argv[0], 1, path);
  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, "Missing required OpenSSL support (see --enable-openssl configure option)");
#endif /* PR_USE_OPENSSL */
}

/* usage: ProxyTLSCertificateFile path */
MODRET set_proxytlscertfile(cmd_rec *cmd) {
#ifdef PR_USE_OPENSSL
  int res;
  char *path;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];

  PRIVS_ROOT
  res = file_exists(path);
  PRIVS_RELINQUISH

  if (!res) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", path, "' does not exist",
      NULL));
  }

  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  add_config_param_str(cmd->argv[0], 1, path);
  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, "Missing required OpenSSL support (see --enable-openssl configure option)");
#endif /* PR_USE_OPENSSL */
}

/* usage: ProxyTLSCertificateKeyFile path */
MODRET set_proxytlscertkeyfile(cmd_rec *cmd) {
#ifdef PR_USE_OPENSSL
  int res;
  char *path;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  path = cmd->argv[1];

  PRIVS_ROOT
  res = file_exists(path);
  PRIVS_RELINQUISH

  if (!res) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", path, "' does not exist",
      NULL));
  }

  if (*path != '/') {
    CONF_ERROR(cmd, "parameter must be an absolute path");
  }

  add_config_param_str(cmd->argv[0], 1, path);
  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, "Missing required OpenSSL support (see --enable-openssl configure option)");
#endif /* PR_USE_OPENSSL */
}

/* usage: ProxyTLSCipherSuite ciphers */
MODRET set_proxytlsciphersuite(cmd_rec *cmd) {
#ifdef PR_USE_OPENSSL
  config_rec *c = NULL;
  char *ciphersuite = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  ciphersuite = cmd->argv[1];
  c = add_config_param(cmd->argv[0], 1, NULL);

  /* Make sure that EXPORT ciphers cannot be used, per Bug#4163. */
  c->argv[0] = pstrcat(c->pool, "!EXPORT:", ciphersuite, NULL);

  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, "Missing required OpenSSL support (see --enable-openssl configure option)");
#endif /* PR_USE_OPENSSL */
}

/* usage: ProxyTLSEngine on|off|auto */
MODRET set_proxytlsengine(cmd_rec *cmd) {
#ifdef PR_USE_OPENSSL
  int engine = -1;
  config_rec *c;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_GLOBAL|CONF_VIRTUAL);

  engine = get_boolean(cmd, 1);
  if (engine == -1) {
    if (strcasecmp(cmd->argv[1], "auto") == 0) {
      engine = PROXY_TLS_ENGINE_AUTO;

    } else {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unknown ProxyTLSEngine value: '",
        cmd->argv[1], "'", NULL));
    }

  } else {
    if (engine == TRUE) {
      engine = PROXY_TLS_ENGINE_ON;

    } else {
      engine = PROXY_TLS_ENGINE_OFF;
    }
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = engine;

  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, "Missing required OpenSSL support (see --enable-openssl configure option)");
#endif /* PR_USE_OPENSSL */
}

/* usage: ProxyTLSOptions ... */
MODRET set_proxytlsoptions(cmd_rec *cmd) {
#ifdef PR_USE_OPENSSL
  config_rec *c = NULL;
  register unsigned int i = 0;
  unsigned long opts = 0UL;

  if (cmd->argc-1 == 0) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  c = add_config_param(cmd->argv[0], 1, NULL);

  for (i = 1; i < cmd->argc; i++) {
    if (strcmp(cmd->argv[i], "EnableDiags") == 0) {
      opts |= PROXY_TLS_OPT_ENABLE_DIAGS;

    } else if (strcmp(cmd->argv[i], "NoSessionCache") == 0) {
      opts |= PROXY_TLS_OPT_NO_SESSION_CACHE;

    } else {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, ": unknown ProxyTLSOption '",
        cmd->argv[i], "'", NULL));
    }
  }

  c->argv[0] = pcalloc(c->pool, sizeof(unsigned long));
  *((unsigned long *) c->argv[0]) = opts;

  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, "Missing required OpenSSL support (see --enable-openssl configure option)");
#endif /* PR_USE_OPENSSL */
}

/* usage: ProxyTLSPreSharedKey name path */
MODRET set_proxytlspresharedkey(cmd_rec *cmd) {
#ifdef PR_USE_OPENSSL
# if defined(PSK_MAX_PSK_LEN)
  size_t identity_len, path_len;
  char *path;

  CHECK_ARGS(cmd, 2);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  identity_len = strlen(cmd->argv[1]);
  if (identity_len > PSK_MAX_IDENTITY_LEN) {
    char buf[32];

    memset(buf, '\0', sizeof(buf));
    snprintf(buf, sizeof(buf)-1, "%d", (int) PSK_MAX_IDENTITY_LEN);

    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
      "ProxyTLSPreSharedKey identity '", cmd->argv[1],
      "' exceeds maximum length ", buf, cmd->argv[2], NULL))
  }

  /* Ensure that the given path starts with "hex:", denoting the
   * format of the key at the given path.  Support for other formats, e.g.
   * bcrypt or somesuch, will be added later.
   */
  path = cmd->argv[2];
  path_len = strlen(path);
  if (path_len < 5 ||
      strncmp(cmd->argv[2], "hex:", 4) != 0) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
      "unsupported ProxyTLSPreSharedKey format: ", path, NULL))
  }

  (void) add_config_param_str(cmd->argv[0], 2, cmd->argv[1], path);
# else
  pr_log_debug(DEBUG0,
    "%s is not supported by this build/version of OpenSSL, ignoring",
    cmd->argv[0]);
# endif /* PSK support */
  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, "Missing required OpenSSL support (see --enable-openssl configure option)");
#endif /* PR_USE_OPENSSL */
}

/* usage: ProxyTLSProtocol protocols */
MODRET set_proxytlsprotocol(cmd_rec *cmd) {
#ifdef PR_USE_OPENSSL
  register unsigned int i;
  config_rec *c;
  unsigned int tls_protocol = 0;

  if (cmd->argc-1 == 0) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  if (strcasecmp(cmd->argv[1], "all") == 0) {
    /* We're in an additive/subtractive type of configuration. */
    tls_protocol = PROXY_TLS_PROTO_ALL;

    for (i = 2; i < cmd->argc; i++) {
      int disable = FALSE;
      char *proto_name;

      proto_name = cmd->argv[i];

      if (*proto_name == '+') {
        proto_name++;

      } else if (*proto_name == '-') {
        disable = TRUE;
        proto_name++;

      } else {
        /* Using the additive/subtractive approach requires a +/- prefix;
         * it's malformed without such prefaces.
         */
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "missing required +/- prefix: ",
          proto_name, NULL));
      }

      if (strncasecmp(proto_name, "SSLv3", 6) == 0) {
        if (disable) {
          tls_protocol &= ~PROXY_TLS_PROTO_SSL_V3;
        } else {
          tls_protocol |= PROXY_TLS_PROTO_SSL_V3;
        }

      } else if (strncasecmp(proto_name, "TLSv1", 6) == 0) {
        if (disable) {
          tls_protocol &= ~PROXY_TLS_PROTO_TLS_V1;
        } else {
          tls_protocol |= PROXY_TLS_PROTO_TLS_V1;
        }

      } else if (strncasecmp(proto_name, "TLSv1.1", 8) == 0) {
# if OPENSSL_VERSION_NUMBER >= 0x10001000L
        if (disable) {
          tls_protocol &= ~PROXY_TLS_PROTO_TLS_V1_1;
        } else {
          tls_protocol |= PROXY_TLS_PROTO_TLS_V1_1;
        }
# else
        CONF_ERROR(cmd, "Your OpenSSL installation does not support TLSv1.1");
# endif /* OpenSSL 1.0.1 or later */

      } else if (strncasecmp(proto_name, "TLSv1.2", 8) == 0) {
# if OPENSSL_VERSION_NUMBER >= 0x10001000L
        if (disable) {
          tls_protocol &= ~PROXY_TLS_PROTO_TLS_V1_2;
        } else {
          tls_protocol |= PROXY_TLS_PROTO_TLS_V1_2;
        }
# else
        CONF_ERROR(cmd, "Your OpenSSL installation does not support TLSv1.2");
# endif /* OpenSSL 1.0.1 or later */

      } else {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unknown protocol: '",
          cmd->argv[i], "'", NULL));
      }
    }

  } else {
    for (i = 1; i < cmd->argc; i++) {
      if (strncasecmp(cmd->argv[i], "SSLv23", 7) == 0) {
        tls_protocol |= PROXY_TLS_PROTO_SSL_V3;
        tls_protocol |= PROXY_TLS_PROTO_TLS_V1;

      } else if (strncasecmp(cmd->argv[i], "SSLv3", 6) == 0) {
        tls_protocol |= PROXY_TLS_PROTO_SSL_V3;

      } else if (strncasecmp(cmd->argv[i], "TLSv1", 6) == 0) {
        tls_protocol |= PROXY_TLS_PROTO_TLS_V1;

      } else if (strncasecmp(cmd->argv[i], "TLSv1.1", 8) == 0) {
# if OPENSSL_VERSION_NUMBER >= 0x10001000L
        tls_protocol |= PROXY_TLS_PROTO_TLS_V1_1;
# else
        CONF_ERROR(cmd, "Your OpenSSL installation does not support TLSv1.1");
# endif /* OpenSSL 1.0.1 or later */

      } else if (strncasecmp(cmd->argv[i], "TLSv1.2", 8) == 0) {
# if OPENSSL_VERSION_NUMBER >= 0x10001000L
        tls_protocol |= PROXY_TLS_PROTO_TLS_V1_2;
# else
        CONF_ERROR(cmd, "Your OpenSSL installation does not support TLSv1.2");
# endif /* OpenSSL 1.0.1 or later */

      } else {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unknown protocol: '",
          cmd->argv[i], "'", NULL));
      }
    }
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(unsigned int));
  *((unsigned int *) c->argv[0]) = tls_protocol;

  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, "Missing required OpenSSL support (see --enable-openssl configure option)");
#endif /* PR_USE_OPENSSL */
}

/* usage: ProxyTLSTimeoutHandshake timeout */
MODRET set_proxytlstimeouthandshake(cmd_rec *cmd) {
#ifdef PR_USE_OPENSSL
  int timeout = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  if (pr_str_get_duration(cmd->argv[1], &timeout) < 0) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "error parsing timeout value '",
      cmd->argv[1], "': ", strerror(errno), NULL));
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned int));
  *((unsigned int *) c->argv[0]) = timeout;

  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, "Missing required OpenSSL support (see --enable-openssl configure option)");
#endif /* PR_USE_OPENSSL */
}

/* usage: ProxyTLSVerifyServer on|off */
MODRET set_proxytlsverifyserver(cmd_rec *cmd) {
#ifdef PR_USE_OPENSSL
  int verify = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  verify = get_boolean(cmd, 1);
  if (verify == -1) {
    CONF_ERROR(cmd, "expected Boolean parameter");
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = verify;

  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, "Missing required OpenSSL support (see --enable-openssl configure option)");
#endif /* PR_USE_OPENSSL */
}

/* mod_proxy event/dispatch loop. */
static void proxy_cmd_loop(server_rec *s, conn_t *conn) {

  while (TRUE) {
    int res = 0;
    cmd_rec *cmd = NULL;

    pr_signals_handle();

    /* TODO: Insert select(2) call here, where we wait for readability on
     * the client control connection.
     *
     * Bonus points for handling aborts on either control connection,
     * broken data connections, blocked/slow writes to client (how much
     * can/should we buffer?  what about short writes to the client?),
     * timeouts, etc.
     */

    res = pr_cmd_read(&cmd);
    if (res < 0) {
      if (PR_NETIO_ERRNO(session.c->instrm) == EINTR) {
        /* Simple interrupted syscall */
        continue;
      }

#ifndef PR_DEVEL_NO_DAEMON
      /* Otherwise, EOF */
      pr_session_disconnect(NULL, PR_SESS_DISCONNECT_CLIENT_EOF, NULL);
#else
      return;
#endif /* PR_DEVEL_NO_DAEMON */
    }

    pr_timer_reset(PR_TIMER_IDLE, ANY_MODULE);

    if (cmd) {
      /* We unblock responses here so that if any PRE_CMD handlers generate
       * responses (usually errors), those responses are sent to the
       * connecting client.
       */
      pr_response_block(FALSE);

      /* TODO: If we need to, we can exert finer-grained control over
       * command dispatching/routing here.  For example, this is where we
       * could block responses for PRE_CMD handlers, or skip certain
       * modules' handlers.
       */

      pr_cmd_dispatch(cmd);
      destroy_pool(cmd->pool);

      pr_response_block(TRUE);

    } else {
      pr_event_generate("core.invalid-command", NULL);
      pr_response_send(R_500, _("Invalid command: try being more creative"));
    }

    /* Release any working memory allocated in inet */
    pr_inet_clear();
  }
}

/* Command handlers
 */

static int proxy_data_prepare_conns(struct proxy_session *proxy_sess,
    cmd_rec *cmd, conn_t **frontend, conn_t **backend) {
  int res, xerrno = 0;
  conn_t *frontend_conn = NULL, *backend_conn = NULL;
  unsigned int resp_nlines = 0;
  pr_response_t *resp;

  res = proxy_ftp_ctrl_send_cmd(cmd->tmp_pool, proxy_sess->backend_ctrl_conn,
    cmd);
  if (res < 0) {
    xerrno = errno;
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "error sending %s to backend: %s", (char *) cmd->argv[0],
      strerror(xerrno));

    pr_response_add_err(R_500, _("%s: %s"), (char *) cmd->argv[0],
      strerror(xerrno));
    pr_response_flush(&resp_err_list);

    errno = xerrno;
    return -1;
  }

  /* XXX Should handle EPSV_ALL here, too. */
  if (proxy_sess->backend_sess_flags & SF_PASSIVE) {
    pr_netaddr_t *bind_addr = NULL;

    /* Connect to the backend server now. We won't receive the initial
     * response until we connect to the backend data address/port.
     */

    /* Specify the specific address/interface to use as the source address for
     * connections to the destination server.
     */
    bind_addr = proxy_sess->src_addr;
    if (bind_addr == NULL) {
      bind_addr = session.c->local_addr;
    }

    if (pr_netaddr_is_loopback(bind_addr) == TRUE) {
      const char *local_name;
      pr_netaddr_t *local_addr;

      local_name = pr_netaddr_get_localaddr_str(cmd->pool);
      local_addr = pr_netaddr_get_addr(cmd->pool, local_name, NULL);

      if (local_addr != NULL) {
        pr_trace_msg(trace_channel, 14,
          "%s is a loopback address, and unable to reach %s; using %s instead",
          pr_netaddr_get_ipstr(bind_addr),
          pr_netaddr_get_ipstr(proxy_sess->backend_data_addr),
          pr_netaddr_get_ipstr(local_addr));
        bind_addr = local_addr;
      }
    }

    pr_trace_msg(trace_channel, 17,
      "connecting to backend server for passive data transfer for %s",
      (char *) cmd->argv[0]);
    backend_conn = proxy_ftp_conn_connect(cmd->pool, bind_addr,
      proxy_sess->backend_data_addr, FALSE);
    if (backend_conn == NULL) {
      xerrno = errno;

      pr_response_add_err(R_425, _("%s: %s"), (char *) cmd->argv[0],
        strerror(xerrno));
      pr_response_flush(&resp_err_list);

      errno = xerrno;
      return -1;
    }

    proxy_sess->backend_data_conn = backend_conn;

    if (proxy_netio_postopen(backend_conn->instrm) < 0) {
      xerrno = errno;

      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "postopen error for backend data connection input stream: %s",
        strerror(xerrno));
      proxy_inet_close(session.pool, backend_conn);
      proxy_sess->backend_data_conn = NULL;

      errno = xerrno;
      return -1;
    }

    if (proxy_netio_postopen(backend_conn->outstrm) < 0) {
      xerrno = errno;

      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "postopen error for backend data connection output stream: %s",
        strerror(xerrno));
      proxy_inet_close(session.pool, backend_conn);
      proxy_sess->backend_data_conn = NULL;

      errno = xerrno;
      return -1;
    }

  } else if (proxy_sess->backend_sess_flags & SF_PORT) {
    pr_trace_msg(trace_channel, 17,
      "accepting connection from backend server for active data "
      "transfer for %s", (char *) cmd->argv[0]);
    backend_conn = proxy_ftp_conn_accept(cmd->pool,
      proxy_sess->backend_data_conn, proxy_sess->backend_ctrl_conn, FALSE);
    if (backend_conn == NULL) {
      xerrno = errno;

      if (proxy_sess->backend_data_conn != NULL) {
        proxy_inet_close(session.pool, proxy_sess->backend_data_conn);
        proxy_sess->backend_data_conn = NULL;
      }

      pr_response_add_err(R_425, _("%s: %s"), (char *) cmd->argv[0],
        strerror(xerrno));
      pr_response_flush(&resp_err_list);

      errno = xerrno;
      return -1;
    }

    /* We can close our listening socket now. */
    proxy_inet_close(session.pool, proxy_sess->backend_data_conn);
    proxy_sess->backend_data_conn = backend_conn; 

    if (proxy_netio_postopen(backend_conn->instrm) < 0) {
      xerrno = errno;

      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "postopen error for backend data connection input stream: %s",
        strerror(xerrno));
      proxy_inet_close(session.pool, backend_conn);
      proxy_sess->backend_data_conn = NULL;

      errno = xerrno;
      return -1;
    }

    if (proxy_netio_postopen(backend_conn->outstrm) < 0) {
      xerrno = errno;

      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "postopen error for backend data connection output stream: %s",
        strerror(xerrno));
      proxy_inet_close(session.pool, backend_conn);
      proxy_sess->backend_data_conn = NULL;

      errno = xerrno;
      return -1;
    }

    pr_inet_set_nonblock(session.pool, backend_conn);

    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "active backend data connection opened - local  : %s:%d",
      pr_netaddr_get_ipstr(backend_conn->local_addr),
      backend_conn->local_port);
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "active backend data connection opened - remote : %s:%d",
      pr_netaddr_get_ipstr(backend_conn->remote_addr),
      backend_conn->remote_port);
  }

  /* Now we should receive the initial response from the backend server. */
  resp = proxy_ftp_ctrl_recv_resp(cmd->tmp_pool, proxy_sess->backend_ctrl_conn,
    &resp_nlines);
  if (resp == NULL) {
    xerrno = errno;
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "error receiving %s response from backend: %s", (char *) cmd->argv[0],
      strerror(xerrno));

    pr_response_add_err(R_500, _("%s: %s"), (char *) cmd->argv[0],
      strerror(xerrno));
    pr_response_flush(&resp_err_list);

    if (proxy_sess->backend_data_conn != NULL) {
      proxy_inet_close(session.pool, proxy_sess->backend_data_conn);
      proxy_sess->backend_data_conn = NULL;
    }

    errno = xerrno;
    return -1;
  }

  /* If the backend server responds with 4xx/5xx here, close the frontend
   * data connection.
   */
  if (resp->num[0] == '4' || resp->num[0] == '5') {
    res = proxy_ftp_ctrl_send_resp(cmd->tmp_pool,
      proxy_sess->frontend_ctrl_conn, resp, resp_nlines);

    if (proxy_sess->frontend_data_conn) {
      pr_inet_close(session.pool, proxy_sess->frontend_data_conn);
      proxy_sess->frontend_data_conn = session.d = NULL;
    }

    if (proxy_sess->backend_data_conn) {
      proxy_inet_close(session.pool, proxy_sess->backend_data_conn);
      proxy_sess->backend_data_conn = NULL;
    }

    errno = EPERM;
    return -1;
  }

  res = proxy_ftp_ctrl_send_resp(cmd->tmp_pool,
    proxy_sess->frontend_ctrl_conn, resp, resp_nlines);
  if (res < 0) {
    xerrno = errno;

    if (proxy_sess->frontend_data_conn != NULL) {
      pr_inet_close(session.pool, proxy_sess->frontend_data_conn);
      proxy_sess->frontend_data_conn = session.d = NULL;
    }

    if (proxy_sess->backend_data_conn != NULL) {
      proxy_inet_close(session.pool, proxy_sess->backend_data_conn);
      proxy_sess->backend_data_conn = NULL;
    }

    pr_response_block(TRUE);

    pr_response_add_err(R_500, _("%s: %s"), (char *) cmd->argv[0],
      strerror(xerrno));
    pr_response_flush(&resp_err_list);

    errno = xerrno;
    return -1;
  }

  /* Now establish a data connection with the frontend client. */

  if (proxy_sess->frontend_sess_flags & SF_PASSIVE) {
    pr_trace_msg(trace_channel, 17,
      "accepting connection from frontend server for passive data "
      "transfer for %s", (char *) cmd->argv[0]);
    frontend_conn = proxy_ftp_conn_accept(cmd->pool,
      proxy_sess->frontend_data_conn, proxy_sess->frontend_ctrl_conn, TRUE);
    if (frontend_conn == NULL) {
      xerrno = errno;

      if (proxy_sess->frontend_data_conn != NULL) {
        pr_inet_close(session.pool, proxy_sess->frontend_data_conn);
        proxy_sess->frontend_data_conn = session.d = NULL;
      }

      pr_response_add_err(R_425, _("%s: %s"), (char *) cmd->argv[0],
        strerror(xerrno));
      pr_response_flush(&resp_err_list);
    
      errno = xerrno;
      return -1;
    }

    /* Note that we need to set session.d here with the opened conn, for the
     * benefit of other callbacks (e.g. in mod_tls) invoked via these
     * NetIO calls.
     */
    session.d = frontend_conn;

    if (pr_netio_postopen(frontend_conn->instrm) < 0) {
      xerrno = errno;

      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "postopen error for frontend data connection input stream: %s",
        strerror(xerrno));
      pr_inet_close(session.pool, frontend_conn);
      session.d = NULL;

      errno = xerrno;
      return -1;
    }

    if (pr_netio_postopen(frontend_conn->outstrm) < 0) {
      xerrno = errno;

      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "postopen error for frontend data connection output stream: %s",
        strerror(xerrno));
      pr_inet_close(session.pool, frontend_conn);
      session.d = NULL;

      errno = xerrno;
      return -1;
    }

    /* We can close our listening socket now. */
    pr_inet_close(session.pool, proxy_sess->frontend_data_conn);
    proxy_sess->frontend_data_conn = session.d = frontend_conn; 

    pr_inet_set_nonblock(session.pool, frontend_conn);

    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "passive backend data connection opened - local  : %s:%d",
      pr_netaddr_get_ipstr(frontend_conn->local_addr),
      frontend_conn->local_port);
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "passive backend data connection opened - remote : %s:%d",
      pr_netaddr_get_ipstr(frontend_conn->remote_addr),
      frontend_conn->remote_port);

  } else if (proxy_sess->frontend_sess_flags & SF_PORT) {
    pr_netaddr_t *bind_addr;

    /* Connect to the frontend server now. */
  
    if (pr_netaddr_get_family(session.c->local_addr) == pr_netaddr_get_family(session.c->remote_addr)) {
      bind_addr = session.c->local_addr;

    } else {
      /* In this scenario, the server has an IPv6 socket, but the remote client
       * is an IPv4 (or IPv4-mapped IPv6) peer.
       */
      bind_addr = pr_netaddr_v6tov4(session.xfer.p, session.c->local_addr);
    }

    pr_trace_msg(trace_channel, 17,
      "connecting to frontend server for active data transfer for %s",
      (char *) cmd->argv[0]);
    frontend_conn = proxy_ftp_conn_connect(cmd->pool, bind_addr,
      proxy_sess->frontend_data_addr, TRUE);
    if (frontend_conn == NULL) {
      xerrno = errno;

      pr_response_add_err(R_425, _("%s: %s"), (char *) cmd->argv[0],
        strerror(xerrno));
      pr_response_flush(&resp_err_list);

      errno = xerrno;
      return -1;
    }

    /* Note that we need to set session.d here with the opened conn, for the
     * benefit of other callbacks (e.g. in mod_tls) invoked via these
     * NetIO calls.
     */
    session.d = frontend_conn;

    if (pr_netio_postopen(frontend_conn->instrm) < 0) {
      xerrno = errno;

      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "postopen error for frontend data connection input stream: %s",
        strerror(xerrno));
      pr_inet_close(session.pool, frontend_conn);
      session.d = NULL;

      errno = xerrno;
      return -1;
    }

    if (pr_netio_postopen(frontend_conn->outstrm) < 0) {
      xerrno = errno;

      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "postopen error for frontend data connection output stream: %s",
        strerror(xerrno));
      pr_inet_close(session.pool, frontend_conn);
      session.d = NULL;

      errno = xerrno;
      return -1;
    }

    proxy_sess->frontend_data_conn = session.d = frontend_conn;
  }

  *frontend = frontend_conn;
  *backend = backend_conn;
  return 0;
}

MODRET proxy_data(struct proxy_session *proxy_sess, cmd_rec *cmd) {
  int res, xerrno, xfer_direction, xfer_ok = TRUE;
  unsigned int resp_nlines = 0;
  pr_response_t *resp;
  conn_t *frontend_conn = NULL, *backend_conn = NULL;
  off_t bytes_transferred = 0;

  /* We are handling a data transfer command (e.g. LIST, RETR, etc).
   *
   * Thus we need to check the proxy_session->backend_sess_flags, and
   * determine whether we are to connect to the backend server, or open a
   * listening socket to which the backend will connect.  Then we send the
   * given command to the backend.
   *
   * At the same time, we will need to be managing the data connection
   * from the frontend client separately; we will need to multiplex
   * across the four connections: frontend control, frontend data,
   * backend control, backend data.
   */

  if (pr_cmd_cmp(cmd, PR_CMD_APPE_ID) == 0 ||
      pr_cmd_cmp(cmd, PR_CMD_STOR_ID) == 0 ||
      pr_cmd_cmp(cmd, PR_CMD_STOU_ID) == 0) {
    /* Uploading, i.e. writing to backend data conn. */
    xfer_direction = PR_NETIO_IO_WR;

    session.xfer.path = pr_table_get(cmd->notes, "mod_xfer.store-path", NULL);

  } else {
    /* Downloading, i.e. reading from backend data conn.*/
    xfer_direction = PR_NETIO_IO_RD;

    session.xfer.path = pr_table_get(cmd->notes, "mod_xfer.retr-path", NULL);
  }

  res = proxy_data_prepare_conns(proxy_sess, cmd, &frontend_conn,
    &backend_conn);
  if (res < 0) {
    return PR_ERROR(cmd);
  }

  /* If we don't have our frontend/backend connections by now, it's a
   * problem.
   */
  if (frontend_conn == NULL ||
      backend_conn == NULL) {
    xerrno = EPERM;
    pr_response_block(TRUE);

    pr_response_add_err(R_425, _("%s: %s"), (char *) cmd->argv[0],
      strerror(xerrno));
    pr_response_flush(&resp_err_list);

    errno = xerrno;
    return PR_ERROR(cmd);
  }

  /* Allow aborts -- set the current NetIO stream to allow interrupted
   * syscalls, so our SIGURG handler can interrupt it
   */
  switch (xfer_direction) {
    case PR_NETIO_IO_RD:
      proxy_netio_set_poll_interval(backend_conn->instrm, 1);
      pr_netio_set_poll_interval(frontend_conn->outstrm, 1);
      break;

    case PR_NETIO_IO_WR:
      proxy_netio_set_poll_interval(backend_conn->outstrm, 1);
      pr_netio_set_poll_interval(frontend_conn->instrm, 1);
      break;
  }

  proxy_sess->frontend_sess_flags |= SF_XFER;
  proxy_sess->backend_sess_flags |= SF_XFER;

  /* Honor TransferRate directives. */
  pr_throttle_init(cmd);

  if (pr_data_get_timeout(PR_DATA_TIMEOUT_NO_TRANSFER) > 0) {
    pr_timer_reset(PR_TIMER_NOXFER, ANY_MODULE);
  }

  if (pr_data_get_timeout(PR_DATA_TIMEOUT_STALLED) > 0) {
    pr_timer_add(pr_data_get_timeout(PR_DATA_TIMEOUT_STALLED),
      PR_TIMER_STALLED, &proxy_module, proxy_stalled_timeout_cb,
      "TimeoutStalled");
  }

  /* XXX Note: when reading/writing data from data connections, do NOT
   * perform any sort of ASCII translation; we leave the data as is.
   * (Or maybe we SHOULD perform the ASCII translation here, in case of
   * ASCII translation error; the backend server can then be told that
   * the data are binary, and thus relieve the backend of the translation
   * burden.  Configurable?)
   */

  while (TRUE) {
    fd_set rfds;
    struct timeval tv;
    int frontend_data = FALSE, maxfd = -1, timeout = 15;
    conn_t *src_data_conn = NULL, *dst_data_conn = NULL;

    tv.tv_sec = timeout;
    tv.tv_usec = 0;

    pr_signals_handle();

    FD_ZERO(&rfds);

    /* XXX If we wanted to allow/support commands during transfers, we would
     * also need to add the frontend_ctrl_conn instrm fd here for reading.
     * And possibly for ABOR commands, SIGURG.
     */

    /* The source/origin data connection depends on our direction:
     * downloads (IO_RD) from the backend, uploads (IO_WR) to the frontend.
     */
    switch (xfer_direction) {
      case PR_NETIO_IO_RD:
        src_data_conn = proxy_sess->backend_data_conn;
        dst_data_conn = proxy_sess->frontend_data_conn;
        frontend_data = FALSE;
        break;

      case PR_NETIO_IO_WR:
        src_data_conn = proxy_sess->frontend_data_conn;
        dst_data_conn = proxy_sess->backend_data_conn;
        frontend_data = TRUE;
        break;
    }

    FD_SET(PR_NETIO_FD(proxy_sess->backend_ctrl_conn->instrm), &rfds);
    if (PR_NETIO_FD(proxy_sess->backend_ctrl_conn->instrm) > maxfd) {
      maxfd = PR_NETIO_FD(proxy_sess->backend_ctrl_conn->instrm);
    }

    if (src_data_conn != NULL) {
      FD_SET(PR_NETIO_FD(src_data_conn->instrm), &rfds);
      if (PR_NETIO_FD(src_data_conn->instrm) > maxfd) {
        maxfd = PR_NETIO_FD(src_data_conn->instrm);
      }
    }

    res = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    if (res < 0) {
      xerrno = errno;

      if (xerrno == EINTR) {
        pr_signals_handle();
        continue;
      }

      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "error calling select(2) while transferring data: %s",
        strerror(xerrno));

      if (proxy_sess->frontend_data_conn != NULL) {
        pr_inet_close(session.pool, proxy_sess->frontend_data_conn);
        proxy_sess->frontend_data_conn = session.d = NULL;
      }

      if (proxy_sess->backend_data_conn != NULL) {
        proxy_inet_close(session.pool, proxy_sess->backend_data_conn);
        proxy_sess->backend_data_conn = NULL;
      }

      pr_timer_remove(PR_TIMER_STALLED, ANY_MODULE);

      pr_response_add_err(R_500, _("%s: %s"), (char *) cmd->argv[0],
        strerror(xerrno));
      pr_response_flush(&resp_err_list);

      pr_response_block(TRUE);
      errno = xerrno;
      return PR_ERROR(cmd);
    }

    if (res == 0) {
      /* XXX Have MAX_RETRIES logic here. */
      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "timed out waiting for readability on control/data connections, "
        "trying again");
      continue;
    }

    if (src_data_conn != NULL) {
      if (FD_ISSET(PR_NETIO_FD(src_data_conn->instrm), &rfds)) {
        /* Some data arrived on the data connection... */
        pr_buffer_t *pbuf = NULL;

        pr_timer_reset(PR_TIMER_IDLE, ANY_MODULE);
 
        pbuf = proxy_ftp_data_recv(cmd->tmp_pool, src_data_conn, frontend_data);
        if (pbuf == NULL) {
          xerrno = errno;

          (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
            "error receiving from source data connection: %s",
            strerror(xerrno));

        } else {
          pr_timer_reset(PR_TIMER_NOXFER, ANY_MODULE);
          pr_timer_reset(PR_TIMER_STALLED, ANY_MODULE);

          if (pbuf->remaining == 0) {
            /* EOF on the data connection; close BOTH of them. */

            proxy_inet_close(session.pool, proxy_sess->backend_data_conn);
            proxy_sess->backend_data_conn = NULL;

            pr_inet_close(session.pool, proxy_sess->frontend_data_conn);
            proxy_sess->frontend_data_conn = session.d = NULL;

          } else {
            size_t remaining = pbuf->remaining;

            (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
              "received %lu bytes of data from source data connection",
              (unsigned long) remaining);
            session.xfer.total_bytes += remaining;

            bytes_transferred += remaining;
            pr_throttle_pause(bytes_transferred, FALSE);

            res = proxy_ftp_data_send(cmd->tmp_pool, dst_data_conn, pbuf,
              !frontend_data);
            if (res < 0) {
              xerrno = errno;

              (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
                "error writing %lu bytes of data to sink data connection: %s",
                (unsigned long) remaining, strerror(xerrno));

              /* If this happens, close our connection prematurely.
               * XXX Should we try to send an ABOR here, too?  Or SIGURG?
               * XXX Should we only do this for e.g. Broken pipe?
               */
              (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
                "unable to proxy data between frontend/backend, "
                "closing data connections");

              proxy_inet_close(session.pool, proxy_sess->backend_data_conn);
              proxy_sess->backend_data_conn = NULL;

              pr_inet_close(session.pool, proxy_sess->frontend_data_conn);
              proxy_sess->frontend_data_conn = session.d = NULL;
            }
          }
        }
      }
    }

    if (FD_ISSET(PR_NETIO_FD(proxy_sess->backend_ctrl_conn->instrm), &rfds)) {
      /* Some data arrived on the ctrl connection... */
      pr_timer_reset(PR_TIMER_IDLE, ANY_MODULE);

      resp = proxy_ftp_ctrl_recv_resp(cmd->tmp_pool,
        proxy_sess->backend_ctrl_conn, &resp_nlines);
      if (resp == NULL) {
        xerrno = errno;
        (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
          "error receiving response from control connection: %s",
          strerror(xerrno));

        if (proxy_sess->frontend_data_conn != NULL) {
          pr_inet_close(session.pool, proxy_sess->frontend_data_conn);
          proxy_sess->frontend_data_conn = session.d = NULL;
        }

        if (proxy_sess->backend_data_conn != NULL) {
          proxy_inet_close(session.pool, proxy_sess->backend_data_conn);
          proxy_sess->backend_data_conn = NULL;
        }

        /* For a certain number of conditions, if we cannot read the response
         * from the backend, then we should just close the frontend, otherwise
         * we might "leak" to the client the fact that we are fronting some
         * backend server rather than being the server.
         */
        if (xerrno == ECONNRESET ||
            xerrno == ECONNABORTED ||
            xerrno == ENOENT ||
            xerrno == EPIPE) {
          pr_session_disconnect(&proxy_module,
            PR_SESS_DISCONNECT_BY_APPLICATION,
            "Backend control connection lost");
        }

        xfer_ok = FALSE;
        break;

      } else {

        /* If not a 1xx response, close the destination data connection,
         * BEFORE we send the response from the backend to the connected client.
         */
        if (resp->num[0] != '1') {
          switch (xfer_direction) {
            case PR_NETIO_IO_RD:
              if (proxy_sess->frontend_data_conn != NULL) {
                pr_inet_close(session.pool, proxy_sess->frontend_data_conn);
                proxy_sess->frontend_data_conn = session.d = NULL;
              }
              break;

            case PR_NETIO_IO_WR:
              if (proxy_sess->backend_data_conn != NULL) {
                proxy_inet_close(session.pool, proxy_sess->backend_data_conn);
                proxy_sess->backend_data_conn = NULL;
              }
              break;
          }

          /* If the response was a 4xx or 5xx, then we need to note that as
           * a failed transfer.
           */
          /* XXX What about ABOR/aborted transfers? */
          if (resp->num[0] == '4' || resp->num[0] == '5') {
            xfer_ok = FALSE;
          }
        }

        res = proxy_ftp_ctrl_send_resp(cmd->tmp_pool,
          proxy_sess->frontend_ctrl_conn, resp, resp_nlines);
        if (res < 0) {
          xerrno = errno;

          pr_response_add_err(R_500, _("%s: %s"), (char *) cmd->argv[0],
            strerror(xerrno));
          pr_response_flush(&resp_err_list);

          pr_timer_remove(PR_TIMER_STALLED, ANY_MODULE);
          errno = xerrno;
          return PR_ERROR(cmd);
        }

        /* If we get a 1xx response here, keep going.  Otherwise, we're
         * done with this data transfer.
         */
        if (src_data_conn == NULL ||
            (resp->num)[0] != '1') {
          proxy_sess->frontend_sess_flags &= (SF_ALL^SF_PASSIVE);
          proxy_sess->frontend_sess_flags &= (SF_ALL^(SF_ABORT|SF_XFER|SF_PASSIVE|SF_ASCII_OVERRIDE));

          proxy_sess->backend_sess_flags &= (SF_ALL^SF_PASSIVE);
          proxy_sess->backend_sess_flags &= (SF_ALL^(SF_ABORT|SF_XFER|SF_PASSIVE|SF_ASCII_OVERRIDE));
          break;
        }
      }
    }
  }

  if (pr_data_get_timeout(PR_DATA_TIMEOUT_STALLED) > 0) {
    pr_timer_remove(PR_TIMER_STALLED, ANY_MODULE);
  }

  pr_throttle_pause(bytes_transferred, TRUE);

  pr_response_clear(&resp_list);
  pr_response_clear(&resp_err_list);

  return (xfer_ok ? PR_HANDLED(cmd) : PR_ERROR(cmd));
}

MODRET proxy_eprt(cmd_rec *cmd, struct proxy_session *proxy_sess) {
  int res, xerrno;
  pr_netaddr_t *remote_addr = NULL;
  unsigned short remote_port;
  unsigned char *allow_foreign_addr = NULL;

  CHECK_CMD_ARGS(cmd, 2);

  /* We can't just send the frontend's EPRT, as is, to the backend.
   * We need to connect to the frontend's EPRT; we need to open a listening
   * socket and send its address to the backend in our EPRT command.
   */

  /* XXX How to handle this if we are chrooted, without root privs, for
   * e.g. source ports below 1024?
   */

  remote_addr = proxy_ftp_msg_parse_ext_addr(cmd->tmp_pool, cmd->argv[1],
    session.c->remote_addr, cmd->cmd_id, NULL);
  if (remote_addr == NULL) {
    xerrno = errno;

    pr_trace_msg("proxy", 2, "error parsing EPRT command '%s': %s",
      (char *) cmd->argv[1], strerror(xerrno));

    if (xerrno == EPROTOTYPE) {
#ifdef PR_USE_IPV6
      if (pr_netaddr_use_ipv6()) {
        pr_response_add_err(R_522,
          _("Network protocol not supported, use (1,2)"));

      } else {
        pr_response_add_err(R_522,
          _("Network protocol not supported, use (1)"));
      }
#else
      pr_response_add_err(R_522, _("Network protocol not supported, use (1)"));
#endif /* PR_USE_IPV6 */

    } else {
      pr_response_add_err(R_501, _("Illegal EPRT command"));
    }

    pr_response_flush(&resp_err_list);

    errno = xerrno;
    return PR_ERROR(cmd);
  }

  remote_port = ntohs(pr_netaddr_get_port(remote_addr));

  /* If we are NOT listening on an RFC1918 address, BUT the client HAS
   * sent us an RFC1918 address in its EPRT command (which we know to not be
   * routable), then ignore that address, and use the client's remote address.
   */
  if (pr_netaddr_is_rfc1918(session.c->local_addr) != TRUE &&
      pr_netaddr_is_rfc1918(session.c->remote_addr) != TRUE &&
      pr_netaddr_is_rfc1918(remote_addr) == TRUE) {
    const char *rfc1918_ipstr;

    rfc1918_ipstr = pr_netaddr_get_ipstr(remote_addr);
    remote_addr = pr_netaddr_dup(cmd->tmp_pool, session.c->remote_addr);
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "client sent RFC1918 address '%s' in EPRT command, ignoring it and "
      "using '%s'", rfc1918_ipstr, pr_netaddr_get_ipstr(remote_addr));
  }

  /* Make sure that the address specified matches the address from which
   * the control connection is coming.
   */
  allow_foreign_addr = get_param_ptr(main_server->conf, "AllowForeignAddress",
    FALSE);

  if (allow_foreign_addr == NULL ||
      *allow_foreign_addr == FALSE) {
    if (pr_netaddr_cmp(remote_addr, session.c->remote_addr) != 0 ||
        !remote_port) {
      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
       "Refused EPRT %s (address mismatch)", cmd->arg);
      pr_response_add_err(R_500, _("Illegal EPRT command"));
      pr_response_flush(&resp_err_list);

      errno = EPERM;
      return PR_ERROR(cmd);
    }
  }

  /* Additionally, make sure that the port number used is a "high numbered"
   * port, to avoid bounce attacks.  For remote Windows machines, the
   * port numbers mean little.  However, there are also quite a few Unix
   * machines out there for whom the port number matters...
   */
  if (remote_port < 1024) {
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "Refused EPRT %s (port %d below 1024, possible bounce attack)", cmd->arg,
      remote_port);

    pr_response_add_err(R_500, _("Illegal EPRT command"));
    pr_response_flush(&resp_err_list);

    errno = EPERM;
    return PR_ERROR(cmd);
  }

  proxy_sess->frontend_data_addr = remote_addr;

  switch (proxy_sess->dataxfer_policy) {
    case PR_CMD_PASV_ID:
    case PR_CMD_EPSV_ID: {
      pr_netaddr_t *addr;
      pr_response_t *resp;
      unsigned int resp_nlines = 0;

      addr = proxy_ftp_xfer_prepare_passive(proxy_sess->dataxfer_policy, cmd,
        R_500, proxy_sess);
      if (addr == NULL) {
        return PR_ERROR(cmd);
      }

      resp = palloc(cmd->tmp_pool, sizeof(pr_response_t));
      resp->num = R_200;
      resp->msg = _("EPRT command successful");
      resp_nlines = 1;

      res = proxy_ftp_ctrl_send_resp(cmd->tmp_pool,
        proxy_sess->frontend_ctrl_conn, resp, resp_nlines);
      if (res < 0) {
        xerrno = errno;

        (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
          "error sending '%s %s' response to frontend: %s", resp->num,
          resp->msg, strerror(xerrno));

        errno = xerrno;
        return PR_ERROR(cmd);
      }

      proxy_sess->backend_data_addr = addr;
      proxy_sess->backend_sess_flags |= SF_PASSIVE;
      break;
    }

    case PR_CMD_PORT_ID:
    case PR_CMD_EPRT_ID: 
    default: {
      pr_response_t *resp;
      unsigned int resp_nlines = 0;

      res = proxy_ftp_xfer_prepare_active(proxy_sess->dataxfer_policy, cmd,
        R_425, proxy_sess);
      if (res < 0) {
        return PR_ERROR(cmd);
      }

      resp = palloc(cmd->tmp_pool, sizeof(pr_response_t));
      resp->num = R_200;
      resp->msg = _("EPRT command successful");
      resp_nlines = 1;

      res = proxy_ftp_ctrl_send_resp(cmd->tmp_pool,
        proxy_sess->frontend_ctrl_conn, resp, resp_nlines);
      if (res < 0) {
        xerrno = errno;

        (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
          "error sending '%s %s' response to frontend: %s", resp->num,
          resp->msg, strerror(xerrno));

        errno = xerrno;
        return PR_ERROR(cmd);
      }

      proxy_sess->backend_sess_flags |= SF_PORT;
      break;
    }
  }

  /* If the command was successful, mark it in the session state/flags. */
  proxy_sess->frontend_sess_flags |= SF_PORT;

  return PR_HANDLED(cmd);
}

MODRET proxy_epsv(cmd_rec *cmd, struct proxy_session *proxy_sess) {
  int res, xerrno;
  conn_t *data_conn;
  const char *epsv_msg;
  char resp_msg[PR_RESPONSE_BUFFER_SIZE];
  pr_netaddr_t *bind_addr, *remote_addr;
  pr_response_t *resp;
  unsigned int resp_nlines = 1;

  /* TODO: Handle any possible EPSV params, e.g. "EPSV ALL", properly. */

  switch (proxy_sess->dataxfer_policy) {
    case PR_CMD_PORT_ID:
    case PR_CMD_EPRT_ID:
      res = proxy_ftp_xfer_prepare_active(proxy_sess->dataxfer_policy, cmd,
        R_425, proxy_sess);
      if (res < 0) {
        return PR_ERROR(cmd);
      }

      proxy_sess->backend_sess_flags |= SF_PORT;
      break;

    case PR_CMD_PASV_ID:
    case PR_CMD_EPSV_ID:
    default:
      remote_addr = proxy_ftp_xfer_prepare_passive(proxy_sess->dataxfer_policy,
        cmd, R_500, proxy_sess);
      if (remote_addr == NULL) {
        return PR_ERROR(cmd);
      }

      proxy_sess->backend_data_addr = remote_addr;
      proxy_sess->backend_sess_flags |= SF_PASSIVE;
      break;
  }

  /* We do NOT want to connect here, but would rather wait until the
   * ensuing data transfer-initiating command.  Otherwise, a client could
   * spew PASV commands at us, and we would flood the backend server with
   * data transfer connections needlessly.
   *
   * We DO, however, need to create our own listening connection, so that
   * we can inform the client of the address/port to which IT is to
   * connect for its part of the data transfer.
   *
   * Note that we do NOT use the ProxySourceAddress here, since this listening
   * connection is for the frontend client.
   */

  if (pr_netaddr_get_family(session.c->local_addr) == pr_netaddr_get_family(session.c->remote_addr)) {
    bind_addr = session.c->local_addr;

  } else {
    /* In this scenario, the server has an IPv6 socket, but the remote client
     * is an IPv4 (or IPv4-mapped IPv6) peer.
     */
    bind_addr = pr_netaddr_v6tov4(cmd->pool, session.c->local_addr);
  }

  /* PassivePorts is handled by proxy_ftp_conn_listen(). */
  data_conn = proxy_ftp_conn_listen(cmd->pool, bind_addr, FALSE);
  if (data_conn == NULL) {
    xerrno = errno;

    proxy_inet_close(session.pool, proxy_sess->backend_data_conn);
    proxy_sess->backend_data_conn = NULL;

    pr_response_add_err(R_425,
      _("Unable to build data connection: Internal error"));
    pr_response_flush(&resp_err_list);

    errno = xerrno;
    return PR_ERROR(cmd);
  }

  if (proxy_sess->frontend_data_conn != NULL) {
    /* Make sure that we only have one frontend data connection. */
    pr_inet_close(session.pool, proxy_sess->frontend_data_conn);
    proxy_sess->frontend_data_conn = session.d = NULL;
  }

  proxy_sess->frontend_data_conn = session.d = data_conn;

  epsv_msg = proxy_ftp_msg_fmt_ext_addr(cmd->tmp_pool, data_conn->local_addr,
    data_conn->local_port, cmd->cmd_id, TRUE);

  (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
    "Entering Extended Passive Mode (%s)", epsv_msg);

  /* Change the response to send back to the connecting client, telling it
   * to use OUR address/port.
   */
  resp = palloc(cmd->tmp_pool, sizeof(pr_response_t));
  resp->num = R_229;
  memset(resp_msg, '\0', sizeof(resp_msg));
  snprintf(resp_msg, sizeof(resp_msg)-1, "Entering Extended Passive Mode (%s)",
    epsv_msg);
  resp->msg = resp_msg;

  res = proxy_ftp_ctrl_send_resp(cmd->tmp_pool, proxy_sess->frontend_ctrl_conn,
    resp, resp_nlines);
  if (res < 0) {
    xerrno = errno;

    proxy_inet_close(session.pool, proxy_sess->backend_data_conn);
    proxy_sess->backend_data_conn = NULL;

    proxy_inet_close(session.pool, data_conn);
    pr_response_block(TRUE);

    pr_response_add_err(R_500, _("%s: %s"), (char *) cmd->argv[0],
      strerror(xerrno));
    pr_response_flush(&resp_err_list);

    errno = xerrno;
    return PR_ERROR(cmd);
  }

  proxy_sess->frontend_sess_flags |= SF_PASSIVE;
  return PR_HANDLED(cmd);
}

MODRET proxy_pasv(cmd_rec *cmd, struct proxy_session *proxy_sess) {
  int res, xerrno;
  conn_t *data_conn;
  const char *pasv_msg;
  char resp_msg[PR_RESPONSE_BUFFER_SIZE];
  pr_netaddr_t *bind_addr, *remote_addr;
  pr_response_t *resp;
  unsigned int resp_nlines = 1;

  switch (proxy_sess->dataxfer_policy) {
    case PR_CMD_PORT_ID:
    case PR_CMD_EPRT_ID:
      res = proxy_ftp_xfer_prepare_active(proxy_sess->dataxfer_policy, cmd,
        R_425, proxy_sess);
      if (res < 0) {
        return PR_ERROR(cmd);
      }

      proxy_sess->backend_sess_flags |= SF_PORT;
      break;

    case PR_CMD_PASV_ID:
    case PR_CMD_EPSV_ID:
    default:
      remote_addr = proxy_ftp_xfer_prepare_passive(proxy_sess->dataxfer_policy,
        cmd, R_500, proxy_sess);
      if (remote_addr == NULL) {
        return PR_ERROR(cmd);
      }

      proxy_sess->backend_data_addr = remote_addr;
      proxy_sess->backend_sess_flags |= SF_PASSIVE;
      break;
  }

  /* We do NOT want to connect here, but would rather wait until the
   * ensuing data transfer-initiating command.  Otherwise, a client could
   * spew PASV commands at us, and we would flood the backend server with
   * data transfer connections needlessly.
   *
   * We DO, however, need to create our own listening connection, so that
   * we can inform the client of the address/port to which IT is to
   * connect for its part of the data transfer.
   *
   * Note that we do NOT use the ProxySourceAddress here, since this listening
   * connection is for the frontend client.
   */

  if (pr_netaddr_get_family(session.c->local_addr) == pr_netaddr_get_family(session.c->remote_addr)) {
#ifdef PR_USE_IPV6
    if (pr_netaddr_use_ipv6()) {
      /* Make sure that the family is NOT IPv6, even though the family of the
       * local and remote ends match.  The PASV command cannot be used for
       * IPv6 addresses (Bug#3745).
       */
      if (pr_netaddr_get_family(session.c->local_addr) == AF_INET6) {
        xerrno = EPERM;

        (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
          "Unable to handle PASV for IPv6 address '%s', rejecting command",
          pr_netaddr_get_ipstr(session.c->local_addr));
        pr_response_add_err(R_501, "%s: %s", (char *) cmd->argv[0],
          strerror(xerrno));

        pr_cmd_set_errno(cmd, xerrno);
        errno = xerrno;
        return PR_ERROR(cmd);
      }
    }
#endif /* PR_USE_IPV6 */

    bind_addr = session.c->local_addr;

  } else {
    bind_addr = pr_netaddr_v6tov4(cmd->pool, session.c->local_addr);
  }

  /* PassivePorts is handled by proxy_ftp_conn_listen(). */
  data_conn = proxy_ftp_conn_listen(cmd->pool, bind_addr, TRUE);
  if (data_conn == NULL) {
    xerrno = errno;

    proxy_inet_close(session.pool, proxy_sess->backend_data_conn);
    proxy_sess->backend_data_conn = NULL;

    pr_response_add_err(R_425,
      _("Unable to build data connection: Internal error"));
    pr_response_flush(&resp_err_list);

    errno = xerrno;
    return PR_ERROR(cmd);
  }

  if (proxy_sess->frontend_data_conn != NULL) {
    /* Make sure that we only have one frontend data connection. */
    pr_inet_close(session.pool, proxy_sess->frontend_data_conn);
    proxy_sess->frontend_data_conn = session.d = NULL;
  }

  proxy_sess->frontend_data_conn = session.d = data_conn;

  pasv_msg = proxy_ftp_msg_fmt_addr(cmd->tmp_pool, data_conn->local_addr,
    data_conn->local_port, TRUE);

  (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
    "Entering Passive Mode (%s).", pasv_msg);

  /* Change the response to send back to the connecting client, telling it
   * to use OUR address/port.
   */
  resp = palloc(cmd->tmp_pool, sizeof(pr_response_t));
  resp->num = R_227;
  memset(resp_msg, '\0', sizeof(resp_msg));
  snprintf(resp_msg, sizeof(resp_msg)-1, "Entering Passive Mode (%s).",
    pasv_msg);
  resp->msg = resp_msg;

  res = proxy_ftp_ctrl_send_resp(cmd->tmp_pool, proxy_sess->frontend_ctrl_conn,
    resp, resp_nlines);
  if (res < 0) {
    xerrno = errno;

    proxy_inet_close(session.pool, proxy_sess->backend_data_conn);
    proxy_sess->backend_data_conn = NULL;

    pr_inet_close(session.pool, data_conn);
    proxy_sess->frontend_data_conn = session.d = NULL;
    pr_response_block(TRUE);

    pr_response_add_err(R_500, _("%s: %s"), (char *) cmd->argv[0],
      strerror(xerrno));
    pr_response_flush(&resp_err_list);

    errno = xerrno;
    return PR_ERROR(cmd);
  }

  proxy_sess->frontend_sess_flags |= SF_PASSIVE;
  return PR_HANDLED(cmd);
}

MODRET proxy_port(cmd_rec *cmd, struct proxy_session *proxy_sess) {
  int res, xerrno;
  pr_netaddr_t *remote_addr = NULL;
  unsigned short remote_port;
  unsigned char *allow_foreign_addr = NULL;

  CHECK_CMD_ARGS(cmd, 2);

  /* We can't just send the frontend's PORT, as is, to the backend.
   * We need to connect to the frontend's PORT; we need to open a listening
   * socket and send its address to the backend in our PORT command.
   */

  /* XXX How to handle this if we are chrooted, without root privs, for
   * e.g. source ports below 1024?
   */

  remote_addr = proxy_ftp_msg_parse_addr(cmd->tmp_pool, cmd->argv[1],
    pr_netaddr_get_family(session.c->remote_addr));
  if (remote_addr == NULL) {
    xerrno = errno;

    pr_trace_msg("proxy", 2, "error parsing PORT command '%s': %s",
      (char *) cmd->argv[1], strerror(xerrno));

    pr_response_add_err(R_501, _("Illegal PORT command"));
    pr_response_flush(&resp_err_list);

    errno = xerrno;
    return PR_ERROR(cmd);
  }

  remote_port = ntohs(pr_netaddr_get_port(remote_addr));

  /* If we are NOT listening on an RFC1918 address, BUT the client HAS
   * sent us an RFC1918 address in its PORT command (which we know to not be
   * routable), then ignore that address, and use the client's remote address.
   */
  if (pr_netaddr_is_rfc1918(session.c->local_addr) != TRUE &&
      pr_netaddr_is_rfc1918(session.c->remote_addr) != TRUE &&
      pr_netaddr_is_rfc1918(remote_addr) == TRUE) {
    const char *rfc1918_ipstr;

    rfc1918_ipstr = pr_netaddr_get_ipstr(remote_addr);
    remote_addr = pr_netaddr_dup(cmd->tmp_pool, session.c->remote_addr);
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "client sent RFC1918 address '%s' in PORT command, ignoring it and "
      "using '%s'", rfc1918_ipstr, pr_netaddr_get_ipstr(remote_addr));
  }

  /* Make sure that the address specified matches the address from which
   * the control connection is coming.
   */

  allow_foreign_addr = get_param_ptr(TOPLEVEL_CONF, "AllowForeignAddress",
    FALSE);

  if (allow_foreign_addr == NULL ||
      *allow_foreign_addr == FALSE) {
#ifdef PR_USE_IPV6
    if (pr_netaddr_use_ipv6()) {
      /* We can only compare the PORT-given address against the remote client
       * address if the remote client address is an IPv4-mapped IPv6 address.
       */
      if (pr_netaddr_get_family(session.c->remote_addr) == AF_INET6 &&
          pr_netaddr_is_v4mappedv6(session.c->remote_addr) != TRUE) {
        (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
          "Refused PORT %s (IPv4/IPv6 address mismatch)", cmd->arg);

        pr_response_add_err(R_500, _("Illegal PORT command"));
        pr_response_flush(&resp_err_list);

        errno = EPERM;
        return PR_ERROR(cmd);
      }
    }
#endif /* PR_USE_IPV6 */

    if (pr_netaddr_cmp(remote_addr, session.c->remote_addr) != 0) {
      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "Refused PORT %s (address mismatch)", cmd->arg);

      pr_response_add_err(R_500, _("Illegal PORT command"));
      pr_response_flush(&resp_err_list);

      errno = EPERM;
      return PR_ERROR(cmd);
    }
  }

  /* Additionally, make sure that the port number used is a "high numbered"
   * port, to avoid bounce attacks.  For remote Windows machines, the
   * port numbers mean little.  However, there are also quite a few Unix
   * machines out there for whom the port number matters...
   */
  if (remote_port < 1024) {
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "Refused PORT %s (port %d below 1024, possible bounce attack)", cmd->arg,
      remote_port);

    pr_response_add_err(R_500, _("Illegal PORT command"));
    pr_response_flush(&resp_err_list);

    errno = EPERM;
    return PR_ERROR(cmd);
  }

  proxy_sess->frontend_data_addr = remote_addr;

  switch (proxy_sess->dataxfer_policy) {
    case PR_CMD_PASV_ID:
    case PR_CMD_EPSV_ID: {
      pr_netaddr_t *addr;
      pr_response_t *resp;
      unsigned int resp_nlines = 0;

      addr = proxy_ftp_xfer_prepare_passive(proxy_sess->dataxfer_policy, cmd,
        R_500, proxy_sess);
      if (addr == NULL) {
        return PR_ERROR(cmd);
      }

      resp = palloc(cmd->tmp_pool, sizeof(pr_response_t));
      resp->num = R_200;
      resp->msg = _("PORT command successful");
      resp_nlines = 1;

      res = proxy_ftp_ctrl_send_resp(cmd->tmp_pool,
        proxy_sess->frontend_ctrl_conn, resp, resp_nlines);
      if (res < 0) {
        xerrno = errno;

        (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
          "error sending '%s %s' response to frontend: %s", resp->num,
          resp->msg, strerror(xerrno));

        errno = xerrno;
        return PR_ERROR(cmd);
      }

      proxy_sess->backend_data_addr = addr;
      proxy_sess->backend_sess_flags |= SF_PASSIVE;
      break;
    }

    case PR_CMD_PORT_ID:
    case PR_CMD_EPRT_ID:
    default: {
      pr_response_t *resp;
      unsigned int resp_nlines = 0;

      res = proxy_ftp_xfer_prepare_active(proxy_sess->dataxfer_policy, cmd,
        R_425, proxy_sess);
      if (res < 0) {
        return PR_ERROR(cmd);
      }

      resp = palloc(cmd->tmp_pool, sizeof(pr_response_t));
      resp->num = R_200;
      resp->msg = _("PORT command successful");
      resp_nlines = 1;

      res = proxy_ftp_ctrl_send_resp(cmd->tmp_pool,
        proxy_sess->frontend_ctrl_conn, resp, resp_nlines);
      if (res < 0) {
        xerrno = errno;

        (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
          "error sending '%s %s' response to frontend: %s", resp->num,
          resp->msg, strerror(xerrno));

        errno = xerrno;
        return PR_ERROR(cmd);
      }

      proxy_sess->backend_sess_flags |= SF_PORT;
      break;
    }
  }

  /* If the command was successful, mark it in the session state/flags. */
  proxy_sess->frontend_sess_flags |= SF_PORT;

  return PR_HANDLED(cmd);
}

MODRET proxy_feat(cmd_rec *cmd, struct proxy_session *proxy_sess) {
  modret_t *mr = NULL;
  pr_response_t *resp = NULL;

  mr = proxy_cmd(cmd, proxy_sess, &resp);

  /* If we don't already have our backend feature table allocated, as
   * when the backend server won't support the FEAT command until AFTER
   * authentication occurs, then try to piggyback on the frontend client's
   * FEAT command, and fill our table now.
   */

  if (proxy_sess->backend_features == NULL) {
    if (MODRET_ISHANDLED(mr) &&
        resp != NULL) {
      const char *feat_crlf = "\r\n";
      char *feats, *token;
      size_t token_len = 0;

      pr_trace_msg(trace_channel, 9,
        "populating backend features based on FEAT response to frontend "
        "client");

      proxy_sess->backend_features = pr_table_nalloc(proxy_pool, 0, 4);

      feats = pstrdup(cmd->tmp_pool, resp->msg);
      token = pr_str_get_token2(&feats, (char *) feat_crlf, &token_len);
      while (token != NULL) {
        pr_signals_handle();

        if (token_len > 0) {
          /* The FEAT response lines in which we are interested all start with
           * a single space, per RFC spec.  Ignore any other lines.
           */
          if (token[0] == ' ') {
            char *key, *val, *ptr;

            /* Find the next space in the string, to delimit our key/value
             * pairs.
             */
            ptr = strchr(token + 1, ' ');
            if (ptr != NULL) {
              key = pstrndup(proxy_pool, token + 1, ptr - token - 1);
              val = pstrdup(proxy_pool, ptr + 1);

            } else {
              key = pstrdup(proxy_pool, token + 1);
              val = pstrdup(proxy_pool, "");
            }

            pr_table_add(proxy_sess->backend_features, key, val, 0);
          }
        }

        feats = token + token_len + 1;
        token = pr_str_get_token2(&feats, (char *) feat_crlf, &token_len);
      }
    }
  }

  return mr;
}

MODRET proxy_user(cmd_rec *cmd, struct proxy_session *proxy_sess,
    int *block_responses) {
  int successful = FALSE, res;

  /* Remove any exit handlers installed by mod_xfer.  We do this here,
   * rather than in sess_init, since our sess_init is called BEFORE the
   * sess_init of mod_xfer.
   */
  pr_event_unregister(&xfer_module, "core.exit", NULL);

  if (proxy_sess_state & PROXY_SESS_STATE_BACKEND_AUTHENTICATED) {
    /* If we've already authenticated, then let the backend server deal
     * with this.
     */
    return proxy_cmd(cmd, proxy_sess, NULL);
  }

  switch (proxy_role) {
    case PROXY_ROLE_REVERSE:
      res = proxy_reverse_handle_user(cmd, proxy_sess, &successful,
        block_responses);
      break;

    case PROXY_ROLE_FORWARD:
      res = proxy_forward_handle_user(cmd, proxy_sess, &successful,
        block_responses);
      break;
  }

  if (res < 0) {
    int xerrno = errno;

    if (xerrno != EINVAL) {
      pr_response_add_err(R_500, _("%s: %s"), (char *) cmd->argv[0],
        strerror(xerrno));

    } else {
      pr_response_add_err(R_530, _("Login incorrect."));
    }

    pr_response_flush(&resp_err_list);

    errno = xerrno;
    return PR_ERROR(cmd);
  }

  if (successful) {
    config_rec *c;
    const char *notes_key = "mod_auth.orig-user";
    char *user, *xferlog = PR_XFERLOG_PATH;

    /* For 2xx/3xx responses (others?), stash the user name appropriately. */
    user = cmd->arg;

    (void) pr_table_remove(session.notes, notes_key, NULL);
    if (pr_table_add_dup(session.notes, notes_key, user, 0) < 0) {
      pr_log_debug(DEBUG3, "error stashing '%s' in session.notes: %s",
        notes_key, strerror(errno));
    }

    /* Open the TransferLog here. */
    c = find_config(main_server->conf, CONF_PARAM, "TransferLog", FALSE);
    if (c != NULL) {
      xferlog = c->argv[0];
    }

    if (strncasecmp(xferlog, "none", 5) == 0) {
      xferlog_open(NULL);

    } else {
      xferlog_open(xferlog);
    }

    /* Handle DefaultTransferMode here. */
    c = find_config(main_server->conf, CONF_PARAM, "DefaultTransferMode",
      FALSE);
    if (c != NULL) {
      if (strncasecmp(c->argv[0], "binary", 7) == 0) {
        session.sf_flags &= (SF_ALL^SF_ASCII);

      } else {
        session.sf_flags |= SF_ASCII;
      }

    } else {
      /* ASCII by default. */
      session.sf_flags |= SF_ASCII;
    }
  }

  return (res == 0 ? PR_DECLINED(cmd) : PR_HANDLED(cmd));
}

MODRET proxy_pass(cmd_rec *cmd, struct proxy_session *proxy_sess,
    int *block_responses) {
  int successful = FALSE, res;

  if (proxy_sess_state & PROXY_SESS_STATE_BACKEND_AUTHENTICATED) {
    /* If we've already authenticated, then let the backend server deal with
     * this.
     */
    return proxy_cmd(cmd, proxy_sess, NULL);
  }

  switch (proxy_role) {
    case PROXY_ROLE_REVERSE:
      res = proxy_reverse_handle_pass(cmd, proxy_sess, &successful,
        block_responses);
      break;

    case PROXY_ROLE_FORWARD:
      res = proxy_forward_handle_pass(cmd, proxy_sess, &successful,
        block_responses);
      break;
  }

  if (res < 0) {
    int xerrno = errno;

    if (xerrno != EINVAL) {
      pr_response_add_err(R_500, _("%s: %s"), (char *) cmd->argv[0],
        strerror(xerrno));

    } else {
      pr_response_add_err(R_530, _("Login incorrect."));
    }

    pr_response_flush(&resp_err_list);

    errno = xerrno;
    return PR_ERROR(cmd);
  }

  if (successful) {
    char *user;
    int proxy_auth = FALSE;

    user = pr_table_get(session.notes, "mod_auth.orig-user", NULL);
    session.user = user;

    switch (proxy_role) {
      case PROXY_ROLE_FORWARD:
        proxy_auth = proxy_forward_use_proxy_auth();
        break;

      case PROXY_ROLE_REVERSE:
        proxy_auth = proxy_reverse_use_proxy_auth();
        break;
    }

    /* Unless proxy auth happened, we set the session groups to null,
     * to avoid unexpected behavior when looking up e.g. <Limit> sections.
     */
    if (proxy_auth == FALSE) {
      if (session.group != NULL) {
        pr_trace_msg(trace_channel, 9,
          "clearing unauthenticated primary group name '%s' for user '%s'",
          session.group, session.user);
        session.group = NULL;
      }

      if (session.groups != NULL) {
        register unsigned int i;

        pr_trace_msg(trace_channel, 9,
          "clearing %d unauthenticated additional group %s for user '%s':",
          session.groups->nelts, session.groups->nelts != 1 ? "names" : "name",
          session.user);
        for (i = 0; i < session.groups->nelts; i++) {
          pr_trace_msg(trace_channel, 9,
            "  clearing additional group name '%s'",
            ((char **) session.groups->elts)[i]);
        }

        session.groups = NULL;
      }
    }

    /* XXX Do we need to set other login-related fields here?  E.g.
     * session.uid, session.gid, etc?
     */

    fixup_dirs(main_server, CF_DEFER);
    if (proxy_role == PROXY_ROLE_FORWARD) {
      proxy_restrict_session();
    }
  }

  return PR_HANDLED(cmd);
}

MODRET proxy_type(cmd_rec *cmd, struct proxy_session *proxy_sess) {
  int res, xerrno;
  pr_response_t *resp;
  unsigned int resp_nlines = 0;

  res = proxy_ftp_ctrl_send_cmd(cmd->tmp_pool, proxy_sess->backend_ctrl_conn,
    cmd);
  if (res < 0) {
    xerrno = errno;
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "error sending %s to backend: %s", (char *) cmd->argv[0],
      strerror(xerrno));

    pr_response_add_err(R_500, _("%s: %s"), (char *) cmd->argv[0],
      strerror(xerrno));
    pr_response_flush(&resp_err_list);

    errno = xerrno;
    return PR_ERROR(cmd);
  }

  resp = proxy_ftp_ctrl_recv_resp(cmd->tmp_pool, proxy_sess->backend_ctrl_conn,
    &resp_nlines);
  if (resp == NULL) {
    xerrno = errno;
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "error receiving %s response from backend: %s", (char *) cmd->argv[0],
      strerror(xerrno));

    pr_response_add_err(R_500, _("%s: %s"), (char *) cmd->argv[0],
      strerror(xerrno));
    pr_response_flush(&resp_err_list);

    errno = xerrno;
    return PR_ERROR(cmd);
  }

  if (resp->num[0] == '2') {
    char *type;

    /* This code is duplicated from mod_xfer.c#xfer_type().  Would be nice
     * to factor it out somewhere reusable, i.e. some pr_str_ function.
     */

    type = pstrdup(cmd->tmp_pool, cmd->argv[1]);
    type[0] = toupper(type[0]);

    if (strncmp(type, "A", 2) == 0 ||
        (cmd->argc == 3 &&
         strncmp(type, "L", 2) == 0 &&
         strncmp(cmd->argv[2], "7", 2) == 0)) {

      /* TYPE A(SCII) or TYPE L 7. */
      session.sf_flags |= SF_ASCII;

    } else if (strncmp(type, "I", 2) == 0 ||
        (cmd->argc == 3 &&
         strncmp(type, "L", 2) == 0 &&
         strncmp(cmd->argv[2], "8", 2) == 0)) {

      /* TYPE I(MAGE) or TYPE L 8. */
      session.sf_flags &= (SF_ALL^(SF_ASCII|SF_ASCII_OVERRIDE));
    }
  }

  res = proxy_ftp_ctrl_send_resp(cmd->tmp_pool, proxy_sess->frontend_ctrl_conn,
    resp, resp_nlines);
  if (res < 0) {
    xerrno = errno;

    pr_response_block(TRUE);
    errno = xerrno;
    return PR_ERROR(cmd);
  }

  return PR_HANDLED(cmd);
}

static int proxy_get_cmd_group(cmd_rec *cmd) {
  cmdtable *cmdtab;
  int idx;
  unsigned int h;

  idx = cmd->stash_index;
  h = cmd->stash_hash;

  cmdtab = pr_stash_get_symbol2(PR_SYM_CMD, cmd->argv[0], NULL, &idx, &h);
  while (cmdtab != NULL) {
    pr_signals_handle();

    if (cmdtab->group == NULL ||
        cmdtab->cmd_type != CMD) {
      cmdtab = pr_stash_get_symbol2(PR_SYM_CMD, cmd->argv[0], cmdtab, &idx, &h);
      continue;
    }

    cmd->group = pstrdup(cmd->pool, cmdtab->group);
    return 0;
  }

  if (cmd->group == NULL) {
    errno = ENOENT;
    return -1;
  }

  return 0;
}

static int proxy_have_limit(cmd_rec *cmd, char **resp_code) {
  int res;

  /* Some commands get a free pass. */
  switch (cmd->cmd_id) {
    case PR_CMD_ACCT_ID:
    case PR_CMD_EPRT_ID:
    case PR_CMD_EPSV_ID:
    case PR_CMD_FEAT_ID:
    case PR_CMD_PASS_ID:
    case PR_CMD_PASV_ID:
    case PR_CMD_PORT_ID:
    case PR_CMD_QUIT_ID:
    case PR_CMD_SYST_ID:
    case PR_CMD_USER_ID:
      return 0;

    default:
      break;
  }

  /* Note: since we use a PRE_CMD ANY handler here, the core code does NOT
   * actually look up the specific records for this command.  This means that
   * that the command's command group may not be known.  But to honor any
   * group-based <Limit> sections, we need to look up the command group.
   */
  if (cmd->group == NULL) {
    if (proxy_get_cmd_group(cmd) < 0) {
      pr_trace_msg(trace_channel, 5,
        "error finding group for command '%s': %s", (char *) cmd->argv[0],
        strerror(errno));
    }
  }

  res = dir_check(cmd->tmp_pool, cmd, cmd->group, session.cwd, NULL);
  if (res == 0) {
    /* The appropriate response code depends on the command, unfortunately.
     * See RFC 959, Section 5.4 for the gory details.
     */
    switch (cmd->cmd_id) {
      case PR_CMD_ALLO_ID:
      case PR_CMD_MODE_ID:
      case PR_CMD_REST_ID:
      case PR_CMD_STRU_ID:
      case PR_CMD_TYPE_ID:
        *resp_code = R_501;
        break;

      default:
        *resp_code = R_550;
        break;
    }

    errno = EPERM;
    return -1;
  }

  return 0;
}

MODRET proxy_any(cmd_rec *cmd) {
  int block_responses = TRUE;
  struct proxy_session *proxy_sess;
  modret_t *mr = NULL;
  char *resp_code = R_550;

  if (proxy_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  /* Honor any <Limit> sections for this comand. */
  if (proxy_have_limit(cmd, &resp_code) < 0) {
    int xerrno = errno;

    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "%s denied by <Limit> configuration", (char *) cmd->argv[0]);
    pr_response_add_err(resp_code, "%s: %s", (char *) cmd->argv[0],
      strerror(xerrno));
    pr_cmd_set_errno(cmd, xerrno);
    errno = xerrno;
    return PR_ERROR(cmd);
  }

  proxy_sess = pr_table_get(session.notes, "mod_proxy.proxy-session", NULL);

  /* Backend servers can send "asynchronous" messages to us; we need to check
   * for them.
   */
  if (proxy_ftp_ctrl_handle_async(cmd->tmp_pool, proxy_sess->backend_ctrl_conn,
      proxy_sess->frontend_ctrl_conn) < 0) {
    int xerrno = errno;

    pr_trace_msg(trace_channel, 7,
      "error checking for async messages from the backend server: %s",
      strerror(xerrno));

    if (xerrno == ECONNRESET ||
        xerrno == ECONNABORTED ||
        xerrno == ENOENT ||
        xerrno == EPIPE) {
      pr_session_disconnect(&proxy_module,
        PR_SESS_DISCONNECT_BY_APPLICATION,
        "Backend control connection lost");
    }
  }

  pr_response_block(FALSE);
  pr_timer_reset(PR_TIMER_IDLE, ANY_MODULE);

  /* Commands related to logins and data transfers are handled separately. */

  switch (cmd->cmd_id) {
    case PR_CMD_USER_ID:
      mr = proxy_user(cmd, proxy_sess, &block_responses);
      if (block_responses) {
        pr_response_block(TRUE);
      }
      return mr;

    case PR_CMD_PASS_ID:
      mr = proxy_pass(cmd, proxy_sess, &block_responses);
      if (block_responses) {
        pr_response_block(TRUE);
      }
      return mr;

    case PR_CMD_EPRT_ID:
      if (proxy_sess_state & PROXY_SESS_STATE_CONNECTED) {
        mr = proxy_eprt(cmd, proxy_sess);
        pr_response_block(TRUE);
        return mr;
      }
      break;

    case PR_CMD_EPSV_ID:
      if (proxy_sess_state & PROXY_SESS_STATE_CONNECTED) {
        mr = proxy_epsv(cmd, proxy_sess);
        pr_response_block(TRUE);
        return mr;
      }
      break;

    case PR_CMD_PASV_ID:
      if (proxy_sess_state & PROXY_SESS_STATE_CONNECTED) {
        mr = proxy_pasv(cmd, proxy_sess);
        pr_response_block(TRUE);
        return mr;
      }
      break;

    case PR_CMD_PORT_ID:
      if (proxy_sess_state & PROXY_SESS_STATE_CONNECTED) {
        mr = proxy_port(cmd, proxy_sess);
        pr_response_block(TRUE);
        return mr;
      }
      break;

    case PR_CMD_TYPE_ID:
      if (proxy_sess_state & PROXY_SESS_STATE_CONNECTED) {
        /* Used for setting the ASCII/binary session flag properly, e.g. for
         * TransferLogs.
         */
        mr = proxy_type(cmd, proxy_sess);
        pr_response_block(TRUE);
        return mr;
      }
      break;

    case PR_CMD_LIST_ID:
    case PR_CMD_MLSD_ID:
    case PR_CMD_NLST_ID:
      if (proxy_sess_state & PROXY_SESS_STATE_CONNECTED) {
        session.xfer.p = make_sub_pool(session.pool);
        session.xfer.direction = PR_NETIO_IO_WR;
        mr = proxy_data(proxy_sess, cmd);
        destroy_pool(session.xfer.p);
        memset(&session.xfer, 0, sizeof(session.xfer));

        pr_response_block(TRUE);
        return mr;

      } else {
        pr_response_send(R_530, _("Access denied"));
        return PR_ERROR(cmd);
      }
      break;

    case PR_CMD_APPE_ID:
    case PR_CMD_RETR_ID:
    case PR_CMD_STOR_ID:
    case PR_CMD_STOU_ID:
      if (proxy_sess_state & PROXY_SESS_STATE_CONNECTED) {
        /* In addition to the same setup as for directory listings, we also
         * track more things, for supporting e.g. TransferLog.
         */
        memset(&session.xfer, 0, sizeof(session.xfer));

        if (pr_cmd_cmp(cmd, PR_CMD_RETR_ID) == 0) {
          session.xfer.direction = PR_NETIO_IO_RD;
        } else {
          session.xfer.direction = PR_NETIO_IO_WR;
        }

        session.xfer.p = make_sub_pool(session.pool);
        gettimeofday(&session.xfer.start_time, NULL);

        mr = proxy_data(proxy_sess, cmd);

        proxy_log_xfer(cmd, 'c');
        destroy_pool(session.xfer.p);
        memset(&session.xfer, 0, sizeof(session.xfer));

        pr_response_block(TRUE);
        return mr;

      } else {
        pr_response_send(R_530, _("Access denied"));
        return PR_ERROR(cmd);
      }
      break;

    case PR_CMD_FEAT_ID:
      if (proxy_role == PROXY_ROLE_REVERSE) {
        /* In reverse proxy mode, we do not want to necessarily leak the
         * capabilities of the selected backend server to the client.  Or
         * do we?
         */
        if (proxy_sess_state & PROXY_SESS_STATE_CONNECTED) {
          if (proxy_opts & PROXY_OPT_SHOW_FEATURES) {
            mr = proxy_feat(cmd, proxy_sess);
            return mr;
          }
        }

        return PR_DECLINED(cmd);

      } else {
        mr = proxy_feat(cmd, proxy_sess);
        return mr;      
      }
      break;

    case PR_CMD_HOST_ID:
      /* TODO is there any value in handling the HOST command locally?
       * Answer: yes!  Consider the reverse proxy case + mod_autohost!
       * Thus we DO want to return DECLINED here, BUT we ALSO need to implement
       * the event listener for resetting the forward/reverse (but not tls)
       * APIs.
       */
      return PR_DECLINED(cmd);

    /* Directory changing commands not allowed locally. */
    case PR_CMD_CDUP_ID:
    case PR_CMD_CWD_ID:
    case PR_CMD_MKD_ID:
    case PR_CMD_PWD_ID:
    case PR_CMD_RMD_ID:
    case PR_CMD_XCUP_ID:
    case PR_CMD_XCWD_ID:
    case PR_CMD_XMKD_ID:
    case PR_CMD_XPWD_ID:
    case PR_CMD_XRMD_ID:
      if ((proxy_sess_state & PROXY_SESS_STATE_PROXY_AUTHENTICATED) &&
          !(proxy_sess_state & PROXY_SESS_STATE_CONNECTED)) {
        pr_response_send(R_530, _("Access denied"));
        return PR_ERROR(cmd);
      }
      break;
 
    /* RFC 2228 commands */
    case PR_CMD_ADAT_ID:
    case PR_CMD_AUTH_ID:
    case PR_CMD_CCC_ID:
    case PR_CMD_CONF_ID:
    case PR_CMD_ENC_ID:
    case PR_CMD_MIC_ID:
    case PR_CMD_PBSZ_ID:
    case PR_CMD_PROT_ID:
      return PR_DECLINED(cmd);
  }

  /* If we are not connected to a backend server, then don't try to proxy
   * the command.
   */
  if (!(proxy_sess_state & PROXY_SESS_STATE_CONNECTED)) {
    return PR_DECLINED(cmd);
  }

  if (pr_cmd_cmp(cmd, PR_CMD_QUIT_ID) != 0) {
    /* If we have connected to a backend server, but we have NOT authenticated
     * to that backend server, then reject all commands as "out of sequence"
     * errors (i.e. malicious or misinformed clients).
     */
    if (!(proxy_sess_state & PROXY_SESS_STATE_BACKEND_AUTHENTICATED)) {
      pr_response_add_err(R_530, _("Please login with USER and PASS"));
      return PR_ERROR(cmd);
    }
  }

  return proxy_cmd(cmd, proxy_sess, NULL);
}

/* Event handlers
 */

static void proxy_ctrl_read_ev(const void *event_data, void *user_data) {
  switch (proxy_role) {
    case PROXY_ROLE_REVERSE:
      if (proxy_sess_state & PROXY_SESS_STATE_CONNECTED) {
        proxy_restrict_session();
        pr_event_unregister(&proxy_module, "mod_proxy.ctrl-read",
          proxy_ctrl_read_ev);
      }
      break;

    case PROXY_ROLE_FORWARD:
      /* We don't really need this event listener for forward proxying. */
      pr_event_unregister(&proxy_module, "mod_proxy.ctrl-read",
        proxy_ctrl_read_ev);
      break;
  }
}

static void proxy_exit_ev(const void *event_data, void *user_data) {
  struct proxy_session *proxy_sess;

  proxy_sess = pr_table_get(session.notes, "mod_proxy.proxy-session", NULL);
  if (proxy_sess != NULL) {
    if (proxy_sess->frontend_ctrl_conn != NULL) {
      pr_inet_close(proxy_sess->pool, proxy_sess->frontend_ctrl_conn);
      proxy_sess->frontend_ctrl_conn = NULL;
    }

    if (proxy_sess->frontend_data_conn != NULL) {
      /* Note: if session.d is NULL, ASSUME that the core API's session
       * cleanup already closed that connection, and so doing it here
       * would be redundant (and worse, an attempted double-free);
       * the frontend_data_conn and session.d are the same connection.
       */
      if (session.d != NULL) {
        pr_inet_close(proxy_sess->pool, proxy_sess->frontend_data_conn);
        proxy_sess->frontend_data_conn = session.d = NULL;
      }
    }

    if (proxy_sess->backend_ctrl_conn != NULL) {
      proxy_inet_close(proxy_sess->pool, proxy_sess->backend_ctrl_conn);
      proxy_sess->backend_ctrl_conn = NULL;
    }

    if (proxy_sess->backend_data_conn != NULL) {
      proxy_inet_close(proxy_sess->pool, proxy_sess->backend_data_conn);
      proxy_sess->backend_data_conn = NULL;
    }

    pr_table_remove(session.notes, "mod_proxy.proxy-session", NULL);
  }

  switch (proxy_role) {
    case PROXY_ROLE_REVERSE:
      proxy_reverse_sess_exit(session.pool);
      break;

    case PROXY_ROLE_FORWARD:
      break;
  }

  if (proxy_logfd >= 0) {
    (void) close(proxy_logfd);
    proxy_logfd = -1;
  }
}

#if defined(PR_SHARED_MODULE)
static void proxy_mod_unload_ev(const void *event_data, void *user_data) {
  if (strncmp((const char *) event_data, "mod_proxy.c", 12) == 0) {
    /* Unregister ourselves from all events. */
    pr_event_unregister(&proxy_module, NULL, NULL);

    PRIVS_ROOT
    (void) proxy_rmpath(proxy_pool, proxy_tables_dir);
    PRIVS_RELINQUISH

    destroy_pool(proxy_pool);
    proxy_pool = NULL;

    (void) close(proxy_logfd);
    proxy_logfd = -1;
  }
}
#endif

static void proxy_postparse_ev(const void *event_data, void *user_data) {
  int engine = FALSE;
  config_rec *c;

  c = find_config(main_server->conf, CONF_PARAM, "ProxyEngine", FALSE);
  if (c) {
    engine = *((int *) c->argv[0]);
  }

  if (engine == FALSE) {
    return;
  }

  c = find_config(main_server->conf, CONF_PARAM, "ProxyTables", FALSE);
  if (c == NULL) {
    /* No ProxyTables configured, mod_proxy cannot run. */
    pr_log_pri(PR_LOG_WARNING, MOD_PROXY_VERSION
      ": missing required ProxyTables directive, failing to start up");

    pr_session_disconnect(&proxy_module, PR_SESS_DISCONNECT_BAD_CONFIG,
      "Missing required config");
  }

  proxy_tables_dir = c->argv[0];

  if (proxy_forward_init(proxy_pool, proxy_tables_dir) < 0) {
    pr_log_pri(PR_LOG_WARNING, MOD_PROXY_VERSION
      ": unable to initialize forward proxy, failing to start up: %s",
      strerror(errno));

    pr_session_disconnect(&proxy_module, PR_SESS_DISCONNECT_BAD_CONFIG,
      "Failed forward proxy initialization");
  }

  if (proxy_reverse_init(proxy_pool, proxy_tables_dir) < 0) {
    pr_log_pri(PR_LOG_WARNING, MOD_PROXY_VERSION
      ": unable to initialize reverse proxy, failing to start up: %s",
      strerror(errno));

    pr_session_disconnect(&proxy_module, PR_SESS_DISCONNECT_BAD_CONFIG,
      "Failed reverse proxy initialization");
  }

  if (proxy_tls_init(proxy_pool, proxy_tables_dir) < 0) {
    pr_log_pri(PR_LOG_WARNING, MOD_PROXY_VERSION
      ": unable to initialize TLS support, failing to start up: %s",
      strerror(errno));

    pr_session_disconnect(&proxy_module, PR_SESS_DISCONNECT_BAD_CONFIG,
      "Failed TLS initialization");
  }
}

static void proxy_restart_ev(const void *event_data, void *user_data) {
  int res;

  proxy_forward_free(proxy_pool);
  proxy_reverse_free(proxy_pool);
  proxy_tls_free(proxy_pool);

  res = proxy_db_close(proxy_pool);
  if (res < 0) {
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "error closing database: %s", strerror(errno));
  }
}

static void proxy_shutdown_ev(const void *event_data, void *user_data) {
  proxy_forward_free(proxy_pool);
  proxy_reverse_free(proxy_pool);
  proxy_db_free();

  PRIVS_ROOT
  (void) proxy_rmpath(proxy_pool, proxy_tables_dir);
  PRIVS_RELINQUISH

  destroy_pool(proxy_pool);
  proxy_pool = NULL;

  if (proxy_logfd >= 0) {
    (void) close(proxy_logfd);
    proxy_logfd = -1;
  }
}

static void proxy_timeoutidle_ev(const void *event_data, void *user_data) {
  /* Unblock responses here, so that mod_core's response will be flushed
   * out to the frontend client.
   */
  pr_response_block(FALSE);
}

static void proxy_timeoutnoxfer_ev(const void *event_data, void *user_data) {
  /* Unblock responses here, so that mod_xfer's response will be flushed
   * out to the frontend client.
   */
  pr_response_block(FALSE);
}

static void proxy_timeoutstalled_ev(const void *event_data, void *user_data) {
  /* Unblock responses here, so that mod_xfer's response will be flushed
   * out to the frontend client.
   */
  pr_response_block(FALSE);
}

/* XXX Do we want to support any Controls/ftpctl actions? */

/* Initialization routines
 */

static int proxy_init(void) {

  proxy_pool = make_sub_pool(permanent_pool);
  pr_pool_tag(proxy_pool, MOD_PROXY_VERSION);

#if defined(PR_SHARED_MODULE)
  pr_event_register(&proxy_module, "core.module-unload", proxy_mod_unload_ev,
    NULL);
#endif
  pr_event_register(&proxy_module, "core.postparse", proxy_postparse_ev, NULL);
  pr_event_register(&proxy_module, "core.restart", proxy_restart_ev, NULL);
  pr_event_register(&proxy_module, "core.shutdown", proxy_shutdown_ev, NULL);

  if (proxy_db_init(proxy_pool) < 0) {
    return -1;
  }

  return 0;
}

/* Set defaults for directives that mod_proxy should allow (but whose
 * values are checked e.g. by PRE_CMD handlers):
 *
 *  AllowOverwrite
 *  AllowStoreRestart
 *
 * Unless these directives have already been set, of course.
 */
static void proxy_set_sess_defaults(void) {
  config_rec *c;

  c = find_config(main_server->conf, CONF_PARAM, "AllowOverwrite", FALSE);
  if (c == NULL) {
    c = add_config_param_set(&main_server->conf, "AllowOverwrite", 1, NULL);
    c->argv[0] = palloc(c->pool, sizeof(unsigned char));
    *((unsigned char *) c->argv[0]) = TRUE;
    c->flags |= CF_MERGEDOWN;
  }

  c = find_config(main_server->conf, CONF_PARAM, "AllowStoreRestart", FALSE);
  if (c == NULL) {
    c = add_config_param_set(&main_server->conf, "AllowStoreRestart", 1, NULL);
    c->argv[0] = palloc(c->pool, sizeof(unsigned char));
    *((unsigned char *) c->argv[0]) = TRUE;
    c->flags |= CF_MERGEDOWN;
  }
}

static int proxy_sess_init(void) {
  config_rec *c;
  int res;
  struct proxy_session *proxy_sess;
  const char *sess_dir = NULL;

  c = find_config(main_server->conf, CONF_PARAM, "ProxyEngine", FALSE);
  if (c != NULL) {
    proxy_engine = *((int *) c->argv[0]);
  }

  if (proxy_engine == FALSE) {
    return 0;
  }

  pr_event_register(&proxy_module, "core.exit", proxy_exit_ev, NULL);
  pr_event_register(&proxy_module, "mod_proxy.ctrl-read", proxy_ctrl_read_ev,
    NULL);

  /* Install event handlers for timeouts, so that we can properly close
   * the connections on either side.
   */
  pr_event_register(&proxy_module, "core.timeout-idle",
    proxy_timeoutidle_ev, NULL);
  pr_event_register(&proxy_module, "core.timeout-no-transfer",
    proxy_timeoutnoxfer_ev, NULL);
  pr_event_register(&proxy_module, "core.timeout-stalled",
    proxy_timeoutstalled_ev, NULL);

  c = find_config(main_server->conf, CONF_PARAM, "ProxyLog", FALSE);
  if (c != NULL) {
    char *logname;

    logname = c->argv[0];

    if (strncasecmp(logname, "none", 5) != 0) {
      int xerrno;

      pr_signals_block();
      PRIVS_ROOT
      res = pr_log_openfile(logname, &proxy_logfd, PR_LOG_SYSTEM_MODE);
      xerrno = errno;
      PRIVS_RELINQUISH
      pr_signals_unblock();

      if (res < 0) {
        if (res == -1) {
          pr_log_pri(PR_LOG_NOTICE, MOD_PROXY_VERSION
            ": notice: unable to open ProxyLog '%s': %s", logname,
            strerror(xerrno));

        } else if (res == PR_LOG_WRITABLE_DIR) {
          pr_log_pri(PR_LOG_NOTICE, MOD_PROXY_VERSION
            ": notice: unable to open ProxyLog '%s': parent directory is "
            "world-writable", logname);

        } else if (res == PR_LOG_SYMLINK) {
          pr_log_pri(PR_LOG_NOTICE, MOD_PROXY_VERSION
            ": notice: unable to open ProxyLog '%s': cannot log to a symlink",
            logname);
        }
      }
    }
  }

  proxy_pool = make_sub_pool(session.pool);
  pr_pool_tag(proxy_pool, MOD_PROXY_VERSION);

  c = find_config(main_server->conf, CONF_PARAM, "ProxyOptions", FALSE);
  while (c != NULL) {
    unsigned long opts;

    pr_signals_handle();

    opts = *((unsigned long *) c->argv[0]);
    proxy_opts |= opts;

    c = find_config_next(c, c->next, CONF_PARAM, "ProxyOptions", FALSE);
  }

  c = find_config(main_server->conf, CONF_PARAM, "ProxyRole", FALSE);
  if (c != NULL) {
    proxy_role = *((int *) c->argv[0]);
  }

  proxy_random_init();

  /* Set defaults for directives that mod_proxy should allow. */
  proxy_set_sess_defaults();

  /* Allocate our own session structure, for tracking proxy-specific
   * fields.  Use the session.notes table for stashing/retrieving it as
   * needed.
   */
  proxy_sess = proxy_session_alloc(proxy_pool);
  if (pr_table_add(session.notes, "mod_proxy.proxy-session", proxy_sess,
      sizeof(struct proxy_session)) < 0) {
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "error stashing proxy session note: %s", strerror(errno));

    /* This is a fatal error; mod_proxy won't function without this note. */
    errno = EPERM;
    return -1;
  }

  c = find_config(main_server->conf, CONF_PARAM, "ProxySourceAddress", FALSE);
  if (c != NULL) {
    proxy_sess->src_addr = c->argv[0];
  }

  c = find_config(main_server->conf, CONF_PARAM, "ProxyDataTransferPolicy",
    FALSE);
  if (c != NULL) {
    proxy_sess->dataxfer_policy = *((int *) c->argv[0]);
  }

  c = find_config(main_server->conf, CONF_PARAM, "ProxyTimeoutConnect", FALSE);
  if (c != NULL) {
    proxy_sess->connect_timeout = *((int *) c->argv[0]);

  } else {
    proxy_sess->connect_timeout = PROXY_CONNECT_DEFAULT_TIMEOUT;
  }

  /* Every proxy session starts off in the ProxyTables/empty/ directory. */
  sess_dir = pdircat(proxy_pool, proxy_tables_dir, "empty", NULL);
  if (pr_fsio_chdir_canon(sess_dir, TRUE) < 0) {
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "error setting session directory to '%s': %s", sess_dir, strerror(errno));
  }

  if (proxy_tls_sess_init(proxy_pool) < 0) {
    pr_session_disconnect(&proxy_module, PR_SESS_DISCONNECT_BY_APPLICATION,
      "Unable to initialize TLS API");
  }

  switch (proxy_role) {
    case PROXY_ROLE_REVERSE:
      if (proxy_reverse_sess_init(proxy_pool, proxy_tables_dir,
          proxy_sess) < 0) {
        pr_session_disconnect(&proxy_module, PR_SESS_DISCONNECT_BY_APPLICATION,
          "Unable to initialize reverse proxy API");
      }

      set_auth_check(proxy_reverse_have_authenticated);
      break;

    case PROXY_ROLE_FORWARD:
      if (proxy_forward_sess_init(proxy_pool, proxy_tables_dir,
          proxy_sess) < 0) {
        pr_session_disconnect(&proxy_module, PR_SESS_DISCONNECT_BY_APPLICATION,
          "Unable to initialize forward proxy API");
      }

      /* XXX TODO:
       *   DisplayConnect
       */

      set_auth_check(proxy_forward_have_authenticated);
      break;
  }

  /* XXX set protocol?  What about ssh2 proxying?  How to interact
   * with mod_sftp, which doesn't have the same pipeline of request
   * handling? (Need to add PRE_REQ handling in mod_sftp, to support proxying.)
   *
   * pr_session_set_protocol("proxy"); ?
   *
   * If we do this, we should also add separate "proxy" rows to the DelayTable.
   */

  /* We have to use our own command event loop, since we will also need to
   * watch any data transfer connections with the backend server, in addition
   * to the client control connection.
   */
  pr_cmd_set_handler(proxy_cmd_loop);

  proxy_remove_symbols();
  return 0;
}

/* Module API tables
 */

static conftable proxy_conftab[] = {
  { "ProxyDataTransferPolicy",	set_proxydatatransferpolicy,	NULL },
  { "ProxyEngine",		set_proxyengine,		NULL },
  { "ProxyForwardEnabled",	set_proxyforwardenabled,	NULL },
  { "ProxyForwardMethod",	set_proxyforwardmethod,		NULL },
  { "ProxyForwardTo",		set_proxyforwardto,		NULL },
  { "ProxyLog",			set_proxylog,			NULL },
  { "ProxyOptions",		set_proxyoptions,		NULL },
  { "ProxyRetryCount",		set_proxyretrycount,		NULL },
  { "ProxyReverseConnectPolicy",set_proxyreverseconnectpolicy,	NULL },
  { "ProxyReverseServers",	set_proxyreverseservers,	NULL },
  { "ProxyRole",		set_proxyrole,			NULL },
  { "ProxySourceAddress",	set_proxysourceaddress,		NULL },
  { "ProxyTables",		set_proxytables,		NULL },
  { "ProxyTimeoutConnect",	set_proxytimeoutconnect,	NULL },

  /* TLS support */
  { "ProxyTLSCACertificateFile",set_proxytlscacertfile,		NULL },
  { "ProxyTLSCACertificatePath",set_proxytlscacertpath,		NULL },
  { "ProxyTLSCARevocationFile",	set_proxytlscacrlfile,		NULL },
  { "ProxyTLSCARevocationPath",	set_proxytlscacrlpath,		NULL },
  { "ProxyTLSCertificateFile",	set_proxytlscertfile,		NULL },
  { "ProxyTLSCertificateKeyFile",set_proxytlscertkeyfile,	NULL },
  { "ProxyTLSCipherSuite",	set_proxytlsciphersuite,	NULL },
  { "ProxyTLSEngine",		set_proxytlsengine,		NULL },
  { "ProxyTLSOptions",		set_proxytlsoptions,		NULL },
  { "ProxyTLSPreSharedKey",	set_proxytlspresharedkey,	NULL },
  { "ProxyTLSProtocol",		set_proxytlsprotocol,		NULL },
  { "ProxyTLSTimeoutHandshake",	set_proxytlstimeouthandshake,	NULL },
  { "ProxyTLSVerifyServer",	set_proxytlsverifyserver,	NULL },

  /* Support TransferPriority for proxied connections? */
  /* Deliberately ignore/disable HiddenStores in mod_proxy configs */
  /* Two timeouts, one for frontend and one for backend? */

  { NULL }
};

static cmdtable proxy_cmdtab[] = {
  /* XXX Should this be marked with a CL_ value, for logging? */
  { CMD,	C_ANY,	G_NONE,	proxy_any,	FALSE, FALSE },

  { 0, NULL }
};

module proxy_module = {
  /* Always NULL */
  NULL, NULL,

  /* Module API version */
  0x20,

  /* Module name */
  "proxy",

  /* Module configuration handler table */
  proxy_conftab,

  /* Module command handler table */
  proxy_cmdtab,

  /* Module authentication handler table */
  NULL,

  /* Module initialization */
  proxy_init,

  /* Session initialization */
  proxy_sess_init,

  /* Module version */
  MOD_PROXY_VERSION
};

