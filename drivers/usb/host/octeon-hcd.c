/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2008-2012 Cavium, Inc.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/of.h>

#include <asm/time.h>
#include <asm/delay.h>

#include <asm/octeon/octeon.h>
#include <asm/octeon/cvmx-usb.h>
#include <asm/octeon/cvmx-iob-defs.h>


#undef OCTEON_HCD_DEBUG

#ifdef OCTEON_HCD_DEBUG

#define DEBUG_CALL()			pr_info("OcteonUSB: %s called\n", __func__)
#define DEBUG_SUBMIT(format, ...)       pr_info("OcteonUSB: %s " format, __func__, ##__VA_ARGS__)
#define DEBUG_ROOT_HUB(format, ...)	pr_info(format, ##__VA_ARGS__)
#define DEBUG_ERROR(format, ...)	pr_err("OcteonUSB: %s " format, __func__, ##__VA_ARGS__)

#else

#define DEBUG_CALL()			do {} while (0)
#define DEBUG_SUBMIT(format, ...)	do {} while (0)
#define DEBUG_ROOT_HUB(format, ...)	do {} while (0)
#define DEBUG_ERROR(format, ...)	do {} while (0)

#endif

#define DEBUG_FATAL(format, ...)        pr_crit(format, ##__VA_ARGS__)

struct octeon_hcd {
	spinlock_t lock;
	int usb_num;
	cvmx_usb_state_t usb;
	struct tasklet_struct dequeue_tasklet;
	struct list_head dequeue_list;
};

/* convert between an HCD pointer and the corresponding struct octeon_hcd */
static inline struct octeon_hcd *hcd_to_octeon(struct usb_hcd *hcd)
{
	return (struct octeon_hcd *)(hcd->hcd_priv);
}

static inline struct usb_hcd *octeon_to_hcd(struct octeon_hcd *p)
{
	return container_of((void *)p, struct usb_hcd, hcd_priv);
}

static inline struct octeon_hcd *cvmx_usb_to_octeon(cvmx_usb_state_t *p)
{
	return container_of(p, struct octeon_hcd, usb);
}

static irqreturn_t octeon_usb_irq(struct usb_hcd *hcd)
{
	struct octeon_hcd *priv = hcd_to_octeon(hcd);
	unsigned long flags;
	DEBUG_CALL();
	spin_lock_irqsave(&priv->lock, flags);
	cvmx_usb_poll(&priv->usb);
	spin_unlock_irqrestore(&priv->lock, flags);
	return IRQ_HANDLED;
}

static void octeon_usb_port_callback(cvmx_usb_state_t *usb,
				     cvmx_usb_callback_t reason,
				     cvmx_usb_complete_t status,
				     int pipe_handle,
				     int submit_handle,
				     int bytes_transferred,
				     void *user_data)
{
	struct octeon_hcd *priv = cvmx_usb_to_octeon(usb);
	DEBUG_CALL();
	spin_unlock(&priv->lock);
	usb_hcd_poll_rh_status(octeon_to_hcd(priv));
	spin_lock(&priv->lock);
}

static int octeon_usb_start(struct usb_hcd *hcd)
{
	struct octeon_hcd *priv = hcd_to_octeon(hcd);
	unsigned long flags;
	DEBUG_CALL();
	hcd->state = HC_STATE_RUNNING;
	spin_lock_irqsave(&priv->lock, flags);
	cvmx_usb_register_callback(&priv->usb, CVMX_USB_CALLBACK_PORT_CHANGED,
				   octeon_usb_port_callback, NULL);
	spin_unlock_irqrestore(&priv->lock, flags);
	return 0;
}

static void octeon_usb_stop(struct usb_hcd *hcd)
{
	struct octeon_hcd *priv = hcd_to_octeon(hcd);
	unsigned long flags;
	DEBUG_CALL();
	spin_lock_irqsave(&priv->lock, flags);
	cvmx_usb_register_callback(&priv->usb, CVMX_USB_CALLBACK_PORT_CHANGED,
				   NULL, NULL);
	spin_unlock_irqrestore(&priv->lock, flags);
	hcd->state = HC_STATE_HALT;
}

static int octeon_usb_get_frame_number(struct usb_hcd *hcd)
{
	struct octeon_hcd *priv = hcd_to_octeon(hcd);
	DEBUG_CALL();
	return cvmx_usb_get_frame_number(&priv->usb);
}

