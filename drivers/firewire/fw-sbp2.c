/*
 * SBP2 driver (SCSI over IEEE1394)
 *
 * Copyright (C) 2005-2007  Kristian Hoegsberg <krh@bitplanet.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 * The basic structure of this driver is based on the old storage driver,
 * drivers/ieee1394/sbp2.c, originally written by
 *     James Goodwin <jamesg@filanet.com>
 * with later contributions and ongoing maintenance from
 *     Ben Collins <bcollins@debian.org>,
 *     Stefan Richter <stefanr@s5r6.in-berlin.de>
 * and many others.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mod_devicetable.h>
#include <linux/device.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/blkdev.h>
#include <linux/string.h>
#include <linux/stringify.h>
#include <linux/timer.h>

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>

#include "fw-transaction.h"
#include "fw-topology.h"
#include "fw-device.h"

/*
 * So far only bridges from Oxford Semiconductor are known to support
 * concurrent logins. Depending on firmware, four or two concurrent logins
 * are possible on OXFW911 and newer Oxsemi bridges.
 *
 * Concurrent logins are useful together with cluster filesystems.
 */
static int sbp2_param_exclusive_login = 1;
module_param_named(exclusive_login, sbp2_param_exclusive_login, bool, 0644);
MODULE_PARM_DESC(exclusive_login, "Exclusive login to sbp2 device "
		 "(default = Y, use N for concurrent initiators)");

/*
 * Flags for firmware oddities
 *
 * - 128kB max transfer
 *   Limit transfer size. Necessary for some old bridges.
 *
 * - 36 byte inquiry
 *   When scsi_mod probes the device, let the inquiry command look like that
 *   from MS Windows.
 *
 * - skip mode page 8
 *   Suppress sending of mode_sense for mode page 8 if the device pretends to
 *   support the SCSI Primary Block commands instead of Reduced Block Commands.
 *
 * - fix capacity
 *   Tell sd_mod to correct the last sector number reported by read_capacity.
 *   Avoids access beyond actual disk limits on devices with an off-by-one bug.
 *   Don't use this with devices which don't have this bug.
 *
 * - override internal blacklist
 *   Instead of adding to the built-in blacklist, use only the workarounds
 *   specified in the module load parameter.
 *   Useful if a blacklist entry interfered with a non-broken device.
 */
#define SBP2_WORKAROUND_128K_MAX_TRANS	0x1
#define SBP2_WORKAROUND_INQUIRY_36	0x2
#define SBP2_WORKAROUND_MODE_SENSE_8	0x4
#define SBP2_WORKAROUND_FIX_CAPACITY	0x8
#define SBP2_WORKAROUND_OVERRIDE	0x100

static int sbp2_param_workarounds;
module_param_named(workarounds, sbp2_param_workarounds, int, 0644);
MODULE_PARM_DESC(workarounds, "Work around device bugs (default = 0"
	", 128kB max transfer = " __stringify(SBP2_WORKAROUND_128K_MAX_TRANS)
	", 36 byte inquiry = "    __stringify(SBP2_WORKAROUND_INQUIRY_36)
	", skip mode page 8 = "   __stringify(SBP2_WORKAROUND_MODE_SENSE_8)
	", fix capacity = "       __stringify(SBP2_WORKAROUND_FIX_CAPACITY)
	", override internal blacklist = " __stringify(SBP2_WORKAROUND_OVERRIDE)
	", or a combination)");

/* I don't know why the SCSI stack doesn't define something like this... */
typedef void (*scsi_done_fn_t)(struct scsi_cmnd *);

static const char sbp2_driver_name[] = "sbp2";

/*
 * We create one struct sbp2_logical_unit per SBP-2 Logical Unit Number Entry
 * and one struct scsi_device per sbp2_logical_unit.
 */
struct sbp2_logical_unit {
	struct sbp2_target *tgt;
	struct list_head link;
	struct scsi_device *sdev;
	struct fw_address_handler address_handler;
	struct list_head orb_list;

	u64 command_block_agent_address;
	u16 lun;
	int login_id;

	/*
	 * The generation is updated once we've logged in or reconnected
	 * to the logical unit.  Thus, I/O to the device will automatically
	 * fail and get retried if it happens in a window where the device
	 * is not ready, e.g. after a bus reset but before we reconnect.
	 */
	int generation;
	int retries;
	struct delayed_work work;
};

/*
 * We create one struct sbp2_target per IEEE 1212 Unit Directory
 * and one struct Scsi_Host per sbp2_target.
 */
struct sbp2_target {
	struct kref kref;
	struct fw_unit *unit;

	u64 management_agent_address;
	int directory_id;
	int node_id;
	int address_high;

	unsigned workarounds;
	struct list_head lu_list;
};

#define SBP2_MAX_SG_ELEMENT_LENGTH	0xf000
#define SBP2_MAX_SECTORS		255	/* Max sectors supported */
#define SBP2_ORB_TIMEOUT		2000	/* Timeout in ms */

#define SBP2_ORB_NULL			0x80000000

#define SBP2_DIRECTION_TO_MEDIA		0x0
#define SBP2_DIRECTION_FROM_MEDIA	0x1

/* Unit directory keys */
#define SBP2_CSR_FIRMWARE_REVISION	0x3c
#define SBP2_CSR_LOGICAL_UNIT_NUMBER	0x14
#define SBP2_CSR_LOGICAL_UNIT_DIRECTORY	0xd4

/* Management orb opcodes */
#define SBP2_LOGIN_REQUEST		0x0
#define SBP2_QUERY_LOGINS_REQUEST	0x1
#define SBP2_RECONNECT_REQUEST		0x3
#define SBP2_SET_PASSWORD_REQUEST	0x4
#define SBP2_LOGOUT_REQUEST		0x7
#define SBP2_ABORT_TASK_REQUEST		0xb
#define SBP2_ABORT_TASK_SET		0xc
#define SBP2_LOGICAL_UNIT_RESET		0xe
#define SBP2_TARGET_RESET_REQUEST	0xf

