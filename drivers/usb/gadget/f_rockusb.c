/*
 * (C) Copyright 2017
 *
 * Eddie Cai <eddie.cai.linux@gmail.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <config.h>
#include <common.h>
#include <errno.h>
#include <malloc.h>
#include <memalign.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/composite.h>
#include <linux/compiler.h>
#include <version.h>
#include <g_dnl.h>
#include <asm/arch/f_rockusb.h>

static inline struct f_rockusb *func_to_rockusb(struct usb_function *f)
{
	return container_of(f, struct f_rockusb, usb_function);
}

static struct usb_endpoint_descriptor fs_ep_in = {
	.bLength            = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    = USB_DT_ENDPOINT,
	.bEndpointAddress   = USB_DIR_IN,
	.bmAttributes       = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     = cpu_to_le16(64),
};

static struct usb_endpoint_descriptor fs_ep_out = {
	.bLength		= USB_DT_ENDPOINT_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,
	.bEndpointAddress	= USB_DIR_OUT,
	.bmAttributes		= USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize		= cpu_to_le16(64),
};

static struct usb_endpoint_descriptor hs_ep_in = {
	.bLength		= USB_DT_ENDPOINT_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,
	.bEndpointAddress	= USB_DIR_IN,
	.bmAttributes		= USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize		= cpu_to_le16(512),
};

static struct usb_endpoint_descriptor hs_ep_out = {
	.bLength		= USB_DT_ENDPOINT_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,
	.bEndpointAddress	= USB_DIR_OUT,
	.bmAttributes		= USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize		= cpu_to_le16(512),
};

static struct usb_interface_descriptor interface_desc = {
	.bLength		= USB_DT_INTERFACE_SIZE,
	.bDescriptorType	= USB_DT_INTERFACE,
	.bInterfaceNumber	= 0x00,
	.bAlternateSetting	= 0x00,
	.bNumEndpoints		= 0x02,
	.bInterfaceClass	= ROCKUSB_INTERFACE_CLASS,
	.bInterfaceSubClass	= ROCKUSB_INTERFACE_SUB_CLASS,
	.bInterfaceProtocol	= ROCKUSB_INTERFACE_PROTOCOL,
};

static struct usb_descriptor_header *rkusb_fs_function[] = {
	(struct usb_descriptor_header *)&interface_desc,
	(struct usb_descriptor_header *)&fs_ep_in,
	(struct usb_descriptor_header *)&fs_ep_out,
};

static struct usb_descriptor_header *rkusb_hs_function[] = {
	(struct usb_descriptor_header *)&interface_desc,
	(struct usb_descriptor_header *)&hs_ep_in,
	(struct usb_descriptor_header *)&hs_ep_out,
	NULL,
};

static const char rkusb_name[] = "Rockchip Rockusb";

static struct usb_string rkusb_string_defs[] = {
	[0].s = rkusb_name,
	{  }			/* end of list */
};

static struct usb_gadget_strings stringtab_rkusb = {
	.language	= 0x0409,	/* en-us */
	.strings	= rkusb_string_defs,
};

static struct usb_gadget_strings *rkusb_strings[] = {
	&stringtab_rkusb,
	NULL,
};

static struct f_rockusb *rockusb_func;
static void rx_handler_command(struct usb_ep *ep, struct usb_request *req);
static int rockusb_tx_write_csw(u32 tag, int residue, u8 status, int size);

struct f_rockusb *get_rkusb(void)
{
	struct f_rockusb *f_rkusb = rockusb_func;
	if (!f_rkusb) {
		f_rkusb = memalign(CONFIG_SYS_CACHELINE_SIZE, sizeof(*f_rkusb));
		if (!f_rkusb)
			return 0;

		rockusb_func = f_rkusb;
		memset(f_rkusb, 0, sizeof(*f_rkusb));
	}
	return f_rkusb;
}