static void octeon_usb_urb_complete_callback(cvmx_usb_state_t *usb,
					     cvmx_usb_callback_t reason,
					     cvmx_usb_complete_t status,
					     int pipe_handle,
					     int submit_handle,
					     int bytes_transferred,
					     void *user_data)
{
	struct octeon_hcd *priv = cvmx_usb_to_octeon(usb);
	struct urb *urb = user_data;
	DEBUG_CALL();
	urb->actual_length = bytes_transferred;
	urb->hcpriv = NULL;

	if (!list_empty(&urb->urb_list)) {
		/*
		 * It is on the dequeue_list, but we are going to call
		 * usb_hcd_giveback_urb(), so we must clear it from
		 * the list.  We got to it before the
		 * octeon_usb_urb_dequeue_work() tasklet did.
		 */
		list_del(&urb->urb_list);
		/* No longer on the dequeue_list. */
		INIT_LIST_HEAD(&urb->urb_list);
	}

	/*
	 * For Isochronous transactions we need to update the URB
	 * packet status list from data in our private copy.
	 */
	if (usb_pipetype(urb->pipe) == PIPE_ISOCHRONOUS) {
		int i;
		/*
		 * The pointer to the private list is stored in the
		 * setup_packet field.
		 */
		cvmx_usb_iso_packet_t *iso_packet = (cvmx_usb_iso_packet_t *)urb->setup_packet;
		/* Recalculate the transfer size by adding up each packet */
		urb->actual_length = 0;
		for (i = 0; i < urb->number_of_packets; i++) {
			if (iso_packet[i].status == CVMX_USB_COMPLETE_SUCCESS) {
				urb->iso_frame_desc[i].status = 0;
				urb->iso_frame_desc[i].actual_length = iso_packet[i].length;
				urb->actual_length += urb->iso_frame_desc[i].actual_length;
			} else {
				DEBUG_ERROR("ISOCHRONOUS packet=%d of %d status=%d pipe=%d submit=%d size=%d\n",
					    i, urb->number_of_packets,
					iso_packet[i].status, pipe_handle,
					submit_handle, iso_packet[i].length);
				urb->iso_frame_desc[i].status = -EREMOTEIO;
			}
		}
		/* Free the private list now that we don't need it anymore */
		kfree(iso_packet);
		urb->setup_packet = NULL;
	}

	switch (status) {
	case CVMX_USB_COMPLETE_SUCCESS:
		urb->status = 0;
		break;
	case CVMX_USB_COMPLETE_CANCEL:
		if (urb->status == 0)
			urb->status = -ENOENT;
		break;
	case CVMX_USB_COMPLETE_STALL:
		DEBUG_ERROR("status=stall pipe=%d submit=%d size=%d\n", pipe_handle, submit_handle, bytes_transferred);
		urb->status = -EPIPE;
		break;
	case CVMX_USB_COMPLETE_BABBLEERR:
		DEBUG_ERROR("status=babble pipe=%d submit=%d size=%d\n", pipe_handle, submit_handle, bytes_transferred);
		urb->status = -EPIPE;
		break;
	case CVMX_USB_COMPLETE_SHORT:
		DEBUG_ERROR("status=short pipe=%d submit=%d size=%d\n", pipe_handle, submit_handle, bytes_transferred);
		urb->status = -EREMOTEIO;
		break;
	case CVMX_USB_COMPLETE_ERROR:
	case CVMX_USB_COMPLETE_XACTERR:
	case CVMX_USB_COMPLETE_DATATGLERR:
	case CVMX_USB_COMPLETE_FRAMEERR:
		DEBUG_ERROR("status=%d pipe=%d submit=%d size=%d\n", status, pipe_handle, submit_handle, bytes_transferred);
		urb->status = -EPROTO;
		break;
	}
	spin_unlock(&priv->lock);
	usb_hcd_giveback_urb(octeon_to_hcd(priv), urb, urb->status);
	spin_lock(&priv->lock);
}

