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

#include <mach/mach.h>
#include <mach/vm_map.h>

#include "bootstrap_public.h"
#include "bootstrap_private.h"
#include "launch.h"
#include "launch_priv.h"

#define mig_external static
#include "bootstrap.h"
#include "bootstrapUser.c"

#include <sys/param.h>
#include <stdlib.h>
#include <errno.h>

kern_return_t
_launchd_to_launchd(mach_port_t bp, mach_port_t *reqport, mach_port_t *rcvright,
		name_array_t *service_names, mach_msg_type_number_t *service_namesCnt,
		mach_port_array_t *ports, mach_msg_type_number_t *portCnt)
{
	return raw_bootstrap_transfer_subset(bp, reqport, rcvright, service_names, service_namesCnt, ports, portCnt);
}

pid_t
_spawn_via_launchd(const char *label, const char *const *argv, const struct spawn_via_launchd_attr *spawn_attrs, int struct_version)
{
	kern_return_t kr;
	const char *const *tmpp;
	size_t len, buf_len = strlen(label) + 1;
	char *buf = strdup(label);
	uint64_t flags = 0;
	uint32_t argc = 0;
	uint32_t envc = 0;
	pid_t p = -1;
	mode_t u_mask = CMASK;
	mach_port_t obsvr_port = MACH_PORT_NULL;

	for (tmpp = argv; *tmpp; tmpp++) {
		argc++;
		len = strlen(*tmpp) + 1;
		buf = reallocf(buf, buf_len + len);
		strcpy(buf + buf_len, *tmpp);
		buf_len += len;
	}

	if (spawn_attrs) switch (struct_version) {
	case 0:
		if (spawn_attrs->spawn_flags & SPAWN_VIA_LAUNCHD_STOPPED) {
			flags |= SPAWN_WANTS_WAIT4DEBUGGER;
		}
		if (spawn_attrs->spawn_flags & SPAWN_VIA_LAUNCHD_FORCE_PPC) {
			flags |= SPAWN_WANTS_FORCE_PPC;
		}

		if (spawn_attrs->spawn_env) {
			for (tmpp = spawn_attrs->spawn_env; *tmpp; tmpp++) {
				envc++;
				len = strlen(*tmpp) + 1;
				buf = reallocf(buf, buf_len + len);
				strcpy(buf + buf_len, *tmpp);
				buf_len += len;
			}
		}

		if (spawn_attrs->spawn_path) {
			flags |= SPAWN_HAS_PATH;
			len = strlen(spawn_attrs->spawn_path) + 1;
			buf = reallocf(buf, buf_len + len);
			strcpy(buf + buf_len, spawn_attrs->spawn_path);
			buf_len += len;
		}

		if (spawn_attrs->spawn_chdir) {
			flags |= SPAWN_HAS_WDIR;
			len = strlen(spawn_attrs->spawn_chdir) + 1;
			buf = reallocf(buf, buf_len + len);
			strcpy(buf + buf_len, spawn_attrs->spawn_chdir);
			buf_len += len;
		}

		if (spawn_attrs->spawn_umask) {
			flags |= SPAWN_HAS_UMASK;
			u_mask = *spawn_attrs->spawn_umask;
		}

		break;
	default:
		break;
	}

	kr = raw_mpm_spawn(bootstrap_port, buf, buf_len, argc, envc, flags, u_mask, &p, &obsvr_port);

	free(buf);

	if (kr == BOOTSTRAP_SUCCESS) {
		if (spawn_attrs && spawn_attrs->spawn_observer_port) {
			*spawn_attrs->spawn_observer_port = obsvr_port;
		} else {
			mach_port_deallocate(mach_task_self(), obsvr_port);
		}
		return p;
	}

	switch (kr) {
	case BOOTSTRAP_NOT_PRIVILEGED:
		errno = EPERM; break;
	case BOOTSTRAP_NO_MEMORY:
		errno = ENOMEM; break;
	default:
		errno = EINVAL; break;
	}
	return -1;
}

kern_return_t
mpm_wait(mach_port_t ajob, int *wstatus)
{
	return raw_mpm_wait(ajob, wstatus);
}

kern_return_t
mpm_uncork_fork(mach_port_t ajob)
{
	return raw_mpm_uncork_fork(ajob);
}

kern_return_t
bootstrap_create_server(mach_port_t bp, cmd_t server_cmd, uid_t server_uid, boolean_t on_demand, mach_port_t *server_port)
{
	return raw_bootstrap_create_server(bp, server_cmd, server_uid, on_demand, server_port);
}