/* Offsets for command block agent registers */
#define SBP2_AGENT_STATE		0x00
#define SBP2_AGENT_RESET		0x04
#define SBP2_ORB_POINTER		0x08
#define SBP2_DOORBELL			0x10
#define SBP2_UNSOLICITED_STATUS_ENABLE	0x14

/* Status write response codes */
#define SBP2_STATUS_REQUEST_COMPLETE	0x0
#define SBP2_STATUS_TRANSPORT_FAILURE	0x1
#define SBP2_STATUS_ILLEGAL_REQUEST	0x2
#define SBP2_STATUS_VENDOR_DEPENDENT	0x3

#define STATUS_GET_ORB_HIGH(v)		((v).status & 0xffff)
#define STATUS_GET_SBP_STATUS(v)	(((v).status >> 16) & 0xff)
#define STATUS_GET_LEN(v)		(((v).status >> 24) & 0x07)
#define STATUS_GET_DEAD(v)		(((v).status >> 27) & 0x01)
#define STATUS_GET_RESPONSE(v)		(((v).status >> 28) & 0x03)
#define STATUS_GET_SOURCE(v)		(((v).status >> 30) & 0x03)
#define STATUS_GET_ORB_LOW(v)		((v).orb_low)
#define STATUS_GET_DATA(v)		((v).data)

struct sbp2_status {
	u32 status;
	u32 orb_low;
	u8 data[24];
};

struct sbp2_pointer {
	u32 high;
	u32 low;
};

struct sbp2_orb {
	struct fw_transaction t;
	struct kref kref;
	dma_addr_t request_bus;
	int rcode;
	struct sbp2_pointer pointer;
	void (*callback)(struct sbp2_orb * orb, struct sbp2_status * status);
	struct list_head link;
};

#define MANAGEMENT_ORB_LUN(v)			((v))
#define MANAGEMENT_ORB_FUNCTION(v)		((v) << 16)
#define MANAGEMENT_ORB_RECONNECT(v)		((v) << 20)
#define MANAGEMENT_ORB_EXCLUSIVE(v)		((v) ? 1 << 28 : 0)
#define MANAGEMENT_ORB_REQUEST_FORMAT(v)	((v) << 29)
#define MANAGEMENT_ORB_NOTIFY			((1) << 31)

#define MANAGEMENT_ORB_RESPONSE_LENGTH(v)	((v))
#define MANAGEMENT_ORB_PASSWORD_LENGTH(v)	((v) << 16)

struct sbp2_management_orb {
	struct sbp2_orb base;
	struct {
		struct sbp2_pointer password;
		struct sbp2_pointer response;
		u32 misc;
		u32 length;
		struct sbp2_pointer status_fifo;
	} request;
	__be32 response[4];
	dma_addr_t response_bus;
	struct completion done;
	struct sbp2_status status;
};

#define LOGIN_RESPONSE_GET_LOGIN_ID(v)	((v).misc & 0xffff)
#define LOGIN_RESPONSE_GET_LENGTH(v)	(((v).misc >> 16) & 0xffff)

struct sbp2_login_response {
	u32 misc;
	struct sbp2_pointer command_block_agent;
	u32 reconnect_hold;
};
#define COMMAND_ORB_DATA_SIZE(v)	((v))
#define COMMAND_ORB_PAGE_SIZE(v)	((v) << 16)
#define COMMAND_ORB_PAGE_TABLE_PRESENT	((1) << 19)
#define COMMAND_ORB_MAX_PAYLOAD(v)	((v) << 20)
#define COMMAND_ORB_SPEED(v)		((v) << 24)
#define COMMAND_ORB_DIRECTION(v)	((v) << 27)
#define COMMAND_ORB_REQUEST_FORMAT(v)	((v) << 29)
#define COMMAND_ORB_NOTIFY		((1) << 31)

struct sbp2_command_orb {
	struct sbp2_orb base;
	struct {
		struct sbp2_pointer next;
		struct sbp2_pointer data_descriptor;
		u32 misc;
		u8 command_block[12];
	} request;
	struct scsi_cmnd *cmd;
	scsi_done_fn_t done;
	struct sbp2_logical_unit *lu;

	struct sbp2_pointer page_table[SG_ALL] __attribute__((aligned(8)));
	dma_addr_t page_table_bus;
};

/*
 * List of devices with known bugs.
 *
 * The firmware_revision field, masked with 0xffff00, is the best
 * indicator for the type of bridge chip of a device.  It yields a few
 * false positives but this did not break correctly behaving devices
 * so far.  We use ~0 as a wildcard, since the 24 bit values we get
 * from the config rom can never match that.
 */
static const struct {
	u32 firmware_revision;
	u32 model;
	unsigned workarounds;
} sbp2_workarounds_table[] = {
	/* DViCO Momobay CX-1 with TSB42AA9 bridge */ {
		.firmware_revision	= 0x002800,
		.model			= 0x001010,
		.workarounds		= SBP2_WORKAROUND_INQUIRY_36 |
					  SBP2_WORKAROUND_MODE_SENSE_8,
	},
	/* Initio bridges, actually only needed for some older ones */ {
		.firmware_revision	= 0x000200,
		.model			= ~0,
		.workarounds		= SBP2_WORKAROUND_INQUIRY_36,
	},
	/* Symbios bridge */ {
		.firmware_revision	= 0xa0b800,
		.model			= ~0,
		.workarounds		= SBP2_WORKAROUND_128K_MAX_TRANS,
	},

	/*
	 * There are iPods (2nd gen, 3rd gen) with model_id == 0, but
	 * these iPods do not feature the read_capacity bug according
	 * to one report.  Read_capacity behaviour as well as model_id
	 * could change due to Apple-supplied firmware updates though.
	 */

	/* iPod 4th generation. */ {
		.firmware_revision	= 0x0a2700,
		.model			= 0x000021,
		.workarounds		= SBP2_WORKAROUND_FIX_CAPACITY,
	},
	/* iPod mini */ {
		.firmware_revision	= 0x0a2700,
		.model			= 0x000023,
		.workarounds		= SBP2_WORKAROUND_FIX_CAPACITY,
	},
	/* iPod Photo */ {
		.firmware_revision	= 0x0a2700,
		.model			= 0x00007e,
		.workarounds		= SBP2_WORKAROUND_FIX_CAPACITY,
	}
};

