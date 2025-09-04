// SPDX-License-Identifier: BSD-3-Clause
#include <sys/types.h>
#include <fcntl.h>
#include <time.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb.h>
#include "oscompat.h"

#include "qdl.h"

#define DEFAULT_OUT_CHUNK_SIZE (1024 * 1024)

/* USB transfer retry configuration */
#define USB_MAX_RETRIES 5
#define USB_BASE_TIMEOUT_MS 1000
#define USB_MAX_TIMEOUT_MS 10000
#define USB_RETRY_DELAY_MS 100
#define USB_BACKOFF_MULTIPLIER 2
#define USB_MIN_CHUNK_SIZE (64 * 1024)
#define USB_ADAPTIVE_THRESHOLD 3
#define USB_MULTI_DEVICE_THRESHOLD 2

/* Environment variable names for configuration */
#define ENV_USB_MAX_RETRIES "QDL_USB_MAX_RETRIES"
#define ENV_USB_CHUNK_SIZE "QDL_USB_CHUNK_SIZE"
#define ENV_USB_RETRY_DELAY "QDL_USB_RETRY_DELAY"

/* Global state for multi-device detection */
static int g_device_count = 0;
static bool g_multi_device_mode = false;

struct qdl_device_usb {
	struct qdl_device base;
	struct libusb_device_handle *usb_handle;

	int in_ep;
	int out_ep;

	size_t in_maxpktsize;
	size_t out_maxpktsize;
	size_t out_chunk_size;
	size_t adaptive_chunk_size;
	int consecutive_failures;
	int max_retries;
	int retry_delay_ms;
	bool multi_device_detected;

	/* Transfer statistics */
	unsigned long total_bytes_written;
	unsigned long total_transfers;
	unsigned long failed_transfers;
	unsigned long retried_transfers;
};

/*
 * libusb commit f0cce43f882d ("core: Fix definition and use of enum
 * libusb_transfer_type") split transfer type and endpoint transfer types.
 * Provide an alias in order to make the code compile with the old (non-split)
 * definition.
 */
#ifndef LIBUSB_ENDPOINT_TRANSFER_TYPE_BULK
#define LIBUSB_ENDPOINT_TRANSFER_TYPE_BULK LIBUSB_TRANSFER_TYPE_BULK
#endif

/* Detect if multiple QDL devices are active */
static void usb_update_device_count(bool opening)
{
	if (opening) {
		g_device_count++;
		if (g_device_count >= USB_MULTI_DEVICE_THRESHOLD) {
			g_multi_device_mode = true;
			ux_debug("USB: multi-device mode enabled (%d devices)\n", g_device_count);
		}
	} else {
		g_device_count--;
		if (g_device_count < USB_MULTI_DEVICE_THRESHOLD) {
			g_multi_device_mode = false;
			ux_debug("USB: multi-device mode disabled (%d devices)\n", g_device_count);
		}
	}
}

static bool usb_match_usb_serial(struct libusb_device_handle *handle, const char *serial,
				 const struct libusb_device_descriptor *desc)
{
	char buf[128];
	char *p;
	int ret;

	/* If no serial is requested, consider everything a match */
	if (!serial)
		return true;

	ret = libusb_get_string_descriptor_ascii(handle, desc->iProduct, (unsigned char *)buf, sizeof(buf));
	if (ret < 0) {
		warnx("failed to read iProduct descriptor: %s", libusb_strerror(ret));
		return false;
	}

	p = strstr(buf, "_SN:");
	if (!p)
		return false;

	p += strlen("_SN:");
	p[strcspn(p, " _")] = '\0';

	return strcmp(p, serial) == 0;
}

