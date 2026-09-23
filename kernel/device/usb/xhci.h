/**-----------------------------------------------------------------------------

 @file    xhci.h
 @brief   Minimal xHCI host controller and USB HID pointer support

 **-----------------------------------------------------------------------------
 */
#pragma once

/* Bring up the first xHCI controller, enumerate a HID boot pointer and start a
 * kernel thread that feeds its reports to the hardware cursor. Safe to call
 * more than once (the second call does nothing). */
void usb_hid_init(void);

/* Run the bring-up from a dedicated kernel thread so a slow or stuck
 * enumeration cannot block the caller. */
void usb_hid_start(void);
