/*
 * Copyright (c) 1999-2005 Apple Computer, Inc. All rights reserved.
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

#include "config.h"
#include "vproc.h"
#include "vproc_priv.h"
#include "vproc_internal.h"

#include <mach/mach.h>
#include <mach/vm_map.h>
#include <sys/param.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <syslog.h>
#include <pthread.h>
#include <signal.h>
#include <assert.h>
#include <libkern/OSAtomic.h>
#include <sys/syscall.h>

#if HAVE_QUARANTINE
#include <quarantine.h>
#endif

#include "launch.h"
#include "launch_priv.h"
#include "launch_internal.h"
#include "launchd_ktrace.h"

#include "protocol_vproc.h"

#include "reboot2.h"

#define likely(x)	__builtin_expect((bool)(x), true)
#define unlikely(x)	__builtin_expect((bool)(x), false)

static mach_port_t get_root_bootstrap_port(void);

const char *__crashreporter_info__; /* this should get merged with other versions of the symbol */

static int64_t cached_pid = -1;
static struct vproc_shmem_s *vproc_shmem;
static pthread_once_t shmem_inited = PTHREAD_ONCE_INIT;
static uint64_t s_cached_transactions_enabled = 0;

struct vproc_s {
	int32_t refcount;
	mach_port_t j_port;
};

vproc_t vprocmgr_lookup_vproc(const char *label)
{
	struct vproc_s *vp = NULL;
	
	mach_port_t mp = MACH_PORT_NULL;
	kern_return_t kr = vproc_mig_port_for_label(bootstrap_port, (char *)label, &mp);
	if( kr == BOOTSTRAP_SUCCESS ) {
		vp = (struct vproc_s *)calloc(1, sizeof(struct vproc_s));
		if( vp ) {
			vp->refcount = 1;
			mach_port_mod_refs(mach_task_self(), mp, MACH_PORT_RIGHT_SEND, 1);
			vp->j_port = mp;
		}
		mach_port_mod_refs(mach_task_self(), mp, MACH_PORT_RIGHT_SEND, -1);
	}
	
	return vp;
}

vproc_t vproc_retain(vproc_t vp)
{
	int32_t orig = OSAtomicAdd32(1, &vp->refcount) - 1;	
	if( orig <= 0 ) {
		/* We've gone from 0 to 1, which means that this object was due to be freed. */
		__crashreporter_info__ = "Under-retain / over-release of vproc_t.";
		abort();
	}
	
	return vp;
}

void vproc_release(vproc_t vp)
{
	int32_t newval = OSAtomicAdd32(-1, &vp->refcount);
	if( newval < 0 ) {
		/* We're in negative numbers, which is bad. */
		__crashreporter_info__ = "Over-release of vproc_t.";
		abort();
	} else if( newval == 0 ) {
		mach_port_deallocate(mach_task_self(), vp->j_port);
		free(vp);
	}
}

static void
vproc_shmem_init(void)
{
	vm_address_t vm_addr = 0;
	mach_port_t shmem_port;
	kern_return_t kr;

	kr = vproc_mig_setup_shmem(bootstrap_port, &shmem_port);

	//assert(kr == 0);
	if (kr) {
		/* rdar://problem/6416724
		 * If we fail to set up a shared memory page, just allocate a local chunk
		 * of memory. This way, processes can still introspect their own transaction
		 * counts if they're being run under a debugger. Moral of the story: Debug
		 * from the environment you intend to run in.
		 */
		void *_vm_addr = calloc(1, sizeof(struct vproc_shmem_s));
		if( !_vm_addr ) {
			return;
		}

		vm_addr = (vm_address_t)_vm_addr;
	} else {
		kr = vm_map(mach_task_self(), &vm_addr, getpagesize(), 0, true, shmem_port, 0, false,
					VM_PROT_READ|VM_PROT_WRITE, VM_PROT_READ|VM_PROT_WRITE, VM_INHERIT_NONE);
		
		//assert(kr == 0);
		if (kr) return;
		
		kr = mach_port_deallocate(mach_task_self(), shmem_port);
		
		//assert(kr == 0);
	}

	vproc_shmem = (struct vproc_shmem_s *)vm_addr;
}

