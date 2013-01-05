#ifndef __LAUNCHD_CORE_LOGIC__
#define __LAUNCHD_CORE_LOGIC__
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

#define LAUNCHD_MIN_JOB_RUN_TIME 10
#define LAUNCHD_REWARD_JOB_RUN_TIME 60
#define LAUNCHD_FAILED_EXITS_THRESHOLD 10

#include "bootstrap_public.h"

struct jobcb;
struct machservice;


struct machservice *machservice_new(struct jobcb *j, const char *name, mach_port_t *serviceport);
void machservice_delete(struct machservice *);
void machservice_watch(struct machservice *);
mach_port_t machservice_port(struct machservice *);
struct jobcb *machservice_job(struct machservice *);
bool machservice_hidden(struct machservice *);
bool machservice_active(struct machservice *);
const char *machservice_name(struct machservice *);
bootstrap_status_t machservice_status(struct machservice *);


struct jobcb *job_find(struct jobcb *j, const char *label);
struct jobcb *job_find_by_pid(struct jobcb *j, pid_t p);
struct jobcb *job_find_by_port(mach_port_t mp);
struct jobcb *job_import(launch_data_t pload);
launch_data_t job_import_bulk(launch_data_t pload);
struct jobcb *job_new(struct jobcb *p, const char *label, const char *prog, const char *const *argv, const char *stdinpath, mach_port_t);
struct jobcb *job_new_spawn(const char *label, const char *path, const char *workingdir, const char *const *argv, const char *const *env, mode_t *u_mask, bool w4d, bool fppc);
struct jobcb *job_new_via_mach_init(struct jobcb *jbs, const char *cmd, uid_t uid, bool ond);
struct jobcb *job_new_bootstrap(struct jobcb *p, mach_port_t requestorport, mach_port_t checkin_port);
launch_data_t job_export(struct jobcb *j);
launch_data_t job_export_all(void);
void job_dispatch(struct jobcb *j);
void job_dispatch_all_other_semaphores(struct jobcb *j, struct jobcb *nj);
void job_start(struct jobcb *j);
void job_stop(struct jobcb *j);
bool job_active(struct jobcb *j);
void job_checkin(struct jobcb *j);
const char *job_prog(struct jobcb *j);
void job_remove(struct jobcb *j);
void job_remove_all_inactive(struct jobcb *j);
bool job_ack_port_destruction(struct jobcb *j, mach_port_t p);
void job_ack_no_senders(struct jobcb *j);
pid_t job_get_pid(struct jobcb *j);
mach_port_t job_get_bsport(struct jobcb *j);
mach_port_t job_get_reqport(struct jobcb *j);
struct jobcb *job_get_bs(struct jobcb *j);
void job_delete_anything_with_port(struct jobcb *jbs, mach_port_t port);
struct jobcb *job_parent(struct jobcb *j);
void job_uncork_fork(struct jobcb *j);
struct machservice *job_lookup_service(struct jobcb *jbs, const char *name, bool check_parent);
void job_foreach_service(struct jobcb *jbs, void (*bs_iter)(struct machservice *, void *), void *context, bool include_subjobs);
void job_log(struct jobcb *j, int pri, const char *msg, ...) __attribute__((format(printf, 3, 4)));
void job_log_error(struct jobcb *j, int pri, const char *msg, ...) __attribute__((format(printf, 3, 4)));
kern_return_t job_handle_mpm_wait(struct jobcb *j, mach_port_t srp, int *waitstatus);

extern size_t total_children;

extern struct jobcb *root_job;

extern struct jobcb *gc_this_job;

#endif
