/*
 * Copyright (C) 2013-2020 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * This code is a complete clean re-write of the stress tool by
 * Colin Ian King <colin.king@canonical.com> and attempts to be
 * backwardly compatible with the stress tool by Amos Waterland
 * <apw@rossby.metr.ou.edu> but has more stress tests and more
 * functionality.
 *
 */
#include "stress-ng.h"

static const stress_help_t help[] = {
	{ NULL,	"sysinval N",		"start N workers that pass invalid args to syscalls" },
	{ NULL,	"sysinval-ops N",	"stop after N sysinval bogo syscalls" },
	{ NULL,	NULL,		    NULL }
};

#if defined(__linux__)

#define ARG_MASK(x, mask)	(((x) & (mask)) == (mask))

#define SYSCALL_HASH_TABLE_SIZE	(10007)	/* Hash table size (prime) */
#define SYSCALL_FAIL		(0x00)	/* Expected behaviour */
#define	SYSCALL_CRASH		(0x01)	/* Syscalls that crash the child */
#define SYSCALL_ERRNO_ZERO	(0x02)	/* Syscalls that return 0 */

#define MAX_CRASHES		(10)

/*
 *  tuple of system call number and stringified system call
 */
#if defined(__NR_exit)
#define SYS(x)		__NR_ ## x, # x
#define DEFSYS(x)	__NR_ ## x
#elif defined(SYS_exit)
#define SYS(x)		SYS_ ## x, # x
#define DEFSYS(x)	SYS_ ## x
#else
#define SYS(x) 		0, "unknown"
#define DEFSYS(x) 	0
#endif

#define ARG_VALUE(x, v)	{ (x), SIZEOF_ARRAY(v), (unsigned long *)v }

/*
 *  system call argument types
 */
#define ARG_NONE		0x00000000
#define ARG_PTR			0x00000001
#define ARG_INT			0x00000002
#define ARG_UINT		0x00000004
#define ARG_SOCKFD		0x00000010
#define ARG_STRUCT_SOCKADDR	0x00000020
#define ARG_SOCKLEN_T		0x00000040
#define ARG_FLAG		0x00000080
#define ARG_BRK_ADDR		0x00000100
#define ARG_MODE		0x00000200
#define ARG_LEN			0x00000400
#define ARG_SECONDS		0x00001000
#define ARG_BPF_ATTR		0x00002000
#define ARG_EMPTY_FILENAME	0x00004000	/* "" */
#define ARG_DEVZERO_FILENAME	0x00008000	/* /dev/zero */
#define ARG_CLOCKID_T		0x00010000
#define ARG_FUNC_PTR		0x00020000
#define ARG_FD			0x00040000
#define ARG_TIMEOUT		0x00080000
#define ARG_DIRFD		0x00100000
#define ARG_DEVNULL_FILENAME	0x00200000	/* /dev/null */
#define ARG_RND			0x00400000
#define ARG_PID			0x00800000
#define ARG_NON_NULL_PTR	0x01000000
#define ARG_NON_ZERO_LEN	0x02000000
#define ARG_GID			0x04000000
#define ARG_UID			0x08000000
#define ARG_FUTEX_PTR		0x10000000

/*
 *  rotate right for hashing
 */
#define ROR(val)						\
do {								\
	unsigned long tmp = val;				\
	const size_t bits = (sizeof(unsigned long) * 8) - 1;	\
	const unsigned long bit0 = (tmp & 1) << bits;		\
	tmp >>= 1;                              		\
	tmp |= bit0;                            		\
	val = tmp;                              		\
} while (0)

#define SHR_UL(v, shift) ((unsigned long)(((unsigned long long)v) << shift))

/*
 *  per system call testing information, each system call
 *  to be exercised has one or more of these records.
 */
typedef struct {
	const unsigned long syscall;	/* system call number */
	const char *name;		/* text name of system call */
	const int num_args;		/* number of arguments */
	unsigned long args[6];		/* semantic info about each argument */
} syscall_arg_t;

/*
 *  argument semantic information, unique argument types
 *  have one of these records to represent the different
 *  invalid argument values. Keep these values as short
 *  as possible as each new value increases the number of
 *  permutations
 */
typedef struct {
	unsigned long mask;		/* bitmask representing arg type */
	size_t num_values;		/* number of different invalid values */
	unsigned long *values;		/* invalid values */
} syscall_arg_values_t;

/*
 *  hash table entry for syscalls and arguments that need
 *  to be skipped either because they crash the child or
 *  because the system call succeeds
 */
typedef struct syscall_args_hash {
	struct syscall_args_hash *next;	/* next item in list */
	unsigned long hash;		/* has of system call and args */
	unsigned long syscall;		/* system call number */
	unsigned long args[6];		/* arguments */
	uint8_t	 type;			/* type of failure */
} syscall_arg_hash_t;

/*
 *  hash table - in the parent context this records system
 *  calls that crash the child. in the child context this
 *  contains the same crasher data that the parent has plus
 *  a cache of the system calls that return 0 and we don't
 *  want to retest - this child cached data is lost when the
  * child crashes.
 */
static syscall_arg_hash_t *hash_table[SYSCALL_HASH_TABLE_SIZE];

static const int sigs[] = {
#if defined(SIGILL)
	SIGILL,
#endif
#if defined(SIGTRAP)
	SIGTRAP,
#endif
#if defined(SIGFPE)
	SIGFPE,
#endif
#if defined(SIGBUS)
	SIGBUS,
#endif
#if defined(SIGSEGV)
	SIGSEGV,
#endif
#if defined(SIGIOT)
	SIGIOT,
#endif
#if defined(SIGEMT)
	SIGEMT,
#endif
#if defined(SIGALRM)
	SIGALRM,
#endif
#if defined(SIGINT)
	SIGINT,
#endif
#if defined(SIGHUP)
	SIGHUP
#endif
};

static uint8_t *small_ptr;
static uint8_t *page_ptr;

