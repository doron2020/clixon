/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand and Benny Holmgren

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <ifaddrs.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/types.h>
#include <grp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <libgen.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_backend_handle.h"
#include "backend_socket.h"
#include "backend_client.h"
#include "backend_plugin.h"
#include "backend_commit.h"
#include "backend_handle.h"
#include "backend_startup.h"

/* Command line options to be passed to getopt(3) */
#define BACKEND_OPTS "hD:f:l:d:p:b:Fza:u:P:1s:c:g:y:x:o:" 

#define BACKEND_LOGFILE "/usr/local/var/clixon_backend.log"

/*! Clean and close all state of backend (but dont exit). 
 * Cannot use h after this 
 * @param[in]  h  Clixon handle
 */
static int
backend_terminate(clicon_handle h)
{
    yang_spec *yspec;
    char      *pidfile = clicon_backend_pidfile(h);
    int       sockfamily = clicon_sock_family(h);
    char      *sockpath = clicon_sock(h);
    cxobj     *x;
    struct stat st;
    int        ss;

    clicon_debug(1, "%s", __FUNCTION__);
    if ((ss = clicon_socket_get(h)) != -1)
	close(ss);
    if ((x = clicon_module_state_get(h)) != NULL)
	xml_free(x);
    if ((x = clicon_xml_changelog_get(h)) != NULL)
	xml_free(x);
    if ((yspec = clicon_dbspec_yang(h)) != NULL)
	yspec_free(yspec);
    if ((yspec = clicon_config_yang(h)) != NULL)
	yspec_free(yspec);
    if ((x = clicon_nacm_ext(h)) != NULL)
	xml_free(x);
    if ((x = clicon_conf_xml(h)) != NULL)
	xml_free(x);
    stream_publish_exit();
    clixon_plugin_exit(h);
    /* Delete all backend plugin RPC callbacks */
    rpc_callback_delete_all(h);
    /* Delete all backend plugin upgrade callbacks */
    upgrade_callback_delete_all(h); 

    if (pidfile)
	unlink(pidfile);   
    if (sockfamily==AF_UNIX && lstat(sockpath, &st) == 0)
	unlink(sockpath);
	
    xmldb_plugin_unload(h); /* unload storage plugin */
    backend_handle_exit(h); /* Also deletes streams. Cannot use h after this. */
    event_exit();
    clicon_debug(1, "%s done", __FUNCTION__); 
    clicon_log_exit();
    return 0;
}

/*! Unlink pidfile and quit
 */
static void
backend_sig_term(int arg)
{
    static int i=0;

    if (i++ == 0)
	clicon_log(LOG_NOTICE, "%s: %s: pid: %u Signal %d", 
		   __PROGRAM__, __FUNCTION__, getpid(), arg);
    clicon_exit_set(); /* checked in event_loop() */
}

/*! Create backend server socket and register callback
 * @param[in]  h    Clicon handle
 * @retval     s    Server socket file descriptor (see socket(2))
 * @retval    -1    Error
 */
static int
backend_server_socket(clicon_handle h)
{
    int ss;

    /* Open control socket */
    if ((ss = backend_socket_init(h)) < 0)
	return -1;
    /* ss is a server socket that the clients connect to. The callback
       therefore accepts clients on ss */
    if (event_reg_fd(ss, backend_accept_client, h, "server socket") < 0) {
	close(ss);
	return -1;
    }
    return ss;
}

/*! Call plugin_start with -- user options */
static int
plugin_start_useroptions(clicon_handle h,
			 char         *argv0,
			 int           argc,
			 char        **argv)
{
    char *tmp;

    tmp = *(argv-1);
    *(argv-1) = argv0;
    if (clixon_plugin_start(h, argc+1, argv-1) < 0) 
	return -1;
    *(argv-1) = tmp;
    return 0;
}

/*! Load external NACM file
 */