static void
vproc_client_init(void)
{
	char *val = getenv(LAUNCHD_DO_APPLE_INTERNAL_LOGGING);
	if( val ) {
		if( strncmp(val, "true", sizeof("true") - 1) == 0 ) {
			do_apple_internal_logging = true;
		}
	}
	
	vproc_shmem_init();
}

vproc_transaction_t
vproc_transaction_begin(vproc_t vp __attribute__((unused)))
{
	vproc_transaction_t vpt = (vproc_transaction_t)vproc_shmem_init; /* we need a "random" variable that is testable */
#if !TARGET_OS_EMBEDDED
	_vproc_transaction_begin();
#endif

	return vpt;
}

void
_vproc_transaction_begin(void)
{
#if !TARGET_OS_EMBEDDED
	if (unlikely(vproc_shmem == NULL)) {
		int po_r = pthread_once(&shmem_inited, vproc_client_init);
		if (po_r != 0 || vproc_shmem == NULL) {
			return;
		}
	}

	typeof(vproc_shmem->vp_shmem_transaction_cnt) old = 0;
	do {
		old = vproc_shmem->vp_shmem_transaction_cnt;
		
		if (unlikely(old < 0)) {
			if (vproc_shmem->vp_shmem_flags & VPROC_SHMEM_EXITING) {
				_exit(0);
			} else {
				__crashreporter_info__ = "Unbalanced: vproc_transaction_begin()";
			}
			abort();
		}
	} while( !__sync_bool_compare_and_swap(&vproc_shmem->vp_shmem_transaction_cnt, old, old + 1) );
	
	runtime_ktrace(RTKT_VPROC_TRANSACTION_INCREMENT, old + 1, 0, 0);
#endif
}

size_t
_vproc_transaction_count(void)
{
	return likely(vproc_shmem) ? vproc_shmem->vp_shmem_transaction_cnt : INT32_MAX;
}

size_t
_vproc_standby_count(void)
{
#ifdef VPROC_STANDBY_IMPLEMENTED
	return likely(vproc_shmem) ? vproc_shmem->vp_shmem_standby_cnt : INT32_MAX;
#else
	return 0;
#endif
}

size_t
_vproc_standby_timeout(void)
{
	return likely(vproc_shmem) ? vproc_shmem->vp_shmem_standby_timeout : 0;
}

bool
_vproc_pid_is_managed(pid_t p)
{
	boolean_t result = false;
	vproc_mig_pid_is_managed(bootstrap_port, p, &result);
	
	return result;
}

kern_return_t
_vproc_transaction_count_for_pid(pid_t p, int32_t *count, bool *condemned)
{
	boolean_t _condemned = false;
	kern_return_t kr = vproc_mig_transaction_count_for_pid(bootstrap_port, p, count, &_condemned);
	if( kr == KERN_SUCCESS && condemned ) {
		*condemned = _condemned ? true : false;
	}
	
	return kr;
}

void
#if !TARGET_OS_EMBEDDED
_vproc_transaction_try_exit(int status)
{
	if (unlikely(vproc_shmem == NULL)) {
		return;
	}

	if (__sync_bool_compare_and_swap(&vproc_shmem->vp_shmem_transaction_cnt, 0, -1)) {
		vproc_shmem->vp_shmem_flags |= VPROC_SHMEM_EXITING;
		_exit(status);
	}
}
#else
_vproc_transaction_try_exit(int status __attribute__((unused)))
{
	
}
#endif

void
vproc_transaction_end(vproc_t vp __attribute__((unused)), vproc_transaction_t vpt)
{
	if (unlikely(vpt != (vproc_transaction_t)vproc_shmem_init)) {
		__crashreporter_info__ = "Bogus transaction handle passed to vproc_transaction_end() ";
		abort();
	}

#if !TARGET_OS_EMBEDDED
	_vproc_transaction_end();
#endif
}

void
_vproc_transaction_end(void)
{
#if !TARGET_OS_EMBEDDED
	typeof(vproc_shmem->vp_shmem_transaction_cnt) newval;

	if (unlikely(vproc_shmem == NULL)) {
		return;
	}

	newval = __sync_sub_and_fetch(&vproc_shmem->vp_shmem_transaction_cnt, 1);

	runtime_ktrace(RTKT_VPROC_TRANSACTION_DECREMENT, newval, 0, 0);
	if (unlikely(newval < 0)) {
		if (vproc_shmem->vp_shmem_flags & VPROC_SHMEM_EXITING) {
			_exit(0);
		} else {
			__crashreporter_info__ = "Unbalanced: vproc_transaction_end()";
		}
		abort();
	}
#endif
}

