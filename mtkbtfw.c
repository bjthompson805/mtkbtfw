/*-
 * SPDX-License-Identifier: ISC
 *
 * MediaTek MT79xx USB Bluetooth firmware loader for FreeBSD.
 *
 * A userland port of the MT79xx setup path in Linux's
 * drivers/bluetooth/btmtk.c (Copyright (C) 2021 MediaTek Inc., ISC
 * licensed), structured after FreeBSD's rtlbtfw(8).
 *
 * Every step runs over the control endpoint: WMT commands go out wrapped
 * in vendor HCI command 0xfc6f, and their WMT events are polled back with
 * a vendor control IN request.  ng_ubt(4) attaches to these chips before
 * any firmware is loaded, so the loader detaches it, loads the patch,
 * enables the Bluetooth function and then hands the device back.
 */

#include <sys/param.h>
#include <sys/endian.h>
#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libusb.h>

#define	DEFAULT_FW_PATH		"/usr/local/share/mtkbt-firmware"
#define	USB_TIMEOUT		2000	/* ms, one control transfer */
#define	WMT_EVT_TIMEOUT		10000	/* ms, Linux's HCI_INIT_TIMEOUT */
#define	RERUN_GUARD_SEC		60

/* Registers (btmtk.h) */
#define	MTK_REG_HW_ID		0x80000008
#define	MTK_REG_DEV_ID		0x70010200
#define	MTK_REG_FW_VER		0x80021004
#define	MTK_REG_FW_FLAVOR	0x70010020
#define	MTK_EP_RST_OPT		0x74011890
#define	MTK_EP_RST_IN_OUT_OPT	0x00010001
#define	MTK_BT_MISC		0x70002510
#define	MTK_BT_RST_DONE		0x00000100
#define	MTK_BT_RESET_REG_CONNV3	0x70028610
#define	MTK_UDMA_INT_STA_BT	0x74000024
#define	MTK_UDMA_INT_STA_BT1	0x74000308

/* Firmware layout (btmtk.c) */
#define	FW_HEADER_SIZE		32
#define	FW_GD_SIZE		64
#define	FW_SEC_MAP_SIZE		64
#define	SEC_MAP_COMMON_SIZE	12
#define	SEC_MAP_NEED_SEND_SIZE	52
#define	FW_CHUNK		250

#define	HCI_EV_WMT		0xe4
#define	WMT_MAX_EVENT_SIZE	64

enum {
	WMT_PATCH_DWNLD = 0x1,
	WMT_FUNC_CTRL = 0x6,
};

enum {
	WMT_INVALID,
	WMT_PATCH_UNDONE,
	WMT_PATCH_PROGRESS,
	WMT_PATCH_DONE,
	WMT_ON_UNDONE,
	WMT_ON_DONE,
	WMT_ON_PROGRESS,
};

enum {
	WMT_PKT_START = 1,
	WMT_PKT_CONTINUE = 2,
	WMT_PKT_END = 3,
};

static int do_debug = 0;
static int do_info = 0;