static int
nacm_load_external(clicon_handle h)
{
    int         retval = -1;
    char       *filename; /* NACM config file */
    yang_spec  *yspec = NULL;
    cxobj      *xt = NULL;
    struct stat st;
    FILE       *f = NULL;
    int         fd;

    filename = clicon_option_str(h, "CLICON_NACM_FILE");
    if (filename == NULL || strlen(filename)==0){
	clicon_err(OE_UNIX, errno, "CLICON_NACM_FILE not set in NACM external mode");
	goto done;
    }
    if (stat(filename, &st) < 0){
	clicon_err(OE_UNIX, errno, "%s", filename);
	goto done;
    }
    if (!S_ISREG(st.st_mode)){
	clicon_err(OE_UNIX, 0, "%s is not a regular file", filename);
	goto done;
    }
    if ((f = fopen(filename, "r")) == NULL) {
	clicon_err(OE_UNIX, errno, "configure file: %s", filename);
	return -1;
    }
    if ((yspec = yspec_new()) == NULL)
	goto done;
    if (yang_spec_parse_module(h, "ietf-netconf-acm", NULL, yspec) < 0)
	goto done;
    fd = fileno(f);
    /* Read configfile */
    if (xml_parse_file(fd, "</clicon>", yspec, &xt) < 0)
	goto done;
    if (xt == NULL){
	clicon_err(OE_XML, 0, "No xml tree in %s", filename);
	goto done;
    }
    if (clicon_nacm_ext_set(h, xt) < 0)
	goto done;
    retval = 0;
 done:
    if (yspec) /* The clixon yang-spec is not used after this */
	yspec_free(yspec);
    if (f)
	fclose(f);
    return retval;
}

/*! Given a retval, transform to status or fatal error
 *
 * @param[in]  ret    Return value from xml validation function
 * @param[out] status Transform status according to rules below
 * @retval    0       OK, status set
 * @retval   -1       Fatal error outside scope of startup_status
 * Transformation rules:
 * 1) retval -1 assume clicon_errno/suberrno set. Special case from xml parser
 * is clicon_suberrno = XMLPARSE_ERRNO which assumes an XML (non-fatal) parse 
 * error which translates to -> STARTUP_ERR
 * All other error cases translates to fatal error
 * 2) retval 0 is xml validation fails -> STARTUP_INVALID
 * 3) retval 1 is OK -> STARTUP_OK
 * 4) any other retval translates to fatal error
 */
static int
ret2status(int                  ret,
	   enum startup_status *status)
{
    int retval = -1;

    switch (ret){
    case -1:
	if (clicon_suberrno != XMLPARSE_ERRNO)
	    goto done;
	clicon_err_reset();
	*status = STARTUP_ERR;
	break;
    case 0:
	*status = STARTUP_INVALID;
	break;
    case 1:
	*status = STARTUP_OK;
	break;
    default:
	clicon_err(OE_CFG, EINVAL, "No such retval %d", retval);
    } /* switch */
    retval = 0;
 done:
    return retval;
}

/*! usage
 */
static void
usage(clicon_handle h,
      char         *argv0)
{
    char *plgdir   = clicon_backend_dir(h);
    char *confsock = clicon_sock(h);
    char *confpid  = clicon_backend_pidfile(h);
    char *group    = clicon_sock_group(h);

    fprintf(stderr, "usage:%s <options>*\n"
	    "where options are\n"
            "\t-h\t\tHelp\n"
    	    "\t-D <level>\tDebug level\n"
    	    "\t-f <file>\tCLICON config file\n"
	    "\t-l (s|e|o|f<file>)  Log on (s)yslog, std(e)rr or std(o)ut (stderr is default) Only valid if -F, if background syslog is on syslog.\n"
	    "\t-d <dir>\tSpecify backend plugin directory (default: %s)\n"
	    "\t-p <dir>\tYang directory path (see CLICON_YANG_DIR)\n"
	    "\t-b <dir>\tSpecify XMLDB database directory\n"
    	    "\t-F\t\tRun in foreground, do not run as daemon\n"
    	    "\t-z\t\tKill other config daemon and exit\n"
    	    "\t-a UNIX|IPv4|IPv6  Internal backend socket family\n"
    	    "\t-u <path|addr>\tInternal socket domain path or IP addr (see -a)(default: %s)\n"
    	    "\t-P <file>\tPid filename (default: %s)\n"
    	    "\t-1\t\tRun once and then quit (dont wait for events)\n"
	    "\t-s <mode>\tSpecify backend startup mode: none|startup|running|init)\n"
	    "\t-c <file>\tLoad extra xml configuration, but don't commit.\n"
	    "\t-g <group>\tClient membership required to this group (default: %s)\n"

	    "\t-y <file>\tLoad yang spec file (override yang main module)\n"
	    "\t-x <plugin>\tXMLDB plugin\n"
	    "\t-o \"<option>=<value>\"\tGive configuration option overriding config file (see clixon-config.yang)\n",
	    argv0,
	    plgdir ? plgdir : "none",
	    confsock ? confsock : "none",
	    confpid ? confpid : "none",
	    group ? group : "none"
	    );
    exit(-1);
}

