/*
 * Copyright (c) 2013 NVIDIA Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef __OTE_PROTOCOL_H__
#define __OTE_PROTOCOL_H__

#include "ote_types.h"

#define TE_IOCTL_MAGIC_NUMBER ('t')
#define TE_IOCTL_OPEN_CLIENT_SESSION \
	_IOWR(TE_IOCTL_MAGIC_NUMBER, 0x10, union te_cmd)
#define TE_IOCTL_CLOSE_CLIENT_SESSION \
	_IOWR(TE_IOCTL_MAGIC_NUMBER, 0x11, union te_cmd)
#define TE_IOCTL_LAUNCH_OPERATION \
	_IOWR(TE_IOCTL_MAGIC_NUMBER, 0x14, union te_cmd)
#define TE_IOCTL_FILE_NEW_REQ \
	_IOR(TE_IOCTL_MAGIC_NUMBER,  0x16, struct te_file_req)
#define TE_IOCTL_FILE_FILL_BUF \
	_IOR(TE_IOCTL_MAGIC_NUMBER,  0x17, struct te_file_req)
#define TE_IOCTL_FILE_REQ_COMPLETE \
	_IOWR(TE_IOCTL_MAGIC_NUMBER, 0x18, struct te_file_req)

#define TE_IOCTL_MIN_NR	_IOC_NR(TE_IOCTL_OPEN_CLIENT_SESSION)
#define TE_IOCTL_MAX_NR	_IOC_NR(TE_IOCTL_FILE_REQ_COMPLETE)

/* shared buffer is 2 pages: 1st are requests, 2nd are params */
#define TE_CMD_DESC_MAX	(PAGE_SIZE / sizeof(struct te_request))
#define TE_PARAM_MAX	(PAGE_SIZE / sizeof(struct te_oper_param))

#define MAX_EXT_SMC_ARGS	12

uint32_t tlk_generic_smc(uint32_t arg0, uint32_t arg1, uint32_t arg2);
uint32_t tlk_extended_smc(uint32_t *args);
void tlk_irq_handler(void);

#ifdef CONFIG_SMP
void switch_cpumask_to_cpu0(void);
void restore_cpumask(void);
#else
static inline void switch_cpumask_to_cpu0(void) {};
static inline void restore_cpumask(void) {};
#endif

static inline uint32_t
TLK_GENERIC_SMC(uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
	uint32_t retval;

	switch_cpumask_to_cpu0();
	retval = tlk_generic_smc(arg0, arg1, arg2);
	restore_cpumask();

	return retval;
}

static inline uint32_t
TLK_EXTENDED_SMC(uint32_t *args)
{
	uint32_t retval;

	switch_cpumask_to_cpu0();
	retval = tlk_extended_smc(args);
	restore_cpumask();

	return retval;
}

struct tlk_device {
	struct te_request *req_addr;
	dma_addr_t req_addr_phys;
	struct te_oper_param *param_addr;
	dma_addr_t param_addr_phys;

	char *req_param_buf;

	unsigned long *param_bitmap;

	struct list_head used_cmd_list;
	struct list_head free_cmd_list;
};

struct te_cmd_req_desc {
	struct te_request *req_addr;
	struct list_head list;
};

struct te_shmem_desc {
	struct list_head list;
	void *buffer;
	size_t size;
	unsigned int mem_type;
	struct page **pages;
	unsigned int nr_pages;
};

struct tlk_context {
	struct tlk_device *dev;
	struct list_head shmem_alloc_list;
};

enum {
	TE_SMC_OPEN_SESSION		= 0xFFFF1004,
	TE_SMC_CLOSE_SESSION		= 0xFFFF1005,
	TE_SMC_LAUNCH_OPERATION		= 0xFFFF1000,
	TE_SMC_REGISTER_IRQ_HANDLER	= 0xFFFF1FF0,
	TE_SMC_NS_IRQ_DONE		= 0xFFFF1FF1,
	TE_SMC_REGISTER_FS_HANDLERS	= 0xFFFF1FF2,
	TE_SMC_REGISTER_REQ_BUF		= 0xFFFF1FF3,
	TE_SMC_FS_OP_DONE		= 0xFFFF1FFF,
	TE_SMC_INIT_LOGGER		= 0xFFFF1FF4,

};

enum {
	TE_PARAM_TYPE_NONE	= 0,
	TE_PARAM_TYPE_INT_RO    = 1,
	TE_PARAM_TYPE_INT_RW    = 2,
	TE_PARAM_TYPE_MEM_RO    = 3,
	TE_PARAM_TYPE_MEM_RW    = 4,
};

struct te_oper_param {
	uint32_t index;
	uint32_t type;
	union {
		struct {
			uint32_t val;
		} Int;
		struct {
			void  *base;
			uint32_t len;
		} Mem;
	} u;
	void *next_ptr_user;
};

struct te_operation {
	uint32_t command;
	struct te_oper_param *list_head;
	/* Maintain a pointer to tail of list to easily add new param node */
	struct te_oper_param *list_tail;
	uint32_t list_count;
	uint32_t status;
	uint32_t iterface_side;
};

struct te_service_id {
	uint32_t time_low;
	uint16_t time_mid;
	uint16_t time_hi_and_version;
	uint8_t clock_seq_and_node[8];
};

/*
 * OpenSession
 */
struct te_opensession {
	struct te_service_id dest_uuid;
	struct te_operation operation;
	uint32_t answer;
};

/*
 * CloseSession
 */
struct te_closesession {
	uint32_t	session_id;
	uint32_t	answer;
};

/*
 * LaunchOperation
 */
struct te_launchop {
	uint32_t		session_id;
	struct te_operation	operation;
	uint32_t		answer;
};

union te_cmd {
	struct te_opensession	opensession;
	struct te_closesession	closesession;
	struct te_launchop	launchop;
};

struct te_request {
	uint32_t		type;
	uint32_t		session_id;
	uint32_t		command_id;
	struct te_oper_param	*params;
	uint32_t		params_size;
	uint32_t		dest_uuid[4];
	uint32_t		result;
	uint32_t		result_origin;
};

struct te_answer {
	uint32_t	result;
	uint32_t	session_id;
	uint32_t	result_origin;
};

void te_open_session(struct te_opensession *cmd,
	struct te_request *request,
	struct tlk_context *context);

void te_close_session(struct te_closesession *cmd,
	struct te_request *request,
	struct tlk_context *context);

void te_launch_operation(struct te_launchop *cmd,
	struct te_request *request,
	struct tlk_context *context);

#define TE_MAX_FILE_NAME_LEN	64

enum te_file_req_type {
	OTE_FILE_REQ_READ	= 0,
	OTE_FILE_REQ_WRITE	= 1,
	OTE_FILE_REQ_DELETE	= 2,
	OTE_FILE_REQ_SIZE	= 3,
};

struct te_file_req {
	char name[TE_MAX_FILE_NAME_LEN];
	enum te_file_req_type type;
	void *user_data_buf;
	void *kern_data_buf;
	unsigned long data_len;
	unsigned long result;
	int error;
};

int te_handle_fs_ioctl(struct file *file, unsigned int ioctl_num,
		unsigned long ioctl_param);
void ote_print_logs(void);

#endif