static int usb_try_open(libusb_device *dev, struct qdl_device_usb *qdl, const char *serial)
{
	const struct libusb_endpoint_descriptor *endpoint;
	const struct libusb_interface_descriptor *ifc;
	struct libusb_config_descriptor *config;
	struct libusb_device_descriptor desc;
	struct libusb_device_handle *handle;
	size_t out_size;
	size_t in_size;
	uint8_t type;
	int ret;
	int out;
	int in;
	int k;
	int l;

	ret = libusb_get_device_descriptor(dev, &desc);
	if (ret < 0) {
		warnx("failed to get USB device descriptor");
		return -1;
	}

	/* Consider only devices with vid 0x0506 and known product id */
	if (desc.idVendor != 0x05c6)
		return 0;
	if (desc.idProduct != 0x9008 && desc.idProduct != 0x900e && desc.idProduct != 0x901d)
		return 0;

	ret = libusb_get_active_config_descriptor(dev, &config);
	if (ret < 0) {
		warnx("failed to acquire USB device's active config descriptor");
		return -1;
	}

	for (k = 0; k < config->bNumInterfaces; k++) {
		ifc = config->interface[k].altsetting;

		in = -1;
		out = -1;
		in_size = 0;
		out_size = 0;

		for (l = 0; l < ifc->bNumEndpoints; l++) {
			endpoint = &ifc->endpoint[l];

			type = endpoint->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
			if (type != LIBUSB_ENDPOINT_TRANSFER_TYPE_BULK)
				continue;

			if (endpoint->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
				in = endpoint->bEndpointAddress;
				in_size = endpoint->wMaxPacketSize;
			} else {
				out = endpoint->bEndpointAddress;
				out_size = endpoint->wMaxPacketSize;
			}
		}

		if (ifc->bInterfaceClass != 0xff)
			continue;

		if (ifc->bInterfaceSubClass != 0xff)
			continue;

		/* bInterfaceProtocol of 0xff, 0x10 and 0x11 has been seen */
		if (ifc->bInterfaceProtocol != 0xff &&
		    ifc->bInterfaceProtocol != 16 &&
		    ifc->bInterfaceProtocol != 17)
			continue;

		ret = libusb_open(dev, &handle);
		if (ret < 0) {
			warnx("unable to open USB device");
			continue;
		}

		if (!usb_match_usb_serial(handle, serial, &desc)) {
			libusb_close(handle);
			continue;
		}

		libusb_detach_kernel_driver(handle, ifc->bInterfaceNumber);

		ret = libusb_claim_interface(handle, ifc->bInterfaceNumber);
		if (ret < 0) {
			warnx("failed to claim USB interface");
			libusb_close(handle);
			continue;
		}

		qdl->usb_handle = handle;
		qdl->in_ep = in;
		qdl->out_ep = out;
		qdl->in_maxpktsize = in_size;
		qdl->out_maxpktsize = out_size;

		/* Initialize retry configuration from environment or defaults */
		char *env_retries = getenv(ENV_USB_MAX_RETRIES);
		qdl->max_retries = env_retries ? atoi(env_retries) : USB_MAX_RETRIES;
		if (qdl->max_retries < 1) qdl->max_retries = USB_MAX_RETRIES;

		char *env_delay = getenv(ENV_USB_RETRY_DELAY);
		qdl->retry_delay_ms = env_delay ? atoi(env_delay) : USB_RETRY_DELAY_MS;
		if (qdl->retry_delay_ms < 50) qdl->retry_delay_ms = USB_RETRY_DELAY_MS;

		char *env_chunk = getenv(ENV_USB_CHUNK_SIZE);
		if (env_chunk) {
			size_t env_chunk_size = strtoul(env_chunk, NULL, 0);
			if (env_chunk_size >= USB_MIN_CHUNK_SIZE) {
				qdl->out_chunk_size = env_chunk_size;
			}
		}

		if (qdl->out_chunk_size && qdl->out_chunk_size % out_size) {
			ux_err("WARNING: requested out-chunk-size must be multiple of the device's wMaxPacketSize %ld, using %ld\n",
			       out_size, out_size);
			qdl->out_chunk_size = out_size;
		} else if (!qdl->out_chunk_size) {
			qdl->out_chunk_size = DEFAULT_OUT_CHUNK_SIZE;
		}

		/* Initialize adaptive chunk size */
		qdl->adaptive_chunk_size = qdl->out_chunk_size;
		qdl->consecutive_failures = 0;
		qdl->multi_device_detected = g_multi_device_mode;

		/* Update global device count */
		usb_update_device_count(true);

		/* Adjust defaults for multi-device scenarios */
		if (g_multi_device_mode) {
			qdl->adaptive_chunk_size = MIN(qdl->out_chunk_size / 2, 512 * 1024);
			qdl->max_retries = (qdl->max_retries > 5) ? qdl->max_retries : 5;
			ux_device_info(&qdl->base, "USB: multi-device mode detected, using conservative settings\n");
		}

		/* Initialize statistics */
		qdl->total_bytes_written = 0;
		qdl->total_transfers = 0;
		qdl->failed_transfers = 0;
		qdl->retried_transfers = 0;

		ux_device_debug(&qdl->base, "USB: using out-chunk-size of %ld, max-retries=%d, retry-delay=%dms\n",
			 qdl->out_chunk_size, qdl->max_retries, qdl->retry_delay_ms);

		break;
	}

	libusb_free_config_descriptor(config);

	return !!qdl->usb_handle;
}

