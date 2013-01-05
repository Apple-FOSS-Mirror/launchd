/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */
#ifndef __LAUNCHD_RUNTIME_H__
#define __LAUNCHD_RUNTIME_H__

#include <mach/mach.h>
#include <sys/types.h>
#include <bsm/libbsm.h>
#include <stdbool.h>
#include <stdint.h>
#include <float.h>
#include <syslog.h>

#include "launchd_runtime_kill.h"
#include "launchd_ktrace.h"

#if 0

/* I need to do more testing of these macros */

#define min_of_type(x) \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), long double), LDBL_MIN, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), double), DBL_MIN, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), float), FLT_MIN, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), char), 0, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), signed char), INT8_MIN, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), short), INT16_MIN, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), int), INT32_MIN, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), long), (__builtin_choose_expr(sizeof(x) == 4, INT32_MIN, INT64_MIN)), \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), long long), INT64_MIN, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), unsigned char), 0, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), unsigned short), 0, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), unsigned int), 0, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), unsigned long), 0, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), unsigned long long), 0, \
	(void)0))))))))))))))

#define max_of_type(x) \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), long double), LDBL_MAX, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), double), DBL_MAX, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), float), FLT_MAX, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), char), UINT8_MAX, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), signed char), INT8_MAX, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), short), INT16_MIN, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), int), INT32_MAX, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), long), (__builtin_choose_expr(sizeof(x) == 4, INT32_MAX, INT64_MAX)), \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), long long), INT64_MAX, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), unsigned char), UINT8_MAX, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), unsigned short), UINT16_MAX, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), unsigned int), UINT32_MAX, \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), unsigned long), (__builtin_choose_expr(sizeof(x) == 4, UINT32_MAX, UINT64_MAX)), \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(x), unsigned long long), UINT64_MAX, \
	(void)0))))))))))))))

#endif

#define	likely(x)	__builtin_expect((bool)(x), true)
#define	unlikely(x)	__builtin_expect((bool)(x), false)

struct ldcred {
	uid_t   euid;
	uid_t   uid;
	gid_t   egid;
	gid_t   gid;
	pid_t   pid;
};

/*
 * Use launchd_assumes() when we can recover, even if it means we leak or limp along.
 *
 * Use launchd_assert() for core initialization routines.
 */
#define launchd_assumes(e)	\
	(unlikely(!(e)) ? _log_launchd_bug(__rcs_file_version__, __FILE__, __LINE__, #e), false : true)

#define launchd_assert(e)	if (__builtin_constant_p(e)) { char __compile_time_assert__[e ? 1 : -1] __attribute__((unused)); } else if (!launchd_assumes(e)) { abort(); }

void _log_launchd_bug(const char *rcs_rev, const char *path, unsigned int line, const char *test);

typedef void (*kq_callback)(void *, struct kevent *);
typedef boolean_t (*mig_callback)(mach_msg_header_t *, mach_msg_header_t *);
typedef void (*timeout_callback)(void);

extern bool pid1_magic;
extern bool low_level_debug;
extern char g_username[128];
extern char g_my_label[128];
extern bool g_shutdown_debugging;
extern bool g_verbose_boot;
extern bool g_use_gmalloc;
extern bool g_log_per_user_shutdown;
extern bool g_log_strict_usage;
extern bool g_embedded_shutdown_log;
extern int32_t g_sync_frequency;
extern pid_t g_wsp;

mach_port_t runtime_get_kernel_port(void);
extern boolean_t launchd_internal_demux(mach_msg_header_t *Request, mach_msg_header_t *Reply);

void runtime_add_ref(void);
void runtime_del_ref(void);
void runtime_add_weak_ref(void);
void runtime_del_weak_ref(void);

void launchd_runtime_init(void);
void launchd_runtime_init2(void);
void launchd_runtime(void) __attribute__((noreturn));

void launchd_log_vm_stats(void);

int runtime_close(int fd);
int runtime_fsync(int fd);

#define RUNTIME_ADVISABLE_IDLE_TIMEOUT 30

void runtime_set_timeout(timeout_callback to_cb, unsigned int sec);
kern_return_t runtime_add_mport(mach_port_t name, mig_callback demux, mach_msg_size_t msg_size);
kern_return_t runtime_remove_mport(mach_port_t name);
struct ldcred *runtime_get_caller_creds(void);

const char *signal_to_C_name(unsigned int sig);
const char *reboot_flags_to_C_names(unsigned int flags);
const char *proc_flags_to_C_names(unsigned int flags);

int kevent_bulk_mod(struct kevent *kev, size_t kev_cnt);
int kevent_mod(uintptr_t ident, short filter, u_short flags, u_int fflags, intptr_t data, void *udata);
void log_kevent_struct(int level, struct kevent *kev_base, int indx);

pid_t runtime_fork(mach_port_t bsport);

mach_msg_return_t launchd_exc_runtime_once(mach_port_t port, mach_msg_size_t rcv_msg_size, mach_msg_size_t send_msg_size, mig_reply_error_t *bufRequest, mig_reply_error_t *bufReply, mach_msg_timeout_t to);

kern_return_t runtime_log_forward(uid_t forward_uid, gid_t forward_gid, vm_offset_t inval, mach_msg_type_number_t invalCnt);
kern_return_t runtime_log_drain(mach_port_t srp, vm_offset_t *outval, mach_msg_type_number_t *outvalCnt);

#define LOG_APPLEONLY		0x4141504c /* AAPL in hex */
#define LOG_SCOLDING		0x3030493b
#define LOG_CONSOLE			(1 << 31)

struct runtime_syslog_attr {
	const char *from_name;
	const char *about_name;
	const char *session_name;
	int priority;
	uid_t from_uid;
	pid_t from_pid;
	pid_t about_pid;
};

int runtime_setlogmask(int maskpri);
void runtime_closelog(void);
void runtime_syslog(int pri, const char *message, ...) __attribute__((format(printf, 2, 3)));
void runtime_vsyslog(struct runtime_syslog_attr *attr, const char *message, va_list args) __attribute__((format(printf, 2, 0)));
void runtime_log_push(void);

int64_t runtime_get_wall_time(void) __attribute__((warn_unused_result));
uint64_t runtime_get_opaque_time(void) __attribute__((warn_unused_result));
uint64_t runtime_get_opaque_time_of_event(void) __attribute__((pure, warn_unused_result));
uint64_t runtime_opaque_time_to_nano(uint64_t o) __attribute__((const, warn_unused_result));
uint64_t runtime_get_nanoseconds_since(uint64_t o) __attribute__((pure, warn_unused_result));

kern_return_t launchd_set_bport(mach_port_t name);
kern_return_t launchd_get_bport(mach_port_t *name);
kern_return_t launchd_mport_notify_req(mach_port_t name, mach_msg_id_t which);
kern_return_t launchd_mport_notify_cancel(mach_port_t name, mach_msg_id_t which);
kern_return_t launchd_mport_create_recv(mach_port_t *name);
kern_return_t launchd_mport_deallocate(mach_port_t name);
kern_return_t launchd_mport_make_send(mach_port_t name);
kern_return_t launchd_mport_copy_send(mach_port_t name);
kern_return_t launchd_mport_close_recv(mach_port_t name);

#endif
