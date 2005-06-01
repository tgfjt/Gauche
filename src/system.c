/*
 * system.c - system interface
 *
 *   Copyright (c) 2000-2004 Shiro Kawai, All rights reserved.
 * 
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 * 
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   3. Neither the name of the authors nor the names of its contributors
 *      may be used to endorse or promote products derived from this
 *      software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  $Id: system.c,v 1.64 2005-06-01 18:47:54 shirok Exp $
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <dirent.h>
#include <locale.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifndef __MINGW32__
#include <grp.h>
#include <pwd.h>
#else   /*__MINGW32__*/
#include <windows.h>
#include <lm.h>
#include <tlhelp32.h>
#endif  /*__MINGW32__*/

#define LIBGAUCHE_BODY
#include "gauche.h"
#include "gauche/class.h"
#include "gauche/builtin-syms.h"

#ifdef HAVE_GLOB_H
#include <glob.h>
#endif

/*
 * Auxiliary system interface functions.   See syslib.stub for
 * Scheme binding.
 */

/*
 * Conversion between off_t and Scheme integer.
 * off_t might be either 32bit or 64bit.  However, as far as I know,
 * on ILP32 machines off_t is kept 32bits for compabitility and
 * a separate off64_t is defined for 64bit offset access.
 * To aim completeness I have to support the case that
 * sizeof(off_t) > sizeof(long).  For the time being, I just signal
 * an error outside the long value.
 */
off_t Scm_IntegerToOffset(ScmObj i)
{
    if (SCM_INTP(i)) {
        return (off_t)SCM_INT_VALUE(i);
    } else if (SCM_BIGNUMP(i)) {
        if (SCM_BIGNUM_SIZE(i) > 1
            || SCM_BIGNUM(i)->values[0] > LONG_MAX) {
            Scm_Error("offset value too large: %S", i);
        }
        return (off_t)Scm_GetInteger(i);
    }
    Scm_Error("bad value as offset: %S", i);
    return (off_t)-1;       /* dummy */
}

ScmObj Scm_OffsetToInteger(off_t off)
{
#if SIZEOF_OFF_T == SIZEOF_LONG
    return Scm_MakeInteger(off);
#else
    if (off <= LONG_MAX && off >= LONG_MIN) {
        return Scm_MakeInteger(off);
    } else {
        Scm_Error("offset value too large to support");
        return Scm_MakeInteger(-1); /* dummy */
    }
#endif
}

/*===============================================================
 * OBSOLETED: Wrapper to the system call to handle signals.
 * Use SCM_SYSCALL_{I|P} macro instead.
 */
int Scm_SysCall(int r)
{
    Scm_Warn("Obsoleted API Scm_SysCall is called.");
    if (r < 0 && errno == EINTR) {
        ScmVM *vm = Scm_VM();
        errno = 0;
        SCM_SIGCHECK(vm);
    }
    return r;
}

void *Scm_PtrSysCall(void *r)
{
    Scm_Warn("Obsoleted API Scm_PtrSysCall is called.");
    if (r == NULL && errno == EINTR) {
        ScmVM *vm = Scm_VM();
        errno = 0;
        SCM_SIGCHECK(vm);
    }
    return r;
}

/*
 * A utility function for the procedures that accepts either port or
 * integer file descriptor.  Returns the file descriptor.  If port_or_fd
 * is a port that is not associated with the system file, and needfd is
 * true, signals error.  Otherwise it returns -1.
 */
int Scm_GetPortFd(ScmObj port_or_fd, int needfd)
{
    int fd = -1;
    if (SCM_INTP(port_or_fd)) {
        fd = SCM_INT_VALUE(port_or_fd);
    } else if (SCM_PORTP(port_or_fd)) {
        fd = Scm_PortFileNo(SCM_PORT(port_or_fd));
        if (fd < 0 && needfd) {
            Scm_Error("the port is not associated with a system file descriptor: %S",
                      port_or_fd);
        }
    } else {
        Scm_Error("port or small integer required, but got %S", port_or_fd);
    }
    return fd;
}

/*===============================================================
 * Directory primitives (dirent.h)
 *   We don't provide the iterator primitives, but a function which
 *   reads entire directory.
 */

/* Returns a list of directory entries.  If pathname is not a directory,
   or can't be opened by some reason, an error is signalled. */
ScmObj Scm_ReadDirectory(ScmString *pathname)
{
    ScmObj head = SCM_NIL, tail = SCM_NIL;
    ScmVM *vm = Scm_VM();
    struct dirent *dire;
    DIR *dirp = opendir(Scm_GetStringConst(pathname));
    
    if (dirp == NULL) {
        SCM_SIGCHECK(vm);
        Scm_SysError("couldn't open directory %S", pathname);
    }
    while ((dire = readdir(dirp)) != NULL) {
        ScmObj ent = SCM_MAKE_STR_COPYING(dire->d_name);
        SCM_APPEND1(head, tail, ent);
    }
    SCM_SIGCHECK(vm);
    closedir(dirp);
    return head;
}

/* Glob()function. */
/* TODO: allow to take optional flags */
ScmObj Scm_GlobDirectory(ScmString *pattern)
{
#ifdef HAVE_GLOB_H
    glob_t globbed;
    ScmObj head = SCM_NIL, tail = SCM_NIL;
    int i, r;
    SCM_SYSCALL(r, glob(Scm_GetStringConst(pattern), 0, NULL, &globbed));
    if (r) {
        globfree(&globbed);
#ifdef GLOB_NOMATCH
        if (r == GLOB_NOMATCH) return SCM_NIL;
#endif /*!GLOB_NOMATCH*/
        Scm_Error("Couldn't glob %S", pattern);
    }
    for (i = 0; i < globbed.gl_pathc; i++) {
        ScmObj path = SCM_MAKE_STR_COPYING(globbed.gl_pathv[i]);
        SCM_APPEND1(head, tail, path);
    }
    globfree(&globbed);
    return head;
#else  /*!HAVE_GLOB_H*/
    Scm_Error("glob-directory is not supported on this architecture.");
    return SCM_UNDEFINED;
#endif /*!HAVE_GLOB_H*/
}

/*
 * Pathname manipulation
 */

/* Returns the system's native pathname delimiter. */
const char *Scm_PathDelimiter(void)
{
#ifndef __MINGW32__
    return "/";
#else  /*__MINGW32__*/
    return "\\";
#endif /*__MINGW32__*/
}

