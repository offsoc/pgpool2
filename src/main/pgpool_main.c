/* -*-pgpool_main-c-*- */
/*
 * $Header$
 *
 * pgpool: a language independent connection pool server for PostgreSQL
 * written by Tatsuo Ishii
 *
 * Copyright (c) 2003-2023	PgPool Global Development Group
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that copyright notice and this permission
 * notice appear in supporting documentation, and that the name of the
 * author not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior
 * permission. The author makes no representations about the
 * suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/un.h>
#include <netdb.h>
#include <time.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>

#ifdef  __FreeBSD__
#include <netinet/in.h>
#endif

#include "pool.h"
#include "version.h"
#include "pool_config.h"
#include "main/health_check.h"
#include "main/pool_internal_comms.h"
#include "main/pgpool_logger.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/memutils.h"
#include "utils/statistics.h"
#include "utils/pool_ipc.h"
#include "context/pool_process_context.h"
#include "protocol/pool_process_query.h"
#include "protocol/pool_pg_utils.h"
#include "parser/pool_string.h"
#include "auth/pool_passwd.h"
#include "auth/pool_hba.h"
#include "query_cache/pool_memqcache.h"
#include "watchdog/wd_internal_commands.h"
#include "watchdog/wd_lifecheck.h"
#include "watchdog/watchdog.h"
#include "pcp/pcp_worker.h"


/*
 * Reasons for signalling a pgpool-II main process
 */
typedef enum
{
	SIG_FAILOVER_INTERRUPT,		/* signal main to start failover */
	SIG_WATCHDOG_STATE_CHANGED, /* notify main about local watchdog node state
								 * changed */
	SIG_BACKEND_SYNC_REQUIRED,	/* notify main about local backend state sync
								 * required */
	SIG_WATCHDOG_QUORUM_CHANGED,	/* notify main about cluster quorum change
									 * of watchdog cluster */
	SIG_INFORM_QURANTINE_NODES, /* notify main about send degenerate requests
								 * for all quarantine nodes */
	MAX_INTERUPTS				/* Must be last! */
}			User1SignalReason;


typedef struct User1SignalSlot
{
	sig_atomic_t signalFlags[MAX_INTERUPTS];
}			User1SignalSlot;

/*
 * Process pending signal actions.
 */
#define CHECK_REQUEST \
	do { \
		if (wakeup_request) \
		{ \
			wakeup_children(); \
			wakeup_request = 0; \
		} \
		if (sigusr1_request) \
		{ \
			do {\
				sigusr1_request = 0; \
				sigusr1_interupt_processor(); \
			} while (sigusr1_request == 1); \
		} \
		if (sigchld_request) \
		{ \
			reaper(); \
		} \
		if (reload_config_request) \
		{ \
			reload_config(); \
			reload_config_request = 0; \
		} \
    } while (0)

#define PGPOOLMAXLITSENQUEUELENGTH 10000

#define UNIXSOCK_PATH_BUFLEN sizeof(((struct sockaddr_un *) NULL)->sun_path)

static void signal_user1_to_parent_with_reason(User1SignalReason reason);

static void FileUnlink(int code, Datum path);
static pid_t pcp_fork_a_child(int unix_fd, int inet_fd, char *pcp_conf_file);
static pid_t fork_a_child(int *fds, int id);
static pid_t worker_fork_a_child(ProcessType type, void (*func) (), void *params);
static int	create_unix_domain_socket(struct sockaddr_un un_addr_tmp);
static int	create_inet_domain_socket(const char *hostname, const int port);
static int *create_inet_domain_sockets(const char *hostname, const int port);
static void failover(void);
static bool check_all_backend_down(void);
static void reaper(void);
static void wakeup_children(void);
static void reload_config(void);
static int	pool_pause(struct timeval *timeout);
static void kill_all_children(int sig);
static pid_t fork_follow_child(int old_main_node, int new_primary, int old_primary);
static int	read_status_file(bool discard_status);
static RETSIGTYPE exit_handler(int sig);
static RETSIGTYPE reap_handler(int sig);
static RETSIGTYPE sigusr1_handler(int sig);
static void sigusr1_interupt_processor(void);
static RETSIGTYPE reload_config_handler(int sig);
static RETSIGTYPE wakeup_handler(int sig);

static void initialize_shared_mem_objects(bool clear_memcache_oidmaps);
static int trigger_failover_command(int node, const char *command_line,
						 int old_main_node, int new_main_node, int old_primary);
static int	find_primary_node(void);
static int	find_primary_node_repeatedly(void);
static void terminate_all_childrens(int sig);
static void system_will_go_down(int code, Datum arg);
static char *process_name_from_pid(pid_t pid);
static void sync_backend_from_watchdog(void);
static void update_backend_quarantine_status(void);
static int	get_server_version(POOL_CONNECTION_POOL_SLOT * *slots, int node_id);
static void get_info_from_conninfo(char *conninfo, char *host, int hostlen, char *port, int portlen);

static struct sockaddr_un un_addr;	/* unix domain socket path */
static struct sockaddr_un pcp_un_addr;	/* unix domain socket path for PCP */
ProcessInfo *process_info = NULL;	/* Per child info table on shmem */
volatile	User1SignalSlot *user1SignalSlot = NULL;	/* User 1 signal slot on
														 * shmem */
struct timeval random_start_time;

/*
 * To track health check process ids
 */
static pid_t health_check_pids[MAX_NUM_BACKENDS];

/*
 * Private copy of backend status
 */
BACKEND_STATUS private_backend_status[MAX_NUM_BACKENDS];

/*
 * shmem connection info table
 * this is a three dimension array. i.e.:
 * con_info[pool_config->num_init_children][pool_config->max_pool][MAX_NUM_BACKENDS]
 */
ConnectionInfo *con_info;

static int *fds = NULL;				/* listening file descriptors (UNIX socket,
								 * inet domain sockets) */

static int	pcp_unix_fd;		/* unix domain socket fd for PCP (not used) */
static int	pcp_inet_fd;		/* inet domain socket fd for PCP */
extern char *pcp_conf_file;		/* path for pcp.conf */
extern char *conf_file;
extern char *hba_file;

static volatile sig_atomic_t exiting = 0;		/* non 0 if I'm exiting */
static volatile sig_atomic_t switching = 0;		/* non 0 if I'm failing over or degenerating */

POOL_REQUEST_INFO *Req_info;	/* request info area in shared memory */
volatile sig_atomic_t *InRecovery;	/* non 0 if recovery is started */
volatile sig_atomic_t reload_config_request = 0;
static volatile sig_atomic_t sigusr1_request = 0;
static volatile sig_atomic_t sigchld_request = 0;
static volatile sig_atomic_t wakeup_request = 0;

static int	pipe_fds[2];		/* for delivering signals */

int			my_proc_id;

static BackendStatusRecord backend_rec; /* Backend status record */

static pid_t worker_pid = 0;	/* pid of worker process */
static pid_t follow_pid = 0;	/* pid for child process handling follow
								 * command */
static pid_t pcp_pid = 0;		/* pid for child process handling PCP */
static pid_t watchdog_pid = 0;	/* pid for watchdog child process */
static pid_t pgpool_logger_pid = 0; /* pid for pgpool_logger process */
static pid_t wd_lifecheck_pid = 0;	/* pid for child process handling watchdog
									 * lifecheck */

BACKEND_STATUS *my_backend_status[MAX_NUM_BACKENDS];	/* Backend status buffer */
int			my_main_node_id;	/* Main node id buffer */

/*
 * Dummy varibale to suppress compiler warnings by discarding return values
 * from write(2) in signal handlers
 */
static int dummy_status;

/*
 * Snapshot Isolation manage area
 */
volatile SI_ManageInfo *si_manage_info;

/*
* pgpool main program
*/

int
PgpoolMain(bool discard_status, bool clear_memcache_oidmaps)
{
	int			i;
	char		unix_domain_socket_path[UNIXSOCK_PATH_BUFLEN + 1024];

	sigjmp_buf	local_sigjmp_buf;

	/*
	 * to prevent the variable set on a register so that longjmp() does not
	 * discard the content
	 */
	volatile bool first = true;

	/* For PostmasterRandom */
	gettimeofday(&random_start_time, NULL);

	processState = INITIALIZING;

	/*
	 * Restore previous backend status if possible
	 */
	read_status_file(discard_status);

	/* set unix domain socket path for connections to pgpool */
	memset(unix_domain_socket_path, 0, sizeof(unix_domain_socket_path));
	snprintf(unix_domain_socket_path, sizeof(unix_domain_socket_path), "%s/.s.PGSQL.%d",
			 pool_config->socket_dir,
			 pool_config->port);
	if (strlen(unix_domain_socket_path) >= UNIXSOCK_PATH_BUFLEN)
	{
		ereport(FATAL,
			   (errmsg("could not create Unix-domain sockets"),
				errdetail("Unix-domain socket path \"%s\" is too long (maximum %d bytes)",
				unix_domain_socket_path,
				(int) (UNIXSOCK_PATH_BUFLEN - 1))));
	}
	snprintf(un_addr.sun_path, sizeof(un_addr.sun_path), "%s",unix_domain_socket_path);

	/* set unix domain socket path for pgpool PCP communication */
	memset(unix_domain_socket_path, 0, sizeof(unix_domain_socket_path));
	snprintf(unix_domain_socket_path, sizeof(unix_domain_socket_path), "%s/.s.PGSQL.%d",
			 pool_config->pcp_socket_dir,
			 pool_config->pcp_port);
	if (strlen(unix_domain_socket_path) >= UNIXSOCK_PATH_BUFLEN)
	{
		ereport(FATAL,
			   (errmsg("could not create PCP Unix-domain sockets"),
				errdetail("PCP Unix-domain socket path \"%s\" is too long (maximum %d bytes)",
				unix_domain_socket_path,
				(int) (UNIXSOCK_PATH_BUFLEN - 1))));
	}
	snprintf(pcp_un_addr.sun_path, sizeof(pcp_un_addr.sun_path), "%s",unix_domain_socket_path);

	/* set up signal handlers */
	pool_signal(SIGPIPE, SIG_IGN);


	/* start the log collector if enabled */
	pgpool_logger_pid = SysLogger_Start();
    /*
     * If using syslogger, close the read side of the pipe.  We don't bother
     * tracking this in fd.c, either.
     */
	if (syslogPipe[0] >= 0)
		close(syslogPipe[0]);
	syslogPipe[0] = -1;

	initialize_shared_mem_objects(clear_memcache_oidmaps);

	/*
	 * install the callback for preparation of system exit
	 */
	on_system_exit(system_will_go_down, (Datum) NULL);

	/* setup signal handlers */
	pool_signal(SIGCHLD, reap_handler);
	pool_signal(SIGUSR1, sigusr1_handler);
	pool_signal(SIGUSR2, wakeup_handler);
	pool_signal(SIGTERM, exit_handler);
	pool_signal(SIGINT, exit_handler);
	pool_signal(SIGQUIT, exit_handler);
	pool_signal(SIGHUP, reload_config_handler);

	if (pool_config->use_watchdog)
	{
		sigset_t	mask;

		wakeup_request = 0;

		/*
		 * Watchdog process fires SIGUSR2 once in stable state
		 * In addition, when wathcodg fails to start with FATAL, the process
		 * exits and SIGCHLD is fired, so we can also expect SIGCHLD from
		 * watchdog process. Finally, we also need to look for the SIGUSR1
		 * signla for the failover requests from other watchdog nodes. In
		 * case a request arrives at the same time when the watchdog has just
		 * been initialized.
		 *
		 * So we need to wait until watchdog is in stable state so only
		 * wait for SIGUSR1, SIGCHLD, and signals those are necessary to make
		 * sure we respond to user requests of shutdown if it arrives while we
		 * are in waiting state.
		 *
		 * Note that SIGUSR1 does not need to be in the wait signal list,
		 * although it's signal handler is already installed, but even if the
		 * SIGUSR1 arrives while watchdog is initializing we will continue
		 * with our normal initialization and will process the failover
		 * request once our backend status will be synchronized across the
		 * cluster
		 */
		sigfillset(&mask);
		sigdelset(&mask, SIGUSR1);
		sigdelset(&mask, SIGCHLD);
		sigdelset(&mask, SIGTERM);
		sigdelset(&mask, SIGINT);
		sigdelset(&mask, SIGQUIT);
		sigdelset(&mask, SIGHUP);

		watchdog_pid = initialize_watchdog();
		ereport(LOG,
				(errmsg("waiting for watchdog to initialize")));
		while (sigusr1_request == 0 && sigchld_request == 0)
		{
			sigsuspend(&mask);
		}
		wakeup_request = 0;

		/* watchdog process fails to start */
		if (sigchld_request)
		{
			reaper();
		}

		ereport(LOG,
				(errmsg("watchdog process is initialized"),
				 errdetail("watchdog messaging data version: %s",WD_MESSAGE_DATA_VERSION)));

		/*
		 * initialize the lifecheck process
		 */
		wd_lifecheck_pid = initialize_watchdog_lifecheck();

		if (sigusr1_request)
		{
			do {
				sigusr1_request = 0;
				sigusr1_interupt_processor();
			} while (sigusr1_request == 1);
		}
	}

	/* create unix domain socket */
	fds = malloc(sizeof(int) * 2);
	if (fds == NULL)
		ereport(FATAL,
				(errmsg("failed to allocate memory in startup process")));

	fds[0] = create_unix_domain_socket(un_addr);
	fds[1] = -1;
	on_proc_exit(FileUnlink, (Datum) un_addr.sun_path);

	/* create inet domain socket if any */
	if (pool_config->listen_addresses[0])
	{
		int		   *inet_fds,
				   *walk;
		int			n = 1;

		inet_fds = create_inet_domain_sockets(pool_config->listen_addresses, pool_config->port);

		for (walk = inet_fds; *walk != -1; walk++)
			n++;

		fds = realloc(fds, sizeof(int) * (n + 1));
		if (fds == NULL)
			ereport(FATAL,
					(errmsg("failed to allocate memory in startup process")));

		n = 1;
		for (walk = inet_fds; *walk != -1; walk++)
		{
			fds[n] = inet_fds[n - 1];
			n++;
		}
		fds[n] = -1;
		free(inet_fds);
	}


	/*
	 * We need to block signal here. Otherwise child might send some signals,
	 * for example SIGUSR1(fail over).  Children will inherit signal blocking
	 * but they do unblock signals at the very beginning of process.  So this
	 * is harmless.
	 */
	POOL_SETMASK(&BlockSig);
	/* fork the children */
	for (i = 0; i < pool_config->num_init_children; i++)
	{
		process_info[i].pid = fork_a_child(fds, i);
		process_info[i].start_time = time(NULL);
	}

	/* create pipe for delivering event */
	if (pipe(pipe_fds) < 0)
	{
		ereport(FATAL,
				(errmsg("failed to create pipe")));
	}

	MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * if the primary node id is not loaded by watchdog, search for it
	 */
	if (Req_info->primary_node_id < 0)
	{
		/* Save primary node id */
		Req_info->primary_node_id = find_primary_node_repeatedly();
	}

	/* fork a child for PCP handling */
	pcp_unix_fd = create_unix_domain_socket(pcp_un_addr);
	/* Add onproc exit to clean up the unix domain socket at exit */
	on_proc_exit(FileUnlink, (Datum) pcp_un_addr.sun_path);

	if (pool_config->pcp_listen_addresses[0])
	{
		pcp_inet_fd = create_inet_domain_socket(pool_config->pcp_listen_addresses, pool_config->pcp_port);
	}
	pcp_pid = pcp_fork_a_child(pcp_unix_fd, pcp_inet_fd, pcp_conf_file);

	/* Fork worker process */
	worker_pid = worker_fork_a_child(PT_WORKER, do_worker_child, NULL);

	/* Fork health check process */
	for (i = 0; i < NUM_BACKENDS; i++)
	{
		if (VALID_BACKEND(i))
			health_check_pids[i] = worker_fork_a_child(PT_HEALTH_CHECK, do_health_check_child, &i);
	}

	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;
		EmitErrorReport();
		MemoryContextSwitchTo(TopMemoryContext);
		FlushErrorState();
		POOL_SETMASK(&BlockSig);

	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/* Create or write status file */
	(void) write_status_file();

	/* This is the main loop */
	for (;;)
	{
		CHECK_REQUEST;

		/*
		 * check for child signals to ensure child startup before reporting
		 * successfull start.
		 */
		if (first)
		{
			int			i;
			int			n;
			POOL_NODE_STATUS *node_status = pool_get_node_status();

			ereport(LOG,
					(errmsg("%s successfully started. version %s (%s)", PACKAGE, VERSION, PGPOOLVERSION)));

			/*
			 * Very early stage node checking. It is assumed that
			 * find_primary_node got called.
			 */
			if (STREAM)
			{
				for (i = 0; i < NUM_BACKENDS; i++)
				{
					ereport(LOG,
							(errmsg("node status[%d]: %d", i, node_status[i])));

					if (node_status[i] == POOL_NODE_STATUS_INVALID)
					{
						ereport(LOG,
								(errmsg("pgpool_main: invalid node found %d", i)));
						if (pool_config->detach_false_primary)
						{
							n = i;
							degenerate_backend_set(&n, 1, REQ_DETAIL_SWITCHOVER | REQ_DETAIL_CONFIRMED);
						}
					}
				}
			}
		}
		first = false;

		processState = SLEEPING;
		for (;;)
		{
			int			r;
			struct timeval t = {3, 0};

			POOL_SETMASK(&UnBlockSig);
			r = pool_pause(&t);
			POOL_SETMASK(&BlockSig);
			if (r > 0)
				break;
		}
	}
}