static struct usb_endpoint_descriptor *rkusb_ep_desc(
struct usb_gadget *g,
struct usb_endpoint_descriptor *fs,
struct usb_endpoint_descriptor *hs)
{
	if (gadget_is_dualspeed(g) && g->speed == USB_SPEED_HIGH)
		return hs;
	return fs;
}

static void rockusb_complete(struct usb_ep *ep, struct usb_request *req)
{
	int status = req->status;
	if (!status)
		return;
	debug("status: %d ep '%s' trans: %d\n", status, ep->name, req->actual);
}

/* config the rockusb device*/
static int rockusb_bind(struct usb_configuration *c, struct usb_function *f)
{
	int id;
	struct usb_gadget *gadget = c->cdev->gadget;
	struct f_rockusb *f_rkusb = func_to_rockusb(f);
	const char *s;

	id = usb_interface_id(c, f);
	if (id < 0)
		return id;
	interface_desc.bInterfaceNumber = id;

	id = usb_string_id(c->cdev);
	if (id < 0)
		return id;

	rkusb_string_defs[0].id = id;
	interface_desc.iInterface = id;

	f_rkusb->in_ep = usb_ep_autoconfig(gadget, &fs_ep_in);
	if (!f_rkusb->in_ep)
		return -ENODEV;
	f_rkusb->in_ep->driver_data = c->cdev;

	f_rkusb->out_ep = usb_ep_autoconfig(gadget, &fs_ep_out);
	if (!f_rkusb->out_ep)
		return -ENODEV;
	f_rkusb->out_ep->driver_data = c->cdev;

	f->descriptors = rkusb_fs_function;

	if (gadget_is_dualspeed(gadget)) {
		hs_ep_in.bEndpointAddress = fs_ep_in.bEndpointAddress;
		hs_ep_out.bEndpointAddress = fs_ep_out.bEndpointAddress;
		f->hs_descriptors = rkusb_hs_function;
	}

	s = getenv("serial#");
	if (s)
		g_dnl_set_serialnumber((char *)s);

	return 0;
}

static void rockusb_unbind(struct usb_configuration *c, struct usb_function *f)
{
	/* clear the configuration*/
	memset(rockusb_func, 0, sizeof(*rockusb_func));
}

static void rockusb_disable(struct usb_function *f)
{
	struct f_rockusb *f_rkusb = func_to_rockusb(f);

	usb_ep_disable(f_rkusb->out_ep);
	usb_ep_disable(f_rkusb->in_ep);

	if (f_rkusb->out_req) {
		free(f_rkusb->out_req->buf);
		usb_ep_free_request(f_rkusb->out_ep, f_rkusb->out_req);
		f_rkusb->out_req = NULL;
	}
	if (f_rkusb->in_req) {
		free(f_rkusb->in_req->buf);
		usb_ep_free_request(f_rkusb->in_ep, f_rkusb->in_req);
		f_rkusb->in_req = NULL;
	}
}

static struct usb_request *rockusb_start_ep(struct usb_ep *ep)
{
	struct usb_request *req;

	req = usb_ep_alloc_request(ep, 0);
	if (!req)
		return NULL;

	req->length = EP_BUFFER_SIZE;
	req->buf = memalign(CONFIG_SYS_CACHELINE_SIZE, EP_BUFFER_SIZE);
	if (!req->buf) {
		usb_ep_free_request(ep, req);
		return NULL;
	}
	memset(req->buf, 0, req->length);

	return req;
}