static void
free_orb(struct kref *kref)
{
	struct sbp2_orb *orb = container_of(kref, struct sbp2_orb, kref);

	kfree(orb);
}

static void
sbp2_status_write(struct fw_card *card, struct fw_request *request,
		  int tcode, int destination, int source,
		  int generation, int speed,
		  unsigned long long offset,
		  void *payload, size_t length, void *callback_data)
{
	struct sbp2_logical_unit *lu = callback_data;
	struct sbp2_orb *orb;
	struct sbp2_status status;
	size_t header_size;
	unsigned long flags;

	if (tcode != TCODE_WRITE_BLOCK_REQUEST ||
	    length == 0 || length > sizeof(status)) {
		fw_send_response(card, request, RCODE_TYPE_ERROR);
		return;
	}

	header_size = min(length, 2 * sizeof(u32));
	fw_memcpy_from_be32(&status, payload, header_size);
	if (length > header_size)
		memcpy(status.data, payload + 8, length - header_size);
	if (STATUS_GET_SOURCE(status) == 2 || STATUS_GET_SOURCE(status) == 3) {
		fw_notify("non-orb related status write, not handled\n");
		fw_send_response(card, request, RCODE_COMPLETE);
		return;
	}

	/* Lookup the orb corresponding to this status write. */
	spin_lock_irqsave(&card->lock, flags);
	list_for_each_entry(orb, &lu->orb_list, link) {
		if (STATUS_GET_ORB_HIGH(status) == 0 &&
		    STATUS_GET_ORB_LOW(status) == orb->request_bus) {
			orb->rcode = RCODE_COMPLETE;
			list_del(&orb->link);
			break;
		}
	}
	spin_unlock_irqrestore(&card->lock, flags);

	if (&orb->link != &lu->orb_list)
		orb->callback(orb, &status);
	else
		fw_error("status write for unknown orb\n");

	kref_put(&orb->kref, free_orb);

	fw_send_response(card, request, RCODE_COMPLETE);
}

static void
complete_transaction(struct fw_card *card, int rcode,
		     void *payload, size_t length, void *data)
{
	struct sbp2_orb *orb = data;
	unsigned long flags;

	/*
	 * This is a little tricky.  We can get the status write for
	 * the orb before we get this callback.  The status write
	 * handler above will assume the orb pointer transaction was
	 * successful and set the rcode to RCODE_COMPLETE for the orb.
	 * So this callback only sets the rcode if it hasn't already
	 * been set and only does the cleanup if the transaction
	 * failed and we didn't already get a status write.
	 */
	spin_lock_irqsave(&card->lock, flags);

	if (orb->rcode == -1)
		orb->rcode = rcode;
	if (orb->rcode != RCODE_COMPLETE) {
		list_del(&orb->link);
		spin_unlock_irqrestore(&card->lock, flags);
		orb->callback(orb, NULL);
	} else {
		spin_unlock_irqrestore(&card->lock, flags);
	}

	kref_put(&orb->kref, free_orb);
}

static void
sbp2_send_orb(struct sbp2_orb *orb, struct sbp2_logical_unit *lu,
	      int node_id, int generation, u64 offset)
{
	struct fw_device *device = fw_device(lu->tgt->unit->device.parent);
	unsigned long flags;

	orb->pointer.high = 0;
	orb->pointer.low = orb->request_bus;
	fw_memcpy_to_be32(&orb->pointer, &orb->pointer, sizeof(orb->pointer));

	spin_lock_irqsave(&device->card->lock, flags);
	list_add_tail(&orb->link, &lu->orb_list);
	spin_unlock_irqrestore(&device->card->lock, flags);

	/* Take a ref for the orb list and for the transaction callback. */
	kref_get(&orb->kref);
	kref_get(&orb->kref);

	fw_send_request(device->card, &orb->t, TCODE_WRITE_BLOCK_REQUEST,
			node_id, generation, device->max_speed, offset,
			&orb->pointer, sizeof(orb->pointer),
			complete_transaction, orb);
}

static int sbp2_cancel_orbs(struct sbp2_logical_unit *lu)
{
	struct fw_device *device = fw_device(lu->tgt->unit->device.parent);
	struct sbp2_orb *orb, *next;
	struct list_head list;
	unsigned long flags;
	int retval = -ENOENT;

	INIT_LIST_HEAD(&list);
	spin_lock_irqsave(&device->card->lock, flags);
	list_splice_init(&lu->orb_list, &list);
	spin_unlock_irqrestore(&device->card->lock, flags);

	list_for_each_entry_safe(orb, next, &list, link) {
		retval = 0;
		if (fw_cancel_transaction(device->card, &orb->t) == 0)
			continue;

		orb->rcode = RCODE_CANCELLED;
		orb->callback(orb, NULL);
	}

	return retval;
}

static void
complete_management_orb(struct sbp2_orb *base_orb, struct sbp2_status *status)
{
	struct sbp2_management_orb *orb =
		container_of(base_orb, struct sbp2_management_orb, base);

	if (status)
		memcpy(&orb->status, status, sizeof(*status));
	complete(&orb->done);
}