/*
 * register_node_operation_request()
 *
 * This function enqueues the failover/failback requests, and fires the failover() if the function
 * is not already executing
 */
bool
register_node_operation_request(POOL_REQUEST_KIND kind, int *node_id_set, int count, unsigned char flags)
{
	bool		failover_in_progress;
	pool_sigset_t oldmask;
	int			index;

	/*
	 * if the queue is already full what to do?
	 */
	if ((Req_info->request_queue_tail - MAX_REQUEST_QUEUE_SIZE) == Req_info->request_queue_head)
	{
		return false;
	}
	POOL_SETMASK2(&BlockSig, &oldmask);
	pool_semaphore_lock(REQUEST_INFO_SEM);

	if ((Req_info->request_queue_tail - MAX_REQUEST_QUEUE_SIZE) == Req_info->request_queue_head)
	{
		pool_semaphore_unlock(REQUEST_INFO_SEM);
		POOL_SETMASK(&oldmask);
		return false;
	}
	Req_info->request_queue_tail++;
	index = Req_info->request_queue_tail % MAX_REQUEST_QUEUE_SIZE;
	Req_info->request[index].kind = kind;

	Req_info->request[index].request_details = flags;

	if (count > 0)
		memcpy(Req_info->request[index].node_id, node_id_set, (sizeof(int) * count));
	Req_info->request[index].count = count;
	failover_in_progress = Req_info->switching;
	pool_semaphore_unlock(REQUEST_INFO_SEM);

	POOL_SETMASK(&oldmask);
	if (failover_in_progress == false)
	{
		if (processType == PT_MAIN)
			failover();
		else
			signal_user1_to_parent_with_reason(SIG_FAILOVER_INTERRUPT);
	}

	return true;
}

void
register_watchdog_quorum_change_interupt(void)
{
	signal_user1_to_parent_with_reason(SIG_WATCHDOG_QUORUM_CHANGED);
}

void
register_watchdog_state_change_interupt(void)
{
	signal_user1_to_parent_with_reason(SIG_WATCHDOG_STATE_CHANGED);
}
void
register_backend_state_sync_req_interupt(void)
{
	signal_user1_to_parent_with_reason(SIG_BACKEND_SYNC_REQUIRED);
}
void
register_inform_quarantine_nodes_req(void)
{
	signal_user1_to_parent_with_reason(SIG_INFORM_QURANTINE_NODES);
}

static void
signal_user1_to_parent_with_reason(User1SignalReason reason)
{
	ereport(LOG,
			(errmsg("signal_user1_to_parent_with_reason(%d)", reason)));

	user1SignalSlot->signalFlags[reason] = true;
	pool_signal_parent(SIGUSR1);
}

/*
 * fork a child for PCP
 */
static pid_t
pcp_fork_a_child(int unix_fd, int inet_fd, char *pcp_conf_file)
{
	pid_t		pid;

	pid = fork();

	if (pid == 0)
	{
		on_exit_reset();
		SetProcessGlobalVariables(PT_PCP);

		close(pipe_fds[0]);
		close(pipe_fds[1]);

		/* call PCP child main */
		POOL_SETMASK(&UnBlockSig);
		health_check_timer_expired = 0;
		reload_config_request = 0;
		pcp_main(unix_fd, inet_fd);
	}
	else if (pid == -1)
	{
		ereport(FATAL,
				(errmsg("fork() failed. reason: %m")));
	}

	return pid;
}

/*
* fork a child
*/
static pid_t
fork_a_child(int *fds, int id)
{
	pid_t		pid;

	pid = fork();

	if (pid == 0)
	{
		on_exit_reset();

		/*
		 * Before we unconditionally closed pipe_fds[0] and pipe_fds[1] here,
		 * which is apparently wrong since in the start up of pgpool, pipe(2)
		 * is not called yet and it mistakenly closes fd 0. Now we check the
		 * fd > 0 before close(), expecting pipe returns fds greater than 0.
		 * Note that we cannot unconditionally remove close(2) calls since
		 * fork_a_child() may be called *after* pgpool starting up.
		 */
		if (pipe_fds[0] > 0)
		{
			close(pipe_fds[0]);
			close(pipe_fds[1]);
		}

		SetProcessGlobalVariables(PT_CHILD);

		/* call child main */
		POOL_SETMASK(&UnBlockSig);
		health_check_timer_expired = 0;
		reload_config_request = 0;
		my_proc_id = id;
		do_child(fds);
	}
	else if (pid == -1)
	{
		ereport(FATAL,
				(errmsg("failed to fork a child"),
				 errdetail("system call fork() failed with reason: %m")));
	}

	return pid;
}

/*
* fork worker child process
*/
static pid_t
worker_fork_a_child(ProcessType type, void (*func) (), void *params)
{
	pid_t		pid;

	pid = fork();

	if (pid == 0)
	{
		on_exit_reset();

		/*
		 * Before we unconditionally closed pipe_fds[0] and pipe_fds[1] here,
		 * which is apparently wrong since in the start up of pgpool, pipe(2)
		 * is not called yet and it mistakenly closes fd 0. Now we check the
		 * fd > 0 before close(), expecting pipe returns fds greater than 0.
		 * Note that we cannot unconditionally remove close(2) calls since
		 * fork_a_child() may be called *after* pgpool starting up.
		 */
		if (pipe_fds[0] > 0)
		{
			close(pipe_fds[0]);
			close(pipe_fds[1]);
		}

		SetProcessGlobalVariables(type);

		ereport(LOG,
				(errmsg("process started")));

		/* call child main */
		POOL_SETMASK(&UnBlockSig);
		health_check_timer_expired = 0;
		reload_config_request = 0;
		func(params);
	}
	else if (pid == -1)
	{
		ereport(FATAL,
				(errmsg("failed to fork a child"),
				 errdetail("system call fork() failed with reason: %m")));
	}

	return pid;
}

static int *
create_inet_domain_sockets(const char *hostname, const int port)
{
	int			ret;
	int			fd;
	int			one = 1;
	int			status;
	int			backlog;
	int			n = 0;
	int		   *sockfds;
	char	   *portstr;
	struct addrinfo *walk;
	struct addrinfo *res;
	struct addrinfo hints;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	/*
	 * getaddrinfo() requires a string because it also accepts service names,
	 * such as "http".
	 */
	if (asprintf(&portstr, "%d", port) == -1)
	{
		ereport(FATAL,
				(errmsg("failed to create INET domain socket"),
				 errdetail("asprintf() failed: %m")));
	}

	if ((ret = getaddrinfo((!hostname || strcmp(hostname, "*") == 0) ? NULL : hostname, portstr, &hints, &res)) != 0)
	{
		ereport(FATAL,
				(errmsg("failed to create INET domain socket"),
				 errdetail("getaddrinfo() failed: %s", gai_strerror(ret))));
	}

	free(portstr);

	for (walk = res; walk != NULL; walk = walk->ai_next)
		n++;

	sockfds = malloc(sizeof(int) * (n + 1));
	n = 0;
	for (walk = res; walk != NULL; walk = walk->ai_next)
		sockfds[n++] = -1;
	/* We always terminate the list of sockets with a -1 entry */
	sockfds[n] = -1;

	n = 0;

	for (walk = res; walk != NULL; walk = walk->ai_next)
	{
		char		buf[INET6_ADDRSTRLEN + 1];

		memset(buf, 0, sizeof(buf));
		if ((ret = getnameinfo((struct sockaddr *) walk->ai_addr, walk->ai_addrlen,
							   buf, sizeof(buf), NULL, 0, NI_NUMERICHOST)) != 0)
		{
			ereport(FATAL,
					(errmsg("failed to create INET domain socket"),
					 errdetail("getnameinfo() failed: \"%s\"", gai_strerror(ret))));
		}

		ereport(LOG,
				(errmsg("Setting up socket for %s:%d", buf, port)));

		if ((fd = socket(walk->ai_family, walk->ai_socktype, walk->ai_protocol)) == -1)
		{
			/*
			 * A single failure is not necessarily a problem (machines without
			 * proper dual stack setups), but if we cannot create any socket
			 * at all, we report a FATAL error.
			 */
			ereport(LOG,
					(errmsg("perhaps failed to create INET domain socket"),
					 errdetail("socket(%s) failed: \"%m\"", buf)));
			continue;
		}

		if ((setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &one,
						sizeof(one))) == -1)
		{
			ereport(FATAL,
					(errmsg("failed to create INET domain socket"),
					 errdetail("socket error \"%m\"")));
		}

		if (walk->ai_family == AF_INET6)
		{
			/*
			 * On some machines, depending on the default value in
			 * /proc/sys/net/ipv6/bindv6only, sockets will listen on both IPv6
			 * and IPv4 at the same time. Since we are creating one socket per
			 * address family, disable that option specifically to be sure it
			 * is off.
			 */
			if ((setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one))) == -1)
			{
				ereport(LOG,
						(errmsg("perhaps failed to create INET domain socket"),
						 errdetail("setsockopt(%s, IPV6_V6ONLY) failed: \"%m\"", buf)));
			}
		}

		if (bind(fd, walk->ai_addr, walk->ai_addrlen) != 0)
		{
			ereport(FATAL,
					(errmsg("failed to create INET domain socket"),
					 errdetail("bind on socket failed with error \"%m\"")));
		}

		backlog = pool_config->num_init_children * pool_config->listen_backlog_multiplier;

		if (backlog > PGPOOLMAXLITSENQUEUELENGTH)
			backlog = PGPOOLMAXLITSENQUEUELENGTH;

		status = listen(fd, backlog);
		if (status < 0)
			ereport(FATAL,
					(errmsg("failed to create INET domain socket"),
					 errdetail("listen on socket failed with error \"%m\"")));

		sockfds[n++] = fd;
	}

	freeaddrinfo(res);

	if (n == 0)
	{
		ereport(FATAL,
				(errmsg("failed to create INET domain socket"),
				 errdetail("Failed to create any sockets. See the earlier LOG messages.")));
	}

	return sockfds;
}

/*
* create inet domain socket
*/
static int
create_inet_domain_socket(const char *hostname, const int port)
{
	struct sockaddr_in addr;
	int			fd;
	int			status;
	int			one = 1;
	int			len;
	int			backlog;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1)
	{
		ereport(FATAL,
				(errmsg("failed to create INET domain socket"),
				 errdetail("%m")));
	}
	if ((setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &one,
					sizeof(one))) == -1)
	{
		ereport(FATAL,
				(errmsg("failed to create INET domain socket"),
				 errdetail("%m")));
	}

	memset((char *) &addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;

	if (strcmp(hostname, "*") == 0)
	{
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
	}
	else
	{
		struct hostent *hostinfo;

		hostinfo = gethostbyname(hostname);
		if (!hostinfo)
		{
			ereport(FATAL,
					(errmsg("failed to create INET domain socket"),
					 errdetail("could not resolve hostname \"%s\": error \"%s\"", hostname, hstrerror(h_errno))));

		}
		addr.sin_addr = *(struct in_addr *) hostinfo->h_addr;
	}

	addr.sin_port = htons(port);
	len = sizeof(struct sockaddr_in);

	status = bind(fd, (struct sockaddr *) &addr, len);
	if (status == -1)
	{
		int			saved_errno = errno;
		char		host[NI_MAXHOST],
					servname[NI_MAXSERV];

		if ((status = getnameinfo((struct sockaddr *) &addr, len, host, sizeof(host), servname, sizeof(servname), 0)))
		{
			ereport(NOTICE,
					(errmsg("getnameinfo failed while creating INET domain socket"),
					 errdetail("getnameinfo failed with reason: \"%s\"", gai_strerror(status))));

			snprintf(servname, sizeof(servname), "%d", port);
			snprintf(host, sizeof(host), "%s", hostname);
		}
		ereport(FATAL,
				(errmsg("failed to create INET domain socket"),
				 errdetail("bind on host:\"%s\" server:\"%s\" failed with error \"%s\"", host, servname, strerror(saved_errno))));
	}

	backlog = pool_config->num_init_children * pool_config->listen_backlog_multiplier;

	if (backlog > PGPOOLMAXLITSENQUEUELENGTH)
		backlog = PGPOOLMAXLITSENQUEUELENGTH;

	status = listen(fd, backlog);
	if (status < 0)
		ereport(FATAL,
				(errmsg("failed to create INET domain socket"),
				 errdetail("listen on socket failed with error \"%m\"")));

	return fd;
}

/*
* create UNIX domain socket
*/
static int
create_unix_domain_socket(struct sockaddr_un un_addr_tmp)
{
	struct sockaddr_un addr;
	int			fd;
	int			status;
	int			len;

	/* Delete any pre-existing socket file to avoid failure at bind() time */
	unlink(un_addr_tmp.sun_path);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1)
	{
		ereport(FATAL,
				(errmsg("failed to create a socket"),
				 errdetail("Failed to create UNIX domain socket. error: \"%m\"")));
	}
	memset((char *) &addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", un_addr_tmp.sun_path);
	len = sizeof(struct sockaddr_un);
	status = bind(fd, (struct sockaddr *) &addr, len);
	if (status == -1)
	{
		ereport(FATAL,
				(errmsg("failed to bind a socket: \"%s\"", un_addr_tmp.sun_path),
				 errdetail("bind socket failed with error: \"%m\"")));
	}

	if (chmod(un_addr_tmp.sun_path, 0777) == -1)
	{
		ereport(FATAL,
				(errmsg("failed to bind a socket: \"%s\"", un_addr_tmp.sun_path),
				 errdetail("system call chmod failed with error: \"%m\"")));
	}

	status = listen(fd, PGPOOLMAXLITSENQUEUELENGTH);
	if (status < 0)
	{
		ereport(FATAL,
				(errmsg("failed to bind a socket: \"%s\"", un_addr_tmp.sun_path),
				 errdetail("system call listen() failed with error: \"%m\"")));
	}
	return fd;
}

/*
 * sends the kill signal to all Pgpool children except to
 * the pgpool_logger child
 * wait for the termination of all killed children before returning.
 */
static void
terminate_all_childrens(int sig)
{
	pid_t		wpid;
	int			i;
	int			killed_count = 0;
	int			terminated_count = 0;
	/*
	 * This is supposed to be called from main process
	 */
	if (processType != PT_MAIN)
		return;

	if (sig != SIGTERM && sig != SIGINT && sig != SIGQUIT)
	{
		ereport(LOG,
				(errmsg("invalid terminate signal: \"%d\"", sig),
				 errdetail("ignoring")));

		return;
	}

	for (i = 0; i < pool_config->num_init_children; i++)
	{
		pid_t		pid = process_info[i].pid;

		if (pid != 0)
		{
			kill(pid, sig);
			process_info[i].pid = 0;
			killed_count++;
		}
	}

	if (pcp_pid != 0)
	{
		kill(pcp_pid, sig);
		killed_count++;
	}
	pcp_pid = 0;

	if (worker_pid != 0)
	{
		kill(worker_pid, sig);
		killed_count++;
	}
	worker_pid = 0;

	if (pool_config->use_watchdog)
	{
		if (pool_config->use_watchdog)
		{
			if (watchdog_pid)
			{
				kill(watchdog_pid, sig);
				killed_count++;
			}
			watchdog_pid = 0;

			if (wd_lifecheck_pid)
			{
				kill(wd_lifecheck_pid, sig);
				killed_count++;
			}
			wd_lifecheck_pid = 0;
		}
	}

	for (i = 0 ; i < MAX_NUM_BACKENDS; i++)
	{
		if (health_check_pids[i] != 0)
		{
			kill(health_check_pids[i], sig);
			health_check_pids[i] = 0;
			killed_count++;
		}
	}
	/* wait for all killed children to exit */
	do
	{
		int			ret_pid;

		wpid = waitpid(-1, &ret_pid, 0);
		if (wpid > 0)
			terminated_count++;
	} while (terminated_count < killed_count &&
			 (wpid > 0 || (wpid == -1 && errno == EINTR)));

	if (wpid == -1 && errno != ECHILD)
		ereport(LOG,
				(errmsg("wait() failed"),
				 errdetail("%m")));

}