vproc_standby_t
vproc_standby_begin(vproc_t vp __attribute__((unused)))
{
#ifdef VPROC_STANDBY_IMPLEMENTED
	vproc_standby_t vpsb = (vproc_standby_t)vproc_shmem_init; /* we need a "random" variable that is testable */

	_vproc_standby_begin();

	return vpsb;
#else
	return NULL;
#endif
}

void
_vproc_standby_begin(void)
{
#ifdef VPROC_STANDBY_IMPLEMENTED
	typeof(vproc_shmem->vp_shmem_standby_cnt) newval;

	if (unlikely(vproc_shmem == NULL)) {
		int po_r = pthread_once(&shmem_inited, vproc_client_init);
		if (po_r != 0 || vproc_shmem == NULL) {
			return;
		}
	}

	newval = __sync_add_and_fetch(&vproc_shmem->vp_shmem_standby_cnt, 1);

	if (unlikely(newval < 1)) {
		__crashreporter_info__ = "Unbalanced: vproc_standby_begin()";
		abort();
	}
#else
	return;
#endif
}

void
vproc_standby_end(vproc_t vp __attribute__((unused)), vproc_standby_t vpt __attribute__((unused)))
{
#ifdef VPROC_STANDBY_IMPLEMENTED
	if (unlikely(vpt != (vproc_standby_t)vproc_shmem_init)) {
		__crashreporter_info__ = "Bogus standby handle passed to vproc_standby_end() ";
		abort();
	}

	_vproc_standby_end();
#else
	return;
#endif
}

void
_vproc_standby_end(void)
{
#ifdef VPROC_STANDBY_IMPLEMENTED
	typeof(vproc_shmem->vp_shmem_standby_cnt) newval;

	if( unlikely(vproc_shmem == NULL) ) {
		__crashreporter_info__ = "Process called vproc_standby_end() when not enrolled in transaction model.";
		abort();
	}

	newval = __sync_sub_and_fetch(&vproc_shmem->vp_shmem_standby_cnt, 1);

	if (unlikely(newval < 0)) {
		__crashreporter_info__ = "Unbalanced: vproc_standby_end()";
		abort();
	}
#else
	return;
#endif
}

kern_return_t
_vproc_grab_subset(mach_port_t bp, mach_port_t *reqport, mach_port_t *rcvright, launch_data_t *outval,
		mach_port_array_t *ports, mach_msg_type_number_t *portCnt)
{
	mach_msg_type_number_t outdata_cnt;
	vm_offset_t outdata = 0;
	size_t data_offset = 0;
	launch_data_t out_obj;
	kern_return_t kr;

	if ((kr = vproc_mig_take_subset(bp, reqport, rcvright, &outdata, &outdata_cnt, ports, portCnt))) {
		goto out;
	}

	if ((out_obj = launch_data_unpack((void *)outdata, outdata_cnt, NULL, 0, &data_offset, NULL))) {
		*outval = launch_data_copy(out_obj);
	} else {
		kr = 1;
	}

out:
	if (outdata) {
		mig_deallocate(outdata, outdata_cnt);
	}

	return kr;
}

vproc_err_t
_vproc_post_fork_ping(void)
{
#if !TARGET_OS_EMBEDDED
	au_asid_t s = AU_DEFAUDITSID;
	do {
		mach_port_t session = MACH_PORT_NULL;
		kern_return_t kr = vproc_mig_post_fork_ping(bootstrap_port, mach_task_self(), &session);
		if( kr != KERN_SUCCESS ) {
			/* If this happens, our bootstrap port probably got hosed. */
			_vproc_log(LOG_ERR, "Post-fork ping failed!");
			break;
		}
		
		/* If we get back MACH_PORT_NULL, that means we just stick with the session
		 * we inherited across fork(2).
		 */
		if( session == MACH_PORT_NULL ) {
			s = ~AU_DEFAUDITSID;
			break;
		}
		
		s = _audit_session_join(session);
		if( s == 0 ) {
			_vproc_log_error(LOG_ERR, "Could not join security session!");
			s = AU_DEFAUDITSID;
		} else {
			_vproc_log(LOG_DEBUG, "Joined session %d.", s);
		}
	} while( 0 );
	
	return s != AU_DEFAUDITSID ? NULL : _vproc_post_fork_ping;
#else
	mach_port_t session = MACH_PORT_NULL;
	return vproc_mig_post_fork_ping(bootstrap_port, mach_task_self(), &session) ? _vproc_post_fork_ping : NULL;
#endif
}