static int
sbp2_send_management_orb(struct sbp2_logical_unit *lu, int node_id,
			 int generation, int function, int lun_or_login_id,
			 void *response)
{
	struct fw_device *device = fw_device(lu->tgt->unit->device.parent);
	struct sbp2_management_orb *orb;
	int retval = -ENOMEM;

	orb = kzalloc(sizeof(*orb), GFP_ATOMIC);
	if (orb == NULL)
		return -ENOMEM;

	kref_init(&orb->base.kref);
	orb->response_bus =
		dma_map_single(device->card->device, &orb->response,
			       sizeof(orb->response), DMA_FROM_DEVICE);
	if (dma_mapping_error(orb->response_bus))
		goto fail_mapping_response;

	orb->request.response.high    = 0;
	orb->request.response.low     = orb->response_bus;

	orb->request.misc =
		MANAGEMENT_ORB_NOTIFY |
		MANAGEMENT_ORB_FUNCTION(function) |
		MANAGEMENT_ORB_LUN(lun_or_login_id);
	orb->request.length =
		MANAGEMENT_ORB_RESPONSE_LENGTH(sizeof(orb->response));

	orb->request.status_fifo.high = lu->address_handler.offset >> 32;
	orb->request.status_fifo.low  = lu->address_handler.offset;

	if (function == SBP2_LOGIN_REQUEST) {
		orb->request.misc |=
			MANAGEMENT_ORB_EXCLUSIVE(sbp2_param_exclusive_login) |
			MANAGEMENT_ORB_RECONNECT(0);
	}

	fw_memcpy_to_be32(&orb->request, &orb->request, sizeof(orb->request));

	init_completion(&orb->done);
	orb->base.callback = complete_management_orb;

	orb->base.request_bus =
		dma_map_single(device->card->device, &orb->request,
			       sizeof(orb->request), DMA_TO_DEVICE);
	if (dma_mapping_error(orb->base.request_bus))
		goto fail_mapping_request;

	sbp2_send_orb(&orb->base, lu, node_id, generation,
		      lu->tgt->management_agent_address);

	wait_for_completion_timeout(&orb->done,
				    msecs_to_jiffies(SBP2_ORB_TIMEOUT));

	retval = -EIO;
	if (sbp2_cancel_orbs(lu) == 0) {
		fw_error("orb reply timed out, rcode=0x%02x\n",
			 orb->base.rcode);
		goto out;
	}

	if (orb->base.rcode != RCODE_COMPLETE) {
		fw_error("management write failed, rcode 0x%02x\n",
			 orb->base.rcode);
		goto out;
	}

	if (STATUS_GET_RESPONSE(orb->status) != 0 ||
	    STATUS_GET_SBP_STATUS(orb->status) != 0) {
		fw_error("error status: %d:%d\n",
			 STATUS_GET_RESPONSE(orb->status),
			 STATUS_GET_SBP_STATUS(orb->status));
		goto out;
	}

	retval = 0;
 out:
	dma_unmap_single(device->card->device, orb->base.request_bus,
			 sizeof(orb->request), DMA_TO_DEVICE);
 fail_mapping_request:
	dma_unmap_single(device->card->device, orb->response_bus,
			 sizeof(orb->response), DMA_FROM_DEVICE);
 fail_mapping_response:
	if (response)
		fw_memcpy_from_be32(response,
				    orb->response, sizeof(orb->response));
	kref_put(&orb->base.kref, free_orb);

	return retval;
}

static void
complete_agent_reset_write(struct fw_card *card, int rcode,
			   void *payload, size_t length, void *data)
{
	struct fw_transaction *t = data;

	kfree(t);
}

static int sbp2_agent_reset(struct sbp2_logical_unit *lu)
{
	struct fw_device *device = fw_device(lu->tgt->unit->device.parent);
	struct fw_transaction *t;
	static u32 zero;

	t = kzalloc(sizeof(*t), GFP_ATOMIC);
	if (t == NULL)
		return -ENOMEM;

	fw_send_request(device->card, t, TCODE_WRITE_QUADLET_REQUEST,
			lu->tgt->node_id, lu->generation, device->max_speed,
			lu->command_block_agent_address + SBP2_AGENT_RESET,
			&zero, sizeof(zero), complete_agent_reset_write, t);

	return 0;
}

static void sbp2_release_target(struct kref *kref)
{
	struct sbp2_target *tgt = container_of(kref, struct sbp2_target, kref);
	struct sbp2_logical_unit *lu, *next;
	struct Scsi_Host *shost =
		container_of((void *)tgt, struct Scsi_Host, hostdata[0]);

	list_for_each_entry_safe(lu, next, &tgt->lu_list, link) {
		if (lu->sdev)
			scsi_remove_device(lu->sdev);

		sbp2_send_management_orb(lu, tgt->node_id, lu->generation,
				SBP2_LOGOUT_REQUEST, lu->login_id, NULL);
		fw_core_remove_address_handler(&lu->address_handler);
		list_del(&lu->link);
		kfree(lu);
	}
	scsi_remove_host(shost);
	fw_notify("released %s\n", tgt->unit->device.bus_id);

	put_device(&tgt->unit->device);
	scsi_host_put(shost);
}

static void sbp2_reconnect(struct work_struct *work);

static void sbp2_login(struct work_struct *work)
{
	struct sbp2_logical_unit *lu =
		container_of(work, struct sbp2_logical_unit, work.work);
	struct Scsi_Host *shost =
		container_of((void *)lu->tgt, struct Scsi_Host, hostdata[0]);
	struct scsi_device *sdev;
	struct scsi_lun eight_bytes_lun;
	struct fw_unit *unit = lu->tgt->unit;
	struct fw_device *device = fw_device(unit->device.parent);
	struct sbp2_login_response response;
	int generation, node_id, local_node_id;

	generation    = device->card->generation;
	node_id       = device->node->node_id;
	local_node_id = device->card->local_node->node_id;

	if (sbp2_send_management_orb(lu, node_id, generation,
				SBP2_LOGIN_REQUEST, lu->lun, &response) < 0) {
		if (lu->retries++ < 5) {
			schedule_delayed_work(&lu->work, DIV_ROUND_UP(HZ, 5));
		} else {
			fw_error("failed to login to %s LUN %04x\n",
				 unit->device.bus_id, lu->lun);
			kref_put(&lu->tgt->kref, sbp2_release_target);
		}
		return;
	}

	lu->generation        = generation;
	lu->tgt->node_id      = node_id;
	lu->tgt->address_high = local_node_id << 16;

	/* Get command block agent offset and login id. */
	lu->command_block_agent_address =
		((u64) (response.command_block_agent.high & 0xffff) << 32) |
		response.command_block_agent.low;
	lu->login_id = LOGIN_RESPONSE_GET_LOGIN_ID(response);

	fw_notify("logged in to %s LUN %04x (%d retries)\n",
		  unit->device.bus_id, lu->lun, lu->retries);

#if 0
	/* FIXME: The linux1394 sbp2 does this last step. */
	sbp2_set_busy_timeout(scsi_id);
#endif

	PREPARE_DELAYED_WORK(&lu->work, sbp2_reconnect);
	sbp2_agent_reset(lu);

	memset(&eight_bytes_lun, 0, sizeof(eight_bytes_lun));
	eight_bytes_lun.scsi_lun[0] = (lu->lun >> 8) & 0xff;
	eight_bytes_lun.scsi_lun[1] = lu->lun & 0xff;

	sdev = __scsi_add_device(shost, 0, 0,
				 scsilun_to_int(&eight_bytes_lun), lu);
	if (IS_ERR(sdev)) {
		sbp2_send_management_orb(lu, node_id, generation,
				SBP2_LOGOUT_REQUEST, lu->login_id, NULL);
		/*
		 * Set this back to sbp2_login so we fall back and
		 * retry login on bus reset.
		 */
		PREPARE_DELAYED_WORK(&lu->work, sbp2_login);
	} else {
		lu->sdev = sdev;
		scsi_device_put(sdev);
	}
	kref_put(&lu->tgt->kref, sbp2_release_target);
}

