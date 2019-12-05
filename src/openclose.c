/*
 * Copyright © 2014 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including
 * the next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "libhsakmt.h"

#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <strings.h>
#include "fmm.h"

static const char kfd_device_name[] = "/dev/kfd";
static pid_t parent_pid = -1;
int hsakmt_debug_level;
bool hsakmt_forked;

/* zfb is mainly used during emulation */
int zfb_support;

/* is_forked_child detects when the process has forked since the last
 * time this function was called. We cannot rely on pthread_atfork
 * because the process can fork without calling the fork function in
 * libc (using clone or calling the system call directly).
 */
static bool is_forked_child(void)
{
	pid_t cur_pid;

	if (hsakmt_forked)
		return true;

	cur_pid = getpid();

	if (parent_pid == -1) {
		parent_pid = cur_pid;
		return false;
	}

	if (parent_pid != cur_pid) {
		hsakmt_forked = true;
		return true;
	}

	return false;
}

/* Callbacks from pthread_atfork */
static void prepare_fork_handler(void)
{
	pthread_mutex_lock(&hsakmt_mutex);
}
static void parent_fork_handler(void)
{
	pthread_mutex_unlock(&hsakmt_mutex);
}
static void child_fork_handler(void)
{
	pthread_mutex_init(&hsakmt_mutex, NULL);
	hsakmt_forked = true;
}

/* Call this from the child process after fork. This will clear all
 * data that is duplicated from the parent process, that is not valid
 * in the child.
 * The topology information is duplicated from the parent is valid
 * in the child process so it is not cleared
 */
static void clear_after_fork(void)
{
	clear_process_doorbells();
	clear_events_page();
	fmm_clear_all_mem();
	destroy_device_debugging_memory();
	if (kfd_fd) {
		close(kfd_fd);
		kfd_fd = 0;
	}
	kfd_open_count = 0;
	parent_pid = -1;
	hsakmt_forked = false;
}

static inline void init_page_size(void)
{
	PAGE_SIZE = sysconf(_SC_PAGESIZE);
	PAGE_SHIFT = ffs(PAGE_SIZE) - 1;
}

static HSAKMT_STATUS init_vars_from_env(void)
{
	char *envvar;
	int debug_level;

	/* Normally libraries don't print messages. For debugging purpose, we'll
	 * print messages if an environment variable, HSAKMT_DEBUG_LEVEL, is set.
	 */
	hsakmt_debug_level = HSAKMT_DEBUG_LEVEL_DEFAULT;

	envvar = getenv("HSAKMT_DEBUG_LEVEL");
	if (envvar) {
		debug_level = atoi(envvar);
		if (debug_level >= HSAKMT_DEBUG_LEVEL_ERR &&
				debug_level <= HSAKMT_DEBUG_LEVEL_DEBUG)
			hsakmt_debug_level = debug_level;
	}

	/* Check whether to support Zero frame buffer */
	envvar = getenv("HSA_ZFB");
	if (envvar)
		zfb_support = atoi(envvar);

	/* Force all the GPUs to a certain type, use the below command:
	 * export HSA_FORCE_ASIC_TYPE="10.1.0 1 Navi10 14"
	 * meaning major.minor.step dgpu asic_name asic_id
	 */
	envvar = getenv("HSA_FORCE_ASIC_TYPE");
	if (envvar) {
		uint32_t major, minor, step, dgpu, asic_family;

		if ((sscanf(envvar, "%u.%u.%u %u %63s %u", &major, &minor, &step,
				&dgpu, force_asic_name, &asic_family) != 6)
			|| (major > 63 || minor > 255 || step > 255)
			|| dgpu > 1 || asic_family >= CHIP_LAST) {
			pr_err("HSA_FORCE_ASIC_TYPE %s is invalid\n", envvar);
			return HSAKMT_STATUS_ERROR;
		}

		force_asic_entry.major = major;
		force_asic_entry.minor = minor;
		force_asic_entry.stepping = step;
		force_asic_entry.is_dgpu = dgpu;

		force_asic_entry.asic_family = asic_family;

		force_asic = 1;
	}

	return HSAKMT_STATUS_SUCCESS;
}

HSAKMT_STATUS HSAKMTAPI hsaKmtOpenKFD(void)
{
	HSAKMT_STATUS result;
	int fd;
	HsaSystemProperties sys_props;

	pthread_mutex_lock(&hsakmt_mutex);

	/* If the process has forked, the child process must re-initialize
	 * it's connection to KFD. Any references tracked by kfd_open_count
	 * belong to the parent
	 */
	if (is_forked_child())
		clear_after_fork();

	if (kfd_open_count == 0) {
		static bool atfork_installed = false;

		result = init_vars_from_env();
		if (result != HSAKMT_STATUS_SUCCESS)
			goto open_failed;

		fd = open(kfd_device_name, O_RDWR | O_CLOEXEC);

		if (fd == -1) {
			result = HSAKMT_STATUS_KERNEL_IO_CHANNEL_NOT_OPENED;
			goto open_failed;
		}

		kfd_fd = fd;
		kfd_open_count = 1;

		init_page_size();

		result = topology_sysfs_get_system_props(&sys_props);
		if (result != HSAKMT_STATUS_SUCCESS)
			goto topology_sysfs_failed;

		result = fmm_init_process_apertures(sys_props.NumNodes);
		if (result != HSAKMT_STATUS_SUCCESS)
			goto init_process_aperture_failed;

		result = init_process_doorbells(sys_props.NumNodes);
		if (result != HSAKMT_STATUS_SUCCESS)
			goto init_doorbell_failed;

		if (init_device_debugging_memory(sys_props.NumNodes) != HSAKMT_STATUS_SUCCESS)
			pr_warn("Insufficient Memory. Debugging unavailable\n");

		init_counter_props(sys_props.NumNodes);

		if (!atfork_installed) {
			/* Atfork handlers cannot be uninstalled and
			 * must be installed only once. Otherwise
			 * prepare will deadlock when trying to take
			 * the same lock multiple times.
			 */
			pthread_atfork(prepare_fork_handler,
				       parent_fork_handler,
				       child_fork_handler);
			atfork_installed = true;
		}
	} else {
		kfd_open_count++;
		result = HSAKMT_STATUS_KERNEL_ALREADY_OPENED;
	}

	pthread_mutex_unlock(&hsakmt_mutex);
	return result;

init_doorbell_failed:
	fmm_destroy_process_apertures();
init_process_aperture_failed:
topology_sysfs_failed:
	close(fd);
open_failed:
	pthread_mutex_unlock(&hsakmt_mutex);

	return result;
}

HSAKMT_STATUS HSAKMTAPI hsaKmtCloseKFD(void)
{
	HSAKMT_STATUS result;

	pthread_mutex_lock(&hsakmt_mutex);

	if (kfd_open_count > 0)	{
		if (--kfd_open_count == 0) {
			destroy_counter_props();
			destroy_device_debugging_memory();
			destroy_process_doorbells();
			fmm_destroy_process_apertures();
			if (kfd_fd) {
				close(kfd_fd);
				kfd_fd = 0;
			}
		}

		result = HSAKMT_STATUS_SUCCESS;
	} else
		result = HSAKMT_STATUS_KERNEL_IO_CHANNEL_NOT_OPENED;

	pthread_mutex_unlock(&hsakmt_mutex);

	return result;
}