ScmObj Scm_NormalizePathname(ScmString *pathname, int flags)
{
    const char *str = SCM_STRING_START(pathname), *srcp = str;
    int size = SCM_STRING_SIZE(pathname);
    char *buf = NULL, *dstp;
    int bottomp = FALSE;
    
#define SKIP_SLASH \
    while ((*srcp == '/' || *srcp == '\\') && srcp < str+size) { srcp++; }

    if ((flags & SCM_PATH_EXPAND) && size >= 1 && *str == '~') {
#ifndef __MINGW32__
        /* ~user magic */
        const char *p = str+1;
        struct passwd *pwd;
        int dirlen;
        
        for (; p < str+size && *p != '/' && *p != '\0'; p++)
            ;
        if (p == str+1) {
            pwd = getpwuid(geteuid());
            if (pwd == NULL) {
                Scm_SigCheck(Scm_VM());
                Scm_SysError("couldn't get home directory.\n");
            }
        } else {
            char *user = (char *)SCM_MALLOC_ATOMIC(p-str);
            memcpy(user, str+1, p-str-1);
            user[p-str-1] = '\0';
            pwd = getpwnam(user);
            if (pwd == NULL) {
                Scm_SigCheck(Scm_VM());
                Scm_Error("couldn't get home directory of user \"%s\".\n",
                          user);
            }
        }
        srcp = p;
        SKIP_SLASH;
        dirlen = strlen(pwd->pw_dir);
        buf = SCM_NEW_ATOMIC2(char*, dirlen+size+1);
        strcpy(buf, pwd->pw_dir);
        dstp = buf + dirlen;
        if (*(dstp-1) != '/') { *dstp++ = '/'; *(dstp+1) = '\0'; }
#else  /* __MINGW32__ */
	/* we don't support '~' magic on win32 native */
	buf = SCM_NEW_ATOMIC2(char*, size+1);
	dstp = buf;
#endif /* __MINGW32__ */
    } else if ((flags & SCM_PATH_ABSOLUTE) && *str != '/') {
        int dirlen;
#define GETCWD_PATH_MAX 1024  /* TODO: must be configured */
        char p[GETCWD_PATH_MAX];
        if (getcwd(p, GETCWD_PATH_MAX-1) == NULL) {
            Scm_SigCheck(Scm_VM());
            Scm_SysError("couldn't get current directory.");
        }
        dirlen = strlen(p);
        buf = SCM_NEW_ATOMIC2(char*, dirlen+size+1);
        strcpy(buf, p);
        dstp = buf + dirlen;
        if (*(dstp-1) != '/') *dstp++ = '/';
    } else if (flags & SCM_PATH_CANONICALIZE) {
        dstp = buf = SCM_NEW_ATOMIC2(char*, size+1);
        if (*str == '/') {
            *dstp++ = '/';
            SKIP_SLASH;
        }
    } else {
        return SCM_OBJ(pathname); /* nothing to do */
    }

    if (!(flags & SCM_PATH_CANONICALIZE)) {
        size -= srcp-str;
        memcpy(dstp, srcp, size);
        *(dstp + size) = '\0';
        return Scm_MakeString(buf, (dstp-buf)+size, -1, SCM_MAKSTR_COPYING);
    }

    while (srcp < str+size) {
        if (*srcp == '.') {
            if (srcp == str+size-1) {
                *dstp++ = '.'; /* preserve the last dot */
                break;
            }
            if (*(srcp+1) == '/') {
                srcp++;
                SKIP_SLASH;
                continue;
            }
            if (!bottomp
                && *(srcp+1) == '.'
                && (srcp == str+size-2 || *(srcp+2) == '/')) {

                /* back up to parent dir */
                char *q = dstp-2;
                for (;q >= buf; q--) {
                    if (*q == '/') break;
                }
                if (q >= buf) {
                    dstp = q+1;
                } else {
                    bottomp = TRUE;
                    *dstp++ = '.';
                    *dstp++ = '.';
                    *dstp++ = '/';
                }
                srcp += 3;
                continue;
            }
        }
        while ((*dstp++ = *srcp++) != '/' && srcp < str+size)
            ;
        SKIP_SLASH;
    }
    *dstp = '\0';
    return Scm_MakeString(buf, dstp-buf, -1, SCM_MAKSTR_COPYING);
}

ScmObj Scm_BaseName(ScmString *filename)
{
    const char *p, *str = SCM_STRING_START(filename);
    int i, size = SCM_STRING_SIZE(filename);

    if (size == 0) return SCM_MAKE_STR("");
    p = str+size-1;
    while (*p == '/' && size > 0) { p--; size--; } /* ignore trailing '/' */
    if (size == 0) return SCM_MAKE_STR("");
    for (i = 0; i < size; i++, p--) {
        if (*p == '/') break;
    }
    return Scm_MakeString(p+1, i, -1, 0);
}

ScmObj Scm_DirName(ScmString *filename)
{
    const char *p, *str = SCM_STRING_START(filename);
    int i, size = SCM_STRING_SIZE(filename);

    if (size == 0) return SCM_MAKE_STR(".");
    p = str+size-1;
    while (*p == '/' && size > 0) { p--; size--; } /* ignore trailing '/' */
    if (size == 0) return SCM_MAKE_STR("/");
    for (i = size; i > 0; i--, p--) {
        if (*p == '/') break;
    }
    if (i == 0) return SCM_MAKE_STR(".");
    while (*p == '/' && i > 0) { p--; i--; } /* delete trailing '/' */
    if (i == 0) return SCM_MAKE_STR("/");
    return Scm_MakeString(str, i, -1, 0);
}

/* Make mkstemp() work even if the system doesn't have one. */
int Scm_Mkstemp(char *templat)
{
    int fd = -1;
#if defined(HAVE_MKSTEMP)
    SCM_SYSCALL(fd, mkstemp(templat));
    if (fd < 0) Scm_SysError("mkstemp failed");
    return fd;
#else   /*!defined(HAVE_MKSTEMP)*/
    /* Emulate mkstemp. */
    int siz = strlen(templat);
    if (siz < 6) {
        Scm_Error("mkstemp - invalid template: %s", templat);
    }
#define MKSTEMP_MAX_TRIALS 65535   /* avoid infinite loop */
    {
	u_long seed = (u_long)time(NULL);
	int numtry;
	char suffix[7];
	for (numtry=0; numtry<MKSTEMP_MAX_TRIALS; numtry++) {
	    snprintf(suffix, 7, "%06x", seed&0xffffff);
	    memcpy(templat+siz-6, suffix, 7);
#ifndef __MINGW32__
	    SCM_SYSCALL(fd, open(templat, O_CREAT|O_EXCL|O_WRONLY, 0600));
#else  /*__MINGW32__*/
 	    SCM_SYSCALL(fd, open(templat, _O_CREAT|_O_EXCL|_O_WRONLY, 0600));
#endif /*__MINGW32__*/
	    if (fd >= 0) break;
	    seed *= 2654435761UL;
	}
	if (numtry == MKSTEMP_MAX_TRIALS) {
	    Scm_Error("mkstemp failed");
	}
    }
    return fd;
#endif /*!defined(HAVE_MKSTEMP)*/
}


