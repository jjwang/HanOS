/**-----------------------------------------------------------------------------

 @file    xhci.h
 @brief   Minimal xHCI host controller and USB HID pointer support
 @details
 @verbatim

  Declares the xHCI HID bring-up entry point.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

/* Bring up the first xHCI controller, enumerate a HID boot pointer and start a
 * kernel thread that feeds its reports to the hardware cursor. Safe to call
 * more than once (the second call does nothing). */
void usb_hid_init(void);

/* EHCI (USB 2.0) controller and its mass-storage path. usb_msc_* falls back to
 * these when the xHCI controller has no mass-storage device. */
bool ehci_init(void);
bool ehci_msc_ready(void);
uint32_t ehci_msc_sectors(void);
bool ehci_msc_read(uint32_t lba, uint32_t count, void *buf);
bool ehci_msc_write(uint32_t lba, uint32_t count, const void *buf);

/* Run the bring-up from a dedicated kernel thread so a slow or stuck
 * enumeration cannot block the caller. */
void usb_hid_start(void);

/* USB mass storage (Bulk-Only Transport). usb_msc_ready() reports whether a
 * device was enumerated and its SCSI handshake succeeded; the read/write calls
 * move 512-byte sectors through the shared bounce buffer. */
bool usb_msc_ready(void);
uint32_t usb_msc_sectors(void);
bool usb_msc_read(uint32_t lba, uint32_t count, void *buf);
bool usb_msc_write(uint32_t lba, uint32_t count, const void *buf);

/* Wait for the bring-up to finish, then report whether the device is usable.
 * Returns false on timeout or when no device was found. */
bool usb_msc_wait(uint32_t timeout_ms);

/* Wait for the bring-up thread to finish, whatever the result. */
bool usb_bringup_wait(uint32_t timeout_ms);

