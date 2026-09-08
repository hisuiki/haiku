/*
 * Copyright 2007 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * Copyright 2008 Mika Lindqvist, monni1995_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#include "h2upper.h"

#include <util/AutoLock.h>
#include <string.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/HCI/btHCI_transport.h>
#include <kernel.h>

#include "h2debug.h"
#include "h2generic.h"
#include "h2transactions.h"
#include "h2util.h"
#include "snet_buffer.h"


static const uint32 kMaxQueuedAclPackets = 64;


static void
submit_next_acl(bt_usb_dev* bdev)
{
	for (;;) {
		net_buffer* buffer;
		{
			MutexLocker locker(&bdev->aclTxLock);
			if (bdev->aclTxClosing || bdev->aclTxPending
				|| list_is_empty(&bdev->nbuffersTx[BT_ACL]))
				return;

			buffer = (net_buffer*)list_remove_head_item(
				&bdev->nbuffersTx[BT_ACL]);
			bdev->aclTxQueued--;
			bdev->aclTxPending = true;
		}

		if (submit_tx_acl(bdev, buffer) == B_OK)
			return;

		// A failed USB submission has no completion callback. Release its
		// buffer here, clear the in-flight slot, and continue with the next
		// packet so a transient failure cannot stall the entire ACL queue.
		nb_destroy(buffer);
		MutexLocker locker(&bdev->aclTxLock);
		bdev->aclTxPending = false;
	}
}


status_t
queue_acl_packet(hci_id hid, net_buffer* buffer)
{
	bt_usb_dev* bdev = fetch_device(NULL, hid);
	if (bdev == NULL) {
		nb_destroy(buffer);
		return B_ERROR;
	}

	{
		MutexLocker locker(&bdev->aclTxLock);
		if (bdev->aclTxClosing || (bdev->state & RUNNING) == 0) {
			nb_destroy(buffer);
			return B_OK;
		}

		// Audio is real-time.  If a consumer outruns the controller, discard
		// the oldest queued packet instead of accumulating audible latency.
		if (bdev->aclTxQueued == kMaxQueuedAclPackets) {
			net_buffer* oldest = (net_buffer*)list_remove_head_item(
				&bdev->nbuffersTx[BT_ACL]);
			nb_destroy(oldest);
			bdev->aclTxQueued--;
			bdev->stat.rejectedTX++;
		}
		list_add_item(&bdev->nbuffersTx[BT_ACL], buffer);
		bdev->aclTxQueued++;
	}

	submit_next_acl(bdev);
	return B_OK;
}


void
acl_packet_complete(bt_usb_dev* bdev)
{
	{
		MutexLocker locker(&bdev->aclTxLock);
		bdev->aclTxPending = false;
		if (bdev->aclTxClosing)
			return;
	}
	submit_next_acl(bdev);
}


// TODO: split for commands and comunication (ACL & SCO)
void
sched_tx_processing(bt_usb_dev* bdev)
{
	net_buffer* nbuf;
	snet_buffer* snbuf;
	status_t err;

	TRACE("%s: (%p)\n", __func__, bdev);

	if (!TEST_AND_SET(&bdev->state, PROCESSING)) {
		// We are not processing in another thread so... START!!

		do {
			/* Do while this bit is on... so someone should set it before we
			 * stop the iterations
			 */
			bdev->state &= ~SENDING;
			// check Commands
	#ifdef EMPTY_COMMAND_QUEUE
			while (!list_is_empty(&bdev->nbuffersTx[BT_COMMAND])) {
	#else
			if (!list_is_empty(&bdev->nbuffersTx[BT_COMMAND])) {
	#endif
				snbuf = (snet_buffer*)
					list_remove_head_item(&bdev->nbuffersTx[BT_COMMAND]);
				err = submit_tx_command(bdev, snbuf);
				if (err != B_OK) {
					// re-head it
					list_insert_item_before(&bdev->nbuffersTx[BT_COMMAND],
						list_get_first_item(&bdev->nbuffersTx[BT_COMMAND]),
						snbuf);
				}
			}

#ifdef EMPTY_SCO_QUEUE
			while (!list_is_empty(&bdev->nbuffersTx[BT_SCO])) {
#else
			if (!list_is_empty(&bdev->nbuffersTx[BT_SCO])) {
#endif
				nbuf = (net_buffer*)list_remove_head_item(&bdev->nbuffersTx[BT_SCO]);
				err = submit_tx_sco(bdev, nbuf);
				if (err != B_OK) {
					// re-head it
					list_insert_item_before(&bdev->nbuffersTx[BT_SCO],
						list_get_first_item(&bdev->nbuffersTx[BT_SCO]), nbuf);
				}
			}

			// check ACl
#ifdef EMPTY_ACL_QUEUE
			while (!list_is_empty(&bdev->nbuffersTx[BT_ACL])) {
#else
			if (!list_is_empty(&bdev->nbuffersTx[BT_ACL])) {
#endif
				nbuf = (net_buffer*)
					list_remove_head_item(&bdev->nbuffersTx[BT_ACL]);
				err = submit_tx_acl(bdev, nbuf);
				if (err != B_OK) {
					// re-head it
					list_insert_item_before(&bdev->nbuffersTx[BT_ACL],
							list_get_first_item(&bdev->nbuffersTx[BT_ACL]),
							nbuf);
				}
			}

		} while ((bdev->state & SENDING) != 0);

		bdev->state &= ~PROCESSING;

	} else {
		// We are processing so MARK that we need to still go on with that
		bdev->state |= SENDING;
	}
}


status_t
send_packet(hci_id hid, bt_packet_t type, net_buffer* nbuf)
{
	bt_usb_dev* bdev = fetch_device(NULL, hid);
	status_t err = B_OK;

	if (bdev == NULL)
		return B_ERROR;

	// TODO: check if device is actually ready for this
	// TODO: Lock Device

	if (nbuf != NULL) {
		if (type != nbuf->protocol) // a bit strict maybe
			panic("Upper layer has not filled correctly a packet");

		switch (type) {
			case BT_COMMAND:
			case BT_ACL:
			case BT_SCO:
				list_add_item(&bdev->nbuffersTx[type], nbuf);
				bdev->nbuffersPendingTx[type]++;
			break;
			default:
				ERROR("%s: Unknown packet type for sending %d\n", __func__,
					type);
				// TODO: free the net_buffer -> no, allow upper layer
				// handle it with the given error
				err = B_BAD_VALUE;
			break;
		}
	} else {
		TRACE("%s: tx sched provoked", __func__);
	}

	// TODO: check if device is actually ready for this
	// TODO: unlock device

	// sched in any case even if nbuf is null (provoke re-scheduling)
	sched_tx_processing(bdev);

	return err;
}


status_t
send_command(hci_id hid, snet_buffer* snbuf)
{
	bt_usb_dev* bdev = fetch_device(NULL, hid);
	status_t err = B_OK;

	if (bdev == NULL)
		return B_ERROR;

	// TODO: check if device is actually ready for this
	// TODO: mutex

	if (snbuf != NULL) {
		list_add_item(&bdev->nbuffersTx[BT_COMMAND], snbuf);
		bdev->nbuffersPendingTx[BT_COMMAND]++;
	} else {
		err = B_BAD_VALUE;
		TRACE("%s: tx sched provoked", __func__);
	}

	// TODO: check if device is actually ready for this
	// TODO: mutex

	/* sched in All cases even if nbuf is null (hidden way to provoke
	 * re-scheduling)
	 */
	sched_tx_processing(bdev);

	return err;
}