#define	mtk_err(fmt, ...)						\
	fprintf(stderr, "mtkbtfw: %s: "fmt"\n", __func__, ##__VA_ARGS__)
#define	mtk_info(fmt, ...)	do {					\
	if (do_info)							\
		fprintf(stderr, "%s: "fmt"\n", __func__, ##__VA_ARGS__);\
} while (0)
#define	mtk_debug(fmt, ...)	do {					\
	if (do_debug)							\
		fprintf(stderr, "%s: "fmt"\n", __func__, ##__VA_ARGS__);\
} while (0)

struct mtk_devid {
	uint16_t vendor_id;
	uint16_t product_id;
};

/*
 * Matched on vendor/product rather than interface class: ubt-class
 * MediaTek parts that need no patch must be left alone.  Add IDs from
 * Linux's btusb.c quirks_table (BTUSB_MEDIATEK) as they are tested.
 */
static const struct mtk_devid mtk_list[] = {
	{ 0x13d3, 0x3608 },	/* MT7925, ASUS Zenbook S 16 (UM5606WA) */
};

static uint32_t
le32_at(const uint8_t *p)
{
	return (p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
}

static void
msleep(int ms)
{
	usleep(ms * 1000);
}

/* Vendor register read over the control pipe (btmtk_usb_reg_read). */
static int
mtk_reg_read(libusb_device_handle *hdl, uint32_t reg, uint32_t *val)
{
	uint8_t buf[4];
	int r;

	r = libusb_control_transfer(hdl,
	    LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR, 0x63,
	    reg >> 16, reg & 0xffff, buf, sizeof(buf), USB_TIMEOUT);
	if (r != sizeof(buf)) {
		mtk_err("reg 0x%08x: %s", reg, r < 0 ? libusb_strerror(r) :
		    "short read");
		return (-1);
	}
	*val = le32_at(buf);
	mtk_debug("reg 0x%08x = 0x%08x", reg, *val);
	return (0);
}

/* UHW register access (btmtk_usb_uhw_reg_{read,write}). */
static int
mtk_uhw_read(libusb_device_handle *hdl, uint32_t reg, uint32_t *val)
{
	uint8_t buf[4];
	int r;

	r = libusb_control_transfer(hdl, 0xde, 0x01,
	    reg >> 16, reg & 0xffff, buf, sizeof(buf), USB_TIMEOUT);
	if (r != sizeof(buf)) {
		mtk_err("uhw reg 0x%08x: %s", reg, r < 0 ?
		    libusb_strerror(r) : "short read");
		return (-1);
	}
	*val = le32_at(buf);
	return (0);
}

static int
mtk_uhw_write(libusb_device_handle *hdl, uint32_t reg, uint32_t val)
{
	uint8_t buf[4];
	int r;

	buf[0] = val;
	buf[1] = val >> 8;
	buf[2] = val >> 16;
	buf[3] = val >> 24;
	r = libusb_control_transfer(hdl, 0x5e, 0x02,
	    reg >> 16, reg & 0xffff, buf, sizeof(buf), USB_TIMEOUT);
	if (r < 0) {
		mtk_err("uhw reg 0x%08x: %s", reg, libusb_strerror(r));
		return (-1);
	}
	return (0);
}

/*
 * Send one WMT command and wait for its WMT event (btmtk_usb_hci_wmt_sync).
 * The event does not arrive on the interrupt endpoint: it has to be polled
 * from the control pipe, with a short pause between empty polls so the
 * chip has time to produce it.
 */
static int
mtk_wmt_sync(libusb_device_handle *hdl, uint8_t op, uint8_t flag,
    const void *data, uint16_t dlen, int *status)
{
	uint8_t cmd[3 + 5 + FW_CHUNK + 8];
	uint8_t evt[WMT_MAX_EVENT_SIZE];
	struct timespec start, now;
	int hlen = 5 + dlen;
	int r, st = WMT_INVALID;

	if (hlen > 255)
		return (-1);

	/* HCI command header, then the WMT header and payload */
	cmd[0] = 0x6f;
	cmd[1] = 0xfc;
	cmd[2] = hlen;
	cmd[3] = 1;			/* dir: host to chip */
	cmd[4] = op;
	cmd[5] = (dlen + 1) & 0xff;
	cmd[6] = (dlen + 1) >> 8;
	cmd[7] = flag;
	if (dlen > 0)
		memcpy(cmd + 8, data, dlen);

	r = libusb_control_transfer(hdl,
	    LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_DEVICE, 0, 0, 0,
	    cmd, 3 + hlen, USB_TIMEOUT);
	if (r < 0) {
		mtk_err("send op %d: %s", op, libusb_strerror(r));
		return (-1);
	}

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		r = libusb_control_transfer(hdl,
		    LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR, 1, 48, 0,
		    evt, sizeof(evt), USB_TIMEOUT);
		if (r < 0) {
			mtk_err("poll op %d: %s", op, libusb_strerror(r));
			return (-1);
		}
		if (r > 0)
			break;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if ((now.tv_sec - start.tv_sec) * 1000 +
		    (now.tv_nsec - start.tv_nsec) / 1000000 > WMT_EVT_TIMEOUT) {
			mtk_err("op %d: timed out waiting for WMT event", op);
			return (-1);
		}
		usleep(500);
	}

	/* hci event header (2) + wmt header (5) */
	if (r < 7 || evt[0] != HCI_EV_WMT) {
		mtk_err("op %d: unexpected event 0x%02x, %d bytes", op,
		    evt[0], r);
		return (-1);
	}
	if (evt[3] != op) {
		mtk_err("wrong op received %d expected %d", evt[3], op);
		return (-1);
	}

	switch (op) {
	case WMT_FUNC_CTRL:
		if (r < 9)
			st = evt[6] ? WMT_ON_UNDONE : WMT_ON_DONE;
		else if ((evt[7] << 8 | evt[8]) == 0x404)
			st = WMT_ON_DONE;
		else if ((evt[7] << 8 | evt[8]) == 0x420)
			st = WMT_ON_PROGRESS;
		else
			st = WMT_ON_UNDONE;
		break;
	case WMT_PATCH_DWNLD:
		if (evt[6] == 2)
			st = WMT_PATCH_DONE;
		else if (evt[6] == 1)
			st = WMT_PATCH_PROGRESS;
		else
			st = WMT_PATCH_UNDONE;
		break;
	}
	if (status != NULL)
		*status = st;
	return (0);
}

/* Subsystem reset for MT7925 (btmtk_usb_subsys_reset), for -R. */
static int
mtk_subsys_reset(libusb_device_handle *hdl)
{
	uint32_t val;
	int i;

	if (mtk_uhw_read(hdl, MTK_BT_RESET_REG_CONNV3, &val) ||
	    mtk_uhw_write(hdl, MTK_BT_RESET_REG_CONNV3, val | (1 << 5)) ||
	    mtk_uhw_read(hdl, MTK_BT_RESET_REG_CONNV3, &val) ||
	    mtk_uhw_write(hdl, MTK_BT_RESET_REG_CONNV3,
	    (val & 0xffff00ff) | (1 << 13)) ||
	    mtk_uhw_write(hdl, MTK_EP_RST_OPT, MTK_EP_RST_IN_OUT_OPT) ||
	    mtk_uhw_read(hdl, MTK_BT_RESET_REG_CONNV3, &val) ||
	    mtk_uhw_write(hdl, MTK_BT_RESET_REG_CONNV3, val | 1) ||
	    mtk_uhw_write(hdl, MTK_UDMA_INT_STA_BT, 0xff) ||
	    mtk_uhw_read(hdl, MTK_UDMA_INT_STA_BT, &val) ||
	    mtk_uhw_write(hdl, MTK_UDMA_INT_STA_BT1, 0xff) ||
	    mtk_uhw_read(hdl, MTK_UDMA_INT_STA_BT1, &val))
		return (-1);
	msleep(100);

	for (i = 0; i < 50; i++) {
		if (mtk_uhw_read(hdl, MTK_BT_MISC, &val) == 0 &&
		    (val & MTK_BT_RST_DONE))
			return (0);
		msleep(20);
	}
	mtk_err("reset timeout");
	return (-1);
}

static uint8_t *
read_file(const char *path, size_t *len)
{
	struct stat sb;
	uint8_t *buf;
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		mtk_err("%s: %s", path, strerror(errno));
		return (NULL);
	}
	if (fstat(fd, &sb) != 0 || (buf = malloc(sb.st_size)) == NULL) {
		close(fd);
		return (NULL);
	}
	n = read(fd, buf, sb.st_size);
	close(fd);
	if (n != sb.st_size) {
		mtk_err("%s: short read", path);
		free(buf);
		return (NULL);
	}
	*len = sb.st_size;
	return (buf);
}