int
main(int    argc,
     char **argv)
{
    int           retval = -1;
    int           c;
    int           zap;
    int           foreground;
    int           once;
    enum startup_mode_t startup_mode;
    char         *extraxml_file;
    char         *config_group;
    char         *argv0 = argv[0];
    struct stat   st;
    clicon_handle h;
    int           help = 0;
    int           pid;
    char         *pidfile;
    char         *sock;
    int           sockfamily;
    char         *xmldb_plugin;
    int           xml_cache;
    char         *xml_format;
    char         *nacm_mode;
    int           logdst = CLICON_LOG_SYSLOG|CLICON_LOG_STDERR;
    yang_spec    *yspec = NULL;
    yang_spec    *yspecfg = NULL; /* For config XXX clixon bug */
    char         *str;
    int           ss = -1; /* server socket */
    cbuf         *cbret = NULL; /* startup cbuf if invalid */
    enum startup_status status = STARTUP_ERR; /* Startup status */
    int           ret;
    
    /* In the startup, logs to stderr & syslog and debug flag set later */
    clicon_log_init(__PROGRAM__, LOG_INFO, logdst);
    /* Initiate CLICON handle */
    if ((h = backend_handle_init()) == NULL)
	return -1;
    foreground = 0;
    once = 0;
    zap = 0;
    extraxml_file = NULL;

    /*
     * Command-line options for help, debug, and config-file
     */
    opterr = 0;
    optind = 1;
    while ((c = getopt(argc, argv, BACKEND_OPTS)) != -1)
	switch (c) {
	case 'h':
	    /* Defer the call to usage() to later. Reason is that for helpful
	       text messages, default dirs, etc, are not set until later.
	       But this measn that we need to check if 'help' is set before 
	       exiting, and then call usage() before exit.
	    */
	    help = 1; 
	    break;
	case 'D' : /* debug */
	    if (sscanf(optarg, "%d", &debug) != 1)
		usage(h, argv[0]);
	    break;
	case 'f': /* config file */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_CONFIGFILE", optarg);
	    break;
	case 'l': /* Log destination: s|e|o */
	    if ((logdst = clicon_log_opt(optarg[0])) < 0)
		usage(h, argv[0]);
	    if (logdst == CLICON_LOG_FILE &&
		strlen(optarg)>1 &&
		clicon_log_file(optarg+1) < 0)
		goto done;
	    break;
	}
    /* 
     * Here we have the debug flag settings, use that.
     * Syslogs also to stderr, but later turn stderr off in daemon mode. 
     * error only to syslog. debug to syslog
     * XXX: if started in a start-daemon script, there will be irritating
     * double syslogs until fork below. 
     */
    clicon_log_init(__PROGRAM__, debug?LOG_DEBUG:LOG_INFO, logdst); 
    clicon_debug_init(debug, NULL);

    /* Create configure yang-spec */
    if ((yspecfg = yspec_new()) == NULL)
	goto done;

    /* Find and read configfile */
    if (clicon_options_main(h, yspecfg) < 0){
	if (help)
	    usage(h, argv[0]);
	return -1;
    }
    clicon_config_yang_set(h, yspecfg);
    /* External NACM file? */
    nacm_mode = clicon_option_str(h, "CLICON_NACM_MODE");
    if (nacm_mode && strcmp(nacm_mode, "external") == 0)
	if (nacm_load_external(h) < 0)
	    goto done;
    
    /* Now run through the operational args */
    opterr = 1;
    optind = 1;
    while ((c = getopt(argc, argv, BACKEND_OPTS)) != -1)
	switch (c) {
	case 'h' : /* help */
	case 'D' : /* debug */
	case 'f': /* config file */
	case 'l' :
	    break; /* see above */
	case 'd':  /* Plugin directory */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    if (clicon_option_add(h, "CLICON_BACKEND_DIR", optarg) < 0)
		goto done;
	    break;
	case 'b':  /* XMLDB database directory */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    if (clicon_option_add(h, "CLICON_XMLDB_DIR", optarg) < 0)
		goto done;
	    break;
	case 'p' : /* yang dir path */
	    if (clicon_option_add(h, "CLICON_YANG_DIR", optarg) < 0)
		goto done;
	    break;
	case 'F' : /* foreground */
	    foreground = 1;
	    break;
	case 'z': /* Zap other process */
	    zap++;
	    break;
	case 'a': /* internal backend socket address family */
	    if (clicon_option_add(h, "CLICON_SOCK_FAMILY", optarg) < 0)
		goto done;
	    break;
	case 'u': /* config unix domain path / ip address */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    if (clicon_option_add(h, "CLICON_SOCK", optarg) < 0)
		goto done;
	    break;
	case 'P': /* pidfile */
	    if (clicon_option_add(h, "CLICON_BACKEND_PIDFILE", optarg) < 0)
		goto done;
	    break;
	case '1' : /* Quit after reading database once - dont wait for events */
	    once = 1;
	    break;
	case 's' : /* startup mode */
	    if (clicon_option_add(h, "CLICON_STARTUP_MODE", optarg) < 0)
		goto done;
	    if (clicon_startup_mode(h) < 0){
		fprintf(stderr, "Invalid startup mode: %s\n", optarg);
		usage(h, argv[0]);
	    }
	    break;
	case 'c': /* Load application config */
	    extraxml_file = optarg;
	    break;
	case 'g': /* config socket group */
	    if (clicon_option_add(h, "CLICON_SOCK_GROUP", optarg) < 0)
		goto done;
	    break;
	case 'y' : /* Load yang absolute filename */
	    if (clicon_option_add(h, "CLICON_YANG_MAIN_FILE", optarg) < 0)
		goto done;
	    break;
	case 'x' : /* xmldb plugin */
	    if (clicon_option_add(h, "CLICON_XMLDB_PLUGIN", optarg) < 0)
		goto done;
	    break;
	case 'o':{ /* Configuration option */
	    char          *val;
	    if ((val = index(optarg, '=')) == NULL)
		usage(h, argv0);
	    *val++ = '\0';
	    if (clicon_option_add(h, optarg, val) < 0)
		goto done;
	    break;
	}
	default:
	    usage(h, argv[0]);
	    break;
	}

    argc -= optind;
    argv += optind;

    /* Access the remaining argv/argc options (after --) w clicon-argv_get() */
    clicon_argv_set(h, argv0, argc, argv);
    
    clicon_log_init(__PROGRAM__, debug?LOG_DEBUG:LOG_INFO, logdst); 

    /* Defer: Wait to the last minute to print help message */
    if (help)
	usage(h, argv[0]);

    /* Check pid-file, if zap kil the old daemon, else return here */
    if ((pidfile = clicon_backend_pidfile(h)) == NULL){
	clicon_err(OE_FATAL, 0, "pidfile not set");
	goto done;
    }
    sockfamily = clicon_sock_family(h);
    if ((sock = clicon_sock(h)) == NULL){
	clicon_err(OE_FATAL, 0, "sock not set");
	goto done;
    }
    if (pidfile_get(pidfile, &pid) < 0)
	return -1;
    if (zap){
	if (pid && pidfile_zapold(pid) < 0)
	    return -1;
	if (lstat(pidfile, &st) == 0)
	    unlink(pidfile);   
	if (sockfamily==AF_UNIX && lstat(sock, &st) == 0)
	    unlink(sock);   
	backend_terminate(h);
	exit(0); /* OK */
    }
    else
	if (pid){
	    clicon_err(OE_DEMON, 0, "Daemon already running with pid %d\n(Try killing it with %s -z)", 
		       pid, argv0);
	    return -1; /* goto done deletes pidfile */
	}

    /* After this point we can goto done on error 
     * Here there is either no old process or we have killed it,.. 
     */
    if (lstat(pidfile, &st) == 0)
	unlink(pidfile);   
    if (sockfamily==AF_UNIX && lstat(sock, &st) == 0)
	unlink(sock);   

    /* Sanity check: config group exists */
    if ((config_group = clicon_sock_group(h)) == NULL){
	clicon_err(OE_FATAL, 0, "clicon_sock_group option not set");
	return -1;
    }

    if (group_name2gid(config_group, NULL) < 0){
	clicon_log(LOG_ERR, "'%s' does not seem to be a valid user group.\n" /* \n required here due to multi-line log */
		"The config demon requires a valid group to create a server UNIX socket\n"
		"Define a valid CLICON_SOCK_GROUP in %s or via the -g option\n"
		"or create the group and add the user to it. On linux for example:"
		"  sudo groupadd %s\n" 
		"  sudo usermod -a -G %s user\n", 
		   config_group, clicon_configfile(h),
		   config_group, config_group);
	return -1;
    }

    /* Publish stream on pubsub channels.
     * CLICON_STREAM_PUB should be set to URL to where streams are published
     * and configure should be run with --enable-publish
     */
    if (clicon_option_exists(h, "CLICON_STREAM_PUB") &&
	stream_publish_init() < 0)
	goto done;
    if ((xmldb_plugin = clicon_xmldb_plugin(h)) == NULL){
	clicon_log(LOG_ERR, "No xmldb plugin given (specify option CLICON_XMLDB_PLUGIN)."); 
	goto done;
    }
    if (xmldb_plugin_load(h, xmldb_plugin) < 0)
	goto done;
    /* Connect to plugin to get a handle */
    if (xmldb_connect(h) < 0)
	goto done;

    /* Create top-level yang spec and store as option */
    if ((yspec = yspec_new()) == NULL)
	goto done;
    clicon_dbspec_yang_set(h, yspec);	
    /* Load Yang modules
     * 1. Load a yang module as a specific absolute filename */
    if ((str = clicon_yang_main_file(h)) != NULL)
	if (yang_spec_parse_file(h, str, yspec) < 0)
	    goto done;
    /* 2. Load a (single) main module */
    if ((str = clicon_yang_module_main(h)) != NULL)
	if (yang_spec_parse_module(h, str, clicon_yang_module_revision(h),
				   yspec) < 0)
	    goto done;
    /* 3. Load all modules in a directory (will not overwrite file loaded ^) */
    if ((str = clicon_yang_main_dir(h)) != NULL)
	if (yang_spec_load_dir(h, str, yspec) < 0)
	    goto done;
     /* Load clixon lib yang module */
    if (yang_spec_parse_module(h, "clixon-lib", NULL, yspec) < 0)
	goto done;
     /* Load yang module library, RFC7895 */
    if (yang_modules_init(h) < 0)
	goto done;
    /* Add netconf yang spec, used by netconf client and as internal protocol */
    if (netconf_module_load(h) < 0)
	goto done;
    /* Load yang Restconf stream discovery */
     if (clicon_option_bool(h, "CLICON_STREAM_DISCOVERY_RFC8040") &&
	 yang_spec_parse_module(h, "ietf-restconf-monitoring", NULL, yspec)< 0)
	 goto done;
     /* Load yang Netconf stream discovery */
     if (clicon_option_bool(h, "CLICON_STREAM_DISCOVERY_RFC5277") &&
	 yang_spec_parse_module(h, "clixon-rfc5277", NULL, yspec)< 0)
	 goto done;
    /* Set options: database dir and yangspec (could be hidden in connect?)*/
    if (xmldb_setopt(h, "dbdir", clicon_xmldb_dir(h)) < 0)
	goto done;
    if (xmldb_setopt(h, "yangspec", clicon_dbspec_yang(h)) < 0)
	goto done;
    if ((xml_cache = clicon_option_bool(h, "CLICON_XMLDB_CACHE")) >= 0)
	if (xmldb_setopt(h, "xml_cache", (void*)(intptr_t)xml_cache) < 0)
	    goto done;
    if ((xml_format = clicon_option_str(h, "CLICON_XMLDB_FORMAT")) >= 0)
	if (xmldb_setopt(h, "format", (void*)xml_format) < 0)
	    goto done;
    if (xmldb_setopt(h, "pretty", (void*)(intptr_t)clicon_option_bool(h, "CLICON_XMLDB_PRETTY")) < 0)
	goto done;
    if (xmldb_setopt(h, "nacm_mode", (void*)nacm_mode) < 0)
	goto done;
    if (xmldb_setopt(h, "nacm_xtree", (void*)clicon_nacm_ext(h)) < 0)
	goto done;

    /* Initialize server socket and save it to handle */
    if (backend_rpc_init(h) < 0)
	goto done;

    /* Must be after netconf_module_load, but before startup code */
    if (clicon_option_bool(h, "CLICON_XML_CHANGELOG"))
	if (clixon_xml_changelog_init(h) < 0)
	    goto done;
    
    /* Save modules state of the backend (server). Compare with startup XML */
    if (startup_module_state(h, yspec) < 0)
	goto done;
    /* Startup mode needs to be defined,  */
    startup_mode = clicon_startup_mode(h);
    if (startup_mode == -1){ 	
	clicon_log(LOG_ERR, "Startup mode undefined. Specify option CLICON_STARTUP_MODE or specify -s option to clicon_backend."); 
	goto done;
    }
    /* Init running db if it is not there
     */
    if (xmldb_exists(h, "running") != 1)
	if (xmldb_create(h, "running") < 0)
	    return -1;
    /* If startup fails, lib functions report invalidation info in a cbuf */
    if ((cbret = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    switch (startup_mode){
    case SM_INIT: /* Scratch running and start from empty */
	/* [Delete and] create running db */
	if (startup_db_reset(h, "running") < 0)
	    goto done;
    case SM_NONE: /* Fall through *
		   * Load plugins and call plugin_init() */
	if (backend_plugin_initiate(h) != 0) 
	    goto done;
	status = STARTUP_OK;
	break;
    case SM_RUNNING: /* Use running as startup */
	/* Copy original running to startup and treat as startup */
	if (xmldb_copy(h, "running", "tmp") < 0)
	    goto done;
	ret = startup_mode_startup(h, "tmp", cbret);
	if (ret2status(ret, &status) < 0)
	    goto done;
	break;
    case SM_STARTUP: 
	/* Load and commit from startup */
	ret = startup_mode_startup(h, "startup", cbret);
	if (ret2status(ret, &status) < 0)
	    goto done;
	/* if status = STARTUP_INVALID, cbret contains info */
    }
    /* Merge extra XML from file and reset function to running  
     */
    if (status == STARTUP_OK && startup_mode != SM_NONE){
	if ((ret = startup_extraxml(h, extraxml_file, cbret)) < 0)
	    goto done;
	if (ret2status(ret, &status) < 0)
	    goto done;
	/* if status = STARTUP_INVALID, cbret contains info */
    }

    if (status != STARTUP_OK){
	if (cbuf_len(cbret))
	    clicon_log(LOG_NOTICE, "%s: %u %s", __PROGRAM__, getpid(), cbuf_get(cbret));
	if (startup_failsafe(h) < 0){
	    goto done;
	}
    }
    
    /* Initiate the shared candidate. */
    if (xmldb_copy(h, "running", "candidate") < 0)
	goto done;
    /* Set startup status */
    if (clicon_startup_status_set(h, status) < 0)
	goto done;

    if (status == STARTUP_INVALID && cbuf_len(cbret))
	clicon_log(LOG_NOTICE, "%s: %u %s", __PROGRAM__, getpid(), cbuf_get(cbret));
	
    /* Call backend plugin_start with user -- options */
    if (plugin_start_useroptions(h, argv0, argc, argv) <0)
	goto done;
    if (once)
	goto done;

    /* Daemonize and initiate logging. Note error is initiated here to make
       demonized errors OK. Before this stage, errors are logged on stderr 
       also */
    if (foreground==0){
	clicon_log_init(__PROGRAM__, debug?LOG_DEBUG:LOG_INFO, CLICON_LOG_SYSLOG);
	if (daemon(0, 0) < 0){
	    fprintf(stderr, "config: daemon");
	    exit(-1);
	}
    }
    /* Write pid-file */
    if ((pid = pidfile_write(pidfile)) <  0)
	goto done;

    clicon_log(LOG_NOTICE, "%s: %u Started", __PROGRAM__, getpid());
    if (set_signal(SIGTERM, backend_sig_term, NULL) < 0){
	clicon_err(OE_DEMON, errno, "Setting signal");
	goto done;
    }
    if (set_signal(SIGINT, backend_sig_term, NULL) < 0){
	clicon_err(OE_DEMON, errno, "Setting signal");
	goto done;
    }
	
    /* Initialize server socket and save it to handle */
    if ((ss = backend_server_socket(h)) < 0)
	goto done;
    if (clicon_socket_set(h, ss) < 0)
	goto done;
    if (debug)
	clicon_option_dump(h, debug);

    if (stream_timer_setup(0, h) < 0)
	goto done;
    if (event_loop() < 0)
	goto done;
    retval = 0;
  done:
    if (cbret)
	cbuf_free(cbret);
    clicon_log(LOG_NOTICE, "%s: %u Terminated retval:%d", __PROGRAM__, getpid(), retval);
    backend_terminate(h); /* Cannot use h after this */

    return retval;
}