/*
 * Pgpool main process exit handler
 */
static RETSIGTYPE exit_handler(int sig)
{
	int		   *walk;
	int			save_errno = errno;

	POOL_SETMASK(&AuthBlockSig);

	/*
	 * this could happen in a child process if a signal has been sent before
	 * resetting signal handler
	 */
	if (getpid() != mypid)
	{
		POOL_SETMASK(&UnBlockSig);
		proc_exit(0);
	}

	if (sig != SIGTERM && sig != SIGINT && sig != SIGQUIT)
	{
		POOL_SETMASK(&UnBlockSig);
		errno = save_errno;
		return;
	}

	/*
	 * Check if another exit handler instance is already running.  It is
	 * possible that exit_handler is interrupted in the middle by other
	 * signal.
	 */
	if (exiting)
	{
		ereport(LOG,
				(errmsg("exit handler (signal: %d) called. but exit handler is already in progress", sig)));
		POOL_SETMASK(&UnBlockSig);
		errno = save_errno;
		return;
	}

	/* Check to make sure that other exit handler is not running */
	pool_semaphore_lock(MAIN_EXIT_HANDLER_SEM);
	if (exiting == 0)
	{
		exiting = 1;
		pool_semaphore_unlock(MAIN_EXIT_HANDLER_SEM);
	}
	else
	{
		pool_semaphore_unlock(MAIN_EXIT_HANDLER_SEM);
		ereport(LOG,
				(errmsg("exit handler (signal: %d) called. but exit handler is already in progress", sig)));
		POOL_SETMASK(&UnBlockSig);
		errno = save_errno;
		return;
	}

	processState = EXITING;
	ereport(LOG,
			(errmsg("shutting down by signal %d", sig)));

	/* Close listen socket if they are already initialized */
	if (fds)
	{
		for (walk = fds; *walk != -1; walk++)
			close(*walk);
	}

	ereport(LOG,
			(errmsg("terminating all child processes")));
	terminate_all_childrens(sig);

	/*
	 * Send signal to follow child process and it's children.
	 */
	if (follow_pid > 0)
	{
		ereport(LOG, 
				(errmsg("terminating all child processes of follow child")));
		kill(follow_pid, sig);
		switch (sig)
		{
			case SIGINT:
			case SIGTERM:
			case SIGQUIT:
			case SIGSTOP:
			case SIGKILL:
				if (kill(-follow_pid, sig) < 0)
					elog(LOG, "kill(%ld,%d) failed: %m", (long) (-follow_pid), sig);
				break;
			default:
				break;
		}
	}

	POOL_SETMASK(&UnBlockSig);
	ereport(LOG,
			(errmsg("Pgpool-II system is shutdown")));
	process_info = NULL;
	exit(0);
}

/*
 * Calculate next valid main node id.
 * If no valid node found, returns -1.
 */
int
get_next_main_node(void)
{
	int			i;

	for (i = 0; i < pool_config->backend_desc->num_backends; i++)
	{
		/*
		 * Do not use VALID_BACKEND macro in raw mode. VALID_BACKEND return
		 * true only if the argument is main node id. In other words,
		 * standby nodes are false. So need to check backend status with
		 * VALID_BACKEND_RAW.
		 */
		if (RAW_MODE)
		{
			if (VALID_BACKEND_RAW(i))
				break;
		}
		else
		{
			if (VALID_BACKEND(i))
				break;
		}
	}

	if (i == pool_config->backend_desc->num_backends)
		i = -1;

	return i;
}

/*
 * handle SIGUSR1
 *
 */
static RETSIGTYPE sigusr1_handler(int sig)
{
	int			save_errno = errno;

	POOL_SETMASK(&BlockSig);
	sigusr1_request = 1;

	dummy_status = write(pipe_fds[1], "\0", 1);

#ifdef NOT_USED
	if (write(pipe_fds[1], "\0", 1) < 0)
		ereport(WARNING,
				(errmsg("SIGUSR1 handler: write to pipe failed"),
						errdetail("%m")));
#endif

	POOL_SETMASK(&UnBlockSig);

	errno = save_errno;
}


static void
sigusr1_interupt_processor(void)
{
	ereport(LOG,
			(errmsg("Pgpool-II parent process received SIGUSR1")));

	if (user1SignalSlot->signalFlags[SIG_WATCHDOG_QUORUM_CHANGED])
	{
		ereport(LOG,
				(errmsg("Pgpool-II parent process received watchdog quorum change signal from watchdog")));

		user1SignalSlot->signalFlags[SIG_WATCHDOG_QUORUM_CHANGED] = false;
		if (wd_internal_get_watchdog_quorum_state() >= 0)
		{
			ereport(LOG,
					(errmsg("watchdog cluster now holds the quorum"),
					 errdetail("updating the state of quarantine backend nodes")));
			update_backend_quarantine_status();
		}
	}

	if (user1SignalSlot->signalFlags[SIG_INFORM_QURANTINE_NODES])
	{
		ereport(LOG,
				(errmsg("Pgpool-II parent process received inform quarantine nodes signal from watchdog")));

		user1SignalSlot->signalFlags[SIG_INFORM_QURANTINE_NODES] = false;
		degenerate_all_quarantine_nodes();
	}

	if (user1SignalSlot->signalFlags[SIG_BACKEND_SYNC_REQUIRED])
	{
		ereport(LOG,
				(errmsg("Pgpool-II parent process received sync backend signal from watchdog")));

		user1SignalSlot->signalFlags[SIG_BACKEND_SYNC_REQUIRED] = false;
		if (wd_internal_get_watchdog_local_node_state() == WD_STANDBY)
		{
			ereport(LOG,
					(errmsg("leader watchdog has performed failover"),
					 errdetail("syncing the backend states from the LEADER watchdog node")));
			sync_backend_from_watchdog();
		}
	}

	if (user1SignalSlot->signalFlags[SIG_WATCHDOG_STATE_CHANGED])
	{
		ereport(LOG,
				(errmsg("Pgpool-II parent process received watchdog state change signal from watchdog")));

		user1SignalSlot->signalFlags[SIG_WATCHDOG_STATE_CHANGED] = false;
		if (wd_internal_get_watchdog_local_node_state() == WD_STANDBY)
		{
			ereport(LOG,
					(errmsg("we have joined the watchdog cluster as STANDBY node"),
					 errdetail("syncing the backend states from the LEADER watchdog node")));
			sync_backend_from_watchdog();
			/*
			 * we also want to release the follow_primary lock if it was held
			 * by the remote node.
			 * because the change of watchdog coordinator would lead to forever stuck
			 * in the the locked state
			 */
			pool_release_follow_primary_lock(true);
		}
	}
	if (user1SignalSlot->signalFlags[SIG_FAILOVER_INTERRUPT])
	{
		ereport(LOG,
				(errmsg("Pgpool-II parent process has received failover request")));
		user1SignalSlot->signalFlags[SIG_FAILOVER_INTERRUPT] = false;
		if (processState == INITIALIZING)
		{
			ereport(LOG,
					(errmsg("ignoring the failover request, since we are still starting up")));
		}
		else
		{
			failover();
		}
	}
}

/* returns true if all backends are down */
static bool
check_all_backend_down(void)
{
	int			i;

	/* Check to see if all backends are down */
	for (i = 0; i < NUM_BACKENDS; i++)
	{
		if (BACKEND_INFO(i).backend_status != CON_DOWN &&
			BACKEND_INFO(i).backend_status != CON_UNUSED)
		{
			ereport(LOG,
					(errmsg("Node %d is not down (status: %d)",
							i, BACKEND_INFO(i).backend_status)));
			return false;
		}
	}
	return true;
}

/*
 * backend connection error, failover/failback request, if possible
 * failover() must be called under protecting signals.
 */