static int sbp2_add_logical_unit(struct sbp2_target *tgt, int lun_entry)
{
	struct sbp2_logical_unit *lu;

	lu = kmalloc(sizeof(*lu), GFP_KERNEL);
	if (!lu)
		return -ENOMEM;

	lu->address_handler.length           = 0x100;
	lu->address_handler.address_callback = sbp2_status_write;
	lu->address_handler.callback_data    = lu;

	if (fw_core_add_address_handler(&lu->address_handler,
					&fw_high_memory_region) < 0) {
		kfree(lu);
		return -ENOMEM;
	}

	lu->tgt  = tgt;
	lu->sdev = NULL;
	lu->lun  = lun_entry & 0xffff;
	lu->retries = 0;
	INIT_LIST_HEAD(&lu->orb_list);
	INIT_DELAYED_WORK(&lu->work, sbp2_login);

	list_add_tail(&lu->link, &tgt->lu_list);
	return 0;
}

static int sbp2_scan_logical_unit_dir(struct sbp2_target *tgt, u32 *directory)
{
	struct fw_csr_iterator ci;
	int key, value;

	fw_csr_iterator_init(&ci, directory);
	while (fw_csr_iterator_next(&ci, &key, &value))
		if (key == SBP2_CSR_LOGICAL_UNIT_NUMBER &&
		    sbp2_add_logical_unit(tgt, value) < 0)
			return -ENOMEM;
	return 0;
}

static int sbp2_scan_unit_dir(struct sbp2_target *tgt, u32 *directory,
			      u32 *model, u32 *firmware_revision)
{
	struct fw_csr_iterator ci;
	int key, value;

	fw_csr_iterator_init(&ci, directory);
	while (fw_csr_iterator_next(&ci, &key, &value)) {
		switch (key) {

		case CSR_DEPENDENT_INFO | CSR_OFFSET:
			tgt->management_agent_address =
					CSR_REGISTER_BASE + 4 * value;
			break;

		case CSR_DIRECTORY_ID:
			tgt->directory_id = value;
			break;

		case CSR_MODEL:
			*model = value;
			break;

		case SBP2_CSR_FIRMWARE_REVISION:
			*firmware_revision = value;
			break;

		case SBP2_CSR_LOGICAL_UNIT_NUMBER:
			if (sbp2_add_logical_unit(tgt, value) < 0)
				return -ENOMEM;
			break;

		case SBP2_CSR_LOGICAL_UNIT_DIRECTORY:
			if (sbp2_scan_logical_unit_dir(tgt, ci.p + value) < 0)
				return -ENOMEM;
			break;
		}
	}
	return 0;
}

static void sbp2_init_workarounds(struct sbp2_target *tgt, u32 model,
				  u32 firmware_revision)
{
	int i;
	unsigned w = sbp2_param_workarounds;

	if (w)
		fw_notify("Please notify linux1394-devel@lists.sourceforge.net "
			  "if you need the workarounds parameter for %s\n",
			  tgt->unit->device.bus_id);

	if (w & SBP2_WORKAROUND_OVERRIDE)
		goto out;

	for (i = 0; i < ARRAY_SIZE(sbp2_workarounds_table); i++) {

		if (sbp2_workarounds_table[i].firmware_revision !=
		    (firmware_revision & 0xffffff00))
			continue;

		if (sbp2_workarounds_table[i].model != model &&
		    sbp2_workarounds_table[i].model != ~0)
			continue;

		w |= sbp2_workarounds_table[i].workarounds;
		break;
	}
 out:
	if (w)
		fw_notify("Workarounds for %s: 0x%x "
			  "(firmware_revision 0x%06x, model_id 0x%06x)\n",
			  tgt->unit->device.bus_id,
			  w, firmware_revision, model);
	tgt->workarounds = w;
}

static struct scsi_host_template scsi_driver_template;