static const syscall_arg_t syscall_args[] = {
#if DEFSYS(_llseek)
	{ SYS(_llseek), 5, { ARG_FD, ARG_UINT, ARG_UINT, ARG_PTR, ARG_INT } },
#endif
#if DEFSYS(_sysctl)
	{ SYS(_sysctl), 1, { ARG_PTR } },
#endif
#if DEFSYS(accept)
	{ SYS(accept), 3, { ARG_SOCKFD, ARG_PTR | ARG_STRUCT_SOCKADDR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(accept4)
	{ SYS(accept4), 4, { ARG_SOCKFD, ARG_PTR | ARG_STRUCT_SOCKADDR, ARG_PTR, ARG_FLAG, 0, 0 } },
#endif
#if DEFSYS(access)
	{ SYS(access), 2, { ARG_PTR | ARG_EMPTY_FILENAME, ARG_MODE, 0, 0, 0, 0 } },
	{ SYS(access), 2, { ARG_PTR | ARG_DEVZERO_FILENAME, ARG_MODE, 0, 0, 0, 0 } },
#endif
#if DEFSYS(acct)
	{ SYS(acct), 1, { ARG_PTR | ARG_EMPTY_FILENAME, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(add_key)
	{ SYS(add_key), 5, { ARG_PTR, ARG_PTR, ARG_PTR, ARG_LEN, ARG_UINT, 0 } },
#endif
#if DEFSYS(adjtimex)
	{ SYS(adjtimex), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(alarm) && 0
	{ SYS(alarm), 1, { ARG_SECONDS, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(alloc_hugepages)
	/* removed in 2.5.44 */
	{ SYS(alloc_hugepages), 5, { ARG_INT, ARG_PTR, ARG_LEN, ARG_INT, ARG_FLAG) } },
#endif
#if DEFSYS(arc_gettls)
	/* ARC only */
#endif
#if DEFSYS(arc_settls)
	/* ARC only */
#endif
#if DEFSYS(arc_usr_cmpxchg)
	/* ARC only */
#endif
#if DEFSYS(arch_prctl)
	{ SYS(arch_prctl), 2, { ARG_INT, ARG_UINT } },
	{ SYS(arch_prctl), 2, { ARG_INT, ARG_PTR } },
#endif
#if DEFSYS(atomic_barrier)
	/* m68k only */
#endif
#if DEFSYS(atomic_cmpxchg_32)
	/* m68k only */
#endif
#if DEFSYS(bdflush)
	/* deprecated */
	{ SYS(bdflush), 2, { ARG_INT, ARG_PTR } },
	{ SYS(bdflush), 2, { ARG_INT, ARG_UINT } },
#endif
#if DEFSYS(bfin_spinlock)
	/* blackfin, removed in 4.17 */
#endif
#if DEFSYS(bind)
	{ SYS(bind), 3, { ARG_SOCKFD, ARG_PTR | ARG_STRUCT_SOCKADDR, ARG_SOCKLEN_T, 0, 0, 0 } },
#endif
#if DEFSYS(bpf)
	{ SYS(bpf), 3, { ARG_INT, ARG_PTR | ARG_BPF_ATTR, ARG_LEN, 0, 0, 0 } },
#endif
#if DEFSYS(brk)
	{ SYS(brk), 1, { ARG_PTR | ARG_BRK_ADDR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(breakpoint)
	/* ARM OABI only */
#endif
#if DEFSYS(cacheflush)
	{ SYS(cacheflush), 3, { ARG_PTR, ARG_INT, ARG_INT, 0, 0, 0 } },
#endif
#if DEFSYS(capget)
	{ SYS(capget), 2, { ARG_INT, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(capset)
	{ SYS(capset), 2, { ARG_INT, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(chdir)
	{ SYS(chdir), 1, { ARG_PTR | ARG_EMPTY_FILENAME, 0, 0, 0, 0, 0 } },
	{ SYS(chdir), 1, { ARG_PTR | ARG_DEVZERO_FILENAME, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(chmod)
	{ SYS(chmod), 2, { ARG_PTR | ARG_EMPTY_FILENAME, ARG_INT, 0, 0, 0, 0 } },
#endif
#if DEFSYS(chown)
	{ SYS(chown), 2, { ARG_PTR | ARG_EMPTY_FILENAME, ARG_INT, 0, 0, 0, 0 } },
#endif
#if DEFSYS(chroot)
	{ SYS(chroot), 1, { ARG_PTR | ARG_EMPTY_FILENAME, 0, 0, 0, 0, 0 } },
	{ SYS(chroot), 1, { ARG_PTR | ARG_DEVZERO_FILENAME, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(clock_adjtime)
	{ SYS(clock_adjtime), 2, { ARG_CLOCKID_T, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(clock_getres)
	{ SYS(clock_getres), 2, { ARG_CLOCKID_T, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(clock_gettime)
	{ SYS(clock_gettime), 2, { ARG_CLOCKID_T, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(clock_nanosleep)
	{ SYS(clock_nanosleep), 4, { ARG_CLOCKID_T, ARG_UINT, ARG_PTR, ARG_PTR, 0, 0 } },
#endif
#if DEFSYS(clock_settime)
	{ SYS(clock_settime), 2, { ARG_CLOCKID_T, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(clone)
	{ SYS(clone), 6, { ARG_FUNC_PTR, ARG_PTR, ARG_INT, ARG_PTR, ARG_PTR, ARG_PTR } },
#endif
#if DEFSYS(clone2)
	/* IA-64 only */
#endif
#if DEFSYS(clone3)
	{ SYS(clone3), 2, { ARG_PTR, ARG_LEN, 0, 0, 0, 0 } },
#endif
#if DEFSYS(close)
	{ SYS(close), 1, { ARG_FD, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(cmpxchg_badaddr)
	/* Tile only, removed 4.17 */
#endif
#if DEFSYS(connect)
	{ SYS(connect), 3, { ARG_SOCKFD, ARG_PTR, ARG_LEN, 0, 0, 0 } },
#endif
#if DEFSYS(copy_file_range)
	{ SYS(copy_file_range), 6, { ARG_FD, ARG_PTR, ARG_FD, ARG_PTR, ARG_LEN, ARG_FLAG } },
#endif
#if DEFSYS(creat)
	{ SYS(creat), 3, { ARG_EMPTY_FILENAME, ARG_FLAG, ARG_MODE, 0, 0, 0 } },
#endif
#if DEFSYS(create_module)
	{ SYS(create_module), 2, { ARG_PTR, ARG_LEN, 0, 0, 0, 0 } },
#endif
#if DEFSYS(create_module)
	{ SYS(delete_module), 2, { ARG_PTR, ARG_FLAG, 0, 0, 0, 0 } },
#endif
#if DEFSYS(dma_memcpy)
	/* blackfin, removed in 4.17 */
#endif
#if DEFSYS(dup)
	{ SYS(dup), 1, { ARG_FD, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(dup2)
	{ SYS(dup2), 2, { ARG_FD, ARG_FD, 0, 0, 0, 0 } },
#endif
#if DEFSYS(dup3)
	{ SYS(dup3), 3, { ARG_FD, ARG_FD, ARG_FLAG, 0, 0, 0 } },
#endif
#if DEFSYS(epoll_create)
	{ SYS(epoll_create), 1, { ARG_LEN,  0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(epoll_create1)
	{ SYS(epoll_create1), 1, { ARG_FLAG, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(epoll_ctl)
	{ SYS(epoll_ctl), 4, { ARG_FD, ARG_INT, ARG_FD, ARG_PTR, 0, 0 } },
#endif
#if DEFSYS(epoll_ctl_add)
	{ SYS(epoll_ctl_add), 4, { ARG_FD, ARG_INT, ARG_FD, ARG_PTR, 0, 0 } },
#endif
#if DEFSYS(epoll_wait)
	{ SYS(epoll_wait), 4, { ARG_FD, ARG_PTR, ARG_INT, ARG_TIMEOUT, 0, 0 } },
#endif
#if DEFSYS(epoll_pwait)
	{ SYS(epoll_pwait), 5, { ARG_FD, ARG_PTR, ARG_INT, ARG_TIMEOUT, ARG_PTR, 0 } },
#endif
#if DEFSYS(evendfd)
	{ SYS(eventfd), 2, { ARG_INT, ARG_FLAG, 0, 0, 0, 0 } },
#endif
#if DEFSYS(evendfd2)
	{ SYS(eventfd2), 2, { ARG_INT, ARG_FLAG, 0, 0, 0, 0 } },
#endif
#if DEFSYS(exec)
	/* too risky for now */
#endif
#if DEFSYS(execv)
	/* too risky for now */
#endif
#if DEFSYS(execve)
	/* too risky for now */
#endif
#if DEFSYS(exit)
	/* exiting the testing child is not a good idea */
#endif
#if DEFSYS(exit_group)
	/* exiting the testing child is not a good idea */
#endif
#if DEFSYS(faccessat)
	{ SYS(faccessat), 4, { ARG_DIRFD, ARG_EMPTY_FILENAME, ARG_MODE, ARG_FLAG, 0, 0 } },
	{ SYS(faccessat), 4, { ARG_DIRFD, ARG_DEVNULL_FILENAME, ARG_MODE, ARG_FLAG, 0, 0 } },
#endif
#if DEFSYS(fadvise64)
	{ SYS(fadvise64), 4, { ARG_FD, ARG_UINT, ARG_LEN, ARG_INT, 0, 0 } },
#endif
#if DEFSYS(fadvise64_64)
	/* TODO */
#endif
#if DEFSYS(fallocate)
	{ SYS(fallocate), 4, { ARG_FD, ARG_MODE, ARG_INT, ARG_INT, 0, 0 } },
#endif
#if DEFSYS(fanotify_init)
	{ SYS(fanotify_init), 2, { ARG_FLAG, ARG_FLAG, 0, 0, 0, 0 } },
#endif
#if DEFSYS(fanotify_mark)
	{ SYS(fanotify_mark), 5, { ARG_FD, ARG_FLAG, ARG_UINT, ARG_FD, ARG_EMPTY_FILENAME, 0 } },
#endif
#if DEFSYS(fchdir)
	{ SYS(fchdir), 1, { ARG_FD, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(fchmod)
	{ SYS(fchmod), 2, { ARG_FD, ARG_MODE, 0, 0, 0, 0 } },
#endif
#if DEFSYS(fchmodat)
	{ SYS(fchmodat), 4, { ARG_DIRFD, ARG_EMPTY_FILENAME, ARG_MODE, ARG_FLAG, 0, 0 } },
#endif
#if DEFSYS(fchown)
	// { SYS(fchown), 3, { ARG_FD, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(fchownat)
	{ SYS(fchownat), 5, { ARG_DIRFD, ARG_EMPTY_FILENAME, ARG_UINT, ARG_UINT, ARG_UINT, 0 } },
#endif
#if DEFSYS(fcntl)
	{ SYS(fcntl), 6, { ARG_FD, ARG_RND, ARG_RND, ARG_RND, ARG_RND, ARG_RND } },
#endif
#if DEFSYS(fcntl64)
	{ SYS(fcntl64), 6, { ARG_FD, ARG_RND, ARG_RND, ARG_RND, ARG_RND, ARG_RND } },
#endif
#if DEFSYS(fdatasync)
	{ SYS(fdatasync), 1, { ARG_FD, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(fgetxattr)
	{ SYS(fgetxattr), 4, { ARG_FD, ARG_EMPTY_FILENAME, ARG_PTR, ARG_LEN, 0, 0 } },
	{ SYS(fgetxattr), 4, { ARG_FD, ARG_DEVNULL_FILENAME, ARG_PTR, ARG_LEN, 0, 0 } },
#endif
#if DEFSYS(finit_module)
	{ SYS(finit_module), 3, { ARG_PTR, ARG_LEN, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(flistxattr) || defined(HAVE_FLISTXATTR)
	{ SYS(flistxattr), 3, { ARG_FD, ARG_PTR, ARG_LEN, 0, 0, 0 } },
#endif
#if DEFSYS(flock)
	{ SYS(flock), 2, { ARG_FD, ARG_INT, 0, 0, 0, 0 } },
#endif
#if DEFSYS(fremovexattr) || defined(HAVE_FREMOVEXATTR)
	{ SYS(fremovexattr), 2, { ARG_FD, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(fsconfig)
#endif
#if defined(fsetxattr) || defined(HAVE_FSETXATTR)
	{ SYS(fsetxattr), 5, { ARG_FD, ARG_PTR, ARG_PTR, ARG_LEN, ARG_FLAG, 0, } },
#endif
#if DEFSYS(fsmount)
#endif
#if DEFSYS(fsopen)
#endif
#if DEFSYS(fspick)
#endif
#if DEFSYS(fstat)
	{ SYS(fstat), 2, { ARG_FD, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(fstat64)
#endif
#if DEFSYS(fstatat)
	{ SYS(fstatat), 4, { ARG_DIRFD, ARG_EMPTY_FILENAME, ARG_PTR, ARG_FLAG, 0, 0 } },
#endif
#if DEFSYS(fstatat64)
	{ SYS(fstatat64), 4, { ARG_DIRFD, ARG_EMPTY_FILENAME, ARG_PTR, ARG_FLAG, 0, 0 } },
#endif
#if DEFSYS(fstatfs)
	{ SYS(fstatfs), 2, { ARG_FD, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(fsync)
	{ SYS(fsync), 1, { ARG_FD, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(ftruncate)
	{ SYS(ftruncate), 2, { ARG_FD, ARG_LEN, 0, 0, 0, 0 } },
#endif
#if DEFSYS(ftruncate64)
	{ SYS(ftruncate64), 2, { ARG_FD, ARG_LEN, 0, 0, 0, 0 } },
#endif
#if DEFSYS(futex)
	{ SYS(futex), 6, { ARG_FUTEX_PTR, ARG_INT, ARG_INT, ARG_FUTEX_PTR, ARG_FUTEX_PTR, ARG_INT } },
#endif
#if DEFSYS(futimens)
	{ SYS(futimens), 4, { ARG_FD, ARG_EMPTY_FILENAME, ARG_PTR, ARG_FLAG, 0, 0 } },
#endif
#if DEFSYS(get_kernel_syms)
	/* deprecated in 2.6 */
	{ SYS(get_kernel_syms), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(get_mempolicy)
	{ SYS(get_mempolicy), 5, { ARG_PTR, ARG_PTR, ARG_UINT, ARG_PTR, ARG_FLAG, 0 } },
#endif
#if DEFSYS(get_robust_list)
	{ SYS(get_robust_list), 3, { ARG_PID, ARG_PTR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(get_thread_area)
	{ SYS(get_thread_area), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(get_tls)
	/* ARM OABI only */
#endif
#if DEFSYS(getcpu)
	{ SYS(getcpu), 3, { ARG_NON_NULL_PTR, ARG_NON_NULL_PTR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(getcwd)
	{ SYS(getcwd), 2, { ARG_PTR, ARG_LEN, 0, 0, 0, 0 } },
#endif
#if DEFSYS(getdtablesize)
	/* SPARC, removed in 2.6.26 */
#endif
#if DEFSYS(getdents)
	{ SYS(getdents), 3, { ARG_FD, ARG_PTR, ARG_LEN, 0, 0, 0 } },
#endif
#if DEFSYS(getdents64)
	{ SYS(getdents64), 3, { ARG_FD, ARG_PTR, ARG_LEN, 0, 0, 0 } },
#endif
#if DEFSYS(getdomainname)
	{ SYS(getdomainname), 2, { ARG_PTR, ARG_LEN, 0, 0, 0, 0 } },
#endif
#if DEFSYS(getegid)
	/* { SYS(getegid), 0, { 0, 0, 0, 0, 0, 0 } }, */
#endif
#if DEFSYS(getegid32)
	/* { SYS(getegid32), 0, { 0, 0, 0, 0, 0, 0 } }, */
#endif
#if DEFSYS(geteuid)
	/* { SYS(geteuid), 0, { 0, 0, 0, 0, 0, 0 } }, */
#endif
#if DEFSYS(geteuid32)
	/* { SYS(geteuid), 0, { 0, 0, 0, 0, 0, 0 } }, */
#endif
#if DEFSYS(getgid)
	/* { SYS(getgid), 0, { 0, 0, 0, 0, 0, 0 } }, */
#endif
#if DEFSYS(getgid32)
	/* { SYS(getgid32), 0, { 0, 0, 0, 0, 0, 0 } }, */
#endif
#if DEFSYS(getgroups)
	{ SYS(getgroups), 2, { ARG_INT, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(getgroups32)
	{ SYS(getgroups32), 2, { ARG_INT, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(gethostname)
	{ SYS(gethostname), 2, { ARG_PTR, ARG_LEN, 0, 0, 0, 0 } },
#endif
#if DEFSYS(getitimer)
	{ SYS(getitimer), 2, { ARG_INT, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(getpagesize)
	/* { SYS(getpagesize), 0, { 0, 0, 0, 0, 0, 0 } }, */
#endif
#if DEFSYS(getpeername)
	{ SYS(getpeername), 3, { ARG_SOCKFD, ARG_PTR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(getpgid)
	{ SYS(getpgid), 1, { ARG_PID, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(getpid)
	/* { SYS(getpid), 0, { 0, 0, 0, 0, 0, 0 } }, */
#endif
#if DEFSYS(getppid)
	/* { SYS(getppid), 0, { 0, 0, 0, 0, 0, 0 } }, */
#endif
#if DEFSYS(getpriority)
	{ SYS(getpriority), 2, { ARG_INT, ARG_INT, 0, 0, 0, 0 } },
#endif
#if DEFSYS(getrandom)
	{ SYS(getrandom), 3, { ARG_PTR, ARG_INT, ARG_FLAG, 0, 0, 0 } },
#endif
#if DEFSYS(getresgid)
	{ SYS(getresgid), 3, { ARG_PTR, ARG_PTR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(getresgid32)
	{ SYS(getresgid32), 3, { ARG_PTR, ARG_PTR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(getresuid)
	{ SYS(getresuid), 3, { ARG_PTR, ARG_PTR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(getresuid32)
	{ SYS(getresuid), 3, { ARG_PTR, ARG_PTR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(getrlimit)
	{ SYS(getrlimit), 2, { ARG_RND, ARG_PTR, 0, 0, 0, 0 } },
	{ SYS(getrlimit), 2, { ARG_INT, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(getrusage)
	{ SYS(getrusage), 2, { ARG_RND, ARG_PTR, 0, 0, 0, 0 } },
	{ SYS(getrusage), 2, { ARG_INT, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(getsid)
	{ SYS(getsid), 1, { ARG_PID, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(getsockname)
	{ SYS(getsockname), 3, { ARG_SOCKFD, ARG_PTR | ARG_STRUCT_SOCKADDR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(getsockopt)
	{ SYS(getsockopt), 5, { ARG_SOCKFD, ARG_INT, ARG_INT, ARG_PTR, ARG_PTR, 0 } },
#endif
#if DEFSYS(gettid)
	/* { SYS(gettid), 0, { 0, 0, 0, 0, 0, 0 } }, */
#endif
#if DEFSYS(getuid)
	/* { SYS(getuid), 0, { 0, 0, 0, 0, 0, 0 } }, */
#endif
#if DEFSYS(getuid32)
	/* { SYS(getuid32), 0, { 0, 0, 0, 0, 0, 0 } }, */
#endif
#if DEFSYS(getunwind)
	/* IA-64-specific, obsolete too */
	{ SYS(getunwind), 2, { ARG_PTR, ARG_LEN, 0, 0, 0, 0 } },
#endif
#if DEFSYS(gettimeofday)
	{ SYS(gettimeofday), 2, { ARG_NON_NULL_PTR, ARG_NON_NULL_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(getxattr)
	{ SYS(getxattr), 4, { ARG_EMPTY_FILENAME, ARG_PTR, ARG_PTR, ARG_LEN, 0, 0 } },
	{ SYS(getxattr), 4, { ARG_DEVNULL_FILENAME, ARG_PTR, ARG_PTR, ARG_LEN, 0, 0 } },
#endif
#if DEFSYS(getxgid)
	/* Alpha only */
#endif
#if DEFSYS(getxpid)
	/* Alpha only */
#endif
#if DEFSYS(getxuid)
	/* Alpha only */
#endif
#if DEFSYS(init_module)
	{ SYS(init_module), 3, { ARG_PTR, ARG_LEN, ARG_PTR } },
#endif
#if DEFSYS(inotify_add_watch)
	{ SYS(inotify_add_watch), 3, { ARG_FD, ARG_EMPTY_FILENAME, ARG_UINT, 0, 0, 0 } },
	{ SYS(inotify_add_watch), 3, { ARG_FD, ARG_DEVNULL_FILENAME, ARG_UINT, 0, 0, 0 } },
#endif
#if DEFSYS(inotify_init)
	/* { SYS(inotify_init), 0, { 0, 0, 0, 0, 0, 0 } }, */
#endif
#if DEFSYS(inotify_init1)
	{ SYS(inotify_init1), 3, { ARG_FLAG, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(inotify_rm_watch)
	{ SYS(inotify_rm_watch), 2, { ARG_FD, ARG_INT, 0, 0, 0, 0 } },
#endif
#if DEFSYS(io_cancel)
	{ SYS(io_cancel), 3, { ARG_INT, ARG_PTR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(io_destroy)
	{ SYS(io_destroy), 1, { ARG_INT, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(io_getevents)
	{ SYS(io_getevents), 5, { ARG_INT, ARG_INT, ARG_INT, ARG_PTR, ARG_PTR, 0 } },
#endif
#if DEFSYS(io_pgetevents)
	/* TODO */
#endif
#if DEFSYS(io_setup)
	{ SYS(io_setup), 2, { ARG_UINT, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(io_submit)
	{ SYS(io_setup), 3, { ARG_UINT, ARG_INT, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(io_uring_enter)
#endif
#if DEFSYS(io_uring_register)
#endif
#if DEFSYS(io_uring_setup)
#endif
#if DEFSYS(ioctl)
	{ SYS(ioctl), 4, { ARG_FD, ARG_UINT, ARG_PTR, ARG_PTR, 0, 0 } },
#endif
#if DEFSYS(ioperm)
	{ SYS(ioperm), 3, { ARG_UINT, ARG_UINT, ARG_INT, 0, 0, 0 } },
#endif
#if DEFSYS(iopl)
	{ SYS(iopl), 1, { ARG_INT, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(ioprio_get)
	{ SYS(ioprio_get), 2, { ARG_INT, ARG_INT, 0, 0, 0, 0 } },
#endif
#if DEFSYS(ioprio_set)
	{ SYS(ioprio_set), 3, { ARG_INT, ARG_INT, ARG_INT, 0, 0, 0 } },
#endif
#if DEFSYS(ipc)
	{ SYS(ipc), 6, { ARG_UINT, ARG_INT, ARG_INT, ARG_INT, ARG_PTR, ARG_UINT } },
#endif
#if DEFSYS(kcmp)
	{ SYS(kcmp), 5, { ARG_PID, ARG_PID, ARG_INT, ARG_UINT, ARG_UINT, 0 } },
#endif
#if DEFSYS(kern_features)
	/* SPARC64 only */
#endif
#if DEFSYS(kexec_file_load)
	/* { SYS(kexec_file_load), 5, { ARG_FD, ARG_FD, ARG_UINT, ARG_PTR, ARG_FLAG, 0 } }, */
#endif
#if DEFSYS(kexec_load)
	/* { SYS(kexec_load), 4, { ARG_UINT, ARG_UINT, ARG_PTR, ARG_FLAG, 0, 0 } }, */
#endif
#if DEFSYS(keyctl)
	{ SYS(keyctl), 6, { ARG_INT, ARG_UINT, ARG_UINT, ARG_UINT, ARG_UINT, ARG_UINT } },
#endif
#if DEFSYS(kill)
	/* { SYS(kill), ARG_PID, ARG_INT, 0, 0, 0, 0 } }, */
#endif
#if DEFSYS(lchown)
	{ SYS(lchown), 3, { ARG_EMPTY_FILENAME, ARG_INT, ARG_INT, 0, 0, 0 } },
#endif
#if DEFSYS(lchown32)
	{ SYS(lchown32), 3, { ARG_EMPTY_FILENAME, ARG_INT, ARG_INT, 0, 0, 0 } },
#endif
#if DEFSYS(link)
	{ SYS(link), 2, { ARG_EMPTY_FILENAME, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(linkat)
	{ SYS(linkat), 5, { ARG_FD, ARG_EMPTY_FILENAME, ARG_FD, ARG_EMPTY_FILENAME, ARG_INT, 0 } },
#endif
#if DEFSYS(listen)
	{ SYS(listen), 2, { ARG_SOCKFD, ARG_INT, 0, 0, 0, 0 } },
#endif
#if DEFSYS(listxattr)
	{ SYS(listxattr), 3, { ARG_EMPTY_FILENAME, ARG_PTR, ARG_LEN, 0, 0, 0 } },
#endif
#if DEFSYS(llistxattr)
	{ SYS(llistxattr), 3, { ARG_EMPTY_FILENAME, ARG_PTR, ARG_LEN, 0, 0, 0 } },
#endif
#if DEFSYS(lookup_dcookie)
	{ SYS(lookup_dcookie), 3, { ARG_UINT, ARG_PTR, ARG_LEN, 0, 0, 0 } },
#endif
#if DEFSYS(lremovexattr)
	{ SYS(lremovexattr), 3, { ARG_EMPTY_FILENAME, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(lseek)
	{ SYS(lseek), 3, { ARG_FD, ARG_UINT, ARG_INT, 0, 0, 0 } },
#endif
#if DEFSYS(lsetxattr)
	{ SYS(lsetxattr), 5, { ARG_EMPTY_FILENAME, ARG_PTR, ARG_PTR, ARG_LEN, ARG_INT, 0 } },
#endif
#if DEFSYS(lstat)
	{ SYS(lstat), 2, { ARG_EMPTY_FILENAME, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(lstat64)
	{ SYS(lstat64), 2, { ARG_EMPTY_FILENAME, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(madvise)
	{ SYS(madvise), 3, { ARG_PTR, ARG_LEN, ARG_INT, 0, 0, 0 } },
#endif
#if DEFSYS(mbind)
	{ SYS(mbind), 6, { ARG_PTR, ARG_UINT, ARG_INT, ARG_PTR, ARG_UINT, ARG_UINT } },
#endif
#if DEFSYS(memory_ordering)
	/* SPARC64 only */
#endif
#if DEFSYS(membarrier)
	{ SYS(membarrier), 2, { ARG_INT, ARG_FLAG, 0, 0, 0, 0 } },
#endif
#if DEFSYS(memfd_create)
	{ SYS(memfd_create), 2, { ARG_EMPTY_FILENAME, ARG_UINT, 0, 0, 0, 0 } },
#endif
#if DEFSYS(migrate_pages)
	{ SYS(migrate_pages), 4, { ARG_PID, ARG_UINT, ARG_PTR, ARG_PTR, 0, 0 } },
#endif
#if DEFSYS(mincore)
	{ SYS(mincore), 3, { ARG_PTR, ARG_LEN, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(mkdir)
	{ SYS(mkdir), 2, { ARG_EMPTY_FILENAME, ARG_MODE, 0, 0, 0, 0 } },
#endif
#if DEFSYS(mkdirat)
	{ SYS(mkdirat), 3, { ARG_DIRFD, ARG_EMPTY_FILENAME, ARG_MODE, 0, 0, 0 } },
#endif
#if DEFSYS(mknod)
	{ SYS(mknod), 3, { ARG_EMPTY_FILENAME, ARG_MODE, ARG_UINT, 0, 0, 0 } },
#endif
#if DEFSYS(mknodat)
	{ SYS(mknodat), 4, { ARG_DIRFD, ARG_EMPTY_FILENAME, ARG_MODE, ARG_UINT, 0, 0 } },
#endif
#if DEFSYS(mlock)
	{ SYS(mlock), 2, { ARG_PTR, ARG_LEN, 0, 0, 0, 0 } },
#endif
#if DEFSYS(mlock2)
	{ SYS(mlock2), 2, { ARG_PTR, ARG_LEN, ARG_FLAG, 0, 0, 0 } },
#endif
#if DEFSYS(mlockall)
	{ SYS(mlockall), 1, { ARG_FLAG, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(mmap)
	{ SYS(mmap), 6, { ARG_PTR, ARG_LEN, ARG_INT, ARG_FLAG, ARG_FD, ARG_UINT } },
#endif
#if DEFSYS(mmap2)
	{ SYS(mmap2), 6, { ARG_PTR, ARG_LEN, ARG_INT, ARG_FLAG, ARG_FD, ARG_UINT } },
#endif
#if DEFSYS(modify_ldt)
	{ SYS(modify_ldt), 3, { ARG_INT, ARG_PTR, ARG_LEN, 0, 0, 0 } },
#endif
#if DEFSYS(mount)
	{ SYS(mount), 5, { ARG_EMPTY_FILENAME, ARG_EMPTY_FILENAME, ARG_PTR, ARG_UINT, ARG_UINT, 0 } },
#endif
#if DEFSYS(move_mount)
	//{ SYS(move_mount), 1, { 0, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(move_pages)
	{ SYS(move_pages), 6, { ARG_PID, ARG_UINT, ARG_PTR, ARG_PTR, ARG_PTR, ARG_FLAG } },
#endif
#if DEFSYS(mprotect)
	{ SYS(mprotect), 3, { ARG_PTR, ARG_LEN, ARG_UINT, 0, 0, 0 } },
#endif
#if DEFSYS(mq_close)
	{ SYS(mq_close), 1, { ARG_INT, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(mq_getsetattr)
	{ SYS(mq_getsetattr), 3, { ARG_INT, ARG_PTR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(mq_notify)
	{ SYS(mq_notify), 2, { ARG_INT, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(mq_open)
	{ SYS(mq_open), 4, { ARG_EMPTY_FILENAME, ARG_FLAG, ARG_MODE, ARG_PTR, 0, 0 } },
#endif
#if DEFSYS(mq_receive)
	{ SYS(mq_receive), 4, { ARG_INT, ARG_PTR, ARG_LEN, ARG_INT, 0, 0 } },
#endif
#if DEFSYS(mq_send)
	{ SYS(mq_send), 4, { ARG_INT, ARG_PTR, ARG_LEN, ARG_UINT, 0, 0 } },
#endif
#if DEFSYS(mq_timedreceive)
	{ SYS(mq_timedreceive), 4, { ARG_INT, ARG_PTR, ARG_LEN, ARG_PTR, 0, 0 } },
#endif
#if DEFSYS(mq_timedsend)
	{ SYS(mq_timedsend), 4, { ARG_INT, ARG_PTR, ARG_LEN, ARG_INT, 0, 0 } },
#endif
#if DEFSYS(mq_unlink)
	{ SYS(mq_unlink), 1, { ARG_EMPTY_FILENAME, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(mremap)
	{ SYS(mremap), 5, { ARG_PTR, ARG_LEN, ARG_PTR, ARG_LEN, ARG_FLAG, ARG_PTR } },
#endif
#if DEFSYS(msgctl)
	{ SYS(msgctl), 3, { ARG_INT, ARG_INT, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(msgget)
	{ SYS(msgget), 2, { ARG_INT, ARG_INT, 0, 0, 0, 0 } },
#endif
#if DEFSYS(msgrcv)
	{ SYS(msgrcv), 5, { ARG_INT, ARG_PTR, ARG_LEN, ARG_INT, ARG_INT, 0 } },
#endif
#if DEFSYS(msgsnd)
	{ SYS(msgsnd), 4, { ARG_INT, ARG_PTR, ARG_LEN, ARG_INT, 0, 0 } },
#endif
#if DEFSYS(msync)
	{ SYS(msync), 3, { ARG_PTR, ARG_LEN, ARG_FLAG, 0, 0, 0 } },
#endif
#if DEFSYS(munlock)
	{ SYS(munlock), 2, { ARG_PTR, ARG_LEN, 0, 0, 0, 0 } },
#endif
#if DEFSYS(munlockall)
	{ SYS(munlockall), 1, { ARG_INT, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(munmap)
	//{ SYS(munmap), 2, { ARG_PTR, ARG_LEN, 0, 0, 0, 0 } },
#endif
#if DEFSYS(name_to_handle_at)
	{ SYS(name_to_handle_at), 5, { ARG_DIRFD, ARG_EMPTY_FILENAME, ARG_PTR, ARG_PTR, ARG_FLAG } },
#endif
#if DEFSYS(nanosleep)
	{ SYS(nanosleep), 2, { ARG_PTR, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(newfstatat)
	/* TODO */
#endif
#if DEFSYS(nfsservctl)
	{ SYS(nfsservctl), 3, { ARG_INT, ARG_PTR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(nice)
	{ SYS(nice), 1, { ARG_INT, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(old_adjtimex)
	/* TODO */
#endif
#if DEFSYS(old_getrlimit)
	/* TODO */
#endif
#if DEFSYS(oldolduname)
	/* TODO */
#endif
#if DEFSYS(oldstat)
	/* TODO */
#endif
#if DEFSYS(oldumount)
	/* TODO */
#endif
#if DEFSYS(olduname)
	/* TODO */
#endif
#if DEFSYS(open)
	{ SYS(open), 3, { ARG_EMPTY_FILENAME, ARG_FLAG, ARG_MODE, 0, 0, 0 } },
#endif
#if DEFSYS(open_by_handle_at)
	{ SYS(open_by_handle_at), 3, { ARG_FD, ARG_PTR, ARG_FLAG, 0, 0, 0 } },
#endif
#if DEFSYS(open_tree)
	//{ SYS(open_tree), 1, { 0, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(openat)
	{ SYS(openat), 4, { ARG_DIRFD, ARG_EMPTY_FILENAME, ARG_FLAG, ARG_MODE, 0, 0 } },
#endif
#if DEFSYS(openat2)
	{ SYS(openat2), 4, { ARG_DIRFD, ARG_EMPTY_FILENAME, ARG_PTR, ARG_LEN, 0, 0 } },
#endif
#if DEFSYS(or1k_atomic)
	/* OpenRISC 1000 only */
#endif
#if DEFSYS(pause)
	/* { SYS(pause), 0, { 0, 0, 0, 0, 0, 0 } }, */
#endif
#if DEFSYS(pciconfig_iobase)
	/* { SYS(pciconfig_iobase), 3, { ARG_UINT, ARG_UINT, ARG_UINT, 0, 0, 0 } }, */
#endif
#if DEFSYS(pciconfig_read)
	/* { SYS(pciconfig_read), 3, { ARG_UINT, ARG_UINT, ARG_UINT, ARG_LEN, ARG_PTR, 0 } }, */
#endif
#if DEFSYS(pciconfig_write)
	/* { SYS(pciconfig_write), 3, { ARG_UINT, ARG_UINT, ARG_UINT, ARG_LEN, ARG_PTR, 0 } }, */
#endif
#if DEFSYS(perf_event_open)
	{ SYS(perf_event_open), 5, { ARG_PTR, ARG_PID, ARG_INT, ARG_INT, ARG_FLAG, 0 } },
#endif
#if DEFSYS(personality)
	{ SYS(personality), 1, { ARG_UINT, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(pidfd_getfd)
	{ SYS(pidfd_getfd), 3, { ARG_INT, ARG_INT, ARG_FLAG, 0, 0, 0 } },
#endif
#if DEFSYS(pidfd_open)
	{ SYS(pidfd_open), 2, { ARG_PID, ARG_FLAG, 0, 0, 0, 0 } },
#endif
#if DEFSYS(pidfd_send_signal)
	{ SYS(pidfd_send_signal), 4, { ARG_INT, ARG_INT, ARG_PTR, ARG_FLAG, 0, 0 } },
#endif
#if DEFSYS(pipe)
	{ SYS(pipe), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(pipe2)
	{ SYS(pipe2), 2, { ARG_PTR, ARG_FLAG, 0, 0, 0, 0 } },
#endif
#if DEFSYS(pivot_root)
	{ SYS(pivot_root), 2, { ARG_EMPTY_FILENAME, ARG_EMPTY_FILENAME, 0, 0, 0, 0 } },
#endif
#if DEFSYS(pkey_alloc)
	{ SYS(pkey_alloc), 2, { ARG_FLAG, ARG_UINT, 0, 0, 0, 0 } },
#endif
#if DEFSYS(pkey_free)
	{ SYS(pkey_free), 1, { ARG_INT, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(pkey_get)
	{ SYS(pkey_get), 1, { ARG_INT, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(pkey_mprotect)
	{ SYS(pkey_mprotect), 3, { ARG_PTR, ARG_LEN, ARG_INT, 0, 0, 0 } },
#endif
#if DEFSYS(pkey_set)
	{ SYS(pkey_set), 2, { ARG_INT, ARG_INT, 0, 0, 0, 0 } },
#endif
#if DEFSYS(poll)
	{ SYS(poll), 3, { ARG_PTR, ARG_INT, ARG_INT, 0, 0, 0 } },
#endif
#if DEFSYS(ppoll)
	{ SYS(ppoll), 4, { ARG_PTR, ARG_INT, ARG_PTR, ARG_PTR, 0, 0 } },
#endif
#if DEFSYS(prctl)
	{ SYS(prctl), 5, { ARG_INT, ARG_UINT, ARG_UINT, ARG_UINT, ARG_UINT, 0 } },
#endif
#if DEFSYS(pread)
	{ SYS(pread), 4, { ARG_FD, ARG_PTR, ARG_LEN, ARG_UINT, 0, 0 } },
#endif
#if DEFSYS(pread64)
	{ SYS(pread64), 4, { ARG_FD, ARG_PTR, ARG_LEN, ARG_UINT, 0, 0 } },
#endif
#if DEFSYS(preadv)
	{ SYS(preadv), 4, { ARG_FD, ARG_PTR, ARG_INT, ARG_UINT, 0, 0 } },
#endif
#if DEFSYS(preadv2)
	{ SYS(preadv2), 4, { ARG_FD, ARG_PTR, ARG_INT, ARG_UINT, ARG_FLAG, 0 } },
#endif
#if DEFSYS(prlimit)
	{ SYS(prlimit), 2, { ARG_INT, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(prlimit64)
	{ SYS(prlimit64), 2, { ARG_INT, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(process_madvise)
	{ SYS(process_madvise), 6, { ARG_INT, ARG_PID, ARG_PTR, ARG_LEN, ARG_INT, ARG_FLAG } },
#endif
#if DEFSYS(process_vm_readv)
	{ SYS(process_vm_readv), 6, { ARG_PID, ARG_PTR, ARG_UINT, ARG_PTR, ARG_UINT, ARG_UINT } },
#endif
#if DEFSYS(process_vm_writev)
	{ SYS(process_vm_writev), 6, { ARG_PID, ARG_PTR, ARG_UINT, ARG_PTR, ARG_UINT, ARG_UINT } },
#endif
#if DEFSYS(pselect)
	{ SYS(pselect), 6, { ARG_INT, ARG_PTR, ARG_PTR, ARG_PTR, ARG_PTR, ARG_PTR } },
#endif
#if DEFSYS(ptrace)
	{ SYS(ptrace), 4, { ARG_INT, ARG_PID, ARG_PTR, ARG_PTR, 0, 0 } },
#endif
#if DEFSYS(pwrite)
	{ SYS(pwrite), 4, { ARG_FD, ARG_PTR, ARG_LEN, ARG_UINT, 0, 0 } },
#endif
#if DEFSYS(pwritev)
	{ SYS(pwritev), 4, { ARG_FD, ARG_PTR, ARG_INT, ARG_UINT, 0, 0 } },
#endif
#if DEFSYS(pwritev2)
	{ SYS(pwritev2), 4, { ARG_FD, ARG_PTR, ARG_INT, ARG_UINT, ARG_FLAG, 0 } },
#endif
#if DEFSYS(query_module)
	{ SYS(query_module), 5, { ARG_PTR, ARG_INT, ARG_PTR, ARG_LEN, ARG_PTR, 0 } },
#endif
#if DEFSYS(quotactl)
	{ SYS(quotactl), 5, { ARG_INT, ARG_PTR, ARG_INT, ARG_PTR, 0, 0 } },
#endif
#if DEFSYS(read)
	{ SYS(read), 3, { ARG_FD, ARG_PTR, ARG_LEN, 0, 0, 0 } },
#endif
#if DEFSYS(readahead)
	{ SYS(readahead), 3, { ARG_FD, ARG_UINT, ARG_LEN, 0, 0, 0 } },
#endif
#if DEFSYS(readdir)
	{ SYS(readdir), 3, { ARG_FD, ARG_PTR, ARG_LEN, 0, 0, 0 } },
#endif
#if DEFSYS(readlink)
	{ SYS(readlink), 3, { ARG_EMPTY_FILENAME, ARG_PTR, ARG_LEN, 0, 0, 0 } },
#endif
#if DEFSYS(readlinkat)
	{ SYS(readlinkat), 4, { ARG_DIRFD, ARG_EMPTY_FILENAME, ARG_PTR, ARG_LEN, 0, 0 } },
#endif
#if DEFSYS(readv)
	{ SYS(readv), 3, { ARG_FD, ARG_PTR, ARG_INT, 0, 0, 0 } },
#endif
#if DEFSYS(reboot)
	// { SYS(reboot), 3, { ARG_INT, ARG_INT, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(recv)
	{ SYS(recv), 4, { ARG_SOCKFD, ARG_PTR, ARG_LEN, ARG_FLAG, 0, 0 } },
#endif
#if DEFSYS(recvfrom)
	{ SYS(recvfrom), 6, { ARG_SOCKFD, ARG_PTR, ARG_LEN, ARG_FLAG, ARG_PTR, ARG_PTR } },
#endif
#if DEFSYS(recvmsg)
	{ SYS(recvmsg), 3, { ARG_SOCKFD, ARG_PTR, ARG_FLAG, 0, 0, 0 } },
#endif
#if DEFSYS(recvmmsg)
	{ SYS(recvmmsg), 5, { ARG_SOCKFD, ARG_PTR, ARG_LEN, ARG_FLAG, ARG_PTR, 0 } },
#endif
#if DEFSYS(remap_file_pages)
	{ SYS(remap_file_pages), 5, { ARG_PTR, ARG_LEN, ARG_INT, ARG_UINT, ARG_FLAG, 0 } },
#endif
#if DEFSYS(removexattr)
	{ SYS(removexattr), 2, { ARG_EMPTY_FILENAME, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(rename)
	{ SYS(rename), 2, { ARG_EMPTY_FILENAME, ARG_EMPTY_FILENAME, 0, 0, 0, 0 } },
#endif
#if DEFSYS(renameat)
	{ SYS(renameat), 4, { ARG_DIRFD, ARG_EMPTY_FILENAME, ARG_DIRFD, ARG_EMPTY_FILENAME, 0, 0 } },
#endif
#if DEFSYS(renameat2)
	{ SYS(renameat2), 5, { ARG_DIRFD, ARG_EMPTY_FILENAME, ARG_DIRFD, ARG_EMPTY_FILENAME, ARG_FLAG, 0 } },
#endif
#if DEFSYS(request_key)
	{ SYS(request_key), 4, { ARG_PTR, ARG_PTR, ARG_PTR, ARG_INT, 0, 0 } },
#endif
#if DEFSYS(riscv_flush_icache)
	{ SYS(riscv_flush_icache), 3, { ARG_PTR, ARG_PTR, ARG_INT, 0, 0, 0 } },
#endif
#if DEFSYS(rmdir)
	//{ SYS(rmdir), 1, { ARG_EMPTY_FILENAME, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(rseq)
	{ SYS(rseq), 4, { ARG_PTR, ARG_LEN, ARG_FLAG, ARG_UINT, 0, 0 } },
#endif
#if DEFSYS(sigaction)
	{ SYS(sigaction), 3, { ARG_INT, ARG_PTR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(rt_sigaction)
	{ SYS(rt_sigaction), 3, { ARG_INT, ARG_PTR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(rt_sigpending)
	{ SYS(rt_sigpending), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(rt_sigprocmask)
	{ SYS(rt_sigprocmask), 4, { ARG_INT, ARG_PTR, ARG_PTR, ARG_LEN, 0, 0 } },
#endif
#if DEFSYS(rt_sigqueueinfo)
	{ SYS(rt_sigqueueinfo), 3, { ARG_PID, ARG_INT, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(rt_sigreturn)
	//{ SYS(rt_sigreturn), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(rt_sigsuspend)
	{ SYS(rt_sigsuspend), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(rt_sigtimedwait)
	{ SYS(rt_sigtimedwait), 3, { ARG_PTR, ARG_PTR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(rt_tgsigqueueinfo)
	{ SYS(rt_tgsigqueueinfo), 4, { ARG_PID, ARG_PID, ARG_INT, ARG_PTR, 0, 0 } },
#endif
#if DEFSYS(rtas)
	/* TODO s390x */
#endif
#if DEFSYS(s390_runtime_instr)
	{ SYS(s390_runtime_instr), 2, { ARG_INT, ARG_INT, 0, 0, 0, 0 } },
#endif
#if DEFSYS(s390_pci_mmio_read)
	{ SYS(s390_pci_mmio_read), 3, { ARG_UINT, ARG_PTR, ARG_LEN, 0, 0, 0 } },
#endif
#if DEFSYS(s390_pci_mmio_write)
	{ SYS(s390_pci_mmio_write), 3, { ARG_UINT, ARG_PTR, ARG_LEN, 0, 0, 0 } },
#endif
#if DEFSYS(s390_sthyi)
	{ SYS(s390_sthyi), 4, { ARG_UINT, ARG_PTR, ARG_PTR, ARG_FLAG, 0, 0 } },
#endif
#if DEFSYS(s390_guarded_storage)
	{ SYS(s390_guarded_storage), 4, { ARG_INT, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(sched_get_priority_max)
	{ SYS(sched_get_priority_max), 1, { ARG_INT, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(sched_get_priority_min)
	{ SYS(sched_get_priority_min), 1, { ARG_INT, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(sched_getaffinity)
	{ SYS(sched_getaffinity), 3, { ARG_PID, ARG_LEN, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(sched_getattr)
	{ SYS(sched_getattr), 3, { ARG_PID, ARG_PTR, ARG_FLAG, 0, 0, 0 } },
#endif
#if DEFSYS(sched_getparam)
	{ SYS(sched_getparam), 2, { ARG_PID, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(sched_getscheduler)
	{ SYS(sched_getscheduler), 1, { ARG_PID, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(sched_get_rr_interval)
	{ SYS(sched_get_rr_interval), 2, { ARG_PID, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(sched_set_affinity)
	/* SPARC & SPARC64 */
	{ SYS(sched_set_affinity), 3, { ARG_PID, ARG_LEN, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(sched_setaffinity)
	{ SYS(sched_setaffinity), 3, { ARG_PID, ARG_LEN, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(sched_setattr)
	{ SYS(sched_setattr), 3, { ARG_PID, ARG_PTR, ARG_FLAG, 0, 0, 0 } },
#endif
#if DEFSYS(sched_setparam)
	{ SYS(sched_setparam), 2, { ARG_PID, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(sched_yield)
	//{ SYS(sched_yield), 0, { 0, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(seccomp)
	{ SYS(seccomp), 3, { ARG_UINT, ARG_FLAG, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(select)
	{ SYS(select), 5, { ARG_FD, ARG_PTR, ARG_PTR, ARG_PTR, ARG_PTR, 0 } },
#endif
#if DEFSYS(semctl)
	{ SYS(semctl), 6, { ARG_INT, ARG_INT, ARG_INT, ARG_PTR, ARG_PTR, ARG_PTR } },
#endif
#if DEFSYS(semget)
	{ SYS(semget), 3, { ARG_INT, ARG_INT, ARG_FLAG, 0, 0, 0 } },
#endif
#if DEFSYS(semop)
	{ SYS(semop), 3, { ARG_INT, ARG_PTR, ARG_LEN, 0, 0, 0 } },
#endif
#if DEFSYS(semtimedop)
	{ SYS(semtimedop), 4, { ARG_INT, ARG_PTR, ARG_LEN, ARG_PTR, 0, 0 } },
#endif
/*
 *  The following are not system calls, ignored for now
 */
#if 0
#if DEFSYS(sem_destroy)
	{ SYS(sem_destroy), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(sem_init)
	{ SYS(sem_init), 3, { ARG_PTR, ARG_INT, ARG_INT, 0, 0, 0 } },
#endif
#if DEFSYS(sem_post)
	{ SYS(sem_post), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(sem_wait)
	{ SYS(sem_wait), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(sem_trywait)
	{ SYS(sem_trywait), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(sem_timedwait)
	{ SYS(sem_timedwait), 2, { ARG_PTR, ARG_PTR, 0, 0, 0, 0 } },
#endif
#endif
#if DEFSYS(send)
	{ SYS(send), 4, { ARG_SOCKFD, ARG_PTR, ARG_LEN, ARG_FLAG, 0, 0 } },
#endif
#if DEFSYS(sendfile)
	{ SYS(sendfile), 4, { ARG_FD, ARG_FD, ARG_UINT, ARG_LEN, 0, 0 } },
#endif
#if DEFSYS(sendfile64)
	{ SYS(sendfile64), 4, { ARG_FD, ARG_FD, ARG_UINT, ARG_LEN, 0, 0 } },
#endif
#if DEFSYS(sendmmsg)
	{ SYS(sendmmsg), 4, { ARG_SOCKFD, ARG_PTR, ARG_INT, ARG_FLAG, 0, 0 } },
#endif
#if DEFSYS(sendmsg)
	{ SYS(sendmsg), 3, { ARG_SOCKFD, ARG_PTR, ARG_FLAG, 0, 0, 0 } },
#endif
#if DEFSYS(sendto)
	{ SYS(sendto), 6, { ARG_SOCKFD, ARG_PTR, ARG_LEN, ARG_FLAG, ARG_PTR, ARG_LEN } },
#endif
#if DEFSYS(set_mempolicy)
	{ SYS(set_mempolicy), 3, { ARG_INT, ARG_PTR, ARG_UINT, 0, 0, 0 } },
#endif
#if DEFSYS(set_robust_list)
	{ SYS(set_robust_list), 2, { ARG_PTR, ARG_LEN, 0, 0, 0, 0 } },
#endif
#if DEFSYS(set_thread_area)
	{ SYS(set_thread_area), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(set_tid_address)
	{ SYS(set_tid_address), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setdomainname)
	//{ SYS(setdomainname), 2, { ARG_PTR, ARG_LEN, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setfsgid)
	{ SYS(setfsgid), 1, { ARG_GID, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setfsgid32)
	{ SYS(setfsgid32), 1, { ARG_GID, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setfsuid)
	{ SYS(setfsuid), 1, { ARG_GID, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setfsuid32)
	{ SYS(setfsuid32), 1, { ARG_GID, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setgid)
	{ SYS(setgid), 1, { ARG_GID, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setgid32)
	{ SYS(setgid32), 1, { ARG_GID, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setgroups)
	{ SYS(setgroups), 2, { ARG_LEN, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setgroups32)
	{ SYS(setgroups32), 2, { ARG_LEN, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(sethae)
	/* ALPHA only */
#endif
#if DEFSYS(sethostname)
	{ SYS(sethostname), 2, { ARG_PTR, ARG_LEN, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setitimer)
	{ SYS(setitimer), 3, { ARG_INT, ARG_NON_NULL_PTR, ARG_NON_NULL_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(setmntent)
	{ SYS(setmntent), 2, { ARG_EMPTY_FILENAME, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setns)
	{ SYS(setns), 2, { ARG_FD, ARG_INT, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setpgid)
	{ SYS(setpgid), 2, { ARG_PID, ARG_PID, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setpgrp)
	/* ALPHA, alternative to setpgid */
	{ SYS(setpgrp), 2, { ARG_PID, ARG_PID, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setpriority)
	{ SYS(setpriority), 3, { ARG_INT, ARG_INT, ARG_INT, 0, 0, 0 } },
#endif
#if DEFSYS(setregid)
	{ SYS(setregid), 2, { ARG_GID, ARG_GID, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setregid32)
	{ SYS(setregid32), 2, { ARG_GID, ARG_GID, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setresgid)
	{ SYS(setresgid), 3, { ARG_GID, ARG_GID, ARG_GID, 0, 0, 0 } },
#endif
#if DEFSYS(setresgid32)
	{ SYS(setresgid32), 3, { ARG_GID, ARG_GID, ARG_GID, 0, 0, 0 } },
#endif
#if DEFSYS(setresuid)
	{ SYS(setresuid), 3, { ARG_UID, ARG_UID, ARG_UID, 0, 0, 0 } },
#endif
#if DEFSYS(setresuid32)
	{ SYS(setresuid32), 3, { ARG_UID, ARG_UID, ARG_UID, 0, 0, 0 } },
#endif
#if DEFSYS(setreuid)
	{ SYS(setreuid), 2, { ARG_UID, ARG_UID, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setreuid32)
	{ SYS(setreuid32), 2, { ARG_UID, ARG_UID, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setrlimit)
	{ SYS(setrlimit), 2, { ARG_INT, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setsid)
	//{ SYS(setsid), 0, { 0, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setsockopt)
	{ SYS(setsockopt), 5, { ARG_SOCKFD, ARG_INT, ARG_INT, ARG_PTR, ARG_LEN, 0 } },
#endif
#if DEFSYS(settimeofday)
	{ SYS(settimeofday), 2, { ARG_PTR, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setuid)
	{ SYS(setuid), 1, { ARG_UID, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setuid32)
	{ SYS(setuid32), 1, { ARG_UID, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(setxattr)
	{ SYS(setxattr), 5, { ARG_EMPTY_FILENAME, ARG_PTR, ARG_PTR, ARG_LEN, ARG_FLAG, 0 } },
#endif
#if DEFSYS(sgetmask)
	{ SYS(sgetmask), 1, { ARG_UINT, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(shmat)
	{ SYS(shmat), 3, { ARG_INT, ARG_PTR, ARG_FLAG, 0, 0, 0 } },
#endif
#if DEFSYS(shmctl)
	{ SYS(shmctl), 3, { ARG_INT, ARG_INT, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(shmdt)
	{ SYS(shmdt), 3, { ARG_INT, ARG_PTR, ARG_FLAG, 0, 0, 0 } },
#endif
#if DEFSYS(shmget)
	{ SYS(shmget), 3, { ARG_INT, ARG_LEN, ARG_FLAG, 0, 0, 0 } },
#endif
#if DEFSYS(shutdown)
	{ SYS(shutdown), 2, { ARG_SOCKFD, ARG_INT, 0, 0, 0, 0 } },
#endif
#if DEFSYS(sigaction)
	{ SYS(sigaction), 3, { ARG_INT, ARG_PTR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(sigaltstack)
	{ SYS(sigaltstack), 3, { ARG_NON_NULL_PTR, ARG_NON_NULL_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(signal)
	{ SYS(signal), 2, { ARG_INT, ARG_NON_NULL_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(signalfd)
	{ SYS(signalfd), 3, { ARG_FD, ARG_PTR, ARG_LEN, 0, 0, 0 } },
#endif
#if DEFSYS(signalfd4)
	{ SYS(signalfd4), 3, { ARG_FD, ARG_PTR, ARG_LEN, ARG_FLAG, 0, 0 } },
#endif
#if DEFSYS(sigpending)
	{ SYS(sigpending), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(sigreturn)
	/* { SYS(sigreturn), 4, { ARG_PTR, ARG_PTR, ARG_PTR, ARG_PTR, 0, 0 } }, */
#endif
#if DEFSYS(sigsuspend)
	{ SYS(sigsuspend), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(sigtimedwait)
	{ SYS(sigtimedwait), 3, { ARG_PTR, ARG_PTR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(sigwaitinfo)
	{ SYS(sigwaitinfo), 2, { ARG_PTR, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(socket)
	{ SYS(socket), 3, { ARG_INT, ARG_INT, ARG_INT, 0, 0, 0 } },
#endif
#if DEFSYS(socketcall)
	{ SYS(socketcall), 2, { ARG_INT, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(socketpair)
	{ SYS(socketpair), 4, { ARG_INT, ARG_INT, ARG_INT, ARG_PTR, 0, 0 } },
#endif
#if DEFSYS(spill)
	/* Xtensa only */
#endif
#if DEFSYS(splice)
	{ SYS(splice), 6, { ARG_FD, ARG_PTR, ARG_FD, ARG_PTR, ARG_LEN, ARG_FLAG } },
#endif
#if DEFSYS(spu_create)
	/* PowerPC/PowerPC64 only */
	{ SYS(spu_create), 3, { ARG_EMPTY_FILENAME, ARG_FLAG, ARG_MODE, 0, 0, 0 } },
	{ SYS(spu_create), 4, { ARG_EMPTY_FILENAME, ARG_FLAG, ARG_MODE, ARG_INT, 0, 0 } },
#endif
#if DEFSYS(spu_run)
	/* PowerPC/PowerPC64 only */
	{ SYS(spu_run), 3, { ARG_FD, ARG_PTR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(sram_alloc)
	/* Blackfin, remove 4.17 */
#endif
#if DEFSYS(sram_free)
	/* Blackfin, remove 4.17 */
#endif
#if DEFSYS(ssetmask)
	{ SYS(ssetmask), 1, { ARG_UINT, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(stat)
	{ SYS(stat), 2, { ARG_EMPTY_FILENAME, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(stat64)
	{ SYS(stat64), 2, { ARG_EMPTY_FILENAME, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(statfs)
	{ SYS(statfs), 2, { ARG_EMPTY_FILENAME, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(statfs64)
	{ SYS(statfs64), 2, { ARG_EMPTY_FILENAME, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(statx)
	{ SYS(statx), 5, { ARG_DIRFD, ARG_EMPTY_FILENAME, ARG_FLAG, ARG_UINT, ARG_PTR, 0 } },
#endif
#if DEFSYS(stime)
	{ SYS(stime), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(subpage_prot)
	/* PowerPC/PowerPC64 only */
	{ SYS(subpage_prot), 3, { ARG_UINT, ARG_LEN, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(swapcontext)
	/* PowerPC/PowerPC64 only, TODO */
#endif
#if DEFSYS(switch_endian)
	/* PowerPC/PowerPC64 only, TODO */
#endif
#if DEFSYS(swapon)
	{ SYS(swapon), 2, { ARG_EMPTY_FILENAME, ARG_INT, 0, 0, 0, 0 } },
#endif
#if DEFSYS(swapoff)
	{ SYS(swapoff), 1, { ARG_EMPTY_FILENAME, 0 , 0, 0, 0, 0 } },
#endif
#if DEFSYS(symlink)
	{ SYS(symlink), 2, { ARG_EMPTY_FILENAME, ARG_EMPTY_FILENAME, 0, 0, 0, 0 } },
#endif
#if DEFSYS(symlinkat)
	{ SYS(symlinkat), 3, { ARG_EMPTY_FILENAME, ARG_FD, ARG_EMPTY_FILENAME, 0, 0, 0 } },
#endif
#if DEFSYS(sync)
	//{ SYS(sync), 0, { 0, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(sync_file_range)
	{ SYS(sync_file_range), 4, { ARG_FD, ARG_UINT, ARG_UINT, ARG_FLAG, 0, 0 } },
#endif
#if DEFSYS(sync_file_range2)
	{ SYS(sync_file_range2), 4, { ARG_FD, ARG_FLAG, ARG_UINT, ARG_UINT, 0, 0 } },
#endif
#if DEFSYS(syncfs)
	{ SYS(syncfs), 1, { ARG_FD, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(sysfs)
	{ SYS(sysfs), 2, { ARG_INT, ARG_PTR, 0, 0, 0, 0 } },
	{ SYS(sysfs), 3, { ARG_INT, ARG_UINT, ARG_PTR, 0, 0, 0 } },
	{ SYS(sysfs), 1, { ARG_INT, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(sysinfo)
	{ SYS(sysinfo), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(syslog)
	{ SYS(syslog), 3, { ARG_INT, ARG_PTR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(sysmips)
	/* ARM OABI and MIPS O32 ABI, TODO */
#endif
#if DEFSYS(tee)
	{ SYS(tee), 4, { ARG_FD, ARG_FD, ARG_LEN, ARG_FLAG, 0, 0 } },
#endif
#if DEFSYS(tgkill)
	/* { SYS(tgkill), 3, { ARG_PID, ARG_PID, ARG_INT, 0, 0, 0 } },*/
#endif
#if DEFSYS(time)
	{ SYS(time), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(timer_create)
	{ SYS(timer_create), 3, { ARG_CLOCKID_T, ARG_PTR, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(timer_delete)
	{ SYS(timer_delete), 1, { ARG_UINT, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(timer_getoverrun)
	{ SYS(timer_getoverrun), 1, { ARG_UINT, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(timer_gettime)
	{ SYS(timer_gettime), 2, { ARG_UINT, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(timer_settime)
	{ SYS(timer_settime), 4, { ARG_UINT, ARG_FLAG, ARG_PTR, ARG_PTR, 0, 0 } },
#endif
#if DEFSYS(timerfd_create)
	{ SYS(timerfd_create), 2, { ARG_CLOCKID_T, ARG_FLAG, 0, 0, 0, 0 } },
#endif
#if DEFSYS(timerfd_gettime)
	{ SYS(timerfd_gettime), 2, { ARG_CLOCKID_T, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(timerfd_settime)
	{ SYS(timer_settime), 4, { ARG_FD, ARG_FLAG, ARG_PTR, ARG_PTR, 0, 0 } },
#endif
#if DEFSYS(times)
	{ SYS(times), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(tkill)
	/* { SYS(tkill), 2, { ARG_PID, ARG_INT, 0, 0, 0, 0 }, */
#endif
#if DEFSYS(truncate)
	{ SYS(truncate), 2, { ARG_EMPTY_FILENAME, ARG_LEN, 0, 0, 0, 0 } },
#endif
#if DEFSYS(truncate64)
	{ SYS(truncate64), 2, { ARG_EMPTY_FILENAME, ARG_LEN, 0, 0, 0, 0 } },
#endif
#if DEFSYS(ugetrlimit)
	/* TODO */
#endif
#if DEFSYS(umask)
	{ SYS(umask), 1, { ARG_UINT, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(umount)
	{ SYS(umount), 1, { ARG_EMPTY_FILENAME, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(umount2)
	/* TODO */
#endif
#if DEFSYS(uname)
	{ SYS(uname), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(unlink)
	{ SYS(unlink), 1, { ARG_EMPTY_FILENAME, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(unlinkat)
	{ SYS(unlinkat), 3, { ARG_DIRFD, ARG_EMPTY_FILENAME, ARG_FLAG, 0, 0, 0 } },
#endif
#if DEFSYS(unshare)
	{ SYS(unshare), 1, { ARG_INT, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(uselib)
	{ SYS(uselib), 1, { ARG_EMPTY_FILENAME, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(userfaultfd)
	{ SYS(userfaultfd), 1, { ARG_FLAG, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(usr26)
	/* ARM OABI only */
#endif
#if DEFSYS(ustat)
	{ SYS(ustat), 2, { ARG_UINT, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(utime)
	{ SYS(utime), 2, { ARG_EMPTY_FILENAME, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(utimensat)
	{ SYS(utimensat), 4, { ARG_DIRFD, ARG_EMPTY_FILENAME, ARG_PTR, ARG_FLAG, 0, 0 } },
#endif
#if DEFSYS(utimes)
	{ SYS(utimes), 2, { ARG_EMPTY_FILENAME, ARG_PTR, 0, 0, 0, 0 } },
#endif
#if DEFSYS(utrap_install)
	/* SPARC64 */
#endif
#if DEFSYS(vm86old)
#endif
#if DEFSYS(vm86)
#endif
#if DEFSYS(vmsplice)
	{ SYS(vmsplice), 4, { ARG_FD, ARG_PTR, ARG_UINT, ARG_FLAG, 0, 0 } },
#endif
#if DEFSYS(wait)
	{ SYS(wait), 1, { ARG_PTR, 0, 0, 0, 0, 0 } },
#endif
#if DEFSYS(wait3)
	{ SYS(wait3), 3, { ARG_PTR, ARG_INT, ARG_PTR, 0, 0, 0 } },
#endif
#if DEFSYS(wait4)
	{ SYS(wait4), 4, { ARG_PID, ARG_PTR, ARG_INT, ARG_PTR, 0, 0 } },
#endif
#if DEFSYS(waitid)
	{ SYS(waitid), 4, { ARG_INT, ARG_INT, ARG_PTR, ARG_INT, 0, 0 } },
#endif
#if DEFSYS(waitpid)
	{ SYS(waitpid), 3, { ARG_PID, ARG_PTR, ARG_INT, 0, 0, 0 } },
#endif
#if DEFSYS(write)
	{ SYS(write), 3, { ARG_FD, ARG_PTR, ARG_LEN, 0, 0, 0 } },
#endif
#if DEFSYS(writev)
	{ SYS(writev), 3, { ARG_FD, ARG_PTR, ARG_LEN, 0, 0, 0 } },
#endif
#if DEFSYS(xtensa)
	/* xtensa only */
#endif
};

/*
 *  running context shared between parent and child
 *  this allows us to have enough data about a system call that
 *  caused the child to crash. Also contains running stats
 *  of the number of system calls made. Must be 1 page or smaller
 */
typedef struct {
	uint64_t hash;
	uint64_t syscall;
	uint64_t type;
	const char *name;
	size_t idx;
	int64_t counter;
	uint64_t skip_crashed;
	uint64_t skip_errno_zero;
	uint64_t crash_count[SIZEOF_ARRAY(syscall_args)];
	unsigned long args[6];
	unsigned char filler[4096];
} syscall_current_context_t;

static syscall_current_context_t *current_context;

static void func_exit(void)
{
	_exit(EXIT_SUCCESS);
}

/*
 *  Various invalid argument values
 */
static unsigned long none_values[] = { 0 };
static unsigned long mode_values[] = { -1, INT_MAX, INT_MIN, ~(long)0, 1ULL << 20 };
static long sockfds[] = { /* sockfd */ 0, 0, -1, INT_MAX, INT_MIN, ~(long)0 };
static long fds[] = { -1, INT_MAX, INT_MIN, ~(long)0 };
static long dirfds[] = { -1, AT_FDCWD, INT_MIN, ~(long)0 };
static long clockids[] = { -1, INT_MAX, INT_MIN, ~(long)0, SHR_UL(0xfe23ULL, 18) };
static long sockaddrs[] = { /*small_ptr*/ 0, /*page_ptr*/ 0, 0, -1, INT_MAX, INT_MIN };
static unsigned long brk_addrs[] = { 0, -1, INT_MAX, INT_MIN, ~(unsigned long)0, 4096 };
static unsigned long empty_filenames[] = { (unsigned long)"", (unsigned long)NULL };
static unsigned long zero_filenames[] = { (unsigned long)"/dev/zero" };
static unsigned long null_filenames[] = { (unsigned long)"/dev/null" };
static long flags[] = { -1, -2, INT_MIN, SHR_UL(0xffffULL, 20) };
static unsigned long lengths[] = { -1, -2, INT_MIN, INT_MAX, ~(unsigned long)0, -SHR_UL(1, 31) };
static long ints[] = { 0, -1, -2, INT_MIN, INT_MAX, SHR_UL(0xff, 30), SHR_UL(1, 30), -SHR_UL(0xff, 30), -SHR_UL(1, 30) };
static unsigned long uints[] = { INT_MAX, SHR_UL(0xff, 30), -SHR_UL(0xff, 30), ~(unsigned long)0 };
static unsigned long func_ptrs[] = { (unsigned long)func_exit };
static unsigned long ptrs[] = { /*small_ptr*/ 0, /*page_ptr*/ 0, 0, -1, INT_MAX, INT_MIN, ~(long)4096 };
static unsigned long futex_ptrs[] = { /*small_ptr*/ 0, /*page_ptr*/ 0 };
static unsigned long non_null_ptrs[] = { /*small_ptr*/ 0, /*page_ptr*/ 0, -1, INT_MAX, INT_MIN, ~(long)4096 };
static long socklens[] = { 0, -1, INT_MAX, INT_MIN, 8192 };
static unsigned long timeouts[] = { 0 };
static pid_t pids[] = { INT_MIN, -1, INT_MAX, ~0 };
static gid_t gids[] = { ~(long)0, INT_MAX };
static uid_t uids[] = { ~(long)0, INT_MAX };

/*
 *  mapping of invalid arg types to invalid arg values
 */
static const syscall_arg_values_t arg_values[] = {
	ARG_VALUE(ARG_MODE, mode_values),
	ARG_VALUE(ARG_SOCKFD, sockfds),
	ARG_VALUE(ARG_FD, fds),
	ARG_VALUE(ARG_DIRFD, dirfds),
	ARG_VALUE(ARG_CLOCKID_T, clockids),
	ARG_VALUE(ARG_PID, pids),
	ARG_VALUE(ARG_PTR | ARG_STRUCT_SOCKADDR, sockaddrs),
	ARG_VALUE(ARG_BRK_ADDR, brk_addrs),
	ARG_VALUE(ARG_EMPTY_FILENAME, empty_filenames),
	ARG_VALUE(ARG_DEVZERO_FILENAME, zero_filenames),
	ARG_VALUE(ARG_DEVNULL_FILENAME, null_filenames),
	ARG_VALUE(ARG_FLAG, flags),
	ARG_VALUE(ARG_SOCKLEN_T, socklens),
	ARG_VALUE(ARG_TIMEOUT, timeouts),
	ARG_VALUE(ARG_LEN, lengths),
	ARG_VALUE(ARG_GID, gids),
	ARG_VALUE(ARG_UID, uids),
	ARG_VALUE(ARG_INT, ints),
	ARG_VALUE(ARG_UINT, uints),
	ARG_VALUE(ARG_FUNC_PTR, func_ptrs),
	ARG_VALUE(ARG_NON_NULL_PTR, non_null_ptrs),
	ARG_VALUE(ARG_FUTEX_PTR, futex_ptrs),
	ARG_VALUE(ARG_PTR, ptrs),
};

static void MLOCKED_TEXT stress_inval_handler(int signum)
{
	(void)signum;

	_exit(1);
}

/*
 *   stress_syscall_hash()
 *	generate a simple hash on system call and call arguments
 */
static unsigned long stress_syscall_hash(
	const unsigned long syscall,
	const unsigned long args[6])
{
	unsigned long hash = syscall;

	ROR(hash);
	ROR(hash);
	hash ^= (args[0]);
	ROR(hash);
	ROR(hash);
	hash ^= (args[1]);
	ROR(hash);
	ROR(hash);
	hash ^= (args[2]);
	ROR(hash);
	ROR(hash);
	hash ^= (args[3]);
	ROR(hash);
	ROR(hash);
	hash ^= (args[4]);
	ROR(hash);
	ROR(hash);
	hash ^= (args[5]);

	return hash % SYSCALL_HASH_TABLE_SIZE;
}

/*
 *  hash_table_add()
 *	add system call info to the hash table
 * 	- will silently fail if out of memory
 */
static void hash_table_add(
	const unsigned long hash,
	const unsigned long syscall_num,
	const unsigned long *args,
	const uint8_t type)
{
	syscall_arg_hash_t *h;

	h = calloc(1, sizeof(*h));
	if (!h)
		return;
	h->hash = hash;
	h->syscall = syscall_num;
	h->type = type;
	(void)memcpy(h->args, args, sizeof(h->args));
	h->next = hash_table[hash];
	hash_table[hash] = h;
}

/*
 *  hash_table_free()
 *	free the hash table
 */
static void hash_table_free(void)
{
	size_t i;

	for (i = 0; i < SIZEOF_ARRAY(hash_table); i++) {
		syscall_arg_hash_t *h = hash_table[i];

		while (h) {
			syscall_arg_hash_t *next = h->next;

			free(h);
			h = next;
		}
		hash_table[i] = NULL;
	}
}

/*
 *  syscall_permute()
 *	recursively permute all possible system call invalid arguments
 *	- if the system call crashes, the call info is cached in
 *	  the current_context for the parent to record the failure
 *	  so it's not called again.
 *	- if the system call returns 0, the call info is saved
 *	  in the hash table so it won't get called again. This is
 * 	  just in the child context and is lost when the child
 *	  crashes
 */
static void syscall_permute(
	const stress_args_t *args,
	const int arg_num,
	const syscall_arg_t *syscall_arg)
{
	unsigned long arg = syscall_arg->args[arg_num];
	size_t i;
	unsigned long *values = NULL;
	unsigned long rnd_values[4];
	size_t num_values = 0;

	if (arg_num >= syscall_arg->num_args) {
		int ret;
		const unsigned long syscall_num = syscall_arg->syscall;
		const unsigned long hash = stress_syscall_hash(syscall_num, current_context->args);
		syscall_arg_hash_t *h = hash_table[hash];

		while (h) {
			if (!memcmp(h->args, current_context->args, sizeof(h->args))) {
				switch (h->type) {
				case SYSCALL_CRASH:
					current_context->skip_crashed++;
					break;
				case SYSCALL_ERRNO_ZERO:
					current_context->skip_errno_zero++;
					break;
				default:
					break;
				}
				return;
			}
			h = h->next;
		}

		errno = 0;
		current_context->counter++;
		current_context->hash = hash;
		current_context->type = SYSCALL_CRASH;	/* Assume it will crash */
		ret = syscall(syscall_num,
			current_context->args[0],
			current_context->args[1],
			current_context->args[2],
			current_context->args[3],
			current_context->args[4],
			current_context->args[5]);
		/*
		 *  For this child we remember syscalls that don't fail
		 *  so we don't retry them
		 */
		if (ret == 0)
			hash_table_add(hash, syscall_num, current_context->args, SYSCALL_ERRNO_ZERO);
		current_context->type = SYSCALL_FAIL;	/* it just failed */
		return;
	}

	switch (arg) {
	case ARG_NONE:
		values = none_values;
		num_values = 1;
		break;
	case ARG_RND:
		/*
		 *  Provide some 'random' values
		 */
		rnd_values[0] = stress_mwc64();
		rnd_values[1] = SHR_UL(stress_mwc32(), 20);
		rnd_values[2] = (unsigned long)small_ptr;
		rnd_values[3] = (unsigned long)page_ptr;
		values = rnd_values;
		num_values = 4;
		break;
	default:
		/*
		 *  Find the arg type to determine the arguments to use
		 */
		for (i = 0; i < SIZEOF_ARRAY(arg_values); i++) {
			if (ARG_MASK(arg, arg_values[i].mask)) {
				values = arg_values[i].values;
				num_values = arg_values[i].num_values;
				break;
			}
		}
		break;
	}
	/*
	 *  This should not fail!
	 */
	if (!num_values) {
		pr_dbg("%s: argument %d has bad mask %lx\n", args->name, arg_num, arg);
		current_context->args[arg_num] = 0;
		return;
	}

	/*
	 *  And permute and call all the argument values for this
	 *  specific argument
	 */
	for (i = 0; i < num_values; i++) {
		current_context->args[arg_num] = values[i];
		syscall_permute(args, arg_num + 1, syscall_arg);
		current_context->args[arg_num] = 0;
	}
}

/*
 *  Call a system call in a child context so we don't clobber
 *  the parent
 */
static inline int stress_do_syscall(const stress_args_t *args)
{
	pid_t pid;
	int rc = 0;

	(void)stress_mwc32();

	if (!keep_stressing_flag())
		return 0;

	if (stress_drop_capabilities(args->name) < 0)
		return EXIT_NO_RESOURCE;

	pid = fork();
	if (pid < 0) {
		_exit(EXIT_NO_RESOURCE);
	} else if (pid == 0) {
		size_t i, n;
		size_t reorder[SIZEOF_ARRAY(syscall_args)];

		/* We don't want bad ops clobbering this region */
		stress_unmap_shared();
		stress_process_dumpable(false);

		/* Drop all capabilities */
		if (stress_drop_capabilities(args->name) < 0) {
			_exit(EXIT_NO_RESOURCE);
		}
		for (i = 0; i < SIZEOF_ARRAY(sigs); i++) {
			if (stress_sighandler(args->name, sigs[i], stress_inval_handler, NULL) < 0)
				_exit(EXIT_FAILURE);
		}

		(void)setpgid(0, g_pgrp);
		stress_parent_died_alarm();
		stress_mwc_reseed();

		for (i = 0; i < SIZEOF_ARRAY(reorder); i++) {
			reorder[i] = i;
		}

		while (keep_stressing_flag()) {
			const size_t sz = SIZEOF_ARRAY(reorder);
			/*
			 *  Shuffle syscall order
			 */
			for (n = 0; n < 5; n++) {
				for (i = 0; i < SIZEOF_ARRAY(reorder); i++) {
					register size_t tmp;
					register size_t j = (sz == 0) ? 0 : stress_mwc32() % sz;

					tmp = reorder[i];
					reorder[i] = reorder[j];
					reorder[j] = tmp;
				}
			}

			for (i = 0; keep_stressing() && (i < SIZEOF_ARRAY(syscall_args)); i++) {
				size_t idx;
				const size_t j = reorder[i];
				struct itimerval it;


				(void)memset(current_context->args, 0, sizeof(current_context->args));
				current_context->syscall = syscall_args[j].syscall;
				idx = &syscall_args[j] - syscall_args;
				current_context->idx = idx;
				current_context->name = syscall_args[j].name;

				/* Ignore too many crashes from this system call */
				if (current_context->crash_count[idx] >= MAX_CRASHES)
					continue;
				/*
				 * Force abort if we take too long
				 */
				it.it_interval.tv_sec = 0;
				it.it_interval.tv_usec = 100000;
				it.it_value.tv_sec = 0;
				it.it_value.tv_usec = 100000;
				if (setitimer(ITIMER_REAL, &it, NULL) < 0) {
					pr_fail("%s: setitimer failed, errno=%d (%s)\n",
						args->name, errno, strerror(errno));
					continue;
				}
				syscall_permute(args, 0, &syscall_args[j]);
			}
			hash_table_free();
		}
		_exit(EXIT_SUCCESS);
	} else {
		int ret, status;

		/*
		 *  Don't use retry shim_waitpid here, we want to force
		 *  kill the child no matter what happens at this point
		 */
		ret = waitpid(pid, &status, 0);
		if (ret < 0) {
			/*
			 *  SIGALRM or a waitpid failure, so force
			 *  kill and reap of child to make sure
			 *  it is really dead and buried
			 */
			(void)kill(pid, SIGKILL);
			ret = waitpid(pid, &status, 0);
			(void)ret;
		}
		if (current_context->type == SYSCALL_CRASH) {
			const size_t idx = current_context->idx;

#if 0
			printf("CRASHED: %s(%" PRIx64 ",%" PRIx64 ",%" PRIx64
					   ",%" PRIx64 ",%" PRIx64 ",%" PRIx64 ") %zd\n",
				current_context->name,
				current_context->args[0],
				current_context->args[1],
				current_context->args[2],
				current_context->args[3],
				current_context->args[4],
				current_context->args[5],
				idx);
#endif
			hash_table_add(current_context->hash,
				current_context->syscall,
				current_context->args,
				SYSCALL_CRASH);

			if (idx < SIZEOF_ARRAY(syscall_args))
				current_context->crash_count[idx]++;
		}
		rc = WEXITSTATUS(status);
	}
	return rc;
}

static int stress_sysinval_child(const stress_args_t *args, void *context)
{
	(void)context;

	do {
		(void)stress_mwc32();
		stress_do_syscall(args);
	} while (keep_stressing());

	return EXIT_SUCCESS;
}

/*
 *  stress_sysinval
 *	stress system calls with bad addresses
 */
static int stress_sysinval(const stress_args_t *args)
{
	int ret;
	const size_t page_size = args->page_size;
	const size_t current_context_size =
		(sizeof(*current_context) + page_size) & ~(page_size - 1);
	size_t small_ptr_size = page_size << 1;

	sockfds[0] = socket(AF_UNIX, SOCK_STREAM, 0);

	current_context = (syscall_current_context_t*)
		mmap(NULL, current_context_size, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (current_context == MAP_FAILED) {
		pr_fail("%s: mmap failed, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		return EXIT_NO_RESOURCE;
	}

	small_ptr = (uint8_t *)mmap(NULL, small_ptr_size, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (small_ptr == MAP_FAILED) {
		pr_fail("%s: mmap failed, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		(void)munmap((void *)current_context, page_size);
		return EXIT_NO_RESOURCE;
	}
#if defined(HAVE_MPROTECT)
	(void)mprotect(small_ptr + page_size, page_size, PROT_NONE);
#else
	(void)munmap((void *)(small_ptr + page_size), page_size);
	small_ptr_size -= page_size;
#endif

	page_ptr = (uint8_t *)mmap(NULL, page_size, PROT_NONE,
		MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (page_ptr == MAP_FAILED) {
		pr_fail("%s: mmap failed, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		(void)munmap((void *)small_ptr, small_ptr_size);
		(void)munmap((void *)current_context, page_size);
		return EXIT_NO_RESOURCE;
	}

	sockaddrs[0] = (long)(small_ptr + page_size - 1);
	sockaddrs[1] = (long)page_ptr;
	ptrs[0] = (long)(small_ptr + page_size -1);
	ptrs[1] = (long)page_ptr;
	non_null_ptrs[0] = (long)(small_ptr + page_size -1);
	non_null_ptrs[1] = (long)page_ptr;
	futex_ptrs[0] = (long)(small_ptr + page_size -1);
	futex_ptrs[1] = (long)page_ptr;

	if (args->instance == 0)
		pr_dbg("%s: exercising %zd system calls\n", args->name, SIZEOF_ARRAY(syscall_args));

	ret = stress_oomable_child(args, NULL, stress_sysinval_child, STRESS_OOMABLE_DROP_CAP);

	pr_dbg("%s: %" PRIu64 " unique syscalls patterns causing child termination\n",
		args->name, current_context->skip_crashed);
	pr_dbg("%s: %" PRIu64 " unique syscall patterns not failing\n",
		args->name, current_context->skip_errno_zero);

	set_counter(args, current_context->counter);

	(void)munmap((void *)page_ptr, page_size);
	(void)munmap((void *)small_ptr, small_ptr_size);
	(void)munmap((void *)current_context, current_context_size);
	if (sockfds[0] >= 0)
		(void)close(sockfds[0]);

	hash_table_free();

	return ret;
}

stressor_info_t stress_sysinval_info = {
	.stressor = stress_sysinval,
	.class = CLASS_OS,
	.help = help
};
#else
stressor_info_t stress_sysinval_info = {
	.stressor = stress_not_implemented,
	.class = CLASS_OS,
	.help = help
};
#endif