static int octeon_usb_urb_enqueue(struct usb_hcd *hcd,
				  struct urb *urb,
				  gfp_t mem_flags)
{
	struct octeon_hcd *priv = hcd_to_octeon(hcd);
	int submit_handle = -1;
	int pipe_handle;
	unsigned long flags;
	cvmx_usb_iso_packet_t *iso_packet;
	struct usb_host_endpoint *ep = urb->ep;

	DEBUG_CALL();

	urb->status = 0;
	INIT_LIST_HEAD(&urb->urb_list); /* not enqueued on dequeue_list */
	spin_lock_irqsave(&priv->lock, flags);

	if (!ep->hcpriv) {
		cvmx_usb_transfer_t transfer_type;
		cvmx_usb_speed_t speed;
		int split_device = 0;
		int split_port = 0;
		switch (usb_pipetype(urb->pipe)) {
		case PIPE_ISOCHRONOUS:
			transfer_type = CVMX_USB_TRANSFER_ISOCHRONOUS;
			break;
		case PIPE_INTERRUPT:
			transfer_type = CVMX_USB_TRANSFER_INTERRUPT;
			break;
		case PIPE_CONTROL:
			transfer_type = CVMX_USB_TRANSFER_CONTROL;
			break;
		default:
			transfer_type = CVMX_USB_TRANSFER_BULK;
			break;
		}
		switch (urb->dev->speed) {
		case USB_SPEED_LOW:
			speed = CVMX_USB_SPEED_LOW;
			break;
		case USB_SPEED_FULL:
			speed = CVMX_USB_SPEED_FULL;
			break;
		default:
			speed = CVMX_USB_SPEED_HIGH;
			break;
		}
		/*
		 * For slow devices on high speed ports we need to
		 * find the hub that does the speed translation so we
		 * know where to send the split transactions
		 */
		if (speed != CVMX_USB_SPEED_HIGH) {
			/* Start at this device and work our way up the usb tree */
			struct usb_device *dev = urb->dev;
			while (dev->parent) {
				/* If our parent is high speed then he'll receive the splits */
				if (dev->parent->speed == USB_SPEED_HIGH) {
					split_device = dev->parent->devnum;
					split_port = dev->portnum;
					break;
				}
				/*
				 * Move up the tree one level. If we
				 * make it all the way up the tree,
				 * then the port must not be in high
				 * speed mode and we don't need a
				 * split.
				 */
				dev = dev->parent;
			}
		}
		pipe_handle = cvmx_usb_open_pipe(&priv->usb,
						0,
						usb_pipedevice(urb->pipe),
						usb_pipeendpoint(urb->pipe),
						speed,
						le16_to_cpu(ep->desc.wMaxPacketSize) & 0x7ff,
						transfer_type,
						usb_pipein(urb->pipe) ? CVMX_USB_DIRECTION_IN : CVMX_USB_DIRECTION_OUT,
						urb->interval,
						(le16_to_cpu(ep->desc.wMaxPacketSize)>>11) & 0x3,
						split_device,
						split_port);
		if (pipe_handle < 0) {
			spin_unlock_irqrestore(&priv->lock, flags);
			DEBUG_ERROR("failed to create pipe\n");
			return -ENOMEM;
		}
		ep->hcpriv = (void *)(0x10000L + pipe_handle);
	} else {
		pipe_handle = 0xffff & (long)ep->hcpriv;
	}

	switch (usb_pipetype(urb->pipe)) {
	case PIPE_ISOCHRONOUS:
		DEBUG_SUBMIT("submit isochronous to %d.%d\n", usb_pipedevice(urb->pipe), usb_pipeendpoint(urb->pipe));
		/*
		 * Allocate a structure to use for our private list of
		 * isochronous packets.
		 */
		iso_packet = kmalloc(urb->number_of_packets * sizeof(cvmx_usb_iso_packet_t), GFP_ATOMIC);
		if (iso_packet) {
			int i;
			/* Fill the list with the data from the URB */
			for (i = 0; i < urb->number_of_packets; i++) {
				iso_packet[i].offset = urb->iso_frame_desc[i].offset;
				iso_packet[i].length = urb->iso_frame_desc[i].length;
				iso_packet[i].status = CVMX_USB_COMPLETE_ERROR;
			}
			/*
			 * Store a pointer to the list in uthe URB
			 * setup_pakcet field.  We know this currently
			 * isn't being used and this saves us a bunch
			 * of logic.
			 */
			urb->setup_packet = (char *)iso_packet;
			submit_handle = cvmx_usb_submit_isochronous(&priv->usb, pipe_handle,
								urb->start_frame,
								0 /* flags */,
								urb->number_of_packets,
								iso_packet,
								urb->transfer_dma,
								urb->transfer_buffer_length,
								octeon_usb_urb_complete_callback,
								urb);
			/* If submit failed we need to free our private packet list */
			if (submit_handle < 0) {
				urb->setup_packet = NULL;
				kfree(iso_packet);
			}
		}
		break;
	case PIPE_INTERRUPT:
		DEBUG_SUBMIT("submit interrupt to %d.%d\n", usb_pipedevice(urb->pipe), usb_pipeendpoint(urb->pipe));
		submit_handle = cvmx_usb_submit_interrupt(&priv->usb, pipe_handle,
							  urb->transfer_dma,
							  urb->transfer_buffer_length,
							  octeon_usb_urb_complete_callback,
							  urb);
		break;
	case PIPE_CONTROL:
		DEBUG_SUBMIT("submit control to %d.%d\n", usb_pipedevice(urb->pipe), usb_pipeendpoint(urb->pipe));
		submit_handle = cvmx_usb_submit_control(&priv->usb, pipe_handle,
							urb->setup_dma,
							urb->transfer_dma,
							urb->transfer_buffer_length,
							octeon_usb_urb_complete_callback,
							urb);
		break;
	case PIPE_BULK:
		DEBUG_SUBMIT("submit bulk to %d.%d\n", usb_pipedevice(urb->pipe), usb_pipeendpoint(urb->pipe));
		submit_handle = cvmx_usb_submit_bulk(&priv->usb, pipe_handle,
						     urb->transfer_dma,
						     urb->transfer_buffer_length,
						     octeon_usb_urb_complete_callback,
						     urb);
		break;
	}
	if (submit_handle < 0) {
		spin_unlock_irqrestore(&priv->lock, flags);
		DEBUG_ERROR("failed to submit\n");
		return -ENOMEM;
	}
	urb->hcpriv = (void *)(long)(((submit_handle & 0xffff) << 16) | pipe_handle);
	spin_unlock_irqrestore(&priv->lock, flags);
	return 0;
}