static int usb_open(struct qdl_device *qdl, const char *serial)
{
	struct libusb_device **devs;
	struct libusb_device *dev;
	struct qdl_device_usb *qdl_usb = container_of(qdl, struct qdl_device_usb, base);
	bool wait_printed = false;
	bool found = false;
	ssize_t n;
	int ret;
	int i;

	ret = libusb_init(NULL);
	if (ret < 0)
		err(1, "failed to initialize libusb");

	for (;;) {
		n = libusb_get_device_list(NULL, &devs);
		if (n < 0)
			err(1, "failed to list USB devices");

		for (i = 0; devs[i]; i++) {
			dev = devs[i];

			ret = usb_try_open(dev, qdl_usb, serial);
			if (ret == 1) {
				found = true;
				break;
			}
		}

		libusb_free_device_list(devs, 1);

		if (found)
			return 0;

		if (!wait_printed) {
			ux_device_info(&qdl_usb->base, "Waiting for EDL device\n");
			wait_printed = true;
		}

		usleep(250000);
	}

	return -1;
}

static void usb_close(struct qdl_device *qdl)
{
	struct qdl_device_usb *qdl_usb = container_of(qdl, struct qdl_device_usb, base);

	/* Print transfer statistics */
	if (qdl_usb->total_transfers > 0) {
		ux_device_info(&qdl_usb->base, "USB Transfer Statistics:\n");
		ux_device_info(&qdl_usb->base, "  Total bytes: %lu\n", qdl_usb->total_bytes_written);
		ux_device_info(&qdl_usb->base, "  Total transfers: %lu\n", qdl_usb->total_transfers);
		ux_device_info(&qdl_usb->base, "  Failed transfers: %lu\n", qdl_usb->failed_transfers);
		ux_device_info(&qdl_usb->base, "  Retried transfers: %lu\n", qdl_usb->retried_transfers);
		if (qdl_usb->failed_transfers > 0) {
			ux_device_info(&qdl_usb->base, "  Success rate: %.2f%%\n",
				(double)(qdl_usb->total_transfers - qdl_usb->failed_transfers) * 100.0 / qdl_usb->total_transfers);
		}
	}

	/* Update global device count */
	usb_update_device_count(false);

	libusb_close(qdl_usb->usb_handle);
	libusb_exit(NULL);
}

static int usb_read(struct qdl_device *qdl, void *buf, size_t len, unsigned int timeout)
{
	struct qdl_device_usb *qdl_usb = container_of(qdl, struct qdl_device_usb, base);
	int actual;
	int ret;

	ret = libusb_bulk_transfer(qdl_usb->usb_handle, qdl_usb->in_ep, buf, len, &actual, timeout);
	if ((ret != 0 && ret != LIBUSB_ERROR_TIMEOUT) ||
	    (ret == LIBUSB_ERROR_TIMEOUT && actual == 0))
		return -1;

	return actual;
}