vproc_err_t
_vprocmgr_init(const char *session_type)
{
	if (vproc_mig_init_session(bootstrap_port, (char *)session_type, _audit_session_self()) == 0) {
		return NULL;
	}

	return (vproc_err_t)_vprocmgr_init;
}

vproc_err_t
_vprocmgr_move_subset_to_user(uid_t target_user, const char *session_type, uint64_t flags)
{
	kern_return_t kr = 0;
	bool is_bkgd = (strcmp(session_type, VPROCMGR_SESSION_BACKGROUND) == 0);
	int64_t ldpid, lduid;

	if (vproc_swap_integer(NULL, VPROC_GSK_MGR_PID, 0, &ldpid) != 0) {
		return (vproc_err_t)_vprocmgr_move_subset_to_user;
	}

	if (vproc_swap_integer(NULL, VPROC_GSK_MGR_UID, 0, &lduid) != 0) {
		return (vproc_err_t)_vprocmgr_move_subset_to_user;
	}

	if (!is_bkgd && ldpid != 1) {
		if (lduid == getuid()) {
			return NULL;
		}
		/*
		 * Not all sessions can be moved.
		 * We should clean up this mess someday.
		 */
		return (vproc_err_t)_vprocmgr_move_subset_to_user;
	}

	mach_port_t puc = 0, rootbs = get_root_bootstrap_port();
	
	if (vproc_mig_lookup_per_user_context(rootbs, target_user, &puc) != 0) {
		return (vproc_err_t)_vprocmgr_move_subset_to_user;
	}
	
	if( is_bkgd ) {		
		task_set_bootstrap_port(mach_task_self(), puc);
		mach_port_deallocate(mach_task_self(), bootstrap_port);
		bootstrap_port = puc;
	} else {
		kr = vproc_mig_move_subset(puc, bootstrap_port, (char *)session_type, _audit_session_self(), flags);
		mach_port_deallocate(mach_task_self(), puc);
	}
	
	cached_pid = -1;

	if (kr) {
		return (vproc_err_t)_vprocmgr_move_subset_to_user;
	}

	return _vproc_post_fork_ping();
}

vproc_err_t
_vprocmgr_switch_to_session(const char *target_session, vproc_flags_t flags __attribute__((unused)))
{
	mach_port_t new_bsport = MACH_PORT_NULL;
	kern_return_t kr = KERN_FAILURE;

	mach_port_t tnp = MACH_PORT_NULL;
	task_name_for_pid(mach_task_self(), getpid(), &tnp);
	if( (kr = vproc_mig_switch_to_session(bootstrap_port, tnp, (char *)target_session, _audit_session_self(), &new_bsport)) != KERN_SUCCESS ) {
		_vproc_log(LOG_NOTICE, "_vprocmgr_switch_to_session(): kr = 0x%x", kr);
		return (vproc_err_t)_vprocmgr_switch_to_session;
	}
	
	task_set_bootstrap_port(mach_task_self(), new_bsport);
	mach_port_deallocate(mach_task_self(), bootstrap_port);
	bootstrap_port = new_bsport;
	
	return !issetugid() ? _vproc_post_fork_ping() : NULL;
}

vproc_err_t 
_vprocmgr_detach_from_console(vproc_flags_t flags __attribute__((unused)))
{
	return _vprocmgr_switch_to_session(VPROCMGR_SESSION_BACKGROUND, 0);
}

