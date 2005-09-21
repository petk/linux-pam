/* pam_mail module */

/*
 * $Id$
 *
 * Written by Andrew Morgan <morgan@linux.kernel.org> 1996/3/11
 * $HOME additions by David Kinchlea <kinch@kinch.ark.com> 1997/1/7
 * mailhash additions by Chris Adams <cadams@ro.com> 1998/7/11
 */

#include "config.h"

#include <ctype.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>

#ifdef HAVE_PATHS_H
#include <paths.h>
#endif

#define DEFAULT_MAIL_DIRECTORY    PAM_PATH_MAILDIR
#define MAIL_FILE_FORMAT          "%s%s/%s"
#define MAIL_ENV_NAME             "MAIL"
#define MAIL_ENV_FORMAT           MAIL_ENV_NAME "=%s"
#define YOUR_MAIL_VERBOSE_FORMAT  "You have %s mail in %s."
#define YOUR_MAIL_STANDARD_FORMAT "You have %smail."
#define NO_MAIL_STANDARD_FORMAT   "No mail."

/*
 * here, we make a definition for the externally accessible function
 * in this file (this definition is required for static a module
 * but strongly encouraged generally) it is used to instruct the
 * modules include file to define the function prototypes.
 */

#define PAM_SM_SESSION
#define PAM_SM_AUTH

#include <security/pam_modules.h>
#include <security/_pam_macros.h>
#include <security/pam_modutil.h>
#include <security/pam_ext.h>

/* argument parsing */

#define PAM_DEBUG_ARG		0x0001
#define PAM_NO_LOGIN		0x0002
#define PAM_LOGOUT_TOO		0x0004
#define PAM_NEW_MAIL_DIR	0x0010
#define PAM_MAIL_SILENT		0x0020
#define PAM_NO_ENV		0x0040
#define PAM_HOME_MAIL		0x0100
#define PAM_EMPTY_TOO		0x0200
#define PAM_STANDARD_MAIL	0x0400
#define PAM_QUIET_MAIL		0x1000

static int
_pam_parse (const pam_handle_t *pamh, int flags, int argc,
	    const char **argv, char **maildir, size_t *hashcount)
{
    int ctrl=0;

    if (flags & PAM_SILENT) {
	ctrl |= PAM_MAIL_SILENT;
    }

    *hashcount = 0;

    /* step through arguments */
    for (; argc-- > 0; ++argv) {

	/* generic options */

	if (!strcmp(*argv,"debug"))
	    ctrl |= PAM_DEBUG_ARG;
	else if (!strcmp(*argv,"quiet"))
	    ctrl |= PAM_QUIET_MAIL;
	else if (!strcmp(*argv,"standard"))
	    ctrl |= PAM_STANDARD_MAIL | PAM_EMPTY_TOO;
	else if (!strncmp(*argv,"dir=",4)) {
	    *maildir = x_strdup(4+*argv);
	    if (*maildir != NULL) {
		D(("new mail directory: %s", *maildir));
		ctrl |= PAM_NEW_MAIL_DIR;
	    } else {
		pam_syslog (pamh, LOG_CRIT,
			    "failed to duplicate mail directory - ignored");
	    }
	} else if (!strncmp(*argv,"hash=",5)) {
	    char *ep = NULL;
	    *hashcount = strtoul(*argv+5,&ep,10);
	    if (!ep) {
		*hashcount = 0;
	    }
	} else if (!strcmp(*argv,"close")) {
	    ctrl |= PAM_LOGOUT_TOO;
	} else if (!strcmp(*argv,"nopen")) {
	    ctrl |= PAM_NO_LOGIN;
	} else if (!strcmp(*argv,"noenv")) {
	    ctrl |= PAM_NO_ENV;
	} else if (!strcmp(*argv,"empty")) {
	    ctrl |= PAM_EMPTY_TOO;
	} else {
	    pam_syslog(pamh,LOG_ERR,"pam_parse: unknown option; %s",*argv);
	}
    }

    if ((*hashcount != 0) && !(ctrl & PAM_NEW_MAIL_DIR)) {
	*maildir = x_strdup(DEFAULT_MAIL_DIRECTORY);
	ctrl |= PAM_NEW_MAIL_DIR;
    }

    return ctrl;
}