static void
failover(void)
{
	int			i,
				j,
				k;
	int			node_id;
	int			new_main_node;
	int			new_primary = -1;
	int			nodes[MAX_NUM_BACKENDS];
	bool		need_to_restart_children = true;
	bool		partial_restart = false;
	int			status;
	int			sts;
	bool		need_to_restart_pcp = false;
	bool		all_backend_down = true;
	bool		sync_required = false;

	ereport(DEBUG1,
			(errmsg("failover handler called")));

	memset(nodes, 0, sizeof(int) * MAX_NUM_BACKENDS);

	/*
	 * this could happen in a child process if a signal has been sent before
	 * resetting signal handler
	 */
	if (getpid() != mypid)
	{
		ereport(DEBUG1,
				(errmsg("failover handler called"),
				 errdetail("I am not parent")));
		kill(pcp_pid, SIGUSR2);
		return;
	}

	/*
	 * processing SIGTERM, SIGINT or SIGQUIT
	 */
	if (exiting)
	{
		ereport(DEBUG1,
				(errmsg("failover handler called while exiting")));
		kill(pcp_pid, SIGUSR2);
		return;
	}

	/*
	 * processing fail over or switch over
	 */
	if (switching)
	{
		ereport(DEBUG1,
				(errmsg("failover handler called while switching")));
		kill(pcp_pid, SIGUSR2);
		return;
	}

	Req_info->switching = true;
	switching = 1;
	for (;;)
	{
		POOL_REQUEST_KIND reqkind;
		int			queue_index;
		int			node_id_set[MAX_NUM_BACKENDS];
		int			node_count;
		unsigned char request_details;
		bool		search_primary = true;

		pool_semaphore_lock(REQUEST_INFO_SEM);

		if (Req_info->request_queue_tail == Req_info->request_queue_head)	/* request queue is
																			 * empty */
		{
			switching = 0;
			Req_info->switching = false;
			pool_semaphore_unlock(REQUEST_INFO_SEM);
			break;
		}

		/* make a local copy of request */
		Req_info->request_queue_head++;
		queue_index = Req_info->request_queue_head % MAX_REQUEST_QUEUE_SIZE;
		memcpy(node_id_set, Req_info->request[queue_index].node_id, (sizeof(int) * Req_info->request[queue_index].count));
		reqkind = Req_info->request[queue_index].kind;
		request_details = Req_info->request[queue_index].request_details;
		node_count = Req_info->request[queue_index].count;
		pool_semaphore_unlock(REQUEST_INFO_SEM);

		ereport(DEBUG1,
				(errmsg("failover handler"),
				 errdetail("kind: %d flags: %x node_count: %d index:%d", reqkind, request_details, node_count, queue_index)));

		if (reqkind == CLOSE_IDLE_REQUEST)
		{
			kill_all_children(SIGUSR1);
			continue;
		}

		/* inform all remote watchdog nodes that we are starting the failover */
		wd_failover_start();

		/*
		 * if not in replication mode/native replication mode, we treat this a
		 * restart request. otherwise we need to check if we have already
		 * failovered.
		 */
		ereport(DEBUG1,
				(errmsg("failover handler"),
				 errdetail("starting to select new main node")));
		node_id = node_id_set[0];

		/* failback request? */
		if (reqkind == NODE_UP_REQUEST)
		{
			if (node_id < 0 || node_id >= MAX_NUM_BACKENDS ||
				(reqkind == NODE_UP_REQUEST && !(RAW_MODE &&
												 BACKEND_INFO(node_id).backend_status == CON_DOWN) && VALID_BACKEND(node_id)) ||
				(reqkind == NODE_DOWN_REQUEST && !VALID_BACKEND(node_id)))
			{
				if (node_id < 0 || node_id >= MAX_NUM_BACKENDS)
					ereport(LOG,
							(errmsg("invalid failback request, node id: %d is invalid. node id must be between [0 and %d]", node_id, MAX_NUM_BACKENDS)));
				else
					ereport(LOG,
							(errmsg("invalid failback request, status: [%d] of node id : %d is invalid for failback", BACKEND_INFO(node_id).backend_status, node_id)));

				continue;
			}

			ereport(LOG,
					(errmsg("starting fail back. reconnect host %s(%d)",
							BACKEND_INFO(node_id).backend_hostname,
							BACKEND_INFO(node_id).backend_port)));

			/* Check to see if all backends are down */
			all_backend_down = check_all_backend_down();

			BACKEND_INFO(node_id).backend_status = CON_CONNECT_WAIT;	/* unset down status */
			pool_set_backend_status_changed_time(node_id);

			if ((request_details & REQ_DETAIL_UPDATE))
			{
				/* remove the quarantine flag */
				BACKEND_INFO(node_id).quarantine = false;

				/*
				 * do not search for primary node when handling the quarantine
				 * nodes
				 */
				search_primary = false;

				/*
				 * recalculate the main node id after setting the backend
				 * status of quarantined node, this will bring us to the old
				 * main_node_id that was beofre the quarantine state
				 */
				Req_info->main_node_id = get_next_main_node();
				if (Req_info->primary_node_id == -1 &&
					BACKEND_INFO(node_id).role == ROLE_PRIMARY)
				{
					/*
					 * if the failback request is for the quarantined node and
					 * that node had a primary role before it was quarantined,
					 * restore the primary node status for that node. this is
					 * important for the failover script to get the proper
					 * value of old primary
					 */
					ereport(LOG,
							(errmsg("failover: failing back the quarantine node that was primary before it was quarantined"),
							 errdetail("all children needs a restart")));
					Req_info->primary_node_id = node_id;

					/*
					 * since we changed the primary node so restart of all
					 * children is required
					 */
					need_to_restart_children = true;
					partial_restart = false;
				}
				else if (all_backend_down == false)
				{
					ereport(LOG,
							(errmsg("Do not restart children because we are failing back node id %d host: %s port: %d and we are in streaming replication mode and not all backends were down", node_id,
									BACKEND_INFO(node_id).backend_hostname,
									BACKEND_INFO(node_id).backend_port)));
					need_to_restart_children = false;
					partial_restart = false;
				}
				else
				{
					need_to_restart_children = true;
					partial_restart = false;
				}
			}
			else
			{
				/*
				 * The request is a proper failbak request and not because of
				 * the update status of quarantined node
				 */
				(void) write_status_file();

				trigger_failover_command(node_id, pool_config->failback_command,
										 MAIN_NODE_ID, get_next_main_node(), PRIMARY_NODE_ID);
			}

			sync_required = true;
		}
		else if (reqkind == PROMOTE_NODE_REQUEST)
		{
			if (node_id != -1 && VALID_BACKEND(node_id))
			{
				ereport(LOG,
						(errmsg("starting promotion. promote host %s(%d)",
								BACKEND_INFO(node_id).backend_hostname,
								BACKEND_INFO(node_id).backend_port)));
			}
			else
			{
				ereport(LOG,
						(errmsg("failover: no backends are promoted")));
				continue;
			}
		}
		else					/* NODE_DOWN_REQUEST &&
								 * NODE_QUARANTINE_REQUEST */
		{
			int			cnt = 0;

			for (i = 0; i < node_count; i++)
			{
				if (node_id_set[i] != -1 && (BACKEND_INFO(node_id_set[i]).quarantine == true ||
											 ((RAW_MODE && VALID_BACKEND_RAW(node_id_set[i])) ||
											  VALID_BACKEND(node_id_set[i]))))
				{
					ereport(LOG,
							(errmsg("starting %s. shutdown host %s(%d)",
									(reqkind == NODE_QUARANTINE_REQUEST) ? "quarantine" : "degeneration",
									BACKEND_INFO(node_id_set[i]).backend_hostname,
									BACKEND_INFO(node_id_set[i]).backend_port)));

					BACKEND_INFO(node_id_set[i]).backend_status = CON_DOWN; /* set down status */
					pool_set_backend_status_changed_time(node_id_set[i]);
					if (reqkind == NODE_QUARANTINE_REQUEST)
					{
						BACKEND_INFO(node_id_set[i]).quarantine = true;
					}
					else
					{
						/*
						 * if the degeneration request is for the quarantined
						 * node and that node had a primary role before it was
						 * quarantined, Restore the primary node status for
						 * that node before degenerating it. This is important
						 * for the failover script to get the proper valueo of
						 * old primary
						 */
						if (Req_info->primary_node_id == -1 &&
							BACKEND_INFO(node_id_set[i]).quarantine == true &&
							BACKEND_INFO(node_id_set[i]).role == ROLE_PRIMARY)
						{
							ereport(DEBUG2,
									(errmsg("failover: degenerating the node that was primary node before it was quarantined")));
							Req_info->primary_node_id = node_id_set[i];
							search_primary = false;
						}
						BACKEND_INFO(node_id_set[i]).quarantine = false;
						(void) write_status_file();
					}

					/* save down node */
					nodes[node_id_set[i]] = 1;
					cnt++;
				}
			}

			if (cnt == 0)
			{
				ereport(LOG,
						(errmsg("failover: no backends are degenerated")));
				continue;
			}
		}

		new_main_node = get_next_main_node();

		if (new_main_node < 0)
		{
			ereport(LOG,
					(errmsg("failover: no valid backend node found")));
		}

		ereport(DEBUG1,
				(errmsg("failover/failback request details: STREAM: %d reqkind: %d detail: %x node_id: %d",
						STREAM, reqkind, request_details & REQ_DETAIL_SWITCHOVER,
						node_id)));

		/*
		 * On 2011/5/2 Tatsuo Ishii says: if mode is streaming replication and
		 * request is NODE_UP_REQUEST (failback case) we don't need to restart
		 * all children. Existing session will not use newly attached node,
		 * but load balanced node is not changed until this session ends, so
		 * it's harmless anyway.
		 */

		/*
		 * On 2015/9/21 Tatsuo Ishii says: this judgment is not sufficient if
		 * all backends were down. Child process has local status in which all
		 * backends are down. In this case even if new connection arrives from
		 * frontend, the child will not accept it because the local status
		 * shows all backends are down. For this purpose we refer to
		 * "all_backend_down" variable, which was set before updating backend
		 * status.
		 *
		 * See bug 248 for more details.
		 */

		/*
		 * We also need to think about a case when the former primary node did
		 * not exist.  In the case we need to restart all children as
		 * well. For example when previous primary node id is 0 and then it
		 * went down, restarted, re-attached without promotion. Then existing
		 * child process loses connection slot to node 0 and keeps on using it
		 * when node 0 comes back. This could result in segfault later on in
		 * the child process because there's no connection to node id 0.
		 *
		 * Actually we need to think about when ALWAYS_PRIMARY flag is set
		 * *but* DISALLOW_TO_FAILOVER flag is not set case. In the case after
		 * primary failover Req_info->primary_node_id is set, but connection
		 * to the primary node does not exist. So we should do full restart if
		 * requested node id is the former primary node.
		 *
		 * See bug 672 for more details.
		 */
		if (STREAM && reqkind == NODE_UP_REQUEST && all_backend_down == false &&
			Req_info->primary_node_id >= 0 && Req_info->primary_node_id != node_id)
		{
			/*
			 * The decision to restart/no-restart children for update status
			 * request has already been made
			 */
			if (!(request_details & REQ_DETAIL_UPDATE))
			{
				ereport(LOG,
						(errmsg("Do not restart children because we are failing back node id %d host: %s port: %d and we are in streaming replication mode and not all backends were down", node_id,
								BACKEND_INFO(node_id).backend_hostname,
								BACKEND_INFO(node_id).backend_port)));

				need_to_restart_children = false;
				partial_restart = false;
			}
		}

		/*
		 * If the mode is streaming replication and the request is
		 * NODE_DOWN_REQUEST and it's actually a switch over request, we don't
		 * need to restart all children, except the node is primary.
		 */
		else if (STREAM && (reqkind == NODE_DOWN_REQUEST || reqkind == NODE_QUARANTINE_REQUEST) &&
				 request_details & REQ_DETAIL_SWITCHOVER && node_id != PRIMARY_NODE_ID)
		{
			ereport(LOG,
					(errmsg("Do not restart children because we are switching over node id %d host: %s port: %d and we are in streaming replication mode", node_id,
							BACKEND_INFO(node_id).backend_hostname,
							BACKEND_INFO(node_id).backend_port)));

			need_to_restart_children = true;
			partial_restart = true;

			for (i = 0; i < pool_config->num_init_children; i++)
			{
				bool		restart = false;

				for (j = 0; j < pool_config->max_pool; j++)
				{
					for (k = 0; k < NUM_BACKENDS; k++)
					{
						ConnectionInfo *con = pool_coninfo(i, j, k);

						if (con->connected && con->load_balancing_node == node_id)
						{
							ereport(LOG,
									(errmsg("child pid %d needs to restart because pool %d uses backend %d",
											process_info[i].pid, j, node_id)));
							restart = true;
							break;
						}
					}
				}

				if (restart)
				{
					pid_t		pid = process_info[i].pid;

					if (pid)
					{
						kill(pid, SIGQUIT);
						ereport(DEBUG1,
								(errmsg("failover handler"),
								 errdetail("kill process with PID:%d", pid)));
					}
				}
			}
		}
		else
		{
			ereport(LOG,
					(errmsg("Restart all children")));

			/* kill all children */
			for (i = 0; i < pool_config->num_init_children; i++)
			{
				pid_t		pid = process_info[i].pid;

				if (pid)
				{
					kill(pid, SIGQUIT);
					ereport(DEBUG1,
							(errmsg("failover handler"),
							 errdetail("kill process with PID:%d", pid)));
				}
			}

			need_to_restart_children = true;
			partial_restart = false;
		}

		/*
		 * Exec failover_command if needed We do not execute failover when
		 * request is quarantine type
		 */
		if (reqkind == NODE_DOWN_REQUEST)
		{
			for (i = 0; i < pool_config->backend_desc->num_backends; i++)
			{
				if (nodes[i])
				{
					trigger_failover_command(i, pool_config->failover_command,
											 MAIN_NODE_ID, new_main_node, REAL_PRIMARY_NODE_ID);
					sync_required = true;
				}
			}
		}

		if (reqkind == PROMOTE_NODE_REQUEST && VALID_BACKEND(node_id))
		{
			new_primary = node_id;
		}
		else if (reqkind == NODE_QUARANTINE_REQUEST)
		{
			/*
			 * if the quarantine node was the primary node set the newprimary
			 * to -1 (invalid)
			 */
			if (Req_info->primary_node_id == node_id)
			{
				/*
				 * set the role of the node, This will help us restore the
				 * primary node id when the node will come out from quarantine
				 * state
				 */
				BACKEND_INFO(node_id).role = ROLE_PRIMARY;
				new_primary = -1;
			}
			else if (SL_MODE)
			{
				new_primary = Req_info->primary_node_id;
			}
		}

		/*
		 * If the down node was a standby node in streaming replication mode,
		 * we can avoid calling find_primary_node_repeatedly() and recognize
		 * the former primary as the new primary node, which will reduce the
		 * time to process standby down.
		 * This does not apply to the case when no primary node existed
		 * (Req_info->primary_node_id < 0). In this case
		 * find_primary_node_repeatedly() should be called.
		 */
		else if (SL_MODE &&
				 reqkind == NODE_DOWN_REQUEST)
		{
			if (Req_info->primary_node_id >= 0 && Req_info->primary_node_id != node_id)
			{
				new_primary = Req_info->primary_node_id;
			}
			else
			{
				if (Req_info->primary_node_id >= 0)
					BACKEND_INFO(Req_info->primary_node_id).role = ROLE_STANDBY;
				new_primary = find_primary_node_repeatedly();
			}
		}
		else if (search_primary == false)
		{
			ereport(DEBUG1,
					(errmsg("faliover was called on quarantined node. No need to search for primary node")));
			new_primary = Req_info->primary_node_id;
		}
		else
		{
			new_primary = find_primary_node_repeatedly();
		}

		/*
		 * If follow_primary_command is provided and in streaming
		 * replication mode, we start degenerating all backends as they are
		 * not replicated anymore.
		 */
		int			follow_cnt = 0;

		if (STREAM)
		{
			if (*pool_config->follow_primary_command != '\0' ||
				reqkind == PROMOTE_NODE_REQUEST)
			{
				/*
				 * follow primary command is executed in following cases:
				 * - failover against the current primary
				 * - no primary exists and new primary is created by failover
				 * - promote node request
				 */
				if (((reqkind == NODE_DOWN_REQUEST) &&
					 Req_info->primary_node_id >= 0 &&
					 (nodes[Req_info->primary_node_id])) ||
					(reqkind == NODE_DOWN_REQUEST && Req_info->primary_node_id < 0 && new_primary >= 0) ||
					(node_id >= 0 && (reqkind == PROMOTE_NODE_REQUEST) &&
					 (VALID_BACKEND(node_id))))
				{

					for (i = 0; i < pool_config->backend_desc->num_backends; i++)
					{
						/* do not degenerate the new primary */
						if ((new_primary >= 0) && (i != new_primary))
						{
							BackendInfo *bkinfo;

							bkinfo = pool_get_node_info(i);
							ereport(LOG,
									(errmsg("starting follow degeneration. shutdown host %s(%d)",
											bkinfo->backend_hostname,
											bkinfo->backend_port)));
							bkinfo->backend_status = CON_DOWN;	/* set down status */
							pool_set_backend_status_changed_time(i);
							(void) write_status_file();

							follow_cnt++;
						}
					}

					if (follow_cnt == 0)
					{
						ereport(LOG,
								(errmsg("failover: no follow backends are degenerated")));
					}
					else
					{
						/* update new primary node */
						new_main_node = get_next_main_node();
						ereport(LOG,
								(errmsg("failover: %d follow backends have been degenerated", follow_cnt)));
					}
				}
			}
		}

		if ((follow_cnt > 0) && (*pool_config->follow_primary_command != '\0'))
		{
			follow_pid = fork_follow_child(Req_info->primary_node_id, new_primary,
										   Req_info->primary_node_id);
		}

		/* Save primary node id */
		if (Req_info->primary_node_id != new_primary)
		{
			if (Req_info->primary_node_id >= 0)
			{
				pool_set_backend_status_changed_time(Req_info->primary_node_id);
			}
			if (new_primary >= 0)
			{
				BACKEND_INFO(new_primary).role = ROLE_PRIMARY;
				pool_set_backend_status_changed_time(new_primary);
			}
		}
		Req_info->primary_node_id = new_primary;
		ereport(LOG,
				(errmsg("failover: set new primary node: %d", Req_info->primary_node_id)));

		if (new_main_node >= 0)
		{
			Req_info->main_node_id = new_main_node;
			sync_required = true;
			ereport(LOG,
					(errmsg("failover: set new main node: %d", Req_info->main_node_id)));
		}


		/* Kill children and restart them if needed */
		if (need_to_restart_children)
		{
			for (i = 0; i < pool_config->num_init_children; i++)
			{
				/*
				 * Try to kill pgpool child because previous kill signal may
				 * not be received by pgpool child. This could happen if
				 * multiple PostgreSQL are going down (or even starting
				 * pgpool, without starting PostgreSQL can trigger this).
				 * Child calls degenerate_backend() and it tries to aquire
				 * semaphore to write a failover request. In this case the
				 * signal mask is set as well, thus signals are never
				 * received.
				 */

				bool		restart = false;

				if (partial_restart)
				{
					for (j = 0; j < pool_config->max_pool; j++)
					{
						for (k = 0; k < NUM_BACKENDS; k++)
						{
							ConnectionInfo *con = pool_coninfo(i, j, k);

							if (con->connected && con->load_balancing_node == node_id)
							{

								ereport(LOG,
										(errmsg("child pid %d needs to restart because pool %d uses backend %d",
												process_info[i].pid, j, node_id)));
								restart = true;
								break;
							}
						}
					}
				}
				else
					restart = true;

				if (restart)
				{
					if (process_info[i].pid)
					{
						kill(process_info[i].pid, SIGQUIT);

						process_info[i].pid = fork_a_child(fds, i);
						process_info[i].start_time = time(NULL);
					}
				}
				else
					process_info[i].need_to_restart = 1;
			}
		}

		else
		{
			/*
			 * Set restart request to each child. Children will exit(1)
			 * whenever they are convenient.
			 */
			for (i = 0; i < pool_config->num_init_children; i++)
			{
				process_info[i].need_to_restart = 1;
			}
		}

		/*
		 * Send restart request to worker child.
		 */
		kill(worker_pid, SIGUSR1);

		if (sync_required)
			wd_failover_end();

		if (reqkind == NODE_UP_REQUEST)
		{
			ereport(LOG,
					(errmsg("failback done. reconnect host %s(%d)",
							BACKEND_INFO(node_id).backend_hostname,
							BACKEND_INFO(node_id).backend_port)));

			/* Fork health check process if needed */
			for (i = 0; i < NUM_BACKENDS; i++)
			{
				if (health_check_pids[i] == 0)
				{
					ereport(LOG,
							(errmsg("start health check process for host %s(%d)",
									BACKEND_INFO(i).backend_hostname,
									BACKEND_INFO(i).backend_port)));

					health_check_pids[i] = worker_fork_a_child(PT_HEALTH_CHECK, do_health_check_child, &i);
				}
			}
		}
		else if (reqkind == PROMOTE_NODE_REQUEST)
		{
			ereport(LOG,
					(errmsg("promotion done. promoted host %s(%d)",
							BACKEND_INFO(node_id).backend_hostname,
							BACKEND_INFO(node_id).backend_port)));
		}
		else
		{
			/*
			 * Temporary black magic. Without this regression 055 does not
			 * finish
			 */
			fprintf(stderr, "%s done. shutdown host %s(%d)",
					(reqkind == NODE_DOWN_REQUEST) ? "failover" : "quarantine",
					BACKEND_INFO(node_id).backend_hostname,
					BACKEND_INFO(node_id).backend_port);

			ereport(LOG,
					(errmsg("%s done. shutdown host %s(%d)",
							(reqkind == NODE_DOWN_REQUEST) ? "failover" : "quarantine",
							BACKEND_INFO(node_id).backend_hostname,
							BACKEND_INFO(node_id).backend_port)));
		}
		need_to_restart_pcp = true;
	}

	pool_semaphore_lock(REQUEST_INFO_SEM);
	switching = 0;
	Req_info->switching = false;
	pool_semaphore_unlock(REQUEST_INFO_SEM);

	/*
	 * kick wakeup_handler in pcp_child to notice that failover/failback done
	 */
	kill(pcp_pid, SIGUSR2);

	if (need_to_restart_pcp)
	{
		sleep(1);

		/*
		 * Send restart request to pcp child.
		 */
		kill(pcp_pid, SIGUSR1);
		for (;;)
		{
			sts = waitpid(pcp_pid, &status, 0);
			if (sts != -1)
				break;

			if (errno == EINTR)
				continue;
			else
			{
				ereport(WARNING,
						(errmsg("failover: waitpid failed"),
						 errdetail("%m")));
				continue;
			}
		}
		if (WIFSIGNALED(status))
			ereport(LOG,
					(errmsg("PCP child %d exits with status %d by signal %d in failover()", pcp_pid, status, WTERMSIG(status))));
		else
			ereport(LOG,
					(errmsg("PCP child %d exits with status %d in failover()", pcp_pid, status)));

		pcp_pid = pcp_fork_a_child(pcp_unix_fd, pcp_inet_fd, pcp_conf_file);
		ereport(LOG,
				(errmsg("fork a new PCP child pid %d in failover()", pcp_pid)));
	}
}

#ifdef NOT_USED
/*
 * health check timer handler
 */
static RETSIGTYPE health_check_timer_handler(int sig)
{
	int			save_errno = errno;

	POOL_SETMASK(&BlockSig);
	health_check_timer_expired = 1;
	POOL_SETMASK(&UnBlockSig);
	errno = save_errno;
}

/*
 * do_health_check() performs the health check on all backend nodes.
 * The inout parameter health_check_node_id is the starting backend
 * node number for health check and when the function returns or
 * exits with an error health_check_node_id contains the value
 * of last backend node number on which health check was performed.
 *
 * Function returns false if all backend nodes are down and true if all
 * backend nodes are in healthy state
 */
static bool
do_health_check(bool use_template_db, volatile int *health_check_node_id)
{
	POOL_CONNECTION_POOL_SLOT *slot;
	BackendInfo *bkinfo;
	static char *dbname;
	int			i;
	bool		all_nodes_healthy = false;
	char	   *password = get_pgpool_config_user_password(pool_config->health_check_user,
														   pool_config->health_check_password);

	/* Do not execute health check during recovery */
	if (*InRecovery)
		return false;

	if (!strcmp(pool_config->health_check_database, ""))
		dbname = use_template_db ? "template1" : "postgres";
	else
		dbname = pool_config->health_check_database;

	ereport(DEBUG1,
			(errmsg("doing health check against database:%s user:%s",
					dbname, pool_config->health_check_user)));

	/*
	 * Start checking the backed nodes starting from the previously failed
	 * node
	 */
	for (i = *health_check_node_id; i < pool_config->backend_desc->num_backends; i++)
	{
		*health_check_node_id = i;

		/*
		 * Make sure that health check timer has not been expired. Before
		 * called health_check(), health_check_timer_expired is set to 0.
		 * However it is possible that while processing DB nodes health check
		 * timer expired.
		 */
		if (health_check_timer_expired)
		{
			ereport(ERROR,
					(errmsg("health check timer has been already expired before attempting to connect backend node %d", i)));
		}

		bkinfo = pool_get_node_info(i);

		ereport(DEBUG1,
				(errmsg("Backend DB node %d status is %d", i, bkinfo->backend_status)));


		if (bkinfo->backend_status == CON_UNUSED ||
			bkinfo->backend_status == CON_DOWN)
			continue;

		all_nodes_healthy = true;
		ereport(DEBUG1,
				(errmsg("Trying to make persistent DB connection to backend node %d having status %d", i, bkinfo->backend_status)));

		slot = make_persistent_db_connection(i, bkinfo->backend_hostname,
											 bkinfo->backend_port,
											 dbname,
											 pool_config->health_check_user,
											 password ? password : "", false);

		ereport(DEBUG1,
				(errmsg("persistent DB connection to backend node %d having status %d is successful", i, bkinfo->backend_status)));

		discard_persistent_db_connection(slot);
	}
	if (password)
		pfree(password);
	return all_nodes_healthy;
}
#endif