static void octeon_usb_urb_dequeue_work(unsigned long arg)
{
	unsigned long flags;
	struct octeon_hcd *priv = (struct octeon_hcd *)arg;

	spin_lock_irqsave(&priv->lock, flags);

	while (!list_empty(&priv->dequeue_list)) {
		int pipe_handle;
		int submit_handle;
		struct urb *urb = container_of(priv->dequeue_list.next, struct urb, urb_list);
		list_del(&urb->urb_list);
		/* not enqueued on dequeue_list */
		INIT_LIST_HEAD(&urb->urb_list);
		pipe_handle = 0xffff & (long)urb->hcpriv;
		submit_handle = ((long)urb->hcpriv) >> 16;
		cvmx_usb_cancel(&priv->usb, pipe_handle, submit_handle);
	}

	spin_unlock_irqrestore(&priv->lock, flags);
}

static int octeon_usb_urb_dequeue(struct usb_hcd *hcd, struct urb *urb, int status)
{
	struct octeon_hcd *priv = hcd_to_octeon(hcd);
	unsigned long flags;

	DEBUG_CALL();

	if (!urb->dev)
		return -EINVAL;

	spin_lock_irqsave(&priv->lock, flags);

	urb->status = status;
	list_add_tail(&urb->urb_list, &priv->dequeue_list);

	spin_unlock_irqrestore(&priv->lock, flags);

	tasklet_schedule(&priv->dequeue_tasklet);

	return 0;
}

static void octeon_usb_endpoint_disable(struct usb_hcd *hcd, struct usb_host_endpoint *ep)
{
	DEBUG_CALL();
	if (ep->hcpriv) {
		struct octeon_hcd *priv = hcd_to_octeon(hcd);
		int pipe_handle = 0xffff & (long)ep->hcpriv;
		unsigned long flags;
		spin_lock_irqsave(&priv->lock, flags);
		cvmx_usb_cancel_all(&priv->usb, pipe_handle);
		if (cvmx_usb_close_pipe(&priv->usb, pipe_handle))
			DEBUG_ERROR("Closing pipe %d failed\n", pipe_handle);
		spin_unlock_irqrestore(&priv->lock, flags);
		ep->hcpriv = NULL;
	}
}