static int sbp2_probe(struct device *dev)
{
	struct fw_unit *unit = fw_unit(dev);
	struct fw_device *device = fw_device(unit->device.parent);
	struct sbp2_target *tgt;
	struct sbp2_logical_unit *lu;
	struct Scsi_Host *shost;
	u32 model, firmware_revision;

	shost = scsi_host_alloc(&scsi_driver_template, sizeof(*tgt));
	if (shost == NULL)
		return -ENOMEM;

	tgt = (struct sbp2_target *)shost->hostdata;
	unit->device.driver_data = tgt;
	tgt->unit = unit;
	kref_init(&tgt->kref);
	INIT_LIST_HEAD(&tgt->lu_list);

	if (fw_device_enable_phys_dma(device) < 0)
		goto fail_shost_put;

	if (scsi_add_host(shost, &unit->device) < 0)
		goto fail_shost_put;

	/* Initialize to values that won't match anything in our table. */
	firmware_revision = 0xff000000;
	model = 0xff000000;

	/* implicit directory ID */
	tgt->directory_id = ((unit->directory - device->config_rom) * 4
			     + CSR_CONFIG_ROM) & 0xffffff;

	if (sbp2_scan_unit_dir(tgt, unit->directory, &model,
			       &firmware_revision) < 0)
		goto fail_tgt_put;

	sbp2_init_workarounds(tgt, model, firmware_revision);

	get_device(&unit->device);

	/*
	 * We schedule work to do the login so we can easily
	 * reschedule retries. Always get the ref before scheduling
	 * work.
	 */
	list_for_each_entry(lu, &tgt->lu_list, link)
		if (schedule_delayed_work(&lu->work, 0))
			kref_get(&tgt->kref);
	return 0;

 fail_tgt_put:
	kref_put(&tgt->kref, sbp2_release_target);
	return -ENOMEM;

 fail_shost_put:
	scsi_host_put(shost);
	return -ENOMEM;
}

static int sbp2_remove(struct device *dev)
{
	struct fw_unit *unit = fw_unit(dev);
	struct sbp2_target *tgt = unit->device.driver_data;

	kref_put(&tgt->kref, sbp2_release_target);
	return 0;
}

static void sbp2_reconnect(struct work_struct *work)
{
	struct sbp2_logical_unit *lu =
		container_of(work, struct sbp2_logical_unit, work.work);
	struct fw_unit *unit = lu->tgt->unit;
	struct fw_device *device = fw_device(unit->device.parent);
	int generation, node_id, local_node_id;

	generation    = device->card->generation;
	node_id       = device->node->node_id;
	local_node_id = device->card->local_node->node_id;

	if (sbp2_send_management_orb(lu, node_id, generation,
				     SBP2_RECONNECT_REQUEST,
				     lu->login_id, NULL) < 0) {
		if (lu->retries++ >= 5) {
			fw_error("failed to reconnect to %s\n",
				 unit->device.bus_id);
			/* Fall back and try to log in again. */
			lu->retries = 0;
			PREPARE_DELAYED_WORK(&lu->work, sbp2_login);
		}
		schedule_delayed_work(&lu->work, DIV_ROUND_UP(HZ, 5));
		return;
	}

	lu->generation        = generation;
	lu->tgt->node_id      = node_id;
	lu->tgt->address_high = local_node_id << 16;

	fw_notify("reconnected to %s LUN %04x (%d retries)\n",
		  unit->device.bus_id, lu->lun, lu->retries);

	sbp2_agent_reset(lu);
	sbp2_cancel_orbs(lu);

	kref_put(&lu->tgt->kref, sbp2_release_target);
}

static void sbp2_update(struct fw_unit *unit)
{
	struct sbp2_target *tgt = unit->device.driver_data;
	struct sbp2_logical_unit *lu;

	fw_device_enable_phys_dma(fw_device(unit->device.parent));

	/*
	 * Fw-core serializes sbp2_update() against sbp2_remove().
	 * Iteration over tgt->lu_list is therefore safe here.
	 */
	list_for_each_entry(lu, &tgt->lu_list, link) {
		lu->retries = 0;
		if (schedule_delayed_work(&lu->work, 0))
			kref_get(&tgt->kref);
	}
}

#define SBP2_UNIT_SPEC_ID_ENTRY	0x0000609e
#define SBP2_SW_VERSION_ENTRY	0x00010483

static const struct fw_device_id sbp2_id_table[] = {
	{
		.match_flags  = FW_MATCH_SPECIFIER_ID | FW_MATCH_VERSION,
		.specifier_id = SBP2_UNIT_SPEC_ID_ENTRY,
		.version      = SBP2_SW_VERSION_ENTRY,
	},
	{ }
};

static struct fw_driver sbp2_driver = {
	.driver   = {
		.owner  = THIS_MODULE,
		.name   = sbp2_driver_name,
		.bus    = &fw_bus_type,
		.probe  = sbp2_probe,
		.remove = sbp2_remove,
	},
	.update   = sbp2_update,
	.id_table = sbp2_id_table,
};

static unsigned int
sbp2_status_to_sense_data(u8 *sbp2_status, u8 *sense_data)
{
	int sam_status;

	sense_data[0] = 0x70;
	sense_data[1] = 0x0;
	sense_data[2] = sbp2_status[1];
	sense_data[3] = sbp2_status[4];
	sense_data[4] = sbp2_status[5];
	sense_data[5] = sbp2_status[6];
	sense_data[6] = sbp2_status[7];
	sense_data[7] = 10;
	sense_data[8] = sbp2_status[8];
	sense_data[9] = sbp2_status[9];
	sense_data[10] = sbp2_status[10];
	sense_data[11] = sbp2_status[11];
	sense_data[12] = sbp2_status[2];
	sense_data[13] = sbp2_status[3];
	sense_data[14] = sbp2_status[12];
	sense_data[15] = sbp2_status[13];

	sam_status = sbp2_status[0] & 0x3f;

	switch (sam_status) {
	case SAM_STAT_GOOD:
	case SAM_STAT_CHECK_CONDITION:
	case SAM_STAT_CONDITION_MET:
	case SAM_STAT_BUSY:
	case SAM_STAT_RESERVATION_CONFLICT:
	case SAM_STAT_COMMAND_TERMINATED:
		return DID_OK << 16 | sam_status;

	default:
		return DID_ERROR << 16;
	}
}