ScmObj Scm_SysMkstemp(ScmString *templat)
{
#define MKSTEMP_PATH_MAX 1025  /* Geez, remove me */
    char name[MKSTEMP_PATH_MAX];
    ScmObj sname;
    int siz = SCM_STRING_SIZE(templat), fd;
    if (siz >= MKSTEMP_PATH_MAX-6) { 
	Scm_Error("pathname too long: %S", templat);
    }
    memcpy(name, SCM_STRING_START(templat), siz);
    memcpy(name + siz, "XXXXXX", 6);
    name[siz+6] = '\0';
    fd = Scm_Mkstemp(name);
    sname = SCM_MAKE_STR_COPYING(name);
    SCM_RETURN(Scm_Values2(Scm_MakePortWithFd(sname, SCM_PORT_OUTPUT, fd,
					      SCM_PORT_BUFFER_FULL, TRUE),
			   sname));
}

/*===============================================================
 * Stat (sys/stat.h)
 */

static ScmObj stat_allocate(ScmClass *klass, ScmObj initargs)
{
    ScmSysStat *s = SCM_ALLOCATE(ScmSysStat, klass);
    SCM_SET_CLASS(s, SCM_CLASS_SYS_STAT);
    return SCM_OBJ(s);
}

SCM_DEFINE_BUILTIN_CLASS(Scm_SysStatClass,
                         NULL, NULL, NULL,
                         stat_allocate,
                         SCM_CLASS_DEFAULT_CPL);

ScmObj Scm_MakeSysStat(void)
{
    return stat_allocate(&Scm_SysStatClass, SCM_NIL);
}

static ScmObj stat_type_get(ScmSysStat *stat)
{
  if (S_ISDIR(stat->statrec.st_mode)) return (SCM_SYM_DIRECTORY);
  if (S_ISREG(stat->statrec.st_mode)) return (SCM_SYM_REGULAR);
  if (S_ISCHR(stat->statrec.st_mode)) return (SCM_SYM_CHARACTER);
  if (S_ISBLK(stat->statrec.st_mode)) return (SCM_SYM_BLOCK);
  if (S_ISFIFO(stat->statrec.st_mode)) return (SCM_SYM_FIFO);
#ifdef S_ISLNK
  if (S_ISLNK(stat->statrec.st_mode)) return (SCM_SYM_SYMLINK);
#endif
#ifdef S_ISSOCK
  if (S_ISSOCK(stat->statrec.st_mode)) return (SCM_SYM_SOCKET);
#endif
  return (SCM_FALSE);
}

static ScmObj stat_perm_get(ScmSysStat *stat)
{
    return Scm_MakeIntegerFromUI(stat->statrec.st_mode & 0777);
}

#define STAT_GETTER_UI(name) \
  static ScmObj SCM_CPP_CAT3(stat_, name, _get)(ScmSysStat *s) \
  { return Scm_MakeIntegerFromUI((u_long)s->statrec.SCM_CPP_CAT(st_, name)); }
#define STAT_GETTER_TIME(name) \
  static ScmObj SCM_CPP_CAT3(stat_, name, _get)(ScmSysStat *s) \
  { return Scm_MakeSysTime(s->statrec.SCM_CPP_CAT(st_, name)); }

STAT_GETTER_UI(mode)
STAT_GETTER_UI(ino)
STAT_GETTER_UI(dev)
STAT_GETTER_UI(rdev)
STAT_GETTER_UI(nlink)
STAT_GETTER_UI(uid)
STAT_GETTER_UI(gid)
STAT_GETTER_UI(size) /*TODO: check portability of off_t (maybe 64bits)*/
STAT_GETTER_TIME(atime)
STAT_GETTER_TIME(mtime)
STAT_GETTER_TIME(ctime)

static ScmClassStaticSlotSpec stat_slots[] = {
    SCM_CLASS_SLOT_SPEC("type",  stat_type_get,  NULL),
    SCM_CLASS_SLOT_SPEC("perm",  stat_perm_get,  NULL),
    SCM_CLASS_SLOT_SPEC("mode",  stat_mode_get,  NULL),
    SCM_CLASS_SLOT_SPEC("ino",   stat_ino_get,   NULL),
    SCM_CLASS_SLOT_SPEC("dev",   stat_dev_get,   NULL),
    SCM_CLASS_SLOT_SPEC("rdev",  stat_rdev_get,  NULL),
    SCM_CLASS_SLOT_SPEC("nlink", stat_nlink_get, NULL),
    SCM_CLASS_SLOT_SPEC("uid",   stat_uid_get,   NULL),
    SCM_CLASS_SLOT_SPEC("gid",   stat_gid_get,   NULL),
    SCM_CLASS_SLOT_SPEC("size",  stat_size_get,  NULL),
    SCM_CLASS_SLOT_SPEC("atime", stat_atime_get, NULL),
    SCM_CLASS_SLOT_SPEC("mtime", stat_mtime_get, NULL),
    SCM_CLASS_SLOT_SPEC("ctime", stat_ctime_get, NULL),
    { NULL }
};

/*===============================================================
 * Time (sys/time.h)
 */

/* Gauche has two notion of time.  A simple number is used by the low-level
 * system interface (sys-time, sys-gettimeofday).  An object of <time> class
 * is used for higher-level interface, including threads.
 */

/* <time> object */

static ScmObj time_allocate(ScmClass *klass, ScmObj initargs)
{
    ScmTime *t = SCM_ALLOCATE(ScmTime, klass);
    SCM_SET_CLASS(t, SCM_CLASS_TIME);
    t->type = SCM_SYM_TIME_UTC;
    t->sec = t->nsec = 0;
    return SCM_OBJ(t);
}

static void time_print(ScmObj obj, ScmPort *port, ScmWriteContext *ctx)
{
    ScmTime *t = SCM_TIME(obj);
    Scm_Printf(port, "#<%S %lu.%09lu>", t->type, t->sec, t->nsec);
}

SCM_DEFINE_BUILTIN_CLASS(Scm_TimeClass,
                         time_print, NULL, NULL,
                         time_allocate, SCM_CLASS_DEFAULT_CPL);