/*
 * Download every section of an MT79xx patch file
 * (btmtk_setup_firmware_79xx).  Sets *loaded when any section was
 * actually sent; a chip that already has the patch answers PATCH_DONE.
 */
static int
mtk_setup_firmware_79xx(libusb_device_handle *hdl, const uint8_t *fw,
    size_t fwlen, int *loaded)
{
	const uint8_t *map, *p;
	uint32_t section_num, offset, dl_size;
	uint8_t cmd[1 + SEC_MAP_NEED_SEND_SIZE];
	int i, retry, status, dlen, flag;

	*loaded = 0;
	if (fwlen < FW_HEADER_SIZE + FW_GD_SIZE) {
		mtk_err("firmware too small: %zu bytes", fwlen);
		return (-1);
	}
	section_num = le32_at(fw + FW_HEADER_SIZE + 12);
	if (section_num == 0 || section_num > 64 ||
	    fwlen < FW_HEADER_SIZE + FW_GD_SIZE +
	    (size_t)FW_SEC_MAP_SIZE * section_num) {
		mtk_err("bad section count %u", section_num);
		return (-1);
	}
	mtk_info("SW ver 0x%04x, build %.16s, %u sections",
	    fw[22] | fw[23] << 8, (const char *)fw, section_num);

	for (i = 0; i < (int)section_num; i++) {
		map = fw + FW_HEADER_SIZE + FW_GD_SIZE + FW_SEC_MAP_SIZE * i;
		offset = le32_at(map + 4);
		dl_size = le32_at(map + 16);	/* bin_info_spec.dlsize */
		if (dl_size == 0)
			continue;
		if (offset > fwlen || dl_size > fwlen - offset) {
			mtk_err("section %d out of bounds", i);
			return (-1);
		}

		for (retry = 20; retry > 0; retry--) {
			cmd[0] = 0;	/* legacy download mode */
			memcpy(cmd + 1, map + SEC_MAP_COMMON_SIZE,
			    SEC_MAP_NEED_SEND_SIZE);
			if (mtk_wmt_sync(hdl, WMT_PATCH_DWNLD, 0, cmd,
			    sizeof(cmd), &status) != 0)
				return (-1);
			if (status != WMT_PATCH_PROGRESS)
				break;
			msleep(100);
		}
		if (retry == 0) {
			mtk_err("section %d: chip stayed busy", i);
			return (-1);
		}
		if (status == WMT_PATCH_DONE) {
			mtk_info("section %d already loaded", i);
			continue;
		}
		if (status != WMT_PATCH_UNDONE) {
			mtk_err("section %d: bad status %d", i, status);
			return (-1);
		}

		mtk_info("section %d: %u bytes", i, dl_size);
		p = fw + offset;
		flag = WMT_PKT_START;
		while (dl_size > 0) {
			dlen = MIN(FW_CHUNK, dl_size);
			if (flag != WMT_PKT_START)
				flag = dl_size == (uint32_t)dlen ?
				    WMT_PKT_END : WMT_PKT_CONTINUE;
			if (mtk_wmt_sync(hdl, WMT_PATCH_DWNLD, flag, p, dlen,
			    &status) != 0)
				return (-1);
			if (status == WMT_PATCH_PROGRESS) {
				mtk_err("section %d: unexpected progress", i);
				return (-1);
			}
			dl_size -= dlen;
			p += dlen;
			flag = WMT_PKT_CONTINUE;
		}
		*loaded = 1;
	}

	/* Wait a few moments for firmware activation */
	msleep(110);
	return (0);
}