static void
complete_command_orb(struct sbp2_orb *base_orb, struct sbp2_status *status)
{
	struct sbp2_command_orb *orb =
		container_of(base_orb, struct sbp2_command_orb, base);
	struct fw_device *device = fw_device(orb->lu->tgt->unit->device.parent);
	int result;

	if (status != NULL) {
		if (STATUS_GET_DEAD(*status))
			sbp2_agent_reset(orb->lu);

		switch (STATUS_GET_RESPONSE(*status)) {
		case SBP2_STATUS_REQUEST_COMPLETE:
			result = DID_OK << 16;
			break;
		case SBP2_STATUS_TRANSPORT_FAILURE:
			result = DID_BUS_BUSY << 16;
			break;
		case SBP2_STATUS_ILLEGAL_REQUEST:
		case SBP2_STATUS_VENDOR_DEPENDENT:
		default:
			result = DID_ERROR << 16;
			break;
		}

		if (result == DID_OK << 16 && STATUS_GET_LEN(*status) > 1)
			result = sbp2_status_to_sense_data(STATUS_GET_DATA(*status),
							   orb->cmd->sense_buffer);
	} else {
		/*
		 * If the orb completes with status == NULL, something
		 * went wrong, typically a bus reset happened mid-orb
		 * or when sending the write (less likely).
		 */
		result = DID_BUS_BUSY << 16;
	}

	dma_unmap_single(device->card->device, orb->base.request_bus,
			 sizeof(orb->request), DMA_TO_DEVICE);

	if (scsi_sg_count(orb->cmd) > 0)
		dma_unmap_sg(device->card->device, scsi_sglist(orb->cmd),
			     scsi_sg_count(orb->cmd),
			     orb->cmd->sc_data_direction);

	if (orb->page_table_bus != 0)
		dma_unmap_single(device->card->device, orb->page_table_bus,
				 sizeof(orb->page_table), DMA_TO_DEVICE);

	orb->cmd->result = result;
	orb->done(orb->cmd);
}

static int
sbp2_map_scatterlist(struct sbp2_command_orb *orb, struct fw_device *device,
		     struct sbp2_logical_unit *lu)
{
	struct scatterlist *sg;
	int sg_len, l, i, j, count;
	dma_addr_t sg_addr;

	sg = scsi_sglist(orb->cmd);
	count = dma_map_sg(device->card->device, sg, scsi_sg_count(orb->cmd),
			   orb->cmd->sc_data_direction);
	if (count == 0)
		goto fail;

	/*
	 * Handle the special case where there is only one element in
	 * the scatter list by converting it to an immediate block
	 * request. This is also a workaround for broken devices such
	 * as the second generation iPod which doesn't support page
	 * tables.
	 */
	if (count == 1 && sg_dma_len(sg) < SBP2_MAX_SG_ELEMENT_LENGTH) {
		orb->request.data_descriptor.high = lu->tgt->address_high;
		orb->request.data_descriptor.low  = sg_dma_address(sg);
		orb->request.misc |= COMMAND_ORB_DATA_SIZE(sg_dma_len(sg));
		return 0;
	}

	/*
	 * Convert the scatterlist to an sbp2 page table.  If any
	 * scatterlist entries are too big for sbp2, we split them as we
	 * go.  Even if we ask the block I/O layer to not give us sg
	 * elements larger than 65535 bytes, some IOMMUs may merge sg elements
	 * during DMA mapping, and Linux currently doesn't prevent this.
	 */
	for (i = 0, j = 0; i < count; i++) {
		sg_len = sg_dma_len(sg + i);
		sg_addr = sg_dma_address(sg + i);
		while (sg_len) {
			/* FIXME: This won't get us out of the pinch. */
			if (unlikely(j >= ARRAY_SIZE(orb->page_table))) {
				fw_error("page table overflow\n");
				goto fail_page_table;
			}
			l = min(sg_len, SBP2_MAX_SG_ELEMENT_LENGTH);
			orb->page_table[j].low = sg_addr;
			orb->page_table[j].high = (l << 16);
			sg_addr += l;
			sg_len -= l;
			j++;
		}
	}

	fw_memcpy_to_be32(orb->page_table, orb->page_table,
			  sizeof(orb->page_table[0]) * j);
	orb->page_table_bus =
		dma_map_single(device->card->device, orb->page_table,
			       sizeof(orb->page_table), DMA_TO_DEVICE);
	if (dma_mapping_error(orb->page_table_bus))
		goto fail_page_table;

	/*
	 * The data_descriptor pointer is the one case where we need
	 * to fill in the node ID part of the address.  All other
	 * pointers assume that the data referenced reside on the
	 * initiator (i.e. us), but data_descriptor can refer to data
	 * on other nodes so we need to put our ID in descriptor.high.
	 */
	orb->request.data_descriptor.high = lu->tgt->address_high;
	orb->request.data_descriptor.low  = orb->page_table_bus;
	orb->request.misc |=
		COMMAND_ORB_PAGE_TABLE_PRESENT |
		COMMAND_ORB_DATA_SIZE(j);

	return 0;

 fail_page_table:
	dma_unmap_sg(device->card->device, sg, scsi_sg_count(orb->cmd),
		     orb->cmd->sc_data_direction);
 fail:
	return -ENOMEM;
}

/* SCSI stack integration */