pid_t
_spawn_via_launchd(const char *label, const char *const *argv, const struct spawn_via_launchd_attr *spawn_attrs, int struct_version)
{
	size_t i, good_enough_size = 10*1024*1024;
	mach_msg_type_number_t indata_cnt = 0;
	vm_offset_t indata = 0;
	mach_port_t obsvr_port = MACH_PORT_NULL;
	launch_data_t tmp, tmp_array, in_obj;
	const char *const *tmpp;
	kern_return_t kr = 1;
	void *buf = NULL;
	pid_t p = -1;

	if ((in_obj = launch_data_alloc(LAUNCH_DATA_DICTIONARY)) == NULL) {
		goto out;
	}

	if ((tmp = launch_data_new_string(label)) == NULL) {
		goto out;
	}

	launch_data_dict_insert(in_obj, tmp, LAUNCH_JOBKEY_LABEL);

	if ((tmp_array = launch_data_alloc(LAUNCH_DATA_ARRAY)) == NULL) {
		goto out;
	}

	for (i = 0; *argv; i++, argv++) {
		tmp = launch_data_new_string(*argv);
		if (tmp == NULL) {
			goto out;
		}

		launch_data_array_set_index(tmp_array, tmp, i);
	}

	launch_data_dict_insert(in_obj, tmp_array, LAUNCH_JOBKEY_PROGRAMARGUMENTS);

	if (spawn_attrs) switch (struct_version) {
	case 2:
#if HAVE_QUARANTINE
		if (spawn_attrs->spawn_quarantine) {
			char qbuf[QTN_SERIALIZED_DATA_MAX];
			size_t qbuf_sz = QTN_SERIALIZED_DATA_MAX;

			if (qtn_proc_to_data(spawn_attrs->spawn_quarantine, qbuf, &qbuf_sz) == 0) {
				tmp = launch_data_new_opaque(qbuf, qbuf_sz);
				launch_data_dict_insert(in_obj, tmp, LAUNCH_JOBKEY_QUARANTINEDATA);
			}
		}
#endif

		if (spawn_attrs->spawn_seatbelt_profile) {
			tmp = launch_data_new_string(spawn_attrs->spawn_seatbelt_profile);
			launch_data_dict_insert(in_obj, tmp, LAUNCH_JOBKEY_SANDBOXPROFILE);
		}

		if (spawn_attrs->spawn_seatbelt_flags) {
			tmp = launch_data_new_integer(*spawn_attrs->spawn_seatbelt_flags);
			launch_data_dict_insert(in_obj, tmp, LAUNCH_JOBKEY_SANDBOXFLAGS);
		}

		/* fall through */
	case 1:
		if (spawn_attrs->spawn_binpref) {
			tmp_array = launch_data_alloc(LAUNCH_DATA_ARRAY);
			for (i = 0; i < spawn_attrs->spawn_binpref_cnt; i++) {
				tmp = launch_data_new_integer(spawn_attrs->spawn_binpref[i]);
				launch_data_array_set_index(tmp_array, tmp, i);
			}
			launch_data_dict_insert(in_obj, tmp_array, LAUNCH_JOBKEY_BINARYORDERPREFERENCE);
		}
		/* fall through */
	case 0:
		if (spawn_attrs->spawn_flags & SPAWN_VIA_LAUNCHD_STOPPED) {
			tmp = launch_data_new_bool(true);
			launch_data_dict_insert(in_obj, tmp, LAUNCH_JOBKEY_WAITFORDEBUGGER);
		}

		if (spawn_attrs->spawn_env) {
			launch_data_t tmp_dict = launch_data_alloc(LAUNCH_DATA_DICTIONARY);

			for (tmpp = spawn_attrs->spawn_env; *tmpp; tmpp++) {
				char *eqoff, tmpstr[strlen(*tmpp) + 1];

				strcpy(tmpstr, *tmpp);

				eqoff = strchr(tmpstr, '=');

				if (!eqoff) {
					goto out;
				}
				
				*eqoff = '\0';
				
				launch_data_dict_insert(tmp_dict, launch_data_new_string(eqoff + 1), tmpstr);
			}

			launch_data_dict_insert(in_obj, tmp_dict, LAUNCH_JOBKEY_ENVIRONMENTVARIABLES);
		}

		if (spawn_attrs->spawn_path) {
			tmp = launch_data_new_string(spawn_attrs->spawn_path);
			launch_data_dict_insert(in_obj, tmp, LAUNCH_JOBKEY_PROGRAM);
		}

		if (spawn_attrs->spawn_chdir) {
			tmp = launch_data_new_string(spawn_attrs->spawn_chdir);
			launch_data_dict_insert(in_obj, tmp, LAUNCH_JOBKEY_WORKINGDIRECTORY);
		}

		if (spawn_attrs->spawn_umask) {
			tmp = launch_data_new_integer(*spawn_attrs->spawn_umask);
			launch_data_dict_insert(in_obj, tmp, LAUNCH_JOBKEY_UMASK);
		}

		break;
	default:
		break;
	}

	if (!(buf = malloc(good_enough_size))) {
		goto out;
	}

	if ((indata_cnt = launch_data_pack(in_obj, buf, good_enough_size, NULL, NULL)) == 0) {
		goto out;
	}

	indata = (vm_offset_t)buf;

	kr = vproc_mig_spawn(bootstrap_port, indata, indata_cnt, _audit_session_self(), &p, &obsvr_port);

	if (kr == VPROC_ERR_TRY_PER_USER) {
		mach_port_t puc;

		if (vproc_mig_lookup_per_user_context(bootstrap_port, 0, &puc) == 0) {
			kr = vproc_mig_spawn(puc, indata, indata_cnt, _audit_session_self(), &p, &obsvr_port);
			mach_port_deallocate(mach_task_self(), puc);
		}
	}

out:
	if (in_obj) {
		launch_data_free(in_obj);
	}

	if (buf) {
		free(buf);
	}

	switch (kr) {
	case BOOTSTRAP_SUCCESS:
		if (spawn_attrs && spawn_attrs->spawn_observer_port) {
			*spawn_attrs->spawn_observer_port = obsvr_port;
		} else {
			mach_port_deallocate(mach_task_self(), obsvr_port);
		}
		return p;
	case BOOTSTRAP_NOT_PRIVILEGED:
		errno = EPERM; break;
	case BOOTSTRAP_NO_MEMORY:
		errno = ENOMEM; break;
	case BOOTSTRAP_NAME_IN_USE:
		errno = EEXIST; break;
	case 1:
		errno = EIO; break;
	default:
		errno = EINVAL; break;
	}

	return -1;
}