static int rockusb_set_alt(struct usb_function *f,
			    unsigned interface, unsigned alt)
{
	int ret;
	struct usb_composite_dev *cdev = f->config->cdev;
	struct usb_gadget *gadget = cdev->gadget;
	struct f_rockusb *f_rkusb = func_to_rockusb(f);
	const struct usb_endpoint_descriptor *d;

	debug("%s: func: %s intf: %d alt: %d\n",
	      __func__, f->name, interface, alt);

	d = rkusb_ep_desc(gadget, &fs_ep_out, &hs_ep_out);
	ret = usb_ep_enable(f_rkusb->out_ep, d);
	if (ret) {
		printf("failed to enable out ep\n");
		return ret;
	}

	f_rkusb->out_req = rockusb_start_ep(f_rkusb->out_ep);
	if (!f_rkusb->out_req) {
		printf("failed to alloc out req\n");
		ret = -EINVAL;
		goto err;
	}
	f_rkusb->out_req->complete = rx_handler_command;

	d = rkusb_ep_desc(gadget, &fs_ep_in, &hs_ep_in);
	ret = usb_ep_enable(f_rkusb->in_ep, d);
	if (ret) {
		printf("failed to enable in ep\n");
		goto err;
	}

	f_rkusb->in_req = rockusb_start_ep(f_rkusb->in_ep);
	if (!f_rkusb->in_req) {
		printf("failed alloc req in\n");
		ret = -EINVAL;
		goto err;
	}
	f_rkusb->in_req->complete = rockusb_complete;

	ret = usb_ep_queue(f_rkusb->out_ep, f_rkusb->out_req, 0);
	if (ret)
		goto err;

	return 0;
err:
	rockusb_disable(f);
	return ret;
}

static int rockusb_add(struct usb_configuration *c)
{
	struct f_rockusb *f_rkusb = get_rkusb();
	int status;

	debug("%s: cdev: 0x%p\n", __func__, c->cdev);

	f_rkusb->usb_function.name = "f_rockusb";
	f_rkusb->usb_function.bind = rockusb_bind;
	f_rkusb->usb_function.unbind = rockusb_unbind;
	f_rkusb->usb_function.set_alt = rockusb_set_alt;
	f_rkusb->usb_function.disable = rockusb_disable;
	f_rkusb->usb_function.strings = rkusb_strings;

	status = usb_add_function(c, &f_rkusb->usb_function);
	if (status) {
		free(f_rkusb);
		rockusb_func = f_rkusb;
	}
	return status;
}

void rockusb_dev_init(char *dev_type, int dev_index)
{
	struct f_rockusb *f_rkusb = get_rkusb();
	f_rkusb->rockusb_dev_type = dev_type;
	f_rkusb->rockusb_dev_index = dev_index;
}

DECLARE_GADGET_BIND_CALLBACK(usb_dnl_rockusb, rockusb_add);

static int rockusb_tx_write(const char *buffer, unsigned int buffer_size)
{
	struct usb_request *in_req = rockusb_func->in_req;
	int ret;

	memcpy(in_req->buf, buffer, buffer_size);
	in_req->length = buffer_size;
	usb_ep_dequeue(rockusb_func->in_ep, in_req);
	ret = usb_ep_queue(rockusb_func->in_ep, in_req, 0);
	if (ret)
		printf("Error %d on queue\n", ret);
	return 0;
}

static int rockusb_tx_write_str(const char *buffer)
{
	return rockusb_tx_write(buffer, strlen(buffer));
}

#ifdef DEBUG
static void printcbw(char *buf)
{
	ALLOC_CACHE_ALIGN_BUFFER(struct fsg_bulk_cb_wrap, cbw,
				 sizeof(struct fsg_bulk_cb_wrap));

	memcpy((char *)cbw, buf, USB_BULK_CB_WRAP_LEN);

	debug("cbw: signature:%x\n", cbw->signature);
	debug("cbw: tag=%x\n", cbw->tag);
	debug("cbw: data_transfer_length=%d\n", cbw->data_transfer_length);
	debug("cbw: flags=%x\n", cbw->flags);
	debug("cbw: lun=%d\n", cbw->lun);
	debug("cbw: length=%d\n", cbw->length);
	debug("cbw: ucOperCode=%x\n", cbw->CDB[0]);
	debug("cbw: ucReserved=%x\n", cbw->CDB[1]);
	debug("cbw: dwAddress:%x %x %x %x\n", cbw->CDB[5], cbw->CDB[4],
	      cbw->CDB[3], cbw->CDB[2]);
	debug("cbw: ucReserved2=%x\n", cbw->CDB[6]);
	debug("cbw: uslength:%x %x\n", cbw->CDB[8], cbw->CDB[7]);
}

