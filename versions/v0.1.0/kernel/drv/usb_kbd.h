/* NaOS USB HID boot keyboard (via XHCI). Own code. */
#ifndef NAOS_USB_KBD_H
#define NAOS_USB_KBD_H

int usb_kbd_init(void); /* enumerate first HID keyboard. 1 ready */
void usb_kbd_poll(void); /* from the timer tick: events + reports */
void usb_kbd_rescan(void); /* manual re-enumeration (`usb` command) */

#endif /* NAOS_USB_KBD_H */