static int
mtk_is_mediatek(libusb_device *dev)
{
	struct libusb_device_descriptor d;
	int i;

	if (libusb_get_device_descriptor(dev, &d) != 0)
		return (0);
	for (i = 0; i < (int)nitems(mtk_list); i++)
		if (mtk_list[i].vendor_id == d.idVendor &&
		    mtk_list[i].product_id == d.idProduct)
			return (1);
	return (0);
}

static libusb_device *
mtk_find_device(libusb_context *ctx, int bus_id, int dev_id)
{
	libusb_device **list, *found = NULL;
	ssize_t cnt, i;

	cnt = libusb_get_device_list(ctx, &list);
	if (cnt < 0)
		return (NULL);
	for (i = 0; i < cnt; i++)
		if (libusb_get_bus_number(list[i]) == bus_id &&
		    libusb_get_device_address(list[i]) == dev_id &&
		    mtk_is_mediatek(list[i])) {
			found = libusb_ref_device(list[i]);
			break;
		}
	libusb_free_device_list(list, 1);
	return (found);
}

static int
parse_ugen_name(const char *ugen, uint8_t *bus, uint8_t *addr)
{
	char *ep;

	if (strncmp(ugen, "ugen", 4) != 0)
		return (-1);
	*bus = (uint8_t)strtoul(ugen + 4, &ep, 10);
	if (*ep != '.')
		return (-1);
	*addr = (uint8_t)strtoul(ep + 1, &ep, 10);
	if (*ep != '\0')
		return (-1);
	return (0);
}