ScmObj Scm_MakeTime(ScmObj type, long sec, long nsec)
{
    ScmTime *t = SCM_TIME(time_allocate(SCM_CLASS_TIME, SCM_NIL));
    t->type = SCM_FALSEP(type)? SCM_SYM_TIME_UTC : type;
    t->sec = sec;
    t->nsec = nsec;
    return SCM_OBJ(t);
}

ScmObj Scm_CurrentTime(void)
{
#ifdef HAVE_GETTIMEOFDAY
    struct timeval tv;
    int r;
    SCM_SYSCALL(r, gettimeofday(&tv, NULL));
    if (r < 0) Scm_SysError("gettimeofday failed");
    return Scm_MakeTime(SCM_SYM_TIME_UTC, (long)tv.tv_sec, (long)tv.tv_usec*1000);
#else  /* !HAVE_GETTIMEOFDAY */
    return Scm_MakeTime(SCM_SYM_TIME_UTC, (long)time(NULL), 0);
#endif /* !HAVE_GETTIMEOFDAY */
}

ScmObj Scm_IntSecondsToTime(long sec)
{
    return Scm_MakeTime(SCM_SYM_TIME_UTC, sec, 0);
}

ScmObj Scm_RealSecondsToTime(double sec)
{
    double s, frac;
    if (sec > (double)ULONG_MAX || sec < 0) {
        Scm_Error("seconds out of range: %f", sec);
    }
    frac = modf(sec, &s);
    return Scm_MakeTime(SCM_SYM_TIME_UTC, (long)s, (long)(frac * 1.0e9));
}

static ScmObj time_type_get(ScmTime *t)
{
    return t->type;
}

static void time_type_set(ScmTime *t, ScmObj val)
{
    if (!SCM_SYMBOLP(val)) {
        Scm_Error("time type must be a symbol, but got %S", val);
    }
    t->type = val;
}

static ScmObj time_sec_get(ScmTime *t)
{
    return Scm_MakeInteger(t->sec);
}

static void time_sec_set(ScmTime *t, ScmObj val)
{
    if (!SCM_REALP(val)) {
        Scm_Error("real number required, but got %S", val);
    }
    t->sec = Scm_GetInteger(val);
}

static ScmObj time_nsec_get(ScmTime *t)
{
    return Scm_MakeInteger(t->nsec);
}

static void time_nsec_set(ScmTime *t, ScmObj val)
{
    long l;
    if (!SCM_REALP(val)) {
        Scm_Error("real number required, but got %S", val);
    }
    if ((l = Scm_GetInteger(val)) >= 1000000000) {
        Scm_Error("nanoseconds out of range: %ld", l);
    }
    t->nsec = l;
}

static ScmClassStaticSlotSpec time_slots[] = {
    SCM_CLASS_SLOT_SPEC("type",       time_type_get, time_type_set),
    SCM_CLASS_SLOT_SPEC("second",     time_sec_get, time_sec_set),
    SCM_CLASS_SLOT_SPEC("nanosecond", time_nsec_get, time_nsec_set),
    {NULL}
};

/* time_t and conversion routines */
/* NB: I assume time_t is typedefed to either an integral type or
 * a floating point type.  As far as I know it is true on most
 * current architectures.  POSIX doesn't specify so, however; it
 * may be some weird structure.  If you find such an architecture,
 * tweak configure.in and modify the following two functions.
 */
ScmObj Scm_MakeSysTime(time_t t)
{
#ifdef INTEGRAL_TIME_T
    return Scm_MakeIntegerFromUI((unsigned long)t);
#else
    double val = (double)t;
    return Scm_MakeFlonum(val);
#endif
}

time_t Scm_GetSysTime(ScmObj val)
{
    if (SCM_TIMEP(val)) {
#ifdef INTEGRAL_TIME_T
        return (time_t)SCM_TIME(val)->sec;
#else
        return (time_t)((double)SCM_TIME(val)->sec +
                        (double)SCM_TIME(val)->nsec/1.0e9);
#endif
    } else if (SCM_NUMBERP(val)) {
#ifdef INTEGRAL_TIME_T
        return (time_t)Scm_GetUInteger(val);
#else
        return (time_t)Scm_GetDouble(val);
#endif
    } else {
        Scm_Error("bad time value: either a <time> object or a real number is required, but got %S", val);
        return (time_t)0;       /* dummy */
    }
}

ScmObj Scm_TimeToSeconds(ScmTime *t)
{
    if (t->nsec) {
        return Scm_MakeFlonum((double)t->sec + (double)t->nsec/1.0e9);
    } else {
        return Scm_MakeIntegerFromUI(t->sec);
    }
}

#if defined(HAVE_STRUCT_TIMESPEC) || defined (GAUCHE_USE_PTHREADS)
/* Scheme time -> timespec conversion, used by pthread routines.*/
struct timespec *Scm_GetTimeSpec(ScmObj t, struct timespec *spec)
{
    if (SCM_FALSEP(t)) return NULL;
    if (SCM_TIMEP(t)) {
        spec->tv_sec = SCM_TIME(t)->sec;
        spec->tv_nsec = SCM_TIME(t)->nsec;
    } else if (!SCM_REALP(t)) {
        Scm_Error("bad timeout spec: <time> object or real number is required, but got %S", t);
    } else {
        ScmTime *ct = SCM_TIME(Scm_CurrentTime());
        spec->tv_sec = ct->sec;
        spec->tv_nsec = ct->nsec;
        if (SCM_EXACTP(t)) {
            spec->tv_sec += Scm_GetUInteger(t);
        } else if (SCM_FLONUMP(t)) {
            double s;
            spec->tv_nsec += (unsigned long)(modf(Scm_GetDouble(t), &s)*1.0e9);
            spec->tv_sec += (unsigned long)s;
            while (spec->tv_nsec >= 1000000000) {
                spec->tv_nsec -= 1000000000;
                spec->tv_sec += 1;
            }
        } else {
            Scm_Panic("implementation error: Scm_GetTimeSpec: something wrong");
        }
    }
    return spec;
}
#endif /* defined(HAVE_STRUCT_TIMESPEC) || defined (GAUCHE_USE_PTHREADS) */

/* <sys-tm> object */

static ScmObj tm_allocate(ScmClass *klass, ScmObj initargs)
{
    ScmSysTm *st = SCM_ALLOCATE(ScmSysTm, klass);
    SCM_SET_CLASS(st, SCM_CLASS_SYS_TM);
    return SCM_OBJ(st);
}

static void tm_print(ScmObj obj, ScmPort *port, ScmWriteContext *ctx)
{
#define TM_BUFSIZ 50
    char buf[TM_BUFSIZ];
    ScmSysTm *st = SCM_SYS_TM(obj);
    strftime(buf, TM_BUFSIZ, "%a %b %e %T %Y", &st->tm);
    Scm_Printf(port, "#<sys-tm \"%s\">", buf);
#undef TM_BUFSIZ
}