/*
 * handle SIGCHLD
 */
static RETSIGTYPE reap_handler(int sig)
{
	int			save_errno = errno;

	POOL_SETMASK(&BlockSig);
	sigchld_request = 1;

	if (pipe_fds[1])
	{
		dummy_status = write(pipe_fds[1], "\0", 1);
	}

#ifdef NOT_USED
	if (pipe_fds[1] && write(pipe_fds[1], "\0", 1) < 0)
		ereport(WARNING,
				(errmsg("reap_handler: write to pipe failed"),
						errdetail("%m")));
#endif

	POOL_SETMASK(&UnBlockSig);

	errno = save_errno;
}

/*
 * pool_waitpid:
 * nothing more than a wrapper function over NOHANG mode waitpid() or wait3()
 * depending on the existance of waitpid in the system
 */
pid_t
pool_waitpid(int *status)
{
#ifdef HAVE_WAITPID
	return waitpid(-1, status, WNOHANG);
#else
	return wait3(status, WNOHANG, NULL);
#endif
}

/*
 * process_name_from_pid()
 * helper function for reaper() to report the terminating child process type name
 */
static char *
process_name_from_pid(pid_t pid)
{
	if (pid == pcp_pid)
		return "PCP child";
	if (pid == worker_pid)
		return "worker child";
	if (pool_config->use_watchdog)
	{
		if (pid == watchdog_pid)
			return "watchdog child";
		else if (pid == wd_lifecheck_pid)
			return "watchdog lifecheck";
	}
	if (pid == pgpool_logger_pid)
		return "pgpool log collector";
	return "child";
}

/*
 * Attach zombie processes and restart child processes.
 * reaper() must be called protected from signals.
 * Note:
 * In pgpool child can exit in two ways, either by some signal or by
 * calling exit() system function.
 * For the case of child terminating due to a signal the reaper() function
 * always forks a new respective type of child process. But for the case when
 * child got terminated by exit() system call than the function checks the exit
 * code and if the child was exited by POOL_EXIT_FATAL than we do not restarts the
 * terminating child but shutdowns the pgpool-II. This allow
 * the child process to inform parent process of fatal failures which needs
 * to be rectified (e.g startup failure) by user for smooth running of system.
 * Also the child exits with success status POOL_EXIT_NO_RESTART does not gets
 * restarted.
 */
static void
reaper(void)
{
	pid_t		pid;
	int			status;
	int			i;

	ereport(DEBUG1,
			(errmsg("reaper handler")));

	if (exiting)
	{
		ereport(DEBUG1,
				(errmsg("reaper handler: exited because already in exiting mode")));
		return;
	}

	if (switching)
	{
		ereport(DEBUG1,
				(errmsg("reaper handler: exited due to switching")));
		return;
	}

	/* clear SIGCHLD request */
	sigchld_request = 0;

	while ((pid = pool_waitpid(&status)) > 0)
	{
		pid_t		new_pid = 0;
		bool		shutdown_system = false;
		bool		restart_child = true;
		bool		found = false;
		char	   *exiting_process_name = process_name_from_pid(pid);
		bool		process_health_check = false;

		/*
		 * Check if the terminating child wants pgpool main to go down with it
		 */
		if (WIFEXITED(status))
		{
			if (WEXITSTATUS(status) == POOL_EXIT_FATAL)
			{
				ereport(DEBUG1,
						(errmsg("%s process with pid: %d exit with FATAL ERROR. pgpool-II will be shutdown", exiting_process_name, pid)));
				shutdown_system = true;
			}
			else if (WEXITSTATUS(status) == POOL_EXIT_NO_RESTART)
			{
				ereport(DEBUG1,
						(errmsg("%s process with pid: %d exit with SUCCESS. child will not be restarted", exiting_process_name, pid)));

				restart_child = false;
			}
		}
		if (WIFSIGNALED(status))
		{
			/* Child terminated by segmentation fault or sigkill. Report it */
			if (WTERMSIG(status) == SIGSEGV)
				ereport(WARNING,
						(errmsg("%s process with pid: %d was terminated by segmentation fault", exiting_process_name, pid)));
			else if (WTERMSIG(status) == SIGKILL)
				ereport(WARNING,
						(errmsg("%s process with pid: %d was terminated by sigkill", exiting_process_name, pid)));
			else
				ereport(LOG,
						(errmsg("%s process with pid: %d exits with status %d by signal %d", exiting_process_name, pid, status, WTERMSIG(status))));

			/*
			 * If the watchdog process was terminated abonormally. we need to
			 * set the cleanup flag so that the new watchdog process can start
			 * without problems
			 */
			if (pool_config->use_watchdog && pid == watchdog_pid)
			{
				set_watchdog_process_needs_cleanup();
			}
		}
		else
			ereport(LOG,
					(errmsg("%s process with pid: %d exits with status %d", exiting_process_name, pid, status)));

		/* if exiting child process was PCP handler */
		if (pid == pcp_pid)
		{
			found = true;
			if (restart_child)
			{
				pcp_pid = pcp_fork_a_child(pcp_unix_fd, pcp_inet_fd, pcp_conf_file);
				new_pid = pcp_pid;
			}
			else
				pcp_pid = 0;
		}

		/* exiting process was worker process */
		else if (pid == worker_pid)
		{
			found = true;
			if (restart_child)
			{
				worker_pid = worker_fork_a_child(PT_WORKER, do_worker_child, NULL);
				new_pid = worker_pid;
			}
			else
				worker_pid = 0;
		}
		else if (pid == pgpool_logger_pid)
		{
			if (restart_child)
			{
				pgpool_logger_pid = SysLogger_Start();
			}
			else
				pgpool_logger_pid = 0;
		}

		/* exiting process was follow child process */
		else if (pid == follow_pid)
		{
			follow_pid = 0;
		}

		/* exiting process was watchdog process */
		else if (pool_config->use_watchdog)
		{
			if (watchdog_pid == pid)
			{
				found = true;
				if (restart_child)
				{
					watchdog_pid = initialize_watchdog();
					new_pid = watchdog_pid;
				}
				else
					watchdog_pid = 0;
			}
			else if (wd_lifecheck_pid == pid)
			{
				found = true;
				if (restart_child)
				{
					wd_lifecheck_pid = initialize_watchdog_lifecheck();
					new_pid = wd_lifecheck_pid;
				}
				else
					wd_lifecheck_pid = 0;
			}
		}

		/*
		 * we are not able to identify the exiting process yet. check if the
		 * exiting process was child process (handling PG clients)
		 */
		if (found == false)
		{
			/* look for exiting child's pid */
			for (i = 0; i < pool_config->num_init_children; i++)
			{
				if (pid == process_info[i].pid)
				{
					found = true;
					/* if found, fork a new child */
					if (!switching && !exiting && restart_child)
					{
						process_info[i].pid = fork_a_child(fds, i);
						process_info[i].start_time = time(NULL);
						new_pid = process_info[i].pid;
					}
					else
						process_info[i].pid = 0;
					break;
				}
			}
		}

		/* Check health check process */
		if (found == false)
		{
			process_health_check = true;

			for (i = 0; i < NUM_BACKENDS; i++)
			{
				if (pid == health_check_pids[i])
				{
					found = true;

					/* Fork new health check worker */
					if (!switching && !exiting && VALID_BACKEND(i))
					{
						health_check_pids[i] = worker_fork_a_child(PT_HEALTH_CHECK, do_health_check_child, &i);
					}
					else
						health_check_pids[i] = 0;
				}
			}
		}

		if (shutdown_system)
			ereport(FATAL,
					(errmsg("%s process exit with fatal error. exiting pgpool-II", exiting_process_name)));

		else if (restart_child && new_pid)
		{
			/* Report if the child was restarted */
			ereport(LOG,
					(errmsg("fork a new %s process with pid: %d", exiting_process_name, new_pid)));
		}
		else
		{
			if (process_health_check == false)
			{
				/* And the child was not restarted */
				ereport(LOG,
						(errmsg("%s process with pid: %d exited with success and will not be restarted", exiting_process_name, pid)));
			}
		}

	}
	ereport(DEBUG1,
			(errmsg("reaper handler: exiting normally")));
}

/*
 * get node information specified by node_number
 */
BackendInfo *
pool_get_node_info(int node_number)
{
	if (node_number < 0 || node_number >= NUM_BACKENDS)
		return NULL;

	return &BACKEND_INFO(node_number);
}

/*
 * get number of nodes
 */
int
pool_get_node_count(void)
{
	return NUM_BACKENDS;
}

/*
 * get process ids
 */
int *
pool_get_process_list(int *array_size)
{
	int		   *array;
	int			i;

	*array_size = pool_config->num_init_children;
	array = palloc0(*array_size * sizeof(int));
	for (i = 0; i < *array_size; i++)
		array[i] = process_info[i].pid;

	return array;
}

/*
 * get process information specified by pid
 */
ProcessInfo *
pool_get_process_info(pid_t pid)
{
	int			i;

	for (i = 0; i < pool_config->num_init_children; i++)
		if (process_info[i].pid == pid)
			return &process_info[i];

	return NULL;
}

/*
 * handle SIGUSR2
 * Wakeup all processes
 */
static void
wakeup_children(void)
{
	kill_all_children(SIGUSR2);
}


static RETSIGTYPE wakeup_handler(int sig)
{
	int			save_errno = errno;

	wakeup_request = 1;
	if (processState != INITIALIZING)
	{
		POOL_SETMASK(&BlockSig);
		dummy_status = write(pipe_fds[1], "\0", 1);
#ifdef NOT_USED
		if (write(pipe_fds[1], "\0", 1) < 0)
			ereport(WARNING,
					(errmsg("wakeup_handler: write to pipe failed"),
					 errdetail("%m")));
#endif
		POOL_SETMASK(&UnBlockSig);
	}
	errno = save_errno;
}

/*
 * handle SIGHUP
 *
 */
static RETSIGTYPE reload_config_handler(int sig)
{
	int			save_errno = errno;

	POOL_SETMASK(&BlockSig);
	reload_config_request = 1;
	dummy_status = write(pipe_fds[1], "\0", 1);
#ifdef NOT_USED
	if (write(pipe_fds[1], "\0", 1) < 0)
		ereport(WARNING,
				(errmsg("reload_config_handler: write to pipe failed"),
				 errdetail("%m")));
#endif

	POOL_SETMASK(&UnBlockSig);

	errno = save_errno;
}

static void
kill_all_children(int sig)
{
	int			i;

	if (process_info)
	{
		/* kill all children */
		for (i = 0; i < pool_config->num_init_children; i++)
		{
			pid_t		pid = process_info[i].pid;

			if (pid)
			{
				kill(pid, sig);
			}
		}
	}

	if (sig == SIGHUP)
	{
		/* make PCP process reload as well */
		if (pcp_pid > 0)
			kill(pcp_pid, sig);
		
		/* make health check process reload as well */
		for (i = 0; i < NUM_BACKENDS; i++)
		{
			if (health_check_pids[i] > 0)
				kill(health_check_pids[i], sig);
		}

		/* make worker process reload as well */
		if (worker_pid > 0) 
			kill(worker_pid, sig);

		/* make watchdog process reload as well */
		if (watchdog_pid > 0) 
			kill(watchdog_pid, sig);
	}
}

/*
 * pause in a period specified by timeout. If any data is coming
 * through pipe_fds[0], that means one of: failover request(SIGUSR1),
 * SIGCHLD received, children wake up request(SIGUSR2 used in on line
 * recovery processing) or config file reload request(SIGHUP) has been
 * occurred.  In this case this function returns 1.
 * otherwise 0: (no signal event occurred), -1: (error)
 * XXX: is it OK that select(2) error is ignored here?
 */
static int
pool_pause(struct timeval *timeout)
{
	fd_set		rfds;
	int			n;
	char		dummy;

	FD_ZERO(&rfds);
	FD_SET(pipe_fds[0], &rfds);
	n = select(pipe_fds[0] + 1, &rfds, NULL, NULL, timeout);
	if (n == 1)
	{
		if (read(pipe_fds[0], &dummy, 1) < 0)
			ereport(WARNING,
					(errmsg("pool_pause: read on pipe failed"),
					 errdetail("%m")));
	}
	return n;
}

/*
 * sleep for seconds specified by "second".  Unlike pool_pause(), this
 * function guarantees that it will sleep for specified seconds.  This
 * function uses pool_pause() internally. If it informs that there is
 * a pending signal event, they are processed using CHECK_REQUEST
 * macro. Note that most of these processes are done while all signals
 * are blocked.
 */
void
pool_sleep(unsigned int second)
{
	struct timeval current_time,
				sleep_time;

	gettimeofday(&current_time, NULL);
	sleep_time.tv_sec = second + current_time.tv_sec;
	sleep_time.tv_usec = current_time.tv_usec;

	POOL_SETMASK(&UnBlockSig);
	while (sleep_time.tv_sec > current_time.tv_sec)
	{
		struct timeval timeout;
		int			r;

		timeout.tv_sec = sleep_time.tv_sec - current_time.tv_sec;
		timeout.tv_usec = sleep_time.tv_usec - current_time.tv_usec;
		if (timeout.tv_usec < 0)
		{
			timeout.tv_sec--;
			timeout.tv_usec += 1000000;
		}

		r = pool_pause(&timeout);
		POOL_SETMASK(&BlockSig);
		if (r > 0)
			CHECK_REQUEST;
		POOL_SETMASK(&UnBlockSig);
		gettimeofday(&current_time, NULL);
	}
	POOL_SETMASK(&BlockSig);
}

/*
 * trigger_failover_command: execute specified command at failover.
 *                           command_line is null-terminated string.
 */
static int
trigger_failover_command(int node, const char *command_line,
						 int old_main_node, int new_main_node, int old_primary)
{
	int			r = 0;
	String	   *exec_cmd;
	char		port_buf[6];
	char		buf[2];
	BackendInfo *info;
	BackendInfo *newmain;
	BackendInfo *oldprimary;

	if (command_line == NULL || (strlen(command_line) == 0))
		return 0;

	/* check failed nodeID */
	if (node < 0 || node >= NUM_BACKENDS)
		return -1;

	info = pool_get_node_info(node);
	if (!info)
		return -1;

	buf[1] = '\0';
	exec_cmd = init_string("");

	while (*command_line)
	{
		if (*command_line == '%')
		{
			if (*(command_line + 1))
			{
				char		val = *(command_line + 1);

				switch (val)
				{
					case 'p':	/* failed node port */
						snprintf(port_buf, sizeof(port_buf), "%d", info->backend_port);
						string_append_char(exec_cmd, port_buf);
						break;

					case 'D':	/* failed node database directory */
						string_append_char(exec_cmd, info->backend_data_directory);
						break;

					case 'd':	/* failed node id */
						snprintf(port_buf, sizeof(port_buf), "%d", node);
						string_append_char(exec_cmd, port_buf);
						break;

					case 'h':	/* failed host name */
						string_append_char(exec_cmd, info->backend_hostname);
						break;

					case 'H':	/* new main host name */
						newmain = pool_get_node_info(new_main_node);
						if (newmain)
							string_append_char(exec_cmd, newmain->backend_hostname);
						else
							/* no valid new main */
							string_append_char(exec_cmd, "\"\"");
						break;

					case 'm':	/* new main node id */
						snprintf(port_buf, sizeof(port_buf), "%d", new_main_node);
						string_append_char(exec_cmd, port_buf);
						break;

					case 'r':	/* new main node port */
						newmain = pool_get_node_info(get_next_main_node());
						if (newmain)
						{
							snprintf(port_buf, sizeof(port_buf), "%d", newmain->backend_port);
							string_append_char(exec_cmd, port_buf);
						}
						else
							/* no valid new main node */
							string_append_char(exec_cmd, "\"\"");
						break;

					case 'R':	/* new main database directory */
						newmain = pool_get_node_info(get_next_main_node());
						if (newmain)
							string_append_char(exec_cmd, newmain->backend_data_directory);
						else
							/* no valid new main */
							string_append_char(exec_cmd, "\"\"");
						break;

					case 'M':	/* old main node id */
						snprintf(port_buf, sizeof(port_buf), "%d", old_main_node);
						string_append_char(exec_cmd, port_buf);
						break;

					case 'P':	/* old primary node id */
						snprintf(port_buf, sizeof(port_buf), "%d", old_primary);
						string_append_char(exec_cmd, port_buf);
						break;

					case 'N':	/* old primary host name */
						oldprimary = pool_get_node_info(old_primary);
						if (oldprimary)
							string_append_char(exec_cmd, oldprimary->backend_hostname);
						else
							/* no valid old primary */
							string_append_char(exec_cmd, "\"\"");
						break;

					case 'S':	/* old primary port */
						oldprimary = pool_get_node_info(old_primary);
						if (oldprimary)
						{
							snprintf(port_buf, sizeof(port_buf), "%d", oldprimary->backend_port);
							string_append_char(exec_cmd, port_buf);
						}
						else
							/* no valid old primary */
							string_append_char(exec_cmd, "\"\"");
						break;

					case '%':	/* escape */
						string_append_char(exec_cmd, "%");
						break;

					default:	/* ignore */
						break;
				}
				command_line++;
			}
		}
		else
		{
			buf[0] = *command_line;
			string_append_char(exec_cmd, buf);
		}
		command_line++;
	}

	if (strlen(exec_cmd->data) != 0)
	{
		ereport(LOG,
				(errmsg("execute command: %s", exec_cmd->data)));
		r = system(exec_cmd->data);
	}

	free_string(exec_cmd);

	return r;
}