kern_return_t
mpm_wait(mach_port_t ajob, int *wstatus)
{
	return vproc_mig_wait(ajob, wstatus);
}

kern_return_t
mpm_uncork_fork(mach_port_t ajob)
{
	return vproc_mig_uncork_fork(ajob);
}

kern_return_t
_vprocmgr_getsocket(name_t sockpath)
{
	return vproc_mig_getsocket(bootstrap_port, sockpath);
}

vproc_err_t
_vproc_get_last_exit_status(int *wstatus)
{
	int64_t val;

	if (vproc_swap_integer(NULL, VPROC_GSK_LAST_EXIT_STATUS, 0, &val) == 0) {
		*wstatus = (int)val;
		return NULL;
	}

	return (vproc_err_t)_vproc_get_last_exit_status;
}

vproc_err_t
_vproc_send_signal_by_label(const char *label, int sig)
{
	if (vproc_mig_send_signal(bootstrap_port, (char *)label, sig) == 0) {
		return NULL;
	}

	return _vproc_send_signal_by_label;
}

vproc_err_t
_vprocmgr_log_forward(mach_port_t mp, void *data, size_t len)
{
	if (vproc_mig_log_forward(mp, (vm_offset_t)data, len) == 0) {
		return NULL;
	}

	return _vprocmgr_log_forward;
}

vproc_err_t
_vprocmgr_log_drain(vproc_t vp __attribute__((unused)), pthread_mutex_t *mutex, _vprocmgr_log_drain_callback_t func)
{
	mach_msg_type_number_t outdata_cnt, tmp_cnt;
	vm_offset_t outdata = 0;
	struct timeval tv;
	struct logmsg_s *lm;

	if (!func) {
		return _vprocmgr_log_drain;
	}

	if (vproc_mig_log_drain(bootstrap_port, &outdata, &outdata_cnt) != 0) {
		return _vprocmgr_log_drain;
	}

	tmp_cnt = outdata_cnt;

	if (mutex) {
		pthread_mutex_lock(mutex);
	}

	for (lm = (struct logmsg_s *)outdata; tmp_cnt > 0; lm = ((void *)lm + lm->obj_sz)) {
		lm->from_name = (char *)lm + lm->from_name_offset;
		lm->about_name = (char *)lm + lm->about_name_offset;
		lm->msg = (char *)lm + lm->msg_offset;
		lm->session_name = (char *)lm + lm->session_name_offset;

		tv.tv_sec = lm->when / USEC_PER_SEC;
		tv.tv_usec = lm->when % USEC_PER_SEC;

		func(&tv, lm->from_pid, lm->about_pid, lm->sender_uid, lm->sender_gid, lm->pri,
				lm->from_name, lm->about_name, lm->session_name, lm->msg);

		tmp_cnt -= lm->obj_sz;
	}

	if (mutex) {
		pthread_mutex_unlock(mutex);
	}

	if (outdata) {
		mig_deallocate(outdata, outdata_cnt);
	}

	return NULL;
}