SCM_DEFINE_BUILTIN_CLASS(Scm_SysTmClass,
                         tm_print, NULL, NULL,
                         tm_allocate, SCM_CLASS_DEFAULT_CPL);

ScmObj Scm_MakeSysTm(struct tm *tm)
{
    ScmSysTm *st = SCM_NEW(ScmSysTm);
    SCM_SET_CLASS(st, SCM_CLASS_SYS_TM);
    st->tm = *tm;               /* copy */
    return SCM_OBJ(st);
}

#define TM_ACCESSOR(name)                                               \
  static ScmObj SCM_CPP_CAT(name, _get)(ScmSysTm *tm) {                 \
    return Scm_MakeInteger(tm->tm.name);                                \
  }                                                                     \
  static void SCM_CPP_CAT(name, _set)(ScmSysTm *tm, ScmObj val) {       \
    if (!SCM_EXACTP(val))                                               \
      Scm_Error("exact integer required, but got %S", val);             \
    tm->tm.name = Scm_GetInteger(val);                                  \
  }

TM_ACCESSOR(tm_sec)
TM_ACCESSOR(tm_min)
TM_ACCESSOR(tm_hour)
TM_ACCESSOR(tm_mday)
TM_ACCESSOR(tm_mon)
TM_ACCESSOR(tm_year)
TM_ACCESSOR(tm_wday)
TM_ACCESSOR(tm_yday)
TM_ACCESSOR(tm_isdst)

static ScmClassStaticSlotSpec tm_slots[] = {
    SCM_CLASS_SLOT_SPEC("sec", tm_sec_get, tm_sec_set),
    SCM_CLASS_SLOT_SPEC("min", tm_min_get, tm_min_set),
    SCM_CLASS_SLOT_SPEC("hour", tm_hour_get, tm_hour_set),
    SCM_CLASS_SLOT_SPEC("mday", tm_mday_get, tm_mday_set),
    SCM_CLASS_SLOT_SPEC("mon", tm_mon_get, tm_mon_set),
    SCM_CLASS_SLOT_SPEC("year", tm_year_get, tm_year_set),
    SCM_CLASS_SLOT_SPEC("wday", tm_wday_get, tm_wday_set),
    SCM_CLASS_SLOT_SPEC("yday", tm_yday_get, tm_yday_set),
    SCM_CLASS_SLOT_SPEC("isdst", tm_isdst_get, tm_isdst_set),
    { NULL }
};

/*===============================================================
 * Groups (grp.h)
 */

static void grp_print(ScmObj obj, ScmPort *port, ScmWriteContext *ctx)
{
    Scm_Printf(port, "#<sys-group %S>",
               SCM_SYS_GROUP(obj)->name);
}

SCM_DEFINE_BUILTIN_CLASS_SIMPLE(Scm_SysGroupClass, grp_print);

static ScmObj make_group(struct group *g)
{
    ScmObj head = SCM_NIL, tail = SCM_NIL, p;
    char **memp;
    ScmSysGroup *sg = SCM_NEW(ScmSysGroup);
    SCM_SET_CLASS(sg, SCM_CLASS_SYS_GROUP);
    
    sg->name = SCM_MAKE_STR_COPYING(g->gr_name);
#ifdef HAVE_GR_PASSWD
    sg->passwd = SCM_MAKE_STR_COPYING(g->gr_passwd);
#else
    sg->passwd = SCM_FALSE;
#endif
    sg->gid = Scm_MakeInteger(g->gr_gid);
    for (memp = g->gr_mem; *memp; memp++) {
        p = SCM_MAKE_STR_COPYING(*memp);
        SCM_APPEND1(head, tail, p);
    }
    sg->mem = head;
    return SCM_OBJ(sg);
}

ScmObj Scm_GetGroupById(gid_t gid)
{
    struct group *gdata;
    gdata = getgrgid(gid);
    if (gdata == NULL) {
        Scm_SigCheck(Scm_VM());
        return SCM_FALSE;
    } else {
        return make_group(gdata);
    }
}

ScmObj Scm_GetGroupByName(ScmString *name)
{
    struct group *gdata;
    gdata = getgrnam(Scm_GetStringConst(name));
    if (gdata == NULL) {
        Scm_SigCheck(Scm_VM());
        return SCM_FALSE;
    } else {
        return make_group(gdata);
    }
}

#define GRP_GETTER(name) \
  static ScmObj SCM_CPP_CAT3(grp_, name, _get)(ScmSysGroup *s) \
  { return s->name; }

GRP_GETTER(name)
GRP_GETTER(gid)
GRP_GETTER(passwd)
GRP_GETTER(mem)

static ScmClassStaticSlotSpec grp_slots[] = {
    SCM_CLASS_SLOT_SPEC("name",   grp_name_get, NULL),
    SCM_CLASS_SLOT_SPEC("gid",    grp_gid_get, NULL),
    SCM_CLASS_SLOT_SPEC("passwd", grp_passwd_get, NULL),
    SCM_CLASS_SLOT_SPEC("mem",    grp_mem_get, NULL),
    { NULL }
};

/*===============================================================
 * Passwords (pwd.h)
 *   Patch provided by Yuuki Takahashi (t.yuuki@mbc.nifty.com)
 */

static void pwd_print(ScmObj obj, ScmPort *port, ScmWriteContext *ctx)
{
    Scm_Printf(port, "#<sys-passwd %S>",
               SCM_SYS_PASSWD(obj)->name);
}

SCM_DEFINE_BUILTIN_CLASS_SIMPLE(Scm_SysPasswdClass, pwd_print);

static ScmObj make_passwd(struct passwd *pw)
{
    ScmSysPasswd *sp = SCM_NEW(ScmSysPasswd);
    SCM_SET_CLASS(sp, SCM_CLASS_SYS_PASSWD);

    sp->name = SCM_MAKE_STR_COPYING(pw->pw_name);
    sp->uid = Scm_MakeInteger(pw->pw_uid);
    sp->gid = Scm_MakeInteger(pw->pw_gid);
#ifdef HAVE_PW_PASSWD
    sp->passwd = SCM_MAKE_STR_COPYING(pw->pw_passwd);
#else
    sp->passwd = SCM_FALSE;
#endif
#ifdef HAVE_PW_GECOS
    sp->gecos = SCM_MAKE_STR_COPYING(pw->pw_gecos);
#else
    sp->gecos = SCM_FALSE;
#endif
#ifdef HAVE_PW_CLASS
    sp->pwclass = SCM_MAKE_STR_COPYING(pw->pw_class);
#else
    sp->pwclass = SCM_FALSE;
#endif
    sp->dir = SCM_MAKE_STR_COPYING(pw->pw_dir);
    sp->shell = SCM_MAKE_STR_COPYING(pw->pw_shell);
    return SCM_OBJ(sp);
}