/* Check if a libusb error is potentially recoverable */
static bool usb_is_recoverable_error(int libusb_error)
{
	switch (libusb_error) {
	case LIBUSB_ERROR_TIMEOUT:
	case LIBUSB_ERROR_PIPE:
	case LIBUSB_ERROR_BUSY:
	case LIBUSB_ERROR_NO_MEM:
		return true;
	case LIBUSB_ERROR_NO_DEVICE:
	case LIBUSB_ERROR_NOT_FOUND:
	case LIBUSB_ERROR_ACCESS:
	case LIBUSB_ERROR_INVALID_PARAM:
		return false;
	default:
		/* Assume other errors might be recoverable */
		return true;
	}
}

/* Calculate adaptive timeout based on transfer size and retry attempt */
static unsigned int usb_calculate_timeout(size_t transfer_size, int retry_count)
{
	unsigned int base_timeout = USB_BASE_TIMEOUT_MS;
	unsigned int size_factor = (transfer_size > 65536) ? (transfer_size / 65536) : 1;
	unsigned int timeout = base_timeout * size_factor * (retry_count + 1);

	return (timeout > USB_MAX_TIMEOUT_MS) ? USB_MAX_TIMEOUT_MS : timeout;
}

/* Perform a single bulk transfer with retry logic */
static int usb_bulk_transfer_with_retry(struct qdl_device_usb *qdl_usb,
					unsigned char *data, int length,
					int *transferred)
{
	int retry_count = 0;
	int ret = 0;
	int actual = 0;
	unsigned int timeout = 0;
	bool had_retries = false;

	*transferred = 0;
	qdl_usb->total_transfers++;

	while (retry_count <= qdl_usb->max_retries) {
		timeout = usb_calculate_timeout(length, retry_count);

		ret = libusb_bulk_transfer(qdl_usb->usb_handle, qdl_usb->out_ep,
					   data, length, &actual, timeout);

		if (ret == 0) {
			*transferred = actual;
			if (had_retries) {
				qdl_usb->retried_transfers++;
				ux_device_debug(&qdl_usb->base, "USB: transfer succeeded after %d retries\n", retry_count);
			}
			return 0;
		}

		/* Check for partial transfer on timeout */
		if (ret == LIBUSB_ERROR_TIMEOUT && actual > 0) {
			*transferred = actual;
			if (had_retries) {
				qdl_usb->retried_transfers++;
			}
			ux_device_debug(&qdl_usb->base, "USB: partial transfer on timeout: %d/%d bytes\n", actual, length);
			return 0;
		}

		/* If error is not recoverable, fail immediately */
		if (!usb_is_recoverable_error(ret)) {
			ux_device_debug(&qdl_usb->base, "USB: non-recoverable error: %s\n", libusb_strerror(ret));
			break;
		}

		retry_count++;
		had_retries = true;

		if (retry_count <= qdl_usb->max_retries) {
			ux_device_debug(&qdl_usb->base, "USB: transfer failed, retry %d/%d: %s (length=%d, timeout=%ums)\n",
				 retry_count, qdl_usb->max_retries, libusb_strerror(ret), length, timeout);

			/* Exponential backoff delay */
			usleep(qdl_usb->retry_delay_ms * 1000 *
			       (1 << (retry_count - 1)) * USB_BACKOFF_MULTIPLIER);
		}
	}

	qdl_usb->failed_transfers++;
	ux_device_err(&qdl_usb->base, "USB: transfer failed after %d retries: %s (length=%d, final_timeout=%ums)\n",
	       qdl_usb->max_retries, libusb_strerror(ret), length, timeout);
	return ret;
}