static int octeon_usb_hub_status_data(struct usb_hcd *hcd, char *buf)
{
	struct octeon_hcd *priv = hcd_to_octeon(hcd);
	cvmx_usb_port_status_t port_status;
	unsigned long flags;

	DEBUG_CALL();

	spin_lock_irqsave(&priv->lock, flags);
	port_status = cvmx_usb_get_status(&priv->usb);
	spin_unlock_irqrestore(&priv->lock, flags);
	buf[0] = 0;
	buf[0] = port_status.connect_change << 1;

	return buf[0] != 0;
}

static int octeon_usb_hub_control(struct usb_hcd *hcd, u16 typeReq,
				  u16 wValue, u16 wIndex, char *buf, u16 wLength)
{
	struct octeon_hcd *priv = hcd_to_octeon(hcd);
	cvmx_usb_port_status_t usb_port_status;
	int port_status;
	struct usb_hub_descriptor *desc;
	unsigned long flags;

	switch (typeReq) {
	case ClearHubFeature:
		DEBUG_ROOT_HUB("OcteonUSB: ClearHubFeature\n");
		switch (wValue) {
		case C_HUB_LOCAL_POWER:
		case C_HUB_OVER_CURRENT:
			/* Nothing required here */
			break;
		default:
			return -EINVAL;
		}
		break;
	case ClearPortFeature:
		DEBUG_ROOT_HUB("OcteonUSB: ClearPortFeature");
		if (wIndex != 1) {
			DEBUG_ROOT_HUB(" INVALID\n");
			return -EINVAL;
		}

		switch (wValue) {
		case USB_PORT_FEAT_ENABLE:
			DEBUG_ROOT_HUB(" ENABLE");
			spin_lock_irqsave(&priv->lock, flags);
			cvmx_usb_disable(&priv->usb);
			spin_unlock_irqrestore(&priv->lock, flags);
			break;
		case USB_PORT_FEAT_SUSPEND:
			DEBUG_ROOT_HUB(" SUSPEND");
			/* Not supported on Octeon */
			break;
		case USB_PORT_FEAT_POWER:
			DEBUG_ROOT_HUB(" POWER");
			/* Not supported on Octeon */
			break;
		case USB_PORT_FEAT_INDICATOR:
			DEBUG_ROOT_HUB(" INDICATOR");
			/* Port inidicator not supported */
			break;
		case USB_PORT_FEAT_C_CONNECTION:
			DEBUG_ROOT_HUB(" C_CONNECTION");
			/* Clears drivers internal connect status change flag */
			spin_lock_irqsave(&priv->lock, flags);
			cvmx_usb_set_status(&priv->usb, cvmx_usb_get_status(&priv->usb));
			spin_unlock_irqrestore(&priv->lock, flags);
			break;
		case USB_PORT_FEAT_C_RESET:
			DEBUG_ROOT_HUB(" C_RESET");
			/* Clears the driver's internal Port Reset Change flag */
			spin_lock_irqsave(&priv->lock, flags);
			cvmx_usb_set_status(&priv->usb, cvmx_usb_get_status(&priv->usb));
			spin_unlock_irqrestore(&priv->lock, flags);
			break;
		case USB_PORT_FEAT_C_ENABLE:
			DEBUG_ROOT_HUB(" C_ENABLE");
			/* Clears the driver's internal Port Enable/Disable Change flag */
			spin_lock_irqsave(&priv->lock, flags);
			cvmx_usb_set_status(&priv->usb, cvmx_usb_get_status(&priv->usb));
			spin_unlock_irqrestore(&priv->lock, flags);
			break;
		case USB_PORT_FEAT_C_SUSPEND:
			DEBUG_ROOT_HUB(" C_SUSPEND");
			/*
			 * Clears the driver's internal Port Suspend
			 * Change flag, which is set when resume
			 * signaling on the host port is complete.
			 */
			break;
		case USB_PORT_FEAT_C_OVER_CURRENT:
			DEBUG_ROOT_HUB(" C_OVER_CURRENT");
			/* Clears the driver's overcurrent Change flag */
			spin_lock_irqsave(&priv->lock, flags);
			cvmx_usb_set_status(&priv->usb, cvmx_usb_get_status(&priv->usb));
			spin_unlock_irqrestore(&priv->lock, flags);
			break;
		default:
			DEBUG_ROOT_HUB(" UNKNOWN\n");
			return -EINVAL;
		}
		DEBUG_ROOT_HUB("\n");
		break;
	case GetHubDescriptor:
		DEBUG_ROOT_HUB("OcteonUSB: GetHubDescriptor\n");
		desc = (struct usb_hub_descriptor *)buf;
		desc->bDescLength = 9;
		desc->bDescriptorType = 0x29;
		desc->bNbrPorts = 1;
		desc->wHubCharacteristics = 0x08;
		desc->bPwrOn2PwrGood = 1;
		desc->bHubContrCurrent = 0;
		desc->u.hs.DeviceRemovable[0] = 0;
		desc->u.hs.DeviceRemovable[1] = 0xff;
		break;
	case GetHubStatus:
		DEBUG_ROOT_HUB("OcteonUSB: GetHubStatus\n");
		*(__le32 *)buf = 0;
		break;
	case GetPortStatus:
		DEBUG_ROOT_HUB("OcteonUSB: GetPortStatus");
		if (wIndex != 1) {
			DEBUG_ROOT_HUB(" INVALID\n");
			return -EINVAL;
		}

		spin_lock_irqsave(&priv->lock, flags);
		usb_port_status = cvmx_usb_get_status(&priv->usb);
		spin_unlock_irqrestore(&priv->lock, flags);
		port_status = 0;

		if (usb_port_status.connect_change) {
			port_status |= (1 << USB_PORT_FEAT_C_CONNECTION);
			DEBUG_ROOT_HUB(" C_CONNECTION");
		}

		if (usb_port_status.port_enabled) {
			port_status |= (1 << USB_PORT_FEAT_C_ENABLE);
			DEBUG_ROOT_HUB(" C_ENABLE");
		}

		if (usb_port_status.connected) {
			port_status |= USB_PORT_STAT_CONNECTION;
			DEBUG_ROOT_HUB(" CONNECTION");
		}

		if (usb_port_status.port_enabled) {
			port_status |= USB_PORT_STAT_ENABLE;
			DEBUG_ROOT_HUB(" ENABLE");
		}

		if (usb_port_status.port_over_current) {
			port_status |= USB_PORT_STAT_OVERCURRENT;
			DEBUG_ROOT_HUB(" OVER_CURRENT");
		}

		if (usb_port_status.port_powered) {
			port_status |= USB_PORT_STAT_POWER;
			DEBUG_ROOT_HUB(" POWER");
		}

		if (usb_port_status.port_speed == CVMX_USB_SPEED_HIGH) {
			port_status |= USB_PORT_STAT_HIGH_SPEED;
			DEBUG_ROOT_HUB(" HIGHSPEED");
		} else if (usb_port_status.port_speed == CVMX_USB_SPEED_LOW) {
			port_status |= USB_PORT_STAT_LOW_SPEED;
			DEBUG_ROOT_HUB(" LOWSPEED");
		}

		*((__le32 *)buf) = cpu_to_le32(port_status);
		DEBUG_ROOT_HUB("\n");
		break;
	case SetHubFeature:
		DEBUG_ROOT_HUB("OcteonUSB: SetHubFeature\n");
		/* No HUB features supported */
		break;
	case SetPortFeature:
		DEBUG_ROOT_HUB("OcteonUSB: SetPortFeature");
		if (wIndex != 1) {
			DEBUG_ROOT_HUB(" INVALID\n");
			return -EINVAL;
		}

		switch (wValue) {
		case USB_PORT_FEAT_SUSPEND:
			DEBUG_ROOT_HUB(" SUSPEND\n");
			return -EINVAL;
		case USB_PORT_FEAT_POWER:
			DEBUG_ROOT_HUB(" POWER\n");
			return -EINVAL;
		case USB_PORT_FEAT_RESET:
			DEBUG_ROOT_HUB(" RESET\n");
			spin_lock_irqsave(&priv->lock, flags);
			cvmx_usb_disable(&priv->usb);
			if (cvmx_usb_enable(&priv->usb))
				DEBUG_ERROR("Failed to enable the port\n");
			spin_unlock_irqrestore(&priv->lock, flags);
			return 0;
		case USB_PORT_FEAT_INDICATOR:
			DEBUG_ROOT_HUB(" INDICATOR\n");
			/* Not supported */
			break;
		default:
			DEBUG_ROOT_HUB(" UNKNOWN\n");
			return -EINVAL;
		}
		break;
	default:
		DEBUG_ROOT_HUB("OcteonUSB: Unknown root hub request\n");
		return -EINVAL;
	}
	return 0;
}