kern_return_t
bootstrap_subset(mach_port_t bp, mach_port_t requestor_port, mach_port_t *subset_port)
{
	return raw_bootstrap_subset(bp, requestor_port, subset_port);
}

kern_return_t
bootstrap_unprivileged(mach_port_t bp, mach_port_t *unpriv_port)
{
	return raw_bootstrap_unprivileged(bp, unpriv_port);
}

kern_return_t
bootstrap_getsocket(mach_port_t bp, name_t sockpath)
{
	return raw_bootstrap_getsocket(bp, sockpath);
}

kern_return_t
bootstrap_parent(mach_port_t bp, mach_port_t *parent_port)
{
	return raw_bootstrap_parent(bp, parent_port);
}

kern_return_t
bootstrap_register(mach_port_t bp, name_t service_name, mach_port_t sp)
{
	return raw_bootstrap_register(bp, service_name, sp);
}

kern_return_t
bootstrap_create_service(mach_port_t bp, name_t service_name, mach_port_t *sp)
{
	return raw_bootstrap_create_service(bp, service_name, sp);
}

kern_return_t
bootstrap_check_in(mach_port_t bp, name_t service_name, mach_port_t *sp)
{
	return raw_bootstrap_check_in(bp, service_name, sp);
}

kern_return_t
bootstrap_look_up(mach_port_t bp, name_t service_name, mach_port_t *sp)
{
	return raw_bootstrap_look_up(bp, service_name, sp);
}

kern_return_t
bootstrap_look_up_array(mach_port_t bp,
		name_array_t names, mach_msg_type_number_t name_cnt,
		mach_port_array_t *ports, mach_msg_type_number_t *port_cnt,
		boolean_t *all)
{
	unsigned int i;
	kern_return_t r;

	if (name_cnt > BOOTSTRAP_MAX_LOOKUP_COUNT)
		return BOOTSTRAP_BAD_COUNT;

	*port_cnt = name_cnt;

	r = vm_allocate(mach_task_self(), (vm_address_t *)&ports, name_cnt * sizeof(mach_port_t), true);

	if (r != KERN_SUCCESS)
		return r;

	*all = true;

	for (i = 0; i < name_cnt; i++) {
		if (bootstrap_look_up(bp, names[i], &((*ports)[i])) == BOOTSTRAP_SUCCESS)
			continue;
		*all = false;
		ports[i] = MACH_PORT_NULL;
	}

	return BOOTSTRAP_SUCCESS;
}

kern_return_t
bootstrap_status(mach_port_t bp, name_t service_name, bootstrap_status_t *service_active)
{
	mach_port_t p;

	if (bootstrap_check_in(bp, service_name, &p) == BOOTSTRAP_SUCCESS) {
		mach_port_mod_refs(mach_task_self(), p, MACH_PORT_RIGHT_RECEIVE, -1);
		*service_active = BOOTSTRAP_STATUS_ON_DEMAND;
		if (raw_bootstrap_unprivileged(bp, &p) == BOOTSTRAP_SUCCESS) {
			if (bp == p)
				*service_active = BOOTSTRAP_STATUS_INACTIVE;
			mach_port_deallocate(mach_task_self(), p);
		}
		return BOOTSTRAP_SUCCESS;
	} else if (bootstrap_look_up(bp, service_name, &p) == BOOTSTRAP_SUCCESS) {
		mach_port_deallocate(mach_task_self(), p);
		*service_active = BOOTSTRAP_STATUS_ACTIVE;
		return BOOTSTRAP_SUCCESS;
	}

	return BOOTSTRAP_UNKNOWN_SERVICE;
}

kern_return_t
bootstrap_info(mach_port_t bp,
		name_array_t *service_names, mach_msg_type_number_t *service_namesCnt,
		bootstrap_status_array_t *service_active, mach_msg_type_number_t *service_activeCnt)
{
	return raw_bootstrap_info(bp, service_names, service_namesCnt,
			service_active, service_activeCnt);
}

const char *
bootstrap_strerror(kern_return_t r)
{
	switch (r) {
	case BOOTSTRAP_SUCCESS:
		return "Success";
	case BOOTSTRAP_NOT_PRIVILEGED:
		return "Permission denied";
	case BOOTSTRAP_NAME_IN_USE:
	case BOOTSTRAP_SERVICE_ACTIVE:
		return "Service name already exists";
	case BOOTSTRAP_UNKNOWN_SERVICE:
		return "Unknown service name";
	case BOOTSTRAP_BAD_COUNT:
		return "Too many lookups were requested in one request";
	case BOOTSTRAP_NO_MEMORY:
		return "Out of memory";
	default:
		return mach_error_string(r);
	}
}