/*
 * This function is used by find_primary_node().  Find primary node/standby
 * node and returns static array of status for each backend node. This
 * function must not throw ERROR or FATAL.
 */
static POOL_NODE_STATUS pool_node_status[MAX_NUM_BACKENDS];

POOL_NODE_STATUS *
verify_backend_node_status(POOL_CONNECTION_POOL_SLOT * *slots)
{
	POOL_SELECT_RESULT *res;
	int			num_primaries = 0;
	int			num_standbys = 0;
	int			i,
				j;
	BackendInfo *backend_info;

	for (i = 0; i < NUM_BACKENDS; i++)
	{
		pool_node_status[i] = POOL_NODE_STATUS_UNUSED;

		if (!VALID_BACKEND(i))
			continue;

		if (!slots[i])
			continue;

		if (get_query_result(slots, i, "SELECT pg_is_in_recovery()", &res))
		{
			continue;
		}

		get_server_version(slots, i);

		if (res->data[0] && !strcmp(res->data[0], "t"))
		{
			/* Possibly standby */
			pool_node_status[i] = POOL_NODE_STATUS_STANDBY;
			num_standbys++;
		}
		else if (res->data[0] && !strcmp(res->data[0], "f"))
		{
			/* Possibly primary */
			pool_node_status[i] = POOL_NODE_STATUS_PRIMARY;
			num_primaries++;
		}
		free_select_result(res);
	}

	/*
	 * If there's no primary node, there's no point to run additional
	 * testings.
	 */
	if (num_primaries == 0)
	{
		ereport(DEBUG1,
				(errmsg("verify_backend_node_status: there's no primary node")));
		return pool_node_status;
	}

	/*
	 * There's no standby node.
	 */
	if (num_standbys == 0)
	{
		if (num_primaries == 1)
		{
			/*
			 * If there's only one primary node and there's no standby,
			 * there's no point to run additional testings.
			 */
			ereport(DEBUG1,
					(errmsg("verify_backend_node_status: there's no standby node")));
			return pool_node_status;
		}
		else
		{
			/*
			 * There are multiple primaries and there's no standby node.
			 * There's no way to decide which one is correct.  We just leave
			 * the youngest primary node and detach rests if allowed.
			 */
			for (i = 0; i < NUM_BACKENDS; i++)
			{
				if (pool_node_status[i] == POOL_NODE_STATUS_PRIMARY)
				{
					ereport(DEBUG1,
							(errmsg("verify_backend_node_status: decided node %d is the true primary", i)));
					i++;
					for (; i < NUM_BACKENDS; i++)
					{
						if (pool_node_status[i] == POOL_NODE_STATUS_PRIMARY)
						{
							if (pool_config->detach_false_primary)
							{
								ereport(DEBUG1,
										(errmsg("verify_backend_node_status: node %d is a false primary", i)));
								pool_node_status[i] = POOL_NODE_STATUS_INVALID;
							}
							else
								pool_node_status[i] = POOL_NODE_STATUS_UNUSED;
						}
					}
				}
			}
		}
		return pool_node_status;
	}

	/*
	 * There are multiple standbys
	 */
	else
	{
		bool		check_connectivity = false;
		int			wal_receiver_status = 0;
		int			wal_receiver_conninfo = 1;
		char		host[MAX_DB_HOST_NAMELEN];
		char		port[1024];
		int			primary[MAX_NUM_BACKENDS];
		int			true_primary = -1;

		ereport(DEBUG1,
				(errmsg("verify_backend_node_status: multiple standbys: %d", num_standbys)));

		if (!pool_config->detach_false_primary)
		{
			ereport(DEBUG1,
					(errmsg("verify_backend_node_status: detach_false_primary is off and no additional checking is performed")));
			return pool_node_status;
		}

		/*
		 * Check connectivity between primary and standby by using
		 * pg_stat_wal_receiver (only >= 9.6.0) if there's more than or equal
		 * to 1 primary.
		 */
		for (i = 0; i < NUM_BACKENDS; i++)
		{
			if (!VALID_BACKEND(i))
				continue;

			if (get_server_version(slots, i) >= 90600)
			{
				check_connectivity = true;
				break;
			}
		}
		if (!check_connectivity)
		{
			ereport(DEBUG1,
					(errmsg("verify_backend_node_status: server verion is lower than 9.6.0. Skipping connectivity checks")));
			return pool_node_status;
		}

		ereport(DEBUG1,
				(errmsg("verify_backend_node_status: checking connectivity")));

		for (i = 0; i < NUM_BACKENDS; i++)
		{
			primary[i] = 0;

			if (!VALID_BACKEND(i))
				continue;

			if (pool_node_status[i] == POOL_NODE_STATUS_PRIMARY)
			{
				ereport(DEBUG1,
						(errmsg("verify_backend_node_status: %d is primary", i)));

				for (j = 0; j < NUM_BACKENDS; j++)
				{
					if (pool_node_status[j] == POOL_NODE_STATUS_STANDBY)
					{
						ereport(DEBUG1,
								(errmsg("verify_backend_node_status: %d is standby", j)));

						if (get_query_result(slots, j, "SELECT status, conninfo FROM pg_stat_wal_receiver", &res))
						{
							ereport(DEBUG1,
									(errmsg("verify_backend_node_status: call pg_stat_wal_receiver to standby %d failed", j)));
							continue;
						}
						if (res->numrows <= 0)
						{
							ereport(DEBUG1,
									(errmsg("verify_backend_node_status: pg_stat_wal_receiver returned no row. standby %d", j)));
							free_select_result(res);
							continue;
						}
						if (res->nullflags[wal_receiver_status] == -1)
						{
							ereport(DEBUG1,
									(errmsg("verify_backend_node_status: pg_stat_wal_receiver status for standby %d is NULL", j)));
							free_select_result(res);
							continue;
						}
						if (strcmp(res->data[wal_receiver_status], "streaming"))
						{
							ereport(DEBUG1,
									(errmsg("verify_backend_node_status: pg_stat_wal_receiver status is not \"streaming\" for standby %d (%s)", j, res->data[wal_receiver_status])));
							free_select_result(res);
							continue;

						}
						if (res->nullflags[wal_receiver_conninfo] == -1)
						{
							ereport(DEBUG1,
									(errmsg("verify_backend_node_status: pg_stat_wal_receiver conninfo for standby %d is NULL", j)));
							continue;
						}
						get_info_from_conninfo(res->data[wal_receiver_conninfo], host, sizeof(host), port, sizeof(port));
						ereport(DEBUG1,
								(errmsg("verify_backend_node_status: conninfo for standby %d is === %s ===. host:%s port:%s", j, res->data[wal_receiver_conninfo], host, port)));
						free_select_result(res);

						/* get primary backend info */
						backend_info = pool_get_node_info(i);

						/* verify host and port */
						if (((*backend_info->backend_hostname == '/' && *host == '\0') ||
							 /*
							  * It is possible that backend_hostname is Unix
							  * domain socket but wal_receiver connects via
							  * TCP/IP localhost.
							  */
							 (*backend_info->backend_hostname == '/' && !strcmp("localhost", host)) ||
							 !strcmp(backend_info->backend_hostname, host))	&&
							backend_info->backend_port == atoi(port))
						{
							/* the standby connects to the primary */
							primary[i]++;
							if (primary[i] == num_standbys)
								true_primary = i;
						}
						else
						{
							/* the standby does not connect to the primary */
							ereport(LOG,
									(errmsg("verify_backend_node_status: primary %d does not connect to standby %d", i, j)));
						}
					}
				}
			}
		}

		/*
		 * Check if each primary connected standbys. If all standbys connect
		 * to one of primaries, then the primary is good. Other primaries are
		 * false.  If none of primaries does not own all connected standbys,
		 * we cannot judge which primary is good.
		 */
		for (i = 0; i < NUM_BACKENDS; i++)
		{
			ereport(DEBUG1,
					(errmsg("verify_backend_node_status: primary %d owns %d standbys out of %d", i, primary[i], num_standbys)));
			ereport(DEBUG1,
					(errmsg("verify_backend_node_status: true_primary %d", true_primary)));

			if (pool_node_status[i] == POOL_NODE_STATUS_PRIMARY && primary[i] >= 0)
			{
				if (primary[i] < num_standbys)
				{
					ereport(LOG,
							(errmsg("verify_backend_node_status: primary %d owns only %d standbys out of %d", i, primary[i], num_standbys)));

					/*
					 * If the good primary exists and detach_false_primary is
					 * true, then ask to detach the false primary
					 */
					if (true_primary >= 0 && pool_config->detach_false_primary)
						pool_node_status[i] = POOL_NODE_STATUS_INVALID;
				}
			}
		}

	}

	return pool_node_status;
}

POOL_NODE_STATUS *
pool_get_node_status(void)
{
	return pool_node_status;
}

/*
 * Find the primary node (i.e. not standby node) and returns its node
 * id. If no primary node is found, returns -1.
 */
static int
find_primary_node(void)
{
	BackendInfo *bkinfo;
	POOL_CONNECTION_POOL_SLOT *slots[MAX_NUM_BACKENDS];
	int			i;
	POOL_NODE_STATUS *status;
	int			primary = -1;
	char	   *password = NULL;

	/* Streaming replication mode? */
	if (!SL_MODE)
	{
		/*
		 * No point to look for primary node if not in streaming replication
		 * mode.
		 */
		ereport(DEBUG1,
				(errmsg("find_primary_node: not in streaming replication mode")));
		return -1;
	}

	/*
	 * First check for "ALWAYS_PRIMARY" flags exists. If so, do not perform
	 * actual primary node check and just returns the node id.
	 */
	for (i = 0; i < NUM_BACKENDS; i++)
	{
		if (POOL_ALWAYS_PRIMARY & BACKEND_INFO(i).flag)
		{
			ereport(DEBUG1,
					(errmsg("find_primary_node: ALWAYS_PRIMARY flag found. Returns node id: %d", i)));
			return i;
		}
	}

	password = get_pgpool_config_user_password(pool_config->sr_check_user,
											   pool_config->sr_check_password);

	/*
	 * Establish connections to backend
	 */
	for (i = 0; i < NUM_BACKENDS; i++)
	{
		slots[i] = NULL;

		if (!VALID_BACKEND(i))
			continue;

		bkinfo = pool_get_node_info(i);

		slots[i] = make_persistent_db_connection_noerror(i, bkinfo->backend_hostname,
														 bkinfo->backend_port,
														 pool_config->sr_check_database,
														 pool_config->sr_check_user,
														 password ? password : "", false);
		if (!slots[i])
		{
			ereport(LOG,
					(errmsg("find_primary_node: make_persistent_db_connection_noerror failed on node %d", i)));
		}
	}

	if (password)
		pfree(password);

	/* Verify backend status */
	pool_acquire_follow_primary_lock(true, false);
	status = verify_backend_node_status(slots);
	pool_release_follow_primary_lock(false);

	for (i = 0; i < NUM_BACKENDS; i++)
	{
		if (status[i] == POOL_NODE_STATUS_PRIMARY)
		{
			/* This is the primary */
			ereport(LOG,
					(errmsg("find_primary_node: primary node is %d", i)));
			primary = i;
		}
		else if (status[i] == POOL_NODE_STATUS_STANDBY)
		{
			ereport(LOG,
					(errmsg("find_primary_node: standby node is %d", i)));
		}
		else if (status[i] == POOL_NODE_STATUS_INVALID)
		{
			/* Split brain or invalid node */
			ereport(LOG,
					(errmsg("find_primary_node: invalid node %d", i)));
		}
	}

	for (i = 0; i < NUM_BACKENDS; i++)
	{
		if (slots[i])
			discard_persistent_db_connection(slots[i]);
	}

	return primary;
}

static int
find_primary_node_repeatedly(void)
{
	time_t      search_primary_expiration;
	time_t      now;
	int			node_id = -1;
	int			i;

	/* Streaming replication mode? */
	if (!SL_MODE)
	{
		/*
		 * No point to look for primary node if not in streaming replication
		 * mode.
		 */
		ereport(DEBUG1,
				(errmsg("find_primary_node_repeatedly: not in streaming replication mode")));
		return -1;
	}

	/*
	 * If follow primary command is ongoing, skip primary node check.  Just
	 * return current primary node to avoid deadlock between pgpool main
	 * failover() and follow primary process.
	 */
	if (Req_info->follow_primary_ongoing)
	{
		ereport(LOG,
				(errmsg("find_primary_node_repeatedly: follow primary is ongoing. return current primary: %d",
						Req_info->primary_node_id)));

		return Req_info->primary_node_id;
	}

	/*
	 * If all of the backends are down, there's no point to keep on searching
	 * primary node.
	 */
	for (i = 0; i < NUM_BACKENDS; i++)
	{
		if (VALID_BACKEND(i))
			break;
	}
	if (i == NUM_BACKENDS)
	{
		ereport(LOG,
				(errmsg("find_primary_node_repeatedly: all of the backends are down. Giving up finding primary node")));
		return -1;
	}

	/*
	 * Try to find the new primary node and keep trying for
	 * search_primary_node_timeout seconds. search_primary_node_timeout = 0
	 * means never timeout and keep searching indefinitely
	 */
	ereport(LOG,
			(errmsg("find_primary_node_repeatedly: waiting for finding a primary node")));

	/* set expiration time for searching the primary node */
	search_primary_expiration = time(NULL) + pool_config->search_primary_node_timeout;

	for (;;) 
	{    
		now = time(NULL);

		if (pool_config->search_primary_node_timeout == 0 || search_primary_expiration > now)
		{
			node_id = find_primary_node();
			if (node_id != -1)
				break;
			pool_sleep(1);
		}
		else
		{
			ereport(LOG,
					(errmsg("failed to find primary node"),
					 errdetail("find_primary_node_repeatedly: expired after %d seconds",
								pool_config->search_primary_node_timeout)));
			break;
		}
	}
	return node_id;
}

/*
* fork a follow child
*/
static pid_t
fork_follow_child(int old_main_node, int new_primary, int old_primary)
{
	pid_t		pid;
	int			i;

	pid = fork();

	if (pid == 0)
	{
		POOL_SETMASK(&UnBlockSig);

		pool_signal(SIGCHLD, SIG_DFL);
		pool_signal(SIGUSR1, SIG_DFL);
		pool_signal(SIGUSR2, SIG_DFL);
		pool_signal(SIGTERM, SIG_DFL);
		pool_signal(SIGINT, SIG_DFL);
		pool_signal(SIGQUIT, SIG_DFL);
		pool_signal(SIGHUP, SIG_DFL);

		on_exit_reset();

		/*
		 * Set session id if possible
		 */
#ifdef HAVE_SETSID
		if (setsid() < 0)
		{
			ereport(FATAL,
					(errmsg("could not set session id in the fork_follow_child"),
					 errdetail("setsid() system call failed with reason: \"%m\"")));
		}
#endif

		SetProcessGlobalVariables(PT_FOLLOWCHILD);
		/*
		 * when the watchdog is enabled, we would come here
		 * only on the coordinator node.
		 * so before acquiring the local lock, Lock all the
		 * standby nodes so that they should stop false primary
		 * detection until we are finished with the follow primary
		 * command.
		 */
		wd_lock_standby(WD_FOLLOW_PRIMARY_LOCK);
		pool_acquire_follow_primary_lock(true, false);
		Req_info->follow_primary_ongoing = true;
		ereport(LOG,
				(errmsg("start triggering follow command.")));
		for (i = 0; i < pool_config->backend_desc->num_backends; i++)
		{
			BackendInfo *bkinfo;

			bkinfo = pool_get_node_info(i);
			if (bkinfo->backend_status == CON_DOWN)
				trigger_failover_command(i, pool_config->follow_primary_command,
										 old_main_node, new_primary, old_primary);
		}
		Req_info->follow_primary_ongoing = false;
		pool_release_follow_primary_lock(false);
		/* inform standby watchdog nodes to release the lock as well*/
		wd_unlock_standby(WD_FOLLOW_PRIMARY_LOCK);
		exit(0);
	}
	else if (pid == -1)
	{
		ereport(WARNING,
				(errmsg("follow fork() failed"),
				 errdetail("%m")));
		exit(1);
	}
	return pid;
}

