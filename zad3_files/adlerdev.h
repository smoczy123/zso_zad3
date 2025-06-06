#ifndef ADLERDEV_H
#define ADLERDEV_H

/* Section 1: PCI ids. */

#define ADLERDEV_VENDOR_ID				0x0666
#define ADLERDEV_DEVICE_ID				0x0a32

/* Section 2: MMIO registers.  */

/* Interrupt status.  */
#define ADLERDEV_INTR					0x0000
/* And enable.  */
#define ADLERDEV_INTR_ENABLE				0x0004
/* The physical data pointer.  */
#define ADLERDEV_DATA_PTR				0x0008
/* How many data left to process (if non-0, device is working).  */
#define ADLERDEV_DATA_SIZE				0x000c
/* The running sum value.  */
#define ADLERDEV_SUM					0x0010
#define ADLERDEV_BAR_SIZE				0x1000

/* Section 3: misc constants.  */

#define ADLERDEV_SUM_INIT				1
#define ADLERDEV_MOD					65521

#endif