vproc_err_t
vproc_swap_integer(vproc_t vp, vproc_gsk_t key, int64_t *inval, int64_t *outval)
{
	static int64_t cached_is_managed = -1;
	int64_t dummyval = 0;

	switch (key) {
	case VPROC_GSK_MGR_PID:
		if (cached_pid != -1 && outval) {
			*outval = cached_pid;
			return NULL;
		}
		break;
	case VPROC_GSK_IS_MANAGED:
		if (cached_is_managed != -1 && outval) {
			*outval = cached_is_managed;
			return NULL;
		}
		break;
	case VPROC_GSK_TRANSACTIONS_ENABLED:
		/* Shared memory region is required for transactions. */
		if( unlikely(vproc_shmem == NULL) ) {
			int po_r = pthread_once(&shmem_inited, vproc_client_init);
			if( po_r != 0 || vproc_shmem == NULL ) {
				if( outval ) {
					*outval = -1;
				}
				return (vproc_err_t)vproc_swap_integer;
			}
		}
	
		if( s_cached_transactions_enabled && outval ) {
			*outval = s_cached_transactions_enabled;
			return NULL;
		}
		break;
	default:
		break;
	}

	kern_return_t kr = KERN_FAILURE;
	mach_port_t mp = vp ? vp->j_port : bootstrap_port;
	if ((kr = vproc_mig_swap_integer(mp, inval ? key : 0, outval ? key : 0, inval ? *inval : 0, outval ? outval : &dummyval)) == 0) {
		switch (key) {
		case VPROC_GSK_MGR_PID:
			cached_pid = outval ? *outval : dummyval;
			break;
		case VPROC_GSK_IS_MANAGED:
			cached_is_managed = outval ? *outval : dummyval;
			break;
		case VPROC_GSK_TRANSACTIONS_ENABLED:
			/* Once you're in the transaction model, you're in for good. Like the Mafia. */
			s_cached_transactions_enabled = 1;
			break;
		case VPROC_GSK_PERUSER_SUSPEND: {
			char peruser_label[NAME_MAX];
			snprintf(peruser_label, NAME_MAX - 1, "com.apple.launchd.peruser.%u", (uid_t)*inval);
			
			vproc_t pu_vp = vprocmgr_lookup_vproc(peruser_label);
			if( pu_vp ) {
				int status = 0;
				kr = vproc_mig_wait2(bootstrap_port, pu_vp->j_port, &status, false);
				vproc_release(pu_vp);
			}
			break;
		}
		default:
			break;
		}
		return NULL;
	}

	return (vproc_err_t)vproc_swap_integer;
}

mach_port_t
get_root_bootstrap_port(void)
{
	mach_port_t parent_port = 0;
	mach_port_t previous_port = 0;

	do {
		if (previous_port) {
			if (previous_port != bootstrap_port) {
				mach_port_deallocate(mach_task_self(), previous_port);
			}
			previous_port = parent_port;
		} else {
			previous_port = bootstrap_port;
		}

		if (bootstrap_parent(previous_port, &parent_port) != 0) {
			return MACH_PORT_NULL;
		}

	} while (parent_port != previous_port);

	return parent_port;
}