ScmObj Scm_GetPasswdById(uid_t uid)
{
    struct passwd *pdata;
    pdata = getpwuid(uid);
    if (pdata == NULL) {
        Scm_SigCheck(Scm_VM());
        return SCM_FALSE;
    } else {
        return make_passwd(pdata);
    }
}

ScmObj Scm_GetPasswdByName(ScmString *name)
{
    struct passwd *pdata;
    pdata = getpwnam(Scm_GetStringConst(name));
    if (pdata == NULL) {
        Scm_SigCheck(Scm_VM());
        return SCM_FALSE;
    } else {
        return make_passwd(pdata);
    }
}

#define PWD_GETTER(name) \
  static ScmObj SCM_CPP_CAT3(pwd_, name, _get)(ScmSysPasswd *p) \
  { return p->name; }

PWD_GETTER(name)
PWD_GETTER(uid)
PWD_GETTER(gid)
PWD_GETTER(passwd)
PWD_GETTER(gecos)
PWD_GETTER(dir)
PWD_GETTER(shell)
PWD_GETTER(pwclass)

static ScmClassStaticSlotSpec pwd_slots[] = {
    SCM_CLASS_SLOT_SPEC("name",   pwd_name_get, NULL),
    SCM_CLASS_SLOT_SPEC("uid",    pwd_uid_get, NULL),
    SCM_CLASS_SLOT_SPEC("gid",    pwd_gid_get, NULL),
    SCM_CLASS_SLOT_SPEC("passwd", pwd_passwd_get, NULL),
    SCM_CLASS_SLOT_SPEC("gecos",  pwd_gecos_get, NULL),
    SCM_CLASS_SLOT_SPEC("dir",    pwd_dir_get, NULL),
    SCM_CLASS_SLOT_SPEC("shell",  pwd_shell_get, NULL),
    SCM_CLASS_SLOT_SPEC("class",  pwd_pwclass_get, NULL),
    { NULL }
};

/*
 * check if we're suid/sgid-ed.
 * TODO: some system has a special syscall for it; use it if so.
 */
int Scm_IsSugid(void)
{
#ifndef __MINGW32__
    return (geteuid() != getuid() || getegid() != getgid());
#else  /*__MINGW32__*/
    return FALSE;
#endif /*__MINGW32__*/
}

/*===============================================================
 * Exec
 *   execvp(), with optionally setting stdios correctly.
 *
 *   iomap argument, when provided, specifies how the open file descriptors
 *   are treated.  If it is not a pair, nothing will be changed for open
 *   file descriptors.  If it is a pair, it must be a list of
 *   (<to> . <from>), where <tofd> is an integer file descriptor that
 *   executed process will get, and <from> is either an integer file descriptor
 *   or a port.   If a list is passed to iomap, any file descriptors other
 *   than specified in the list will be closed before exec().
 *
 *   If forkp arg is TRUE, this function forks before swapping file
 *   descriptors.  It is more reliable way to fork&exec in multi-threaded
 *   program.  In such a case, this function returns Scheme integer to
 *   show the children's pid.   If for arg is FALSE, this procedure
 *   of course never returns.
 *
 *   On Windows/MinGW port, I'm not sure we can do I/O swapping in
 *   reasonable way.  For now, iomap is ignored.
 */

ScmObj Scm_SysExec(ScmString *file, ScmObj args, ScmObj iomap, int forkp)
{
    int argc = Scm_Length(args), i, j, maxfd, iollen;
    int *tofd = NULL, *fromfd = NULL, *tmpfd = NULL;
    char **argv;
    const char *program;
    ScmObj ap, iop;
    pid_t pid = 0;

    if (argc < 1) {
        Scm_Error("argument list must have at least one element: %S", args);
    }

    /* make a C array of C strings */    
    argv = SCM_NEW_ARRAY(char *, argc+1);
    for (i=0, ap = args; i<argc; i++, ap = SCM_CDR(ap)) {
        if (!SCM_STRINGP(SCM_CAR(ap)))
            Scm_Error("bad argument (string required): %S", SCM_CAR(ap));
        argv[i] = Scm_GetString(SCM_STRING(SCM_CAR(ap)));
    }
    argv[i] = NULL;
    program = Scm_GetStringConst(file);

#ifndef __MINGW32__
    /* setting up iomap table */
    iollen = Scm_Length(iomap);
    if (SCM_PAIRP(iomap)) {
        /* check argument vailidity before duping file descriptors, so that
           we can still use Scm_Error */
        if (iollen < 0) {
            Scm_Error("proper list required for iolist, but got %S", iomap);
        }
        tofd   = SCM_NEW_ATOMIC2(int *, iollen * sizeof(int));
        fromfd = SCM_NEW_ATOMIC2(int *, iollen * sizeof(int));
        tmpfd  = SCM_NEW_ATOMIC2(int *, iollen * sizeof(int));
        i = 0;
        SCM_FOR_EACH(iop, iomap) {
            ScmObj port, elt = SCM_CAR(iop);
            if (!SCM_PAIRP(elt) || !SCM_INTP(SCM_CAR(elt))
                || (!SCM_PORTP(SCM_CDR(elt)) && !SCM_INTP(SCM_CDR(elt)))) {
                Scm_Error("bad iomap specification: needs (int . int-or-port): %S", elt);
            }
            tofd[i] = SCM_INT_VALUE(SCM_CAR(elt));
            if (SCM_INTP(SCM_CDR(elt))) {
                fromfd[i] = SCM_INT_VALUE(SCM_CDR(elt));
            } else {
                port = SCM_CDAR(iop);
                fromfd[i] = Scm_PortFileNo(SCM_PORT(port));
                if (fromfd[i] < 0) {
                    Scm_Error("iolist requires a port that has associated file descriptor, but got %S",
                              SCM_CDAR(iop));
                }
                if (tofd[i] == 0 && !SCM_IPORTP(port))
                    Scm_Error("input port required to make it stdin: %S",
                              port);
                if (tofd[i] == 1 && !SCM_OPORTP(port))
                    Scm_Error("output port required to make it stdout: %S",
                              port);
                if (tofd[i] == 2 && !SCM_OPORTP(port))
                    Scm_Error("output port required to make it stderr: %S",
                              port);
            }
            i++;
        }
    }
    
    /* When requested, call fork() here. */
    if (forkp) {
        SCM_SYSCALL(pid, fork());
        if (pid < 0) Scm_SysError("fork failed");
    }

    /* Now we swap file descriptors and exec().
       We can't throw an error anymore! */
    if (!forkp || pid == 0) {
        /* TODO: use getdtablehi if available */
        if ((maxfd = sysconf(_SC_OPEN_MAX)) < 0) {
            Scm_Panic("failed to get OPEN_MAX value from sysconf");
        }

        for (i=0; i<iollen; i++) {
            if (tofd[i] == fromfd[i]) continue;
            for (j=i+1; j<iollen; j++) {
                if (tofd[i] == fromfd[j]) {
                    int tmp = dup(tofd[i]);
                    if (tmp < 0) Scm_Panic("dup failed: %s", strerror(errno));
                    fromfd[j] = tmp;
                }
            }
            if (dup2(fromfd[i], tofd[i]) < 0)
                Scm_Panic("dup2 failed: %s", strerror(errno));
        }
        for (i=0; i<maxfd; i++) {
            for (j=0; j<iollen; j++) {
                if (i == tofd[j]) break;
            }
            if (j == iollen) close(i);
        }
        execvp(program, (char *const*)argv);
        /* here, we failed */
        Scm_Panic("exec failed: %s: %s", program, strerror(errno));
    }

    /* We come here only when fork is requested. */
    return Scm_MakeInteger(pid);
#else  /* __MINGW32__ */
    if (forkp) {
	Scm_Error("fork() not supported on MinGW port");
    } else {
	execvp(program, (const char *const*)argv);
	Scm_Panic("exec failed: %s: %s", program, strerror(errno));	
    }
    return SCM_FALSE; /* dummy */
#endif /* __MINGW32__ */
}