static const struct hc_driver octeon_hc_driver = {
	.description =      "Octeon USB",
	.product_desc =     "Octeon Host Controller",
	.hcd_priv_size =    sizeof(struct octeon_hcd),
	.irq =              octeon_usb_irq,
	.flags =            HCD_MEMORY | HCD_USB2,
	.start =            octeon_usb_start,
	.stop =             octeon_usb_stop,
	.urb_enqueue =      octeon_usb_urb_enqueue,
	.urb_dequeue =      octeon_usb_urb_dequeue,
	.endpoint_disable = octeon_usb_endpoint_disable,
	.get_frame_number = octeon_usb_get_frame_number,
	.hub_status_data =  octeon_usb_hub_status_data,
	.hub_control =      octeon_usb_hub_control,
};


static int octeon_usb_probe(struct platform_device *pdev)
{
	int status;
	int initialize_flags;
	int usb_num;
	struct resource *res_mem;
	struct device_node *usbn_node;
	int irq = platform_get_irq(pdev, 0);
	struct device *dev = &pdev->dev;
	struct octeon_hcd *priv;
	struct usb_hcd *hcd;
	unsigned long flags;
	u32 clock_rate = 48000000;
	bool is_crystal_clock = false;
	const char *clock_type;
	int i;

	DEBUG_CALL();

	if (dev->of_node == NULL) {
		dev_err(dev, "Error: empty of_node\n");
		return -ENXIO;
	}
	usbn_node = dev->of_node->parent;

	i = of_property_read_u32(usbn_node,
				 "refclk-frequency", &clock_rate);
	if (i) {
		dev_err(dev, "No USBN \"refclk-frequency\"\n");
		return -ENXIO;
	}
	switch (clock_rate) {
	case 12000000:
		initialize_flags = CVMX_USB_INITIALIZE_FLAGS_CLOCK_12MHZ;
		break;
	case 24000000:
		initialize_flags = CVMX_USB_INITIALIZE_FLAGS_CLOCK_24MHZ;
		break;
	case 48000000:
		initialize_flags = CVMX_USB_INITIALIZE_FLAGS_CLOCK_48MHZ;
		break;
	default:
		dev_err(dev, "Illebal USBN \"refclk-frequency\" %u\n", clock_rate);
		return -ENXIO;

	}

	i = of_property_read_string(usbn_node,
				    "refclk-type", &clock_type);

	if (!i && strcmp("crystal", clock_type) == 0)
		is_crystal_clock = true;

	if (is_crystal_clock)
		initialize_flags |= CVMX_USB_INITIALIZE_FLAGS_CLOCK_XO_XI;
	else
		initialize_flags |= CVMX_USB_INITIALIZE_FLAGS_CLOCK_XO_GND;

	res_mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res_mem == NULL) {
		dev_err(dev, "found no memory resource\n");
		return -ENXIO;
	}
	usb_num = (res_mem->start >> 44) & 1;

	if (irq < 0) {
		/* Defective device tree, but we know how to fix it. */
		irq_hw_number_t hwirq = usb_num ? (1 << 6) + 17 : 56;
		irq = irq_create_mapping(NULL, hwirq);
	}
	/*
	 * Set the DMA mask to 64bits so we get buffers already
	 * translated for DMA
	 */
	dev->coherent_dma_mask = ~0;
	dev->dma_mask = &dev->coherent_dma_mask;

	/*
	 * Only cn52XX and cn56XX have DWC_OTG USB hardware and the
	 * IOB priority registers.  Under heavy network load USB
	 * hardware can be starved by the IOB causing a crash.  Give
	 * it a priority boost if it has been waiting more than 400
	 * cycles to avoid this situation.
	 *
	 * Testing indicates that a cnt_val of 8192 is not sufficient,
	 * but no failures are seen with 4096.  We choose a value of
	 * 400 to give a safety factor of 10.
	 */
	if (OCTEON_IS_MODEL(OCTEON_CN52XX) || OCTEON_IS_MODEL(OCTEON_CN56XX)) {
		union cvmx_iob_n2c_l2c_pri_cnt pri_cnt;

		pri_cnt.u64 = 0;
		pri_cnt.s.cnt_enb = 1;
		pri_cnt.s.cnt_val = 400;
		cvmx_write_csr(CVMX_IOB_N2C_L2C_PRI_CNT, pri_cnt.u64);
	}

	hcd = usb_create_hcd(&octeon_hc_driver, dev, dev_name(dev));
	if (!hcd) {
		DEBUG_FATAL("OcteonUSB: Failed to allocate memory for HCD\n");
		return -1;
	}
	hcd->uses_new_polling = 1;
	priv = (struct octeon_hcd *)hcd->hcd_priv;
	priv->usb_num = usb_num;

	spin_lock_init(&priv->lock);

	tasklet_init(&priv->dequeue_tasklet, octeon_usb_urb_dequeue_work, (unsigned long)priv);
	INIT_LIST_HEAD(&priv->dequeue_list);