static int get_folder(pam_handle_t *pamh, int ctrl,
		      char **path_mail, char **folder_p, size_t hashcount)
{
    int retval;
    const char *user, *path;
    char *folder;
    const struct passwd *pwd=NULL;

    retval = pam_get_user(pamh, &user, NULL);
    if (retval != PAM_SUCCESS || user == NULL) {
	pam_syslog(pamh,LOG_ERR, "no user specified");
	return PAM_USER_UNKNOWN;
    }

    if (ctrl & PAM_NEW_MAIL_DIR) {
	path = *path_mail;
	if (*path == '~') {       /* support for $HOME delivery */
	    pwd = pam_modutil_getpwnam(pamh, user);
	    if (pwd == NULL) {
		pam_syslog(pamh,LOG_ERR, "user [%s] unknown", user);
		_pam_overwrite(*path_mail);
		_pam_drop(*path_mail);
		return PAM_USER_UNKNOWN;
	    }
	    /*
	     * "~/xxx" and "~xxx" are treated as same
	     */
	    if (!*++path || (*path == '/' && !*++path)) {
		pam_syslog(pamh,LOG_ALERT, "badly formed mail path [%s]", *path_mail);
		_pam_overwrite(*path_mail);
		_pam_drop(*path_mail);
		return PAM_ABORT;
	    }
	    ctrl |= PAM_HOME_MAIL;
	    if (hashcount != 0) {
		pam_syslog(pamh,LOG_ALERT, "can't do hash= and home directory mail");
	    }
	}
    } else {
	path = DEFAULT_MAIL_DIRECTORY;
    }

    /* put folder together */

    hashcount = hashcount < strlen(user) ? hashcount : strlen(user);

    if (ctrl & PAM_HOME_MAIL) {
	folder = malloc(sizeof(MAIL_FILE_FORMAT)
			+strlen(pwd->pw_dir)+strlen(path));
    } else {
	folder = malloc(sizeof(MAIL_FILE_FORMAT)+strlen(path)+strlen(user)
			+2*hashcount);
    }

    if (folder != NULL) {
	if (ctrl & PAM_HOME_MAIL) {
	    sprintf(folder, MAIL_FILE_FORMAT, pwd->pw_dir, "", path);
	} else {
	    size_t i;
	    char *hash = malloc(2*hashcount+1);

	    if (hash) {
		for (i = 0; i < hashcount; i++) {
		    hash[2*i] = '/';
		    hash[2*i+1] = user[i];
		}
		hash[2*i] = '\0';
		sprintf(folder, MAIL_FILE_FORMAT, path, hash, user);
		_pam_overwrite(hash);
		_pam_drop(hash);
	    } else {
	      _pam_drop(folder);
	      pam_syslog(pamh,LOG_CRIT, "out of memory for mail folder");
	      return PAM_BUF_ERR;
	    }
	}
	D(("folder =[%s]", folder));
    }

    /* tidy up */

    _pam_overwrite(*path_mail);
    _pam_drop(*path_mail);
    user = NULL;

    if (folder == NULL) {
	pam_syslog(pamh,LOG_CRIT, "out of memory for mail folder");
	return PAM_BUF_ERR;
    }

    *folder_p = folder;
    folder = NULL;

    return PAM_SUCCESS;
}

static const char *get_mail_status(int ctrl, const char *folder)
{
    const char *type = NULL;
    static char dir[256];
    struct stat mail_st;
    struct dirent **namelist;
    int i;

    if (stat(folder, &mail_st) == 0) {
	if (S_ISDIR(mail_st.st_mode)) { /* Assume Maildir format */
	    sprintf(dir, "%.250s/new", folder);
	    i = scandir(dir, &namelist, 0, alphasort);
	    if (i > 2) {
		type = "new";
		while (--i)
		    free(namelist[i]);
	    } else {
		while (--i >= 0)
		    free(namelist[i]);
		sprintf(dir, "%.250s/cur", folder);
		i = scandir(dir, &namelist, 0, alphasort);
		if (i > 2) {
		    type = "old";
		    while (--i)
			free(namelist[i]);
		} else if (ctrl & PAM_EMPTY_TOO) {
		    while (--i >= 0)
			free(namelist[i]);
		    type = "no";
		} else {
		    type = NULL;
		}
	    }
	} else {
	    if (mail_st.st_size > 0) {
		if (mail_st.st_atime < mail_st.st_mtime) /* new */
		    type = (ctrl & PAM_STANDARD_MAIL) ? "new " : "new";
		else /* old */
		    type = (ctrl & PAM_STANDARD_MAIL) ? "" : "old";
	    } else if (ctrl & PAM_EMPTY_TOO) {
		type = "no";
	    } else {
		type = NULL;
	    }
	}
    }

    memset(dir, 0, 256);
    memset(&mail_st, 0, sizeof(mail_st));
    D(("user has %s mail in %s folder", type, folder));
    return type;
}