/*===============================================================
 * select
 */

#ifdef HAVE_SELECT
static ScmObj fdset_allocate(ScmClass *klass, ScmObj initargs)
{
    ScmSysFdset *set = SCM_ALLOCATE(ScmSysFdset, klass);
    SCM_SET_CLASS(set, SCM_CLASS_SYS_FDSET);
    set->maxfd = -1;
    FD_ZERO(&set->fdset);
    return SCM_OBJ(set);
}

static ScmSysFdset *fdset_copy(ScmSysFdset *fdset)
{
    ScmSysFdset *set = SCM_NEW(ScmSysFdset);
    SCM_SET_CLASS(set, SCM_CLASS_SYS_FDSET);
    set->maxfd = fdset->maxfd;
    set->fdset = fdset->fdset;
    return set;
}

SCM_DEFINE_BUILTIN_CLASS(Scm_SysFdsetClass, NULL, NULL, NULL,
                         fdset_allocate, SCM_CLASS_DEFAULT_CPL);

static ScmSysFdset *select_checkfd(ScmObj fds)
{
    if (SCM_FALSEP(fds)) return NULL;
    if (!SCM_SYS_FDSET_P(fds))
        Scm_Error("sys-fdset object or #f is required, but got %S", fds);
    return SCM_SYS_FDSET(fds);
}

static struct timeval *select_timeval(ScmObj timeout, struct timeval *tm)
{
    if (SCM_FALSEP(timeout)) return NULL;
    if (SCM_INTP(timeout)) {
        int val = SCM_INT_VALUE(timeout);
        if (val < 0) goto badtv;
        tm->tv_sec = val / 1000000;
        tm->tv_usec = val % 1000000;
        return tm;
    } else if (SCM_BIGNUMP(timeout)) {
        long usec;
        ScmObj sec;
        if (Scm_Sign(timeout) < 0) goto badtv;
        sec = Scm_BignumDivSI(SCM_BIGNUM(timeout), 1000000, &usec);
        tm->tv_sec = Scm_GetInteger(sec);
        tm->tv_usec = usec;
        return tm;
    } else if (SCM_FLONUMP(timeout)) {
        long val = Scm_GetInteger(timeout);
        if (val < 0) goto badtv;
        tm->tv_sec = val / 1000000;
        tm->tv_usec = val % 1000000;
        return tm;
    } else if (SCM_PAIRP(timeout) && SCM_PAIRP(SCM_CDR(timeout))) {
        ScmObj sec = SCM_CAR(timeout);
        ScmObj usec = SCM_CADR(timeout);
        long isec, iusec;
        if (!Scm_IntegerP(sec) || !Scm_IntegerP(usec)) goto badtv;
        isec = Scm_GetInteger(sec);
        iusec = Scm_GetInteger(usec);
        if (isec < 0 || iusec < 0) goto badtv;
        tm->tv_sec = isec;
        tm->tv_usec = iusec;
        return tm;
    }
  badtv:
    Scm_Error("timeval needs to be a real number (in microseconds) or a list of two integers (seconds and microseconds), but got %S", timeout);
    return NULL;                /* dummy */
}

static ScmObj select_int(ScmSysFdset *rfds, ScmSysFdset *wfds,
                         ScmSysFdset *efds, ScmObj timeout)
{
    int numfds, maxfds = 0;
    struct timeval tm;
    if (rfds) maxfds = rfds->maxfd;
    if (wfds && wfds->maxfd > maxfds) maxfds = wfds->maxfd;
    if (efds && efds->maxfd > maxfds) maxfds = efds->maxfd;

    SCM_SYSCALL(numfds, 
                select(maxfds+1,
                       (rfds? &rfds->fdset : NULL),
                       (wfds? &wfds->fdset : NULL),
                       (efds? &efds->fdset : NULL),
                       select_timeval(timeout, &tm)));
    if (numfds < 0) Scm_SysError("select failed");
    return Scm_Values4(Scm_MakeInteger(numfds),
                       (rfds? SCM_OBJ(rfds) : SCM_FALSE),
                       (wfds? SCM_OBJ(wfds) : SCM_FALSE),
                       (efds? SCM_OBJ(efds) : SCM_FALSE));
}

ScmObj Scm_SysSelect(ScmObj rfds, ScmObj wfds, ScmObj efds, ScmObj timeout)
{
    ScmSysFdset *r = select_checkfd(rfds);
    ScmSysFdset *w = select_checkfd(wfds);
    ScmSysFdset *e = select_checkfd(efds);
    return select_int((r? fdset_copy(r) : NULL),
                      (w? fdset_copy(w) : NULL),
                      (e? fdset_copy(e) : NULL),
                      timeout);
}

ScmObj Scm_SysSelectX(ScmObj rfds, ScmObj wfds, ScmObj efds, ScmObj timeout)
{
    ScmSysFdset *r = select_checkfd(rfds);
    ScmSysFdset *w = select_checkfd(wfds);
    ScmSysFdset *e = select_checkfd(efds);
    return select_int(r, w, e, timeout);
}