static void printcsw(char *buf)
{
	ALLOC_CACHE_ALIGN_BUFFER(struct bulk_cs_wrap, csw,
				 sizeof(struct bulk_cs_wrap));
	memcpy((char *)csw, buf, USB_BULK_CS_WRAP_LEN);
	debug("csw: signature:%x\n", csw->signature);
	debug("csw: tag:%x\n", csw->tag);
	debug("csw: residue:%x\n", csw->residue);
	debug("csw: status:%x\n", csw->status);
}
#endif

static int rockusb_tx_write_csw(u32 tag, int residue, u8 status, int size)
{
	ALLOC_CACHE_ALIGN_BUFFER(struct bulk_cs_wrap, csw,
				 sizeof(struct bulk_cs_wrap));
	csw->signature = cpu_to_le32(USB_BULK_CS_SIG);
	csw->tag = tag;
	csw->residue = cpu_to_be32(residue);
	csw->status = status;
#ifdef DEBUG
	printcsw((char *)&csw);
#endif
	return rockusb_tx_write((char *)csw, size);
}

static unsigned int rx_bytes_expected(struct usb_ep *ep)
{
	struct f_rockusb *f_rkusb = get_rkusb();
	int rx_remain = f_rkusb->download_size - f_rkusb->download_bytes;
	unsigned int rem;
	unsigned int maxpacket = ep->maxpacket;

	if (rx_remain <= 0)
		return 0;
	else if (rx_remain > EP_BUFFER_SIZE)
		return EP_BUFFER_SIZE;

	rem = rx_remain % maxpacket;
	if (rem > 0)
		rx_remain = rx_remain + (maxpacket - rem);

	return rx_remain;
}

/* usb_request complete call back to handle down load image */
static void rx_handler_dl_image(struct usb_ep *ep, struct usb_request *req)
{
	struct f_rockusb *f_rkusb = get_rkusb();
	unsigned int transfer_size = 0;
	const unsigned char *buffer = req->buf;
	unsigned int buffer_size = req->actual;

	transfer_size = f_rkusb->download_size - f_rkusb->download_bytes;
	if (!f_rkusb->download_desc) {
		printf("rx_handler_dl_image blk_get_dev\n");
		f_rkusb->download_desc = blk_get_dev(f_rkusb->rockusb_dev_type,
				f_rkusb->rockusb_dev_index);
		if (!f_rkusb->download_desc ||
		    f_rkusb->download_desc->type == DEV_TYPE_UNKNOWN) {
			error("invalid mmc device\n");
			rockusb_tx_write_csw(f_rkusb->download_tag, 0, CSW_FAIL,
					     USB_BULK_CS_WRAP_LEN);
			return;
		}
	}

	if (req->status != 0) {
		printf("Bad status: %d\n", req->status);
		rockusb_tx_write_csw(f_rkusb->download_tag, 0,
				     CSW_FAIL, USB_BULK_CS_WRAP_LEN);
		return;
	}

	if (buffer_size < transfer_size)
		transfer_size = buffer_size;

	memcpy((void *)CONFIG_ROCKUSB_BUF_ADDR + f_rkusb->download_bytes,
	       buffer, transfer_size);
	f_rkusb->download_bytes += transfer_size;

	/* Check if transfer is done */
	if (f_rkusb->download_bytes >= f_rkusb->download_size) {
		int blks = 0, blkcnt = f_rkusb->download_size/512;
		debug("download %d bytes finished, start writing to lba %x\n",
		       f_rkusb->download_bytes, f_rkusb->download_lba);
		blks = blk_dwrite(f_rkusb->download_desc,
				  f_rkusb->download_lba, blkcnt,
					(char *)CONFIG_ROCKUSB_BUF_ADDR);
		if (blks != blkcnt) {
			error("failed writing to device %s: %d\n",
			      f_rkusb->rockusb_dev_type,
			      f_rkusb->rockusb_dev_index);
			rockusb_tx_write_csw(f_rkusb->download_tag, 0,
					     CSW_FAIL, USB_BULK_CS_WRAP_LEN);
			return;
		}

		req->complete = rx_handler_command;
		req->length = EP_BUFFER_SIZE;
		f_rkusb->download_size = 0;
		debug("done\n");
		rockusb_tx_write_csw(f_rkusb->download_tag, 0, CSW_GOOD,
				     USB_BULK_CS_WRAP_LEN);
	} else {
		req->length = rx_bytes_expected(ep);
	}

	req->actual = 0;
	usb_ep_queue(ep, req, 0);
}