static int report_mail(pam_handle_t *pamh, int ctrl
		       , const char *type, const char *folder)
{
    int retval;

    if (!(ctrl & PAM_MAIL_SILENT) ||
	((ctrl & PAM_QUIET_MAIL) && strcmp(type, "new")))
      {
	if (ctrl & PAM_STANDARD_MAIL)
	  if (!strcmp(type, "no"))
	    retval = pam_info (pamh, "%s", NO_MAIL_STANDARD_FORMAT);
	  else
	    retval = pam_info (pamh, YOUR_MAIL_STANDARD_FORMAT, type);
	else
	  retval = pam_info (pamh, YOUR_MAIL_VERBOSE_FORMAT, type, folder);
      }
    else
      {
	D(("keeping quiet"));
	retval = PAM_SUCCESS;
      }

    D(("returning %s", pam_strerror(pamh, retval)));
    return retval;
}

static int _do_mail(pam_handle_t *, int, int, const char **, int);

/* --- authentication functions --- */

PAM_EXTERN int
pam_sm_authenticate (pam_handle_t *pamh UNUSED, int flags UNUSED,
		     int argc UNUSED, const char **argv UNUSED)
{
    return PAM_IGNORE;
}

/* Checking mail as part of authentication */
PAM_EXTERN
int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc,
    const char **argv)
{
    if (!(flags & (PAM_ESTABLISH_CRED|PAM_DELETE_CRED)))
      return PAM_IGNORE;
    return _do_mail(pamh,flags,argc,argv,(flags & PAM_ESTABLISH_CRED));
}

/* --- session management functions --- */

PAM_EXTERN
int pam_sm_close_session(pam_handle_t *pamh,int flags,int argc
			 ,const char **argv)
{
    return _do_mail(pamh,flags,argc,argv,0);
}

/* Checking mail as part of the session management */
PAM_EXTERN
int pam_sm_open_session(pam_handle_t *pamh, int flags, int argc,
    const char **argv)
{
    return _do_mail(pamh,flags,argc,argv,1);
}


/* --- The Beaf (Tm) --- */

static int _do_mail(pam_handle_t *pamh, int flags, int argc,
    const char **argv, int est)
{
    int retval, ctrl;
    size_t hashcount;
    char *path_mail=NULL, *folder;
    const char *type;

    /*
     * this module (un)sets the MAIL environment variable, and checks if
     * the user has any new mail.
     */

    ctrl = _pam_parse(pamh, flags, argc, argv, &path_mail, &hashcount);

    /* Do we have anything to do? */

    if (flags & PAM_SILENT)
	return PAM_SUCCESS;

    /* which folder? */

    retval = get_folder(pamh, ctrl, &path_mail, &folder, hashcount);
    if (retval != PAM_SUCCESS) {
	D(("failed to find folder"));
	return retval;
    }

    /* set the MAIL variable? */

    if (!(ctrl & PAM_NO_ENV) && est) {
	char *tmp;

	tmp = malloc(strlen(folder)+sizeof(MAIL_ENV_FORMAT));
	if (tmp != NULL) {
	    sprintf(tmp, MAIL_ENV_FORMAT, folder);
	    D(("setting env: %s", tmp));
	    retval = pam_putenv(pamh, tmp);
	    _pam_overwrite(tmp);
	    _pam_drop(tmp);
	    if (retval != PAM_SUCCESS) {
		_pam_overwrite(folder);
		_pam_drop(folder);
		pam_syslog(pamh,LOG_CRIT, "unable to set " MAIL_ENV_NAME " variable");
		return retval;
	    }
	} else {
	    pam_syslog(pamh,LOG_CRIT, "no memory for " MAIL_ENV_NAME " variable");
	    _pam_overwrite(folder);
	    _pam_drop(folder);
	    return retval;
	}
    } else {
	D(("not setting " MAIL_ENV_NAME " variable"));
    }

    /*
     * OK. we've got the mail folder... what about its status?
     */

    if ((est && !(ctrl & PAM_NO_LOGIN))
	|| (!est && (ctrl & PAM_LOGOUT_TOO))) {
	type = get_mail_status(ctrl, folder);
	if (type != NULL) {
	    retval = report_mail(pamh, ctrl, type, folder);
	    type = NULL;
	}
    }

    /* Delete environment variable? */
    if ( ! est && ! (ctrl & PAM_NO_ENV) )
	(void) pam_putenv(pamh, MAIL_ENV_NAME);

    _pam_overwrite(folder); /* clean up */
    _pam_drop(folder);

    /* indicate success or failure */

    return retval;
}

#ifdef PAM_STATIC

/* static module data */

struct pam_module _pam_mail_modstruct = {
     "pam_mail",
     pam_sm_authenticate,
     pam_sm_setcred,
     NULL,
     pam_sm_open_session,
     pam_sm_close_session,
     NULL,
};

#endif

/* end of module definition */