static int sbp2_scsi_queuecommand(struct scsi_cmnd *cmd, scsi_done_fn_t done)
{
	struct sbp2_logical_unit *lu = cmd->device->hostdata;
	struct fw_device *device = fw_device(lu->tgt->unit->device.parent);
	struct sbp2_command_orb *orb;
	unsigned max_payload;
	int retval = SCSI_MLQUEUE_HOST_BUSY;

	/*
	 * Bidirectional commands are not yet implemented, and unknown
	 * transfer direction not handled.
	 */
	if (cmd->sc_data_direction == DMA_BIDIRECTIONAL) {
		fw_error("Can't handle DMA_BIDIRECTIONAL, rejecting command\n");
		cmd->result = DID_ERROR << 16;
		done(cmd);
		return 0;
	}

	orb = kzalloc(sizeof(*orb), GFP_ATOMIC);
	if (orb == NULL) {
		fw_notify("failed to alloc orb\n");
		return SCSI_MLQUEUE_HOST_BUSY;
	}

	/* Initialize rcode to something not RCODE_COMPLETE. */
	orb->base.rcode = -1;
	kref_init(&orb->base.kref);

	orb->lu   = lu;
	orb->done = done;
	orb->cmd  = cmd;

	orb->request.next.high   = SBP2_ORB_NULL;
	orb->request.next.low    = 0x0;
	/*
	 * At speed 100 we can do 512 bytes per packet, at speed 200,
	 * 1024 bytes per packet etc.  The SBP-2 max_payload field
	 * specifies the max payload size as 2 ^ (max_payload + 2), so
	 * if we set this to max_speed + 7, we get the right value.
	 */
	max_payload = min(device->max_speed + 7,
			  device->card->max_receive - 1);
	orb->request.misc =
		COMMAND_ORB_MAX_PAYLOAD(max_payload) |
		COMMAND_ORB_SPEED(device->max_speed) |
		COMMAND_ORB_NOTIFY;

	if (cmd->sc_data_direction == DMA_FROM_DEVICE)
		orb->request.misc |=
			COMMAND_ORB_DIRECTION(SBP2_DIRECTION_FROM_MEDIA);
	else if (cmd->sc_data_direction == DMA_TO_DEVICE)
		orb->request.misc |=
			COMMAND_ORB_DIRECTION(SBP2_DIRECTION_TO_MEDIA);

	if (scsi_sg_count(cmd) && sbp2_map_scatterlist(orb, device, lu) < 0)
		goto out;

	fw_memcpy_to_be32(&orb->request, &orb->request, sizeof(orb->request));

	memset(orb->request.command_block,
	       0, sizeof(orb->request.command_block));
	memcpy(orb->request.command_block, cmd->cmnd, COMMAND_SIZE(*cmd->cmnd));

	orb->base.callback = complete_command_orb;
	orb->base.request_bus =
		dma_map_single(device->card->device, &orb->request,
			       sizeof(orb->request), DMA_TO_DEVICE);
	if (dma_mapping_error(orb->base.request_bus))
		goto out;

	sbp2_send_orb(&orb->base, lu, lu->tgt->node_id, lu->generation,
		      lu->command_block_agent_address + SBP2_ORB_POINTER);
	retval = 0;
 out:
	kref_put(&orb->base.kref, free_orb);
	return retval;
}

static int sbp2_scsi_slave_alloc(struct scsi_device *sdev)
{
	struct sbp2_logical_unit *lu = sdev->hostdata;

	sdev->allow_restart = 1;

	if (lu->tgt->workarounds & SBP2_WORKAROUND_INQUIRY_36)
		sdev->inquiry_len = 36;

	return 0;
}

static int sbp2_scsi_slave_configure(struct scsi_device *sdev)
{
	struct sbp2_logical_unit *lu = sdev->hostdata;

	sdev->use_10_for_rw = 1;

	if (sdev->type == TYPE_ROM)
		sdev->use_10_for_ms = 1;

	if (sdev->type == TYPE_DISK &&
	    lu->tgt->workarounds & SBP2_WORKAROUND_MODE_SENSE_8)
		sdev->skip_ms_page_8 = 1;

	if (lu->tgt->workarounds & SBP2_WORKAROUND_FIX_CAPACITY)
		sdev->fix_capacity = 1;

	if (lu->tgt->workarounds & SBP2_WORKAROUND_128K_MAX_TRANS)
		blk_queue_max_sectors(sdev->request_queue, 128 * 1024 / 512);

	return 0;
}

/*
 * Called by scsi stack when something has really gone wrong.  Usually
 * called when a command has timed-out for some reason.
 */
static int sbp2_scsi_abort(struct scsi_cmnd *cmd)
{
	struct sbp2_logical_unit *lu = cmd->device->hostdata;

	fw_notify("sbp2_scsi_abort\n");
	sbp2_agent_reset(lu);
	sbp2_cancel_orbs(lu);

	return SUCCESS;
}

/*
 * Format of /sys/bus/scsi/devices/.../ieee1394_id:
 * u64 EUI-64 : u24 directory_ID : u16 LUN  (all printed in hexadecimal)
 *
 * This is the concatenation of target port identifier and logical unit
 * identifier as per SAM-2...SAM-4 annex A.
 */
static ssize_t
sbp2_sysfs_ieee1394_id_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct scsi_device *sdev = to_scsi_device(dev);
	struct sbp2_logical_unit *lu;
	struct fw_device *device;

	if (!sdev)
		return 0;

	lu = sdev->hostdata;
	device = fw_device(lu->tgt->unit->device.parent);

	return sprintf(buf, "%08x%08x:%06x:%04x\n",
			device->config_rom[3], device->config_rom[4],
			lu->tgt->directory_id, lu->lun);
}

static DEVICE_ATTR(ieee1394_id, S_IRUGO, sbp2_sysfs_ieee1394_id_show, NULL);

static struct device_attribute *sbp2_scsi_sysfs_attrs[] = {
	&dev_attr_ieee1394_id,
	NULL
};

static struct scsi_host_template scsi_driver_template = {
	.module			= THIS_MODULE,
	.name			= "SBP-2 IEEE-1394",
	.proc_name		= sbp2_driver_name,
	.queuecommand		= sbp2_scsi_queuecommand,
	.slave_alloc		= sbp2_scsi_slave_alloc,
	.slave_configure	= sbp2_scsi_slave_configure,
	.eh_abort_handler	= sbp2_scsi_abort,
	.this_id		= -1,
	.sg_tablesize		= SG_ALL,
	.use_clustering		= ENABLE_CLUSTERING,
	.cmd_per_lun		= 1,
	.can_queue		= 1,
	.sdev_attrs		= sbp2_scsi_sysfs_attrs,
};

MODULE_AUTHOR("Kristian Hoegsberg <krh@bitplanet.net>");
MODULE_DESCRIPTION("SCSI over IEEE1394");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(ieee1394, sbp2_id_table);

/* Provide a module alias so root-on-sbp2 initrds don't break. */
#ifndef CONFIG_IEEE1394_SBP2_MODULE
MODULE_ALIAS("sbp2");
#endif

static int __init sbp2_init(void)
{
	return driver_register(&sbp2_driver.driver);
}

static void __exit sbp2_cleanup(void)
{
	driver_unregister(&sbp2_driver.driver);
}

module_init(sbp2_init);
module_exit(sbp2_cleanup);