static void
initialize_shared_mem_objects(bool clear_memcache_oidmaps)
{
	BackendDesc* backend_desc;
	Size	size;
	int		i;

	/*
	 * Calculate the size of required shared memory and try to allocate
	 * everyting in sigle memory segment
	 */
	size = 256;/* let us have some extra space */
	size += MAXALIGN(sizeof(BackendDesc));
	size += MAXALIGN(pool_coninfo_size());
	size += MAXALIGN(pool_config->num_init_children * (sizeof(ProcessInfo)));
	size += MAXALIGN(sizeof(User1SignalSlot));
	size += MAXALIGN(sizeof(POOL_REQUEST_INFO));
	size += MAXALIGN(sizeof(int)); /* for InRecovery */
	size += MAXALIGN(stat_shared_memory_size());
	size += MAXALIGN(health_check_stats_shared_memory_size());
	/* Snapshot Isolation manage area */
	size += MAXALIGN(sizeof(SI_ManageInfo));
	size += MAXALIGN(pool_config->num_init_children * sizeof(pid_t));
	size += MAXALIGN(pool_config->num_init_children * sizeof(pid_t));

	if (pool_is_shmem_cache())
	{
		size += MAXALIGN(pool_shared_memory_cache_size());
		size += MAXALIGN(pool_shared_memory_fsmm_size());
		size += MAXALIGN(pool_hash_size(pool_config->memqcache_max_num_cache));
	}
	if (pool_config->memory_cache_enabled || pool_config->enable_shared_relcache)
		size += MAXALIGN(sizeof(POOL_QUERY_CACHE_STATS));

	if (pool_config->use_watchdog)
	{
		size += MAXALIGN(wd_ipc_get_shared_mem_size());
	}

	ereport(LOG,
			(errmsg("allocating (%zu) bytes of shared memory segment",size)));
	initialize_shared_memory_main_segment(size);

	/* Move the backend descriptors to shared memory */
	backend_desc = pool_shared_memory_segment_get_chunk(sizeof(BackendDesc));
	memcpy(backend_desc, pool_config->backend_desc,sizeof(BackendDesc));
	pfree(pool_config->backend_desc);
	pool_config->backend_desc = backend_desc;

	/* get the shared memory from main segment*/
	con_info = (ConnectionInfo *)pool_shared_memory_segment_get_chunk(pool_coninfo_size());

	process_info = (ProcessInfo *)pool_shared_memory_segment_get_chunk(pool_config->num_init_children * (sizeof(ProcessInfo)));
	for (i = 0; i < pool_config->num_init_children; i++)
		process_info[i].connection_info = pool_coninfo(i, 0, 0);

	user1SignalSlot = (User1SignalSlot *)pool_shared_memory_segment_get_chunk(sizeof(User1SignalSlot));

	Req_info = (POOL_REQUEST_INFO *)pool_shared_memory_segment_get_chunk(sizeof(POOL_REQUEST_INFO));

	InRecovery = (int *)pool_shared_memory_segment_get_chunk(sizeof(int));

	/* Initialize statistics area */
	stat_set_stat_area(pool_shared_memory_segment_get_chunk(stat_shared_memory_size()));
	stat_init_stat_area();

	/* Initialize health check statistics area */
	health_check_stats_init(pool_shared_memory_segment_get_chunk(health_check_stats_shared_memory_size()));

	/* Initialize Snapshot Isolation manage area */
	si_manage_info = (SI_ManageInfo*)pool_shared_memory_segment_get_chunk(sizeof(SI_ManageInfo));

	si_manage_info->snapshot_waiting_children =
		(pid_t*)pool_shared_memory_segment_get_chunk(pool_config->num_init_children * sizeof(pid_t));

	si_manage_info->commit_waiting_children =
		(pid_t*)pool_shared_memory_segment_get_chunk(pool_config->num_init_children * sizeof(pid_t));

	/*
	 * Initialize backend status area. From now on, VALID_BACKEND macro can be
	 * used. (get_next_main_node() uses VALID_BACKEND)
	 */

	for (i = 0; i < MAX_NUM_BACKENDS; i++)
	{
		my_backend_status[i] = &(BACKEND_INFO(i).backend_status);
	}

	/* initialize Req_info */
	Req_info->main_node_id = get_next_main_node();
	Req_info->conn_counter = 0;
	Req_info->switching = false;
	Req_info->request_queue_head = Req_info->request_queue_tail = -1;
	Req_info->primary_node_id = -2;
	*InRecovery = RECOVERY_INIT;

	/*
	 * Initialize shared memory cache
	 */
	if (pool_config->memory_cache_enabled || pool_config->enable_shared_relcache)
	{
		if (pool_is_shmem_cache())
		{
			size_t		size;

			size = pool_shared_memory_cache_size();
			pool_init_memory_cache(size);

			size = pool_shared_memory_fsmm_size();
			if (size == 0)
				ereport(FATAL,
						(errmsg("invalid shared memory size"),
						 errdetail("pool_shared_memory_fsmm_size error")));

			pool_init_fsmm(size);

			pool_allocate_fsmm_clock_hand();

			pool_discard_oid_maps();

			ereport(LOG,
					(errmsg("pool_discard_oid_maps: discarded memqcache oid maps")));

			pool_hash_init(pool_config->memqcache_max_num_cache);

			pool_init_whole_cache_blocks();
		}

#ifdef USE_MEMCACHED
		else
		{
			if (clear_memcache_oidmaps)
			{
				pool_discard_oid_maps();
				ereport(LOG,
						(errmsg("discarded memqcache oid maps")));
			}
			else
			{
				ereport(DEBUG1,
						(errmsg("skipped discarding memqcache oid maps")));
			}
		}
#endif

		pool_init_memqcache_stats();
	}

	/* initialize watchdog IPC unix domain socket address */
	if (pool_config->use_watchdog)
	{
		wd_ipc_initialize_data();
	}

}
/*
* Read the status file
*/
static int
read_status_file(bool discard_status)
{
	FILE	   *fd;
	char		fnamebuf[POOLMAXPATHLEN];
	int			i;
	bool		someone_wakeup = false;
	bool		is_old_format;

	/*
	 * Set backend status changed timestamp so that it is set even if there's
	 * no status file or discard status option is specified.
	 */
	for (i = 0; i < MAX_NUM_BACKENDS; i++)
	{
		pool_set_backend_status_changed_time(i);
	}

	snprintf(fnamebuf, sizeof(fnamebuf), "%s/%s", pool_config->logdir, STATUS_FILE_NAME);
	fd = fopen(fnamebuf, "r");
	if (!fd)
	{
		ereport(LOG,
				(errmsg("Backend status file %s does not exist", fnamebuf)));
		return -1;
	}

	/*
	 * If discard_status is true, unlink pgpool_status and do not restore
	 * previous status.
	 */
	if (discard_status)
	{
		fclose(fd);
		if (unlink(fnamebuf) == 0)
			ereport(LOG,
					(errmsg("Backend status file %s discarded", fnamebuf)));
		else
			ereport(WARNING,
					(errmsg("failed to discard backend status file: \"%s\"", fnamebuf),
					 errdetail("%m")));
		return 0;
	}

	/*
	 * First try out with old format file.
	 */
	is_old_format = true;

	if (fread(&backend_rec, 1, sizeof(backend_rec), fd) == sizeof(backend_rec))
	{
		/* It's likely old binary format status file */
		for (i = 0; i < pool_config->backend_desc->num_backends; i++)
		{
			if (backend_rec.status[i] == CON_DOWN)
			{
				BACKEND_INFO(i).backend_status = CON_DOWN;
				pool_set_backend_status_changed_time(i);
				(void) write_status_file();
				ereport(LOG,
						(errmsg("read_status_file: %d th backend is set to down status", i)));
			}
			else if (backend_rec.status[i] == CON_CONNECT_WAIT ||
					 backend_rec.status[i] == CON_UP)
			{
				BACKEND_INFO(i).backend_status = CON_CONNECT_WAIT;
				pool_set_backend_status_changed_time(i);
				(void) write_status_file();
				someone_wakeup = true;
			}
			else
			{
				/* It seems it's not an old binary format status file */
				is_old_format = false;
				break;
			}
		}
	}
	else
		is_old_format = false;

	fclose(fd);

	if (!is_old_format)
	{
		/*
		 * Fall back to new ascii format file. the format looks like(case is
		 * ignored):
		 *
		 * up|down|unused UP|down|unused : :
		 */
#define MAXLINE 10
		char		readbuf[MAXLINE];

		fd = fopen(fnamebuf, "r");
		if (!fd)
		{
			ereport(LOG,
					(errmsg("Backend status file %s does not exist", fnamebuf)));
			return -1;
		}

		for (i = 0; i < MAX_NUM_BACKENDS; i++)
		{
			BACKEND_INFO(i).backend_status = CON_UNUSED;
			pool_set_backend_status_changed_time(i);
		}

		for (i = 0;; i++)
		{
			readbuf[MAXLINE - 1] = '\0';
			if (fgets(readbuf, MAXLINE - 1, fd) == 0)
				break;

			if (!strncasecmp("up", readbuf, 2))
			{
				BACKEND_INFO(i).backend_status = CON_UP;
				someone_wakeup = true;
			}
			else if (!strncasecmp("down", readbuf, 4))
			{
				BACKEND_INFO(i).backend_status = CON_DOWN;
				pool_set_backend_status_changed_time(i);
				ereport(LOG,
						(errmsg("reading status file: %d th backend is set to down status", i)));
			}
			else if (!strncasecmp("unused", readbuf, 6))
			{
				BACKEND_INFO(i).backend_status = CON_UNUSED;
				pool_set_backend_status_changed_time(i);
			}
			else
			{
				ereport(WARNING,
						(errmsg("invalid data in status file, ignoring..."),
						 errdetail("backend:%d status is invalid: \"%s\"", i, readbuf)));
			}
		}
		fclose(fd);
	}

	/*
	 * If no one woke up, we regard the status file bogus
	 */
	if (someone_wakeup == false)
	{
		for (i = 0; i < pool_config->backend_desc->num_backends; i++)
		{
			BACKEND_INFO(i).backend_status = CON_CONNECT_WAIT;
			pool_set_backend_status_changed_time(i);
		}
		(void) write_status_file();
	}

	return 0;
}

/*
* Write the status file
*/
int
write_status_file(void)
{
	FILE	   *fd;
	int			fdnum;
	char		fnamebuf[POOLMAXPATHLEN];
	char		buf[10];
	int			i;

	if (!pool_config)
	{
		ereport(WARNING,
				(errmsg("pool_config is not set")));
		return 0;
	}

	/*
	 * Check to see if all nodes are down status. If so, skip writing status
	 * file. So pgpool_status will always reflect the last set of nodes to
	 * which any data was written. Upon restart, if the up-to-date (previously
	 * "up") node is in fact down (regardless of whether the stale ("down")
	 * node is back up), pgpool will detect this in its health check and will
	 * fail; if the up-to-date (previously "up") node is back up, then pgpool
	 * will commence using it.
	 *
	 * See [pgpool-general: 4721] for more discussion.
	 */
	for (i = 0; i < pool_config->backend_desc->num_backends; i++)
	{
		if (BACKEND_INFO(i).backend_status != CON_DOWN)
			break;
	}

	if (i && i == pool_config->backend_desc->num_backends)
	{
		ereport(WARNING,
				(errmsg("All the DB nodes are in down status and skip writing status file.")));

		return 0;
	}

	snprintf(fnamebuf, sizeof(fnamebuf), "%s/%s", pool_config->logdir, STATUS_FILE_NAME);
	fd = fopen(fnamebuf, "w");
	if (!fd)
	{
		ereport(WARNING,
				(errmsg("failed to open status file at: \"%s\"", fnamebuf),
				 errdetail("%m")));
		return -1;
	}

	for (i = 0; i < pool_config->backend_desc->num_backends; i++)
	{
		char	   *status;

		if (BACKEND_INFO(i).backend_status == CON_UP ||
			BACKEND_INFO(i).backend_status == CON_CONNECT_WAIT)
			status = "up";
		else if (BACKEND_INFO(i).backend_status == CON_DOWN)
			status = "down";
		else
			status = "unused";

		sprintf(buf, "%s\n", status);
		if (fwrite(buf, 1, strlen(buf), fd) != strlen(buf))
		{
			ereport(WARNING,
					(errmsg("failed to write status file at: \"%s\"", fnamebuf),
					 errdetail("%m")));
			fclose(fd);
			return -1;
		}
	}

	if (fflush(fd) != 0)
	{
		ereport(WARNING,
				(errmsg("failed to write status file at: \"%s\"", fnamebuf),
				 errdetail("%m")));
		fclose(fd);
		return -1;
	}
	fdnum = fileno(fd);
	if (fdnum < 0)
	{
		ereport(WARNING,
				(errmsg("failed to get file number. fsync() will not be performed: \"%s\"", fnamebuf),
				 errdetail("%m")));
		fclose(fd);
		return -1;
	}
	if (fsync(fdnum) != 0)
	{
		ereport(WARNING,
				(errmsg("failed to fsync(): \"%s\"", fnamebuf),
				 errdetail("%m")));
		fclose(fd);
		return -1;
	}

	fclose(fd);

	return 0;
}

static void
reload_config(void)
{
	ereport(LOG,
			(errmsg("reload config files.")));
	MemoryContext oldContext = MemoryContextSwitchTo(TopMemoryContext);

	pool_get_config(conf_file, CFGCXT_RELOAD);

	/* Realoading config file could change backend status */
	(void) write_status_file();

	MemoryContextSwitchTo(oldContext);
	if (pool_config->enable_pool_hba)
		load_hba(hba_file);

	kill_all_children(SIGHUP);
}

/* Call back function to unlink the file */
static void
FileUnlink(int code, Datum path)
{
	char	   *filePath = (char *) path;

	if (unlink(filePath) == 0)
		return;

	/*
	 * We are already exiting the system just produce a log entry to report an
	 * error
	 */
	ereport(LOG,
			(errmsg("unlink failed for file at path \"%s\"", filePath),
			 errdetail("%m")));
}

static void
system_will_go_down(int code, Datum arg)
{
	if (mypid != getpid())
	{
		/* should never happen */
		ereport(LOG, (errmsg("system_will_go_down called from invalid process")));
		return;
	}
	POOL_SETMASK(&AuthBlockSig);
	/* Write status file */
	(void) write_status_file();

	/*
	 * Terminate all childrens. But we may already have killed all the
	 * childrens if we come to this function because of shutdown signal.
	 */

	if (processState != EXITING)
	{
		ereport(LOG,
				(errmsg("shutting down")));
		terminate_all_childrens(SIGINT);
	}

	/*
	 * Send signal to follow child process and it's children.
	 */
	if (follow_pid > 0)
	{
		ereport(LOG, 
				(errmsg("terminating all child processes of follow child")));
		kill(follow_pid, SIGTERM);
		kill(-follow_pid, SIGTERM);
	}

	processState = EXITING;
	POOL_SETMASK(&UnBlockSig);
}

int
pool_send_to_frontend(char *data, int len, bool flush)
{
	if (processType == PT_PCP_WORKER)
		return send_to_pcp_frontend(data, len, flush);
	else if (processType == PT_CHILD)
		return send_to_pg_frontend(data, len, flush);
	return -1;
}

int
pool_frontend_exists(void)
{
	if (processType == PT_PCP_WORKER)
		return pcp_frontend_exists();
	else if (processType == PT_CHILD)
		return pg_frontend_exists();
	return -1;
}