static void cb_test_unit_ready(struct usb_ep *ep, struct usb_request *req)
{
	ALLOC_CACHE_ALIGN_BUFFER(struct fsg_bulk_cb_wrap, cbw,
				 sizeof(struct fsg_bulk_cb_wrap));

	memcpy((char *)cbw, req->buf, USB_BULK_CB_WRAP_LEN);

	rockusb_tx_write_csw(cbw->tag, cbw->data_transfer_length,
			     CSW_GOOD, USB_BULK_CS_WRAP_LEN);
}

static void cb_read_storage_id(struct usb_ep *ep, struct usb_request *req)
{
	ALLOC_CACHE_ALIGN_BUFFER(struct fsg_bulk_cb_wrap, cbw,
				 sizeof(struct fsg_bulk_cb_wrap));
	char emmc_id[] = "EMMC ";

	printf("cb_read_storage_id\n");
	memcpy((char *)cbw, req->buf, USB_BULK_CB_WRAP_LEN);
	rockusb_tx_write_str(emmc_id);
	rockusb_tx_write_csw(cbw->tag, cbw->data_transfer_length, CSW_GOOD,
			     USB_BULK_CS_WRAP_LEN);
}

static void cb_write_lba(struct usb_ep *ep, struct usb_request *req)
{
	ALLOC_CACHE_ALIGN_BUFFER(struct fsg_bulk_cb_wrap, cbw,
				 sizeof(struct fsg_bulk_cb_wrap));
	int sector_count;
	struct f_rockusb *f_rkusb = get_rkusb();

	memcpy((char *)cbw, req->buf, USB_BULK_CB_WRAP_LEN);
	sector_count = (int)get_unaligned_be16(&cbw->CDB[7]);
	f_rkusb->download_lba = get_unaligned_be32(&cbw->CDB[2]);
	f_rkusb->download_size = sector_count * 512;
	f_rkusb->download_bytes = 0;
	f_rkusb->download_tag = cbw->tag;
	if ((0 == f_rkusb->download_size) ||
	    (f_rkusb->download_size > CONFIG_ROCKUSB_BUF_SIZE)) {
		rockusb_tx_write_csw(cbw->tag, cbw->data_transfer_length,
				     CSW_FAIL, USB_BULK_CS_WRAP_LEN);
	} else {
		req->complete = rx_handler_dl_image;
		req->length = rx_bytes_expected(ep);
	}
}

int __weak rkusb_set_reboot_flag(int flag)
{
	struct f_rockusb *f_rkusb = get_rkusb();

	printf("rkusb_set_reboot_flag: %d\n", f_rkusb->reboot_flag);
	return -ENOSYS;
}

static void compl_do_reset(struct usb_ep *ep, struct usb_request *req)
{
	struct f_rockusb *f_rkusb = get_rkusb();

	rkusb_set_reboot_flag(f_rkusb->reboot_flag);
	do_reset(NULL, 0, 0, NULL);
}