/*
 * ng_ubt is reattached by resetting the device.  If that reset ever wipes
 * the patch, devd would run us again on every attach; refuse to run twice
 * for the same device within a minute so that cannot become a loop.
 */
static int
rerun_guard(const char *ugen)
{
	char path[64];
	struct stat sb;
	int fd;

	snprintf(path, sizeof(path), "/var/run/mtkbtfw.%s", ugen);
	if (stat(path, &sb) == 0 && time(NULL) - sb.st_mtime < RERUN_GUARD_SEC)
		return (1);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0)
		close(fd);
	return (0);
}

static void
usage(void)
{
	fprintf(stderr,
	    "Usage: mtkbtfw [-DIRF] -d ugenX.Y [-f firmware path]\n"
	    "    -D: enable debugging\n"
	    "    -d: device to operate upon\n"
	    "    -f: firmware path, if not default (" DEFAULT_FW_PATH ")\n"
	    "    -F: ignore the rerun guard\n"
	    "    -I: enable informational output\n"
	    "    -R: reset the Bluetooth subsystem first\n");
	exit(127);
}

int
main(int argc, char *argv[])
{
	libusb_context *ctx = NULL;
	libusb_device *dev = NULL;
	libusb_device_handle *hdl = NULL;
	const char *ugen = NULL, *fwdir = DEFAULT_FW_PATH;
	uint8_t bus_id, dev_addr, param;
	uint32_t dev_id = 0, fw_ver = 0, fw_flavor = 0;
	char fwname[PATH_MAX];
	uint8_t *fw = NULL;
	size_t fwlen;
	int n, r, retcode = 1, detached = 0, claimed = 0, loaded = 0;
	int do_reset = 0, force = 0;

	while ((n = getopt(argc, argv, "Dd:Ff:hIR")) != -1) {
		switch (n) {
		case 'D':
			do_debug = do_info = 1;
			break;
		case 'd':
			ugen = optarg;
			break;
		case 'F':
			force = 1;
			break;
		case 'f':
			fwdir = optarg;
			break;
		case 'I':
			do_info = 1;
			break;
		case 'R':
			do_reset = 1;
			break;
		default:
			usage();
		}
	}
	if (ugen == NULL || parse_ugen_name(ugen, &bus_id, &dev_addr) != 0)
		usage();
	if (!force && rerun_guard(ugen)) {
		mtk_info("%s handled less than %ds ago, skipping", ugen,
		    RERUN_GUARD_SEC);
		return (0);
	}

	if ((r = libusb_init(&ctx)) != 0) {
		mtk_err("libusb_init: %s", libusb_strerror(r));
		return (1);
	}
	dev = mtk_find_device(ctx, bus_id, dev_addr);
	if (dev == NULL) {
		mtk_err("%s is not a known MediaTek Bluetooth device", ugen);
		goto shutdown;
	}
	if ((r = libusb_open(dev, &hdl)) != 0) {
		mtk_err("libusb_open: %s", libusb_strerror(r));
		goto shutdown;
	}

	if (libusb_kernel_driver_active(hdl, 0) == 1) {
		if ((r = libusb_detach_kernel_driver(hdl, 0)) != 0) {
			mtk_err("detaching ng_ubt: %s", libusb_strerror(r));
			goto shutdown;
		}
		detached = 1;
	}
	if ((r = libusb_claim_interface(hdl, 0)) != 0) {
		mtk_err("claim interface: %s", libusb_strerror(r));
		goto reattach;
	}
	claimed = 1;

	if (do_reset && mtk_subsys_reset(hdl) != 0)
		goto reattach;

	if (mtk_reg_read(hdl, MTK_REG_DEV_ID, &dev_id) != 0 ||
	    mtk_reg_read(hdl, MTK_REG_FW_VER, &fw_ver) != 0 ||
	    mtk_reg_read(hdl, MTK_REG_FW_FLAVOR, &fw_flavor) != 0)
		goto reattach;
	fw_flavor = (fw_flavor & 0x80) >> 7;
	mtk_info("dev id 0x%04x, fw ver 0x%x, flavor %u", dev_id, fw_ver,
	    fw_flavor);

	switch (dev_id) {
	case 0x7925:
		snprintf(fwname, sizeof(fwname),
		    "%s/mediatek/mt7925/BT_RAM_CODE_MT7925_1_%x_hdr.bin",
		    fwdir, (fw_ver & 0xff) + 1);
		break;
	case 0x7961:
		if (fw_flavor) {
			snprintf(fwname, sizeof(fwname),
			    "%s/mediatek/BT_RAM_CODE_MT7961_1a_%x_hdr.bin",
			    fwdir, (fw_ver & 0xff) + 1);
			break;
		}
		/* FALLTHROUGH */
	case 0x7922:
	case 0x7902:
		snprintf(fwname, sizeof(fwname),
		    "%s/mediatek/BT_RAM_CODE_MT%04x_1_%x_hdr.bin",
		    fwdir, dev_id & 0xffff, (fw_ver & 0xff) + 1);
		break;
	default:
		mtk_err("unsupported hardware variant 0x%08x", dev_id);
		goto reattach;
	}

	mtk_info("loading %s", fwname);
	if ((fw = read_file(fwname, &fwlen)) == NULL)
		goto reattach;
	if (mtk_setup_firmware_79xx(hdl, fw, fwlen, &loaded) != 0)
		goto reattach;

	/* Device endpoint reset option, then enable the BT function */
	if (mtk_uhw_write(hdl, MTK_EP_RST_OPT, MTK_EP_RST_IN_OUT_OPT) != 0)
		goto reattach;
	param = 1;
	if (mtk_wmt_sync(hdl, WMT_FUNC_CTRL, 0, &param, 1, NULL) != 0)
		goto reattach;

	mtk_info("%s", loaded ? "firmware loaded" : "firmware was already loaded");
	retcode = 0;

reattach:
	if (claimed)
		libusb_release_interface(hdl, 0);
	if (detached) {
		/*
		 * ng_ubt has to start over against the patched chip; its
		 * first attach saw a chip that refused HCI_Reset.  A device
		 * reset makes the kernel probe and attach it again.
		 */
		r = libusb_reset_device(hdl);
		if (r != 0 && r != LIBUSB_ERROR_NOT_FOUND)
			mtk_err("libusb_reset_device: %s", libusb_strerror(r));
	}

shutdown:
	free(fw);
	if (hdl != NULL)
		libusb_close(hdl);
	if (dev != NULL)
		libusb_unref_device(dev);
	if (ctx != NULL)
		libusb_exit(ctx);
	return (retcode);
}