#endif /* HAVE_SELECT */

/*===============================================================
 * Emulation layer for MinGW port
 */
#ifdef __MINGW32__

/* wide character string -> Scheme-owned MB string.
   the result is utf8.  we should convert it to Gauche's internal encoding,
   but that's the later story... */
static char *w2mdup(LPCWSTR wstr)
{
    char *dst = "";
    if (wstr) {
	/* first, count the required length */
	int count;
	int mbsize = WideCharToMultiByte(CP_UTF8, 0, wstr, -1,
					 NULL, 0, NULL, NULL);
	SCM_ASSERT(mbsize > 0);
	dst = SCM_NEW_ATOMIC2(char*, mbsize+1);
	count = WideCharToMultiByte(CP_UTF8, 0, wstr, -1,
				    dst, mbsize+1, NULL, NULL);
	dst[mbsize] = '\0';
	SCM_ASSERT(count == mbsize);
    }
    return dst;
}

static wchar_t *m2wdup(const char *mbstr)
{
    wchar_t *dst = NULL;
    if (mbstr) {
	/* first, count the required length */
	int count;
	int wcsize = MultiByteToWideChar(CP_UTF8, 0, mbstr, -1, NULL, 0);
	SCM_ASSERT(wcsize >= 0);
	dst = SCM_NEW_ATOMIC2(wchar_t *, wcsize+1);
	count = MultiByteToWideChar(CP_UTF8, 0, mbstr, -1, dst, wcsize);
	dst[wcsize] = 0;
	SCM_ASSERT(count == wcsize);
    }
    return dst;
}

/*
 * Users and groups
 * Kinda Kluge, since we don't have "user id" associated with each
 * user.  (If a domain server is active, Windows security manager seems
 * to assign an unique user id for every user; but it doesn't seem available
 * for stand-alone machine.)
 */

static void convert_user(const USER_INFO_2 *wuser, struct passwd *res)
{
    res->pw_name    = w2mdup(wuser->usri2_name);
    res->pw_passwd  = "*";
    res->pw_uid     = 0;
    res->pw_gid     = 0;
    res->pw_comment = w2mdup(wuser->usri2_comment);
    res->pw_gecos   = w2mdup(wuser->usri2_full_name);
    res->pw_dir     = w2mdup(wuser->usri2_home_dir);
    res->pw_shell   = "";
}

/* Arrgh! thread unsafe!  just for the time being...*/
static struct passwd pwbuf = { "dummy" };

struct passwd *getpwnam(const char *name)
{
    USER_INFO_2 *res;
    if (NetUserGetInfo(NULL, m2wdup(name), 2, (LPBYTE*)&res) != NERR_Success) {
	return NULL;
    }
    convert_user(res, &pwbuf);
    NetApiBufferFree(res);
    return &pwbuf;
}

struct passwd *getpwuid(uid_t uid)
{
    /* for the time being, we just ignore uid and returns the current
       user info. */
#define NAMELENGTH 256
    char buf[NAMELENGTH];
    DWORD len = NAMELENGTH;
    if (GetUserName(buf, &len) == 0) {
	return NULL;
    }
    return getpwnam(buf);
}

static struct group dummy_group = {
    "dummy",
    "",
    100,
    NULL
};

struct group *getgrgid(gid_t gid)
{
    return &dummy_group;
}

struct group *getgrnam(const char *name)
{
    return &dummy_group;
}

/* Kluge kluge kluge */
uid_t getuid(void)
{
    return 0;
}

uid_t geteuid(void)
{
    return 0;
}

gid_t getgid(void)
{
    return 0;
}

gid_t getegid(void)
{
    return 0;
}

/*
 * Getting parent process ID.  I wonder why it is such a hassle, but
 * the use of Process32First is indeed suggested in the MS document.
 */
pid_t getppid(void)
{
    HANDLE snapshot;
    PROCESSENTRY32 entry;
    DWORD myid = GetCurrentProcessId(), parentid;
    int found = FALSE;
    
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
	Scm_Error("couldn't take process snapshot in getppid()");
    }
    entry.dwSize = sizeof(PROCESSENTRY32);
    if (!Process32First(snapshot, &entry)) {
	CloseHandle(snapshot);
	Scm_Error("Process32First failed in getppid()");
    }
    do {
	if (entry.th32ProcessID == myid) {
	    parentid = entry.th32ParentProcessID;
	    found = TRUE;
	    break;
	}
    } while (Process32Next(snapshot, &entry));
    CloseHandle(snapshot);
    if (!found) {
	Scm_Error("couldn't find the current process entry in getppid()");
    }
    return parentid;
}


/*
 * Other obscure stuff
 */

int fork(void)
{
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return -1;
}

int kill(pid_t pid, int signal)
{
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return -1; /*TODO: is there any alternative? */
}

int pipe(int fd[])
{
#define PIPE_BUFFER_SIZE 512
    int r = _pipe(fd, PIPE_BUFFER_SIZE, O_BINARY);
    return r;
}

char *ttyname(int desc)
{
    return NULL;
}

int truncate(const char *path, off_t len)
{
    return -1;
}

int ftruncate(int fd, off_t len)
{
    return -1;
}

unsigned int alarm(unsigned int seconds)
{
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    Scm_SysError("alarm");
    return 0;
}

/* file links */
int link(const char *existing, const char *newpath)
{
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    Scm_Error("link");
    return -1;
#if 0 /* only on NTFS */
    BOOL r = CreateHardLink((LPCTSTR)newpath, (LPCTSTR)existing, NULL);
    return r? -1 : 0;
#endif
}

#endif /*__MINGW32__*/


/*===============================================================
 * Initialization
 */
void Scm__InitSystem(void)
{
    ScmModule *mod = Scm_GaucheModule();
    Scm_InitStaticClass(&Scm_SysStatClass, "<sys-stat>", mod, stat_slots, 0);
    Scm_InitStaticClass(&Scm_TimeClass, "<time>", mod, time_slots, 0);
    Scm_InitStaticClass(&Scm_SysTmClass, "<sys-tm>", mod, tm_slots, 0);
    Scm_InitStaticClass(&Scm_SysGroupClass, "<sys-group>", mod, grp_slots, 0);
    Scm_InitStaticClass(&Scm_SysPasswdClass, "<sys-passwd>", mod, pwd_slots, 0);
#ifdef HAVE_SELECT
    Scm_InitStaticClass(&Scm_SysFdsetClass, "<sys-fdset>", mod, NULL, 0);
#endif
}