static void cb_reboot(struct usb_ep *ep, struct usb_request *req)
{
	ALLOC_CACHE_ALIGN_BUFFER(struct fsg_bulk_cb_wrap, cbw,
				 sizeof(struct fsg_bulk_cb_wrap));
	struct f_rockusb *f_rkusb = get_rkusb();

	f_rkusb->reboot_flag = 0;
	memcpy((char *)cbw, req->buf, USB_BULK_CB_WRAP_LEN);
	f_rkusb->reboot_flag = cbw->CDB[1];
	rockusb_func->in_req->complete = compl_do_reset;
	rockusb_tx_write_csw(cbw->tag, cbw->data_transfer_length, CSW_GOOD,
			     USB_BULK_CS_WRAP_LEN);
}

static void cb_not_support(struct usb_ep *ep, struct usb_request *req)
{
	ALLOC_CACHE_ALIGN_BUFFER(struct fsg_bulk_cb_wrap, cbw,
				 sizeof(struct fsg_bulk_cb_wrap));

	memcpy((char *)cbw, req->buf, USB_BULK_CB_WRAP_LEN);
	printf("Rockusb command %x not support yet\n", cbw->CDB[0]);
	rockusb_tx_write_csw(cbw->tag, 0, CSW_FAIL, USB_BULK_CS_WRAP_LEN);
}

static const struct cmd_dispatch_info cmd_dispatch_info[] = {
	{
		.cmd = K_FW_TEST_UNIT_READY,
		.cb = cb_test_unit_ready,
	},
	{
		.cmd = K_FW_READ_FLASH_ID,
		.cb = cb_read_storage_id,
	},
	{
		.cmd = K_FW_SET_DEVICE_ID,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_TEST_BAD_BLOCK,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_READ_10,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_WRITE_10,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_ERASE_10,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_WRITE_SPARE,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_READ_SPARE,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_ERASE_10_FORCE,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_GET_VERSION,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_LBA_READ_10,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_LBA_WRITE_10,
		.cb = cb_write_lba,
	},
	{
		.cmd = K_FW_ERASE_SYS_DISK,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_SDRAM_READ_10,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_SDRAM_WRITE_10,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_SDRAM_EXECUTE,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_READ_FLASH_INFO,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_GET_CHIP_VER,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_LOW_FORMAT,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_SET_RESET_FLAG,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_SPI_READ_10,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_SPI_WRITE_10,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_SESSION,
		.cb = cb_not_support,
	},
	{
		.cmd = K_FW_RESET,
		.cb = cb_reboot,
	},
};

static void rx_handler_command(struct usb_ep *ep, struct usb_request *req)
{
	void (*func_cb)(struct usb_ep *ep, struct usb_request *req) = NULL;
	ALLOC_CACHE_ALIGN_BUFFER(struct fsg_bulk_cb_wrap, cbw,
				 sizeof(struct fsg_bulk_cb_wrap));
	char *cmdbuf = req->buf;
	int i;

	if (req->status || req->length == 0)
		return;

	memcpy((char *)cbw, req->buf, USB_BULK_CB_WRAP_LEN);
#ifdef DEBUG
	printcbw(req->buf);
#endif

	for (i = 0; i < ARRAY_SIZE(cmd_dispatch_info); i++) {
		if (cmd_dispatch_info[i].cmd == cbw->CDB[0]) {
			func_cb = cmd_dispatch_info[i].cb;
			break;
		}
	}

	if (!func_cb) {
		error("unknown command: %s", (char *)req->buf);
		rockusb_tx_write_str("FAILunknown command");
	} else {
		if (req->actual < req->length) {
			u8 *buf = (u8 *)req->buf;
			buf[req->actual] = 0;
			func_cb(ep, req);
		} else {
			error("buffer overflow");
			rockusb_tx_write_str("FAILbuffer overflow");
		}
	}

	*cmdbuf = '\0';
	req->actual = 0;
	usb_ep_queue(ep, req, 0);
}