/* Adjust chunk size based on transfer success/failure patterns */
static void usb_adjust_chunk_size(struct qdl_device_usb *qdl_usb, bool transfer_success)
{
	if (transfer_success) {
		/* Reset failure counter on success */
		qdl_usb->consecutive_failures = 0;

		/* Gradually increase chunk size if we've been stable */
		if (qdl_usb->adaptive_chunk_size < qdl_usb->out_chunk_size) {
			qdl_usb->adaptive_chunk_size = MIN(qdl_usb->adaptive_chunk_size * 1.5,
							   qdl_usb->out_chunk_size);
			ux_device_debug(&qdl_usb->base, "USB: increased adaptive chunk size to %zu\n",
				 qdl_usb->adaptive_chunk_size);
		}
	} else {
		/* Increase failure counter and reduce chunk size */
		qdl_usb->consecutive_failures++;

		if (qdl_usb->consecutive_failures >= USB_ADAPTIVE_THRESHOLD) {
			qdl_usb->adaptive_chunk_size = (qdl_usb->adaptive_chunk_size / 2 > USB_MIN_CHUNK_SIZE) ?
							qdl_usb->adaptive_chunk_size / 2 : USB_MIN_CHUNK_SIZE;
			ux_device_debug(&qdl_usb->base, "USB: reduced adaptive chunk size to %zu after %d failures\n",
				 qdl_usb->adaptive_chunk_size, qdl_usb->consecutive_failures);
			qdl_usb->consecutive_failures = 0; /* Reset to prevent further reductions */
		}
	}
}

static int usb_write(struct qdl_device *qdl, const void *buf, size_t len)
{
	unsigned char *data = (unsigned char *)buf;
	struct qdl_device_usb *qdl_usb = container_of(qdl, struct qdl_device_usb, base);
	unsigned int count = 0;
	size_t len_orig = len;
	int actual;
	int xfer;
	int ret;

	ux_device_debug(&qdl_usb->base, "USB: writing %zu bytes\n", len);

	while (len > 0) {
		xfer = (len > qdl_usb->adaptive_chunk_size) ? qdl_usb->adaptive_chunk_size : len;

		ret = usb_bulk_transfer_with_retry(qdl_usb, data, xfer, &actual);
		if (ret < 0) {
			usb_adjust_chunk_size(qdl_usb, false);
			ux_device_err(&qdl_usb->base, "USB: bulk write failed after retries\n");
			return -1;
		}

		/* Mark successful transfer */
		usb_adjust_chunk_size(qdl_usb, true);

		if (actual == 0) {
			ux_device_err(&qdl_usb->base, "USB: zero bytes transferred\n");
			return -1;
		}

		count += actual;
		len -= actual;
		data += actual;
		qdl_usb->total_bytes_written += actual;

		/* Add adaptive delay between chunks for multi-device scenarios */
		if (len > 0) {
			int delay_us = 1000; /* Default 1ms */

			if (g_multi_device_mode) {
				/* Increase delay based on device count and chunk size */
				delay_us = 2000 + (g_device_count * 1000);
				if (qdl_usb->adaptive_chunk_size < (DEFAULT_OUT_CHUNK_SIZE / 4)) {
					delay_us *= 2; /* Double delay for small chunks */
				}
			}

			usleep(delay_us);
		}
	}

	/* Send zero-length packet if needed */
	if (len_orig % qdl_usb->out_maxpktsize == 0) {
		ret = usb_bulk_transfer_with_retry(qdl_usb, NULL, 0, &actual);
		if (ret < 0) {
			ux_device_err(&qdl_usb->base, "USB: zero-length packet transfer failed\n");
			return -1;
		}
	}

	ux_device_debug(&qdl_usb->base, "USB: successfully wrote %d bytes\n", count);
	return count;
}

static void usb_set_out_chunk_size(struct qdl_device *qdl, long size)
{
	struct qdl_device_usb *qdl_usb = container_of(qdl, struct qdl_device_usb, base);

	qdl_usb->out_chunk_size = size;
}

struct qdl_device *usb_init(void)
{
	struct qdl_device *qdl = malloc(sizeof(struct qdl_device_usb));

	if (!qdl)
		return NULL;

	memset(qdl, 0, sizeof(struct qdl_device_usb));

	qdl->dev_type = QDL_DEVICE_USB;
	qdl->open = usb_open;
	qdl->read = usb_read;
	qdl->write = usb_write;
	qdl->close = usb_close;
	qdl->set_out_chunk_size = usb_set_out_chunk_size;
	qdl->max_payload_size = 1048576;

	return qdl;
}