static void
update_backend_quarantine_status(void)
{
	/*
	 * Reset the quarantine flag from each backend and set it to con_wait
	 */
	int			i;
	WD_STATES	wd_state = wd_internal_get_watchdog_local_node_state();

	for (i = 0; i < NUM_BACKENDS; i++)
	{
		if (BACKEND_INFO(i).quarantine && BACKEND_INFO(i).backend_status == CON_DOWN)
		{
			/*
			 * Send the failback request for the node we also set the watchdog
			 * flag, so that the failover should only be executed locally
			 * because, we will eventually send the sync backend message to
			 * all standby nodes
			 */
			if (wd_state == WD_COORDINATOR)
				send_failback_request(i, false, REQ_DETAIL_UPDATE | REQ_DETAIL_WATCHDOG);
		}
	}
}

/*
 * The function fetch the current status of all configured backend
 * nodes from the LEADER/COORDINATOR watchdog Pgpool-II and synchronize the
 * local backend states with the cluster wide status of each node.
 *
 * Latter in the funcrtion after syncing the backend node status the function
 * do a partial or full restart of Pgpool-II children depending upon the
 * Pgpool-II mode and type of node status change
 *
 */
static void
sync_backend_from_watchdog(void)
{
	bool		primary_changed = false;
	bool		node_status_was_changed_to_down = false;
	bool		node_status_was_changed_to_up = false;
	bool		need_to_restart_children = false;
	bool		partial_restart = false;
	bool		reload_maste_node_id = false;

	int			down_node_ids[MAX_NUM_BACKENDS];
	int			down_node_ids_index = 0;
	int			i;

	/*
	 * Ask the watchdog to get all the backend states from the
	 * Leader/Coordinator Pgpool-II node
	 */
	WDPGBackendStatus *backendStatus = get_pg_backend_status_from_leader_wd_node();

	if (!backendStatus)
	{
		ereport(WARNING,
				(errmsg("failed to get the backend status from the leader watchdog node"),
				 errdetail("using the local backend node status")));
		return;
	}
	if (backendStatus->node_count <= 0)
	{
		/*
		 * -ve node count is returned by watchdog when the node itself is a
		 * leader and in that case we need to use the loacl backend node
		 * status
		 */
		ereport(LOG,
				(errmsg("I am the leader watchdog node"),
				 errdetail("using the local backend node status")));
		pfree(backendStatus);
		return;
	}

	ereport(LOG,
			(errmsg("leader watchdog node \"%s\" returned status for %d backend nodes", backendStatus->nodeName, backendStatus->node_count)));

	ereport(DEBUG1,
			(errmsg("primary node on leader watchdog node \"%s\" is %d", backendStatus->nodeName, backendStatus->primary_node_id)));


	/*
	 * update the local backend status Also remove quarantine flags
	 */
	for (i = 0; i < backendStatus->node_count; i++)
	{
		BACKEND_INFO(i).quarantine = false;
		if (backendStatus->backend_status[i] == CON_DOWN)
		{
			if (BACKEND_INFO(i).backend_status != CON_DOWN)
			{
				BACKEND_INFO(i).backend_status = CON_DOWN;
				pool_set_backend_status_changed_time(i);
				my_backend_status[i] = &(BACKEND_INFO(i).backend_status);
				reload_maste_node_id = true;
				node_status_was_changed_to_down = true;
				ereport(LOG,
						(errmsg("backend:%d is set to down status", i),
						 errdetail("backend:%d is DOWN on cluster leader \"%s\"", i, backendStatus->nodeName)));
				down_node_ids[down_node_ids_index++] = i;
			}
		}
		else if (backendStatus->backend_status[i] == CON_CONNECT_WAIT ||
				 backendStatus->backend_status[i] == CON_UP)
		{
			if (BACKEND_INFO(i).backend_status != CON_CONNECT_WAIT)
			{
				if (BACKEND_INFO(i).backend_status == CON_DOWN)
					node_status_was_changed_to_up = true;

				BACKEND_INFO(i).backend_status = CON_CONNECT_WAIT;
				pool_set_backend_status_changed_time(i);
				my_backend_status[i] = &(BACKEND_INFO(i).backend_status);
				reload_maste_node_id = true;

				ereport(LOG,
						(errmsg("backend:%d is set to UP status", i),
						 errdetail("backend:%d is UP on cluster leader \"%s\"", i, backendStatus->nodeName)));

			}
		}
	}

	/*
	 * Update primary node id info on the shared memory area if it's different
	 * from the one on leader watchdog node. This should be done only in streaming
	 * or logical replication mode.
	 */
	if (SL_MODE && Req_info->primary_node_id != backendStatus->primary_node_id)
	{
		/* Do not produce this log message if we are starting up the Pgpool-II */
		if (processState != INITIALIZING)
			ereport(LOG,
					(errmsg("primary node:%d on leader watchdog node \"%s\" is different from local primary node:%d",
							backendStatus->primary_node_id, backendStatus->nodeName, Req_info->primary_node_id)));
		/*
		 * leader node returns primary_node_id = -1 when the primary node is
		 * in quarantine state on the leader.  So we will not update our
		 * primary node id when the status of current primary node is not
		 * CON_DOWN while primary_node_id sent by leader watchdong node is -1
		 *
		 * Note that Req_info->primary_node_id could be -2, which is the
		 * initial value. So we need to avoid crash by checking the value is
		 * not lower than 0. Otherwise we will get crash while looking up
		 * BACKEND_INFO array. See Mantis bug id 614 for more details.
		 */
		if (Req_info->primary_node_id >= 0 &&
			backendStatus->primary_node_id == -1 && BACKEND_INFO(Req_info->primary_node_id).backend_status != CON_DOWN)
		{
			ereport(LOG,
                (errmsg("primary node:%d on leader watchdog node \"%s\" seems to be quarantined",
					Req_info->primary_node_id, backendStatus->nodeName),
                errdetail("keeping the current primary")));
		}
		else
		{
			Req_info->primary_node_id = backendStatus->primary_node_id;
			primary_changed = true;
		}
	}

	pfree(backendStatus);

	if (reload_maste_node_id)
	{
		Req_info->main_node_id = get_next_main_node();
	}

	/* We don't need to do anything else if the Pgpool-II is starting up */
	if (processState == INITIALIZING)
		return;

	/*
	 * Decide if All or subset of the Pgpool-II children needs immidiate
	 * restart or we can do that after finishing the current session
	 *
	 * Check if there was no change at all
	 */
	if (node_status_was_changed_to_up == false &&
		node_status_was_changed_to_down == false &&
		primary_changed == false)
	{
		ereport(LOG,
				(errmsg("backend nodes status remains same after the sync from \"%s\"", backendStatus->nodeName)));
		return;
	}
	if (!STREAM)
	{
		/*
		 * If we are not in streaming replication mode restart all child
		 * processes
		 */
		ereport(LOG,
				(errmsg("node status was changed after the sync from \"%s\"", backendStatus->nodeName),
				 errdetail("all children needs to be restarted as we are not in streaming replication mode")));
		need_to_restart_children = true;
		partial_restart = false;
	}
	else if (primary_changed)
	{
		/*
		 * if Primary node was changed, We should restart all children
		 */
		need_to_restart_children = true;
		partial_restart = false;
		ereport(LOG,
				(errmsg("primary node was changed after the sync from \"%s\"", backendStatus->nodeName),
				 errdetail("all children needs to be restarted")));

	}
	else
	{
		if (node_status_was_changed_to_down == false)
		{
			/*
			 * no node was detached, So no need to restart any child process
			 */
			need_to_restart_children = false;
			partial_restart = false;
			ereport(LOG,
					(errmsg("No backend node was detached because of backend status sync from \"%s\"", backendStatus->nodeName),
					 errdetail("no need to restart children")));
		}
		else
		{
			ereport(LOG,
					(errmsg("%d backend node(s) were detached because of backend status sync from \"%s\"", down_node_ids_index, backendStatus->nodeName),
					 errdetail("restarting the children processes")));

			need_to_restart_children = true;
			partial_restart = !check_all_backend_down();
		}
	}

	/* Kill children and restart them if needed */
	if (need_to_restart_children)
	{
		for (i = 0; i < pool_config->num_init_children; i++)
		{
			bool		restart = false;

			if (partial_restart)
			{
				int			j,
							k;

				for (j = 0; j < pool_config->max_pool; j++)
				{
					for (k = 0; k < NUM_BACKENDS; k++)
					{
						int			idx;
						ConnectionInfo *con = pool_coninfo(i, j, k);

						for (idx = 0; idx < down_node_ids_index; idx++)
						{
							int			node_id = down_node_ids[idx];

							if (con->connected && con->load_balancing_node == node_id)
							{
								ereport(LOG,
										(errmsg("child process with PID:%d needs restart, because pool %d uses backend %d",
												process_info[i].pid, j, node_id)));
								restart = true;
								break;
							}
							if (restart)
								break;
						}
					}
				}
			}
			else
			{
				restart = true;
			}

			if (restart)
			{
				if (process_info[i].pid)
				{
					kill(process_info[i].pid, SIGQUIT);

					process_info[i].pid = fork_a_child(fds, i);
					process_info[i].start_time = time(NULL);
				}
			}
			else
				process_info[i].need_to_restart = 1;
		}
	}

	else
	{
		/*
		 * Set restart request to each child. Children will exit(1) whenever
		 * they are convenient.
		 */
		for (i = 0; i < pool_config->num_init_children; i++)
		{
			process_info[i].need_to_restart = 1;
		}
	}

	/*
	 * Send restart request to worker child.
	 */
	kill(worker_pid, SIGUSR1);

	/* Fork health check process if needed */
	for (i = 0; i < NUM_BACKENDS; i++)
	{
		if (health_check_pids[i] == 0)
		{
			ereport(LOG,
					(errmsg("start health check process for host %s(%d)",
							BACKEND_INFO(i).backend_hostname,
							BACKEND_INFO(i).backend_port)));

			health_check_pids[i] = worker_fork_a_child(PT_HEALTH_CHECK, do_health_check_child, &i);
		}
	}
}

/*
 * Obtain backend server version number and cache it.  Note that returned
 * version number is in the static memory area.
 */
static int
get_server_version(POOL_CONNECTION_POOL_SLOT * *slots, int node_id)
{
	static int	server_versions[MAX_NUM_BACKENDS];

	char	   *query;
	POOL_SELECT_RESULT *res;

	if (server_versions[node_id] == 0)
	{
		query = "SELECT current_setting('server_version_num')";

		/* Get backend server version. If the query fails, keep previous info. */
		if (get_query_result(slots, node_id, query, &res) == 0)
		{
			server_versions[node_id] = atoi(res->data[0]);
			ereport(DEBUG1,
					(errmsg("get_server_version: backend %d server version: %d", node_id, server_versions[node_id])));
			free_select_result(res);
		}
	}
	return server_versions[node_id];
}

/*
 * Get info from conninfo string.
 */
static void
get_info_from_conninfo(char *conninfo, char *host, int hostlen, char *port, int portlen)
{
	char	   *p;

	*host = '\0';
	*port = '\0';

	p = strstr(conninfo, "host");
	if (p)
	{
		/* skip "host=" */
		while (*p && *p++ != '=')
			;

		while (*p && hostlen-- && *p != ' ')
			*host++ = *p++;
		*host = '\0';
	}

	p = strstr(conninfo, "port");
	if (p)
	{
		/* skip "port=" */
		while (*p && *p++ != '=')
			;

		while (*p && portlen-- && *p != ' ')
			*port++ = *p++;
		*port = '\0';
	}
}

/*
 * Set backend status changed time for specified backend id.
 */
void
pool_set_backend_status_changed_time(int backend_id)
{
	time_t		tval;

	tval = time(NULL);
	BACKEND_INFO(backend_id).status_changed_time = tval;
}

/*
 * Acquire lock on follow primary command execution.  Follow primary command
 * and detach_false_primary must acquire this lock before execution because
 * they are conflicting each other.  If argument "block" is true, this
 * function will not return until it succeeds in acquiring the lock.  This
 * function returns true if succeeded in acquiring the lock.
 *
 * first arg:block is ignored when remote_request is set
 */
bool
pool_acquire_follow_primary_lock(bool block, bool remote_request)
{
	pool_sigset_t oldmask;
	volatile int	follow_primary_count;

	for (;;)
	{
		POOL_SETMASK2(&BlockSig, &oldmask);
		pool_semaphore_lock(FOLLOW_PRIMARY_SEM);
		follow_primary_count = Req_info->follow_primary_count;

		if (follow_primary_count <= 0)
		{
			/* the lock is not held by anyone */
			ereport(DEBUG1,
					(errmsg("pool_acquire_follow_primary_lock: lock was not held by anyone")));
			break;
		}
		else if (follow_primary_count > 0 && remote_request)
		{
			if (Req_info->follow_primary_lock_held_remotely)
			{
				/* The lock was already held by remote node and we only
				 * support one remote lock
				 */
				ereport(LOG,
						(errmsg("pool_acquire_follow_primary_lock: received remote locking request while lock is already held by the remote node")));

			}
			else
			{
				/* set the flag that watchdog has requested the lock */
				Req_info->follow_primary_lock_pending = true;
			}
			pool_semaphore_unlock(FOLLOW_PRIMARY_SEM);
			POOL_SETMASK(&oldmask);
			/* return and inform that the lock was held by someone */
			ereport(DEBUG1,
					(errmsg("pool_acquire_follow_primary_lock: lock was held by someone %d", follow_primary_count)));
			return false;
		}

		else if (follow_primary_count > 0 && !block)
		{
			pool_semaphore_unlock(FOLLOW_PRIMARY_SEM);
			POOL_SETMASK(&oldmask);
			/* return and inform that the lock was held by someone */
			ereport(DEBUG1,
					(errmsg("pool_acquire_follow_primary_lock: lock was held by someone %d", follow_primary_count)));
			return false;
		}

		pool_semaphore_unlock(FOLLOW_PRIMARY_SEM);
		POOL_SETMASK(&oldmask);
		ereport(DEBUG1,
				(errmsg("pool_acquire_follow_primary_lock: lock was held by someone %d sleeping...", follow_primary_count)));
		sleep(1);
	}

	/* acquire lock */
	Req_info->follow_primary_lock_held_remotely = remote_request;

	Req_info->follow_primary_count = 1;
	pool_semaphore_unlock(FOLLOW_PRIMARY_SEM);
	POOL_SETMASK(&oldmask);

	ereport(DEBUG1,
			(errmsg("pool_acquire_follow_primary_lock: succeeded in acquiring lock")));

	return true;
}

/*
 * Release lock on follow primary command execution.
 */
void
pool_release_follow_primary_lock(bool remote_request)
{
	pool_sigset_t oldmask;

	POOL_SETMASK2(&BlockSig, &oldmask);
	pool_semaphore_lock(FOLLOW_PRIMARY_SEM);
	if (remote_request)
	{
		if (Req_info->follow_primary_lock_held_remotely)
		{
			/* remote request can only release locks held by remote nodes */
			Req_info->follow_primary_count = 0;
			Req_info->follow_primary_lock_held_remotely = false;
			ereport(DEBUG1,
					(errmsg("pool_release_follow_primary_lock relased the remote lock")));
		}
		else if (Req_info->follow_primary_count)
		{
			/*
			 * we have received the release lock request from remote
			 * but the lock is not held by remote node.
			 * Just ignore the request
			 */
			ereport(DEBUG1,
					(errmsg("pool_release_follow_primary_lock is not relasing the lock since it was not held by remote node")));
		}
		/*
		 * Silently ignore, if we received the release request from remote while no lock was held.
		 * Also clear the pending lock request, As we only support single remote lock
		 */
		Req_info->follow_primary_lock_pending = false;

	}
	else /*local request */
	{
		/*
		 * if we have a pending lock request from watchdog
		 * do not remove the actual lock, Just clear the pending flag
		 */
		if (Req_info->follow_primary_lock_pending)
		{
			Req_info->follow_primary_lock_held_remotely = true;
			Req_info->follow_primary_count = 1;
			/* also clear the pending lock flag */
			Req_info->follow_primary_lock_pending = false;
			ereport(DEBUG1,
					(errmsg("pool_release_follow_primary_lock is not relasing the lock and shifting it to coordinator watchdog node")));
		}
		else
		{
			if (Req_info->follow_primary_lock_held_remotely)
			{
				/*
				 * Ideally this should not happen.
				 * yet if for some reason our local node is trying to release a lock
				 * that is held by remote node. Just produce a LOG message and release
				 * the lock
				 */
				ereport(LOG,
						(errmsg("pool_release_follow_primary_lock is relasing the remote lock by local request")));
			}
			Req_info->follow_primary_count = 0;
			Req_info->follow_primary_lock_held_remotely = false;
		}
	}
	pool_semaphore_unlock(FOLLOW_PRIMARY_SEM);
	POOL_SETMASK(&oldmask);

	ereport(DEBUG1,
			(errmsg("pool_release_follow_primary_lock called")));

}