vproc_err_t
vproc_swap_complex(vproc_t vp, vproc_gsk_t key, launch_data_t inval, launch_data_t *outval)
{
	size_t data_offset = 0, good_enough_size = 10*1024*1024;
	mach_msg_type_number_t indata_cnt = 0, outdata_cnt;
	vm_offset_t indata = 0, outdata = 0;
	launch_data_t out_obj;
	void *rval = vproc_swap_complex;
	void *buf = NULL;

	if (inval) {
		if (!(buf = malloc(good_enough_size))) {
			goto out;
		}

		if ((indata_cnt = launch_data_pack(inval, buf, good_enough_size, NULL, NULL)) == 0) {
			goto out;
		}

		indata = (vm_offset_t)buf;
	}

	mach_port_t mp = vp ? vp->j_port : bootstrap_port;
	if (vproc_mig_swap_complex(mp, inval ? key : 0, outval ? key : 0, indata, indata_cnt, &outdata, &outdata_cnt) != 0) {
		goto out;
	}

	if (outval) {
		if (!(out_obj = launch_data_unpack((void *)outdata, outdata_cnt, NULL, 0, &data_offset, NULL))) {
			goto out;
		}

		if (!(*outval = launch_data_copy(out_obj))) {
			goto out;
		}
	}

	rval = NULL;
out:
	if (buf) {
		free(buf);
	}

	if (outdata) {
		mig_deallocate(outdata, outdata_cnt);
	}

	return rval;
}

vproc_err_t
vproc_swap_string(vproc_t vp, vproc_gsk_t key, const char *instr, char **outstr)
{
	launch_data_t instr_data = instr ? launch_data_new_string(instr) : NULL;
	launch_data_t outstr_data = NULL;
	
	vproc_err_t verr = vproc_swap_complex(vp, key, instr_data, &outstr_data);
	if( !verr && outstr ) {
		if( launch_data_get_type(outstr_data) == LAUNCH_DATA_STRING ) {
			*outstr = strdup(launch_data_get_string(outstr_data));
		} else {
			verr = (vproc_err_t)vproc_swap_string;
		}
		launch_data_free(outstr_data);
	}
	if( instr_data ) {
		launch_data_free(instr_data);
	}
	
	return verr;
}

void *
reboot2(uint64_t flags)
{
	if (vproc_mig_reboot2(get_root_bootstrap_port(), flags) == 0) {
		return NULL;
	}

	return reboot2;
}

vproc_err_t
_vproc_kickstart_by_label(const char *label, pid_t *out_pid, mach_port_t *out_port_name, mach_port_t *out_obsrvr_port, vproc_flags_t flags)
{
	mach_port_t junk = MACH_PORT_NULL;
	mach_port_t junk2 = MACH_PORT_NULL;
	
	kern_return_t kr = vproc_mig_kickstart(bootstrap_port, (char *)label, out_pid, out_port_name ?: &junk, out_obsrvr_port ?: &junk2, flags);
	if( kr == KERN_SUCCESS ) {
		if( !out_port_name ) {
			mach_port_mod_refs(mach_task_self(), junk, MACH_PORT_RIGHT_SEND, -1);
		}
		
		if( !out_obsrvr_port ) {
			mach_port_mod_refs(mach_task_self(), junk2, MACH_PORT_RIGHT_SEND, -1);
		}
		
		return NULL;
	}

	return (vproc_err_t)_vproc_kickstart_by_label;
}

vproc_err_t
_vproc_wait_by_label(const char *label, int *out_wstatus)
{
	if (vproc_mig_embedded_wait(bootstrap_port, (char *)label, out_wstatus) == 0) {
		return NULL;
	}

	return (vproc_err_t)_vproc_wait_by_label;
}

vproc_err_t
_vproc_set_global_on_demand(bool state)
{
	int64_t val = state ? ~0 : 0;

	if (vproc_swap_integer(NULL, VPROC_GSK_GLOBAL_ON_DEMAND, &val, NULL) == 0) {
		return NULL;
	}

	return (vproc_err_t)_vproc_set_global_on_demand;
}

void
_vproc_logv(int pri, int err, const char *msg, va_list ap)
{
	char flat_msg[3000];

	vsnprintf(flat_msg, sizeof(flat_msg), msg, ap);

	vproc_mig_log(bootstrap_port, pri, err, flat_msg);
}

void
_vproc_log(int pri, const char *msg, ...)
{
	va_list ap;

	va_start(ap, msg);
	_vproc_logv(pri, 0, msg, ap);
	va_end(ap);
}

void
_vproc_log_error(int pri, const char *msg, ...)
{
	int saved_errno = errno;
	va_list ap;

	va_start(ap, msg);
	_vproc_logv(pri, saved_errno, msg, ap);
	va_end(ap);
}