#ifdef OCTEON_HCD_DEBUG
	initialize_flags |= CVMX_USB_INITIALIZE_FLAGS_DEBUG_INFO | CVMX_USB_INITIALIZE_FLAGS_DEBUG_CALLS;
	initialize_flags |= CVMX_USB_INITIALIZE_FLAGS_DEBUG_TRANSFERS;
	initialize_flags |= CVMX_USB_INITIALIZE_FLAGS_DEBUG_CALLBACKS;
#endif
	status = cvmx_usb_initialize(&priv->usb, usb_num, initialize_flags);
	if (status) {
		DEBUG_FATAL("OcteonUSB: USB initialization failed with %d\n", status);
		kfree(hcd);
		return -1;
	}

	/* This delay is needed for CN3010, but I don't know why... */
	mdelay(10);

	spin_lock_irqsave(&priv->lock, flags);
	cvmx_usb_poll(&priv->usb);
	spin_unlock_irqrestore(&priv->lock, flags);

	status = usb_add_hcd(hcd, irq, IRQF_DISABLED);
	if (status) {
		DEBUG_FATAL("OcteonUSB: USB add HCD failed with %d\n", status);
		kfree(hcd);
		return -1;
	}

	pr_info("OcteonUSB: Registered HCD for port %d on irq %d\n", usb_num, irq);

	octeon_error_tree_enable(CVMX_ERROR_GROUP_USB, usb_num);

	return 0;
}

