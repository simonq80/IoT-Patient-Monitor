/* usb_descriptor.h - header for common device descriptor */

/*
 * Copyright (c) 2017 PHYTEC Messtechnik GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __USB_DESCRIPTOR_H__
#define __USB_DESCRIPTOR_H__

#ifdef CONFIG_USB_CDC_ACM
#define NUMOF_IFACES_CDC_ACM		2
#define NUMOF_ENDPOINTS_CDC_ACM		3
#else
#define NUMOF_IFACES_CDC_ACM		0
#define NUMOF_ENDPOINTS_CDC_ACM		0
#endif

#ifdef CONFIG_USB_MASS_STORAGE
#define NUMOF_IFACES_MASS		1
#define NUMOF_ENDPOINTS_MASS		2
#else
#define NUMOF_IFACES_MASS		0
#define NUMOF_ENDPOINTS_MASS		0
#endif

#define NUMOF_IFACES	(NUMOF_IFACES_CDC_ACM + NUMOF_IFACES_MASS)
#define NUMOF_ENDPOINTS	(NUMOF_ENDPOINTS_CDC_ACM + NUMOF_ENDPOINTS_MASS)

#define FIRST_IFACE_CDC_ACM		0
#define FIRST_IFACE_MASS_STORAGE	NUMOF_IFACES_CDC_ACM

#define MFR_DESC_LENGTH		(sizeof(CONFIG_USB_DEVICE_MANUFACTURER) * 2)
#define MFR_UC_IDX_MAX		(MFR_DESC_LENGTH - 3)
#define MFR_STRING_IDX_MAX	(sizeof(CONFIG_USB_DEVICE_MANUFACTURER) - 2)

#define PRODUCT_DESC_LENGTH	(sizeof(CONFIG_USB_DEVICE_PRODUCT) * 2)
#define PRODUCT_UC_IDX_MAX	(PRODUCT_DESC_LENGTH - 3)
#define PRODUCT_STRING_IDX_MAX	(sizeof(CONFIG_USB_DEVICE_PRODUCT) - 2)

#define SN_DESC_LENGTH		(sizeof(CONFIG_USB_DEVICE_SN) * 2)
#define SN_UC_IDX_MAX		(SN_DESC_LENGTH - 3)
#define SN_STRING_IDX_MAX	(sizeof(CONFIG_USB_DEVICE_SN) - 2)

void usb_fix_unicode_string(int idx_max, int asci_idx_max, u8_t *buf);
u8_t *usb_get_device_descriptor(void);

#endif /* __USB_DESCRIPTOR_H__ */