static int octeon_usb_remove(struct platform_device *pdev)
{
	int status;
	struct device *dev = &pdev->dev;
	struct usb_hcd *hcd = dev_get_drvdata(dev);
	struct octeon_hcd *priv = hcd_to_octeon(hcd);
	unsigned long flags;

	DEBUG_CALL();

	octeon_error_tree_disable(CVMX_ERROR_GROUP_USB, priv->usb_num);

	usb_remove_hcd(hcd);
	tasklet_kill(&priv->dequeue_tasklet);
	spin_lock_irqsave(&priv->lock, flags);
	status = cvmx_usb_shutdown(&priv->usb);
	spin_unlock_irqrestore(&priv->lock, flags);
	if (status)
		DEBUG_FATAL("OcteonUSB: USB shutdown failed with %d\n", status);

	kfree(hcd);

	return 0;
}


static struct of_device_id octeon_usb_match[] = {
	{
		.compatible = "cavium,octeon-5750-usbc",
	},
	{},
};

static struct platform_driver octeon_usb_driver = {
	.driver = {
		.name       = "OcteonUSB",
		.owner		= THIS_MODULE,
		.of_match_table = octeon_usb_match,
	},
	.probe      = octeon_usb_probe,
	.remove     = octeon_usb_remove,
};

static int __init octeon_usb_driver_init(void)
{
	if (usb_disabled())
		return 0;

	return platform_driver_register(&octeon_usb_driver);
}
module_init(octeon_usb_driver_init);

static void __exit octeon_usb_driver_exit(void)
{
	if (usb_disabled())
		return;

	platform_driver_unregister(&octeon_usb_driver);
}
module_exit(octeon_usb_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cavium, Inc. <support@cavium.com>");
MODULE_DESCRIPTION("Cavium Inc. OCTEON USB Host driver.");
