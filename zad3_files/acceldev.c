#include "linux/dma-mapping.h"
#include "linux/slab.h"
#include "linux/types.h"
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/anon_inodes.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/file.h>
#include <linux/kref.h>
#include <linux/interrupt.h>
#include <linux/string.h>

#include "acceldev.h"

#define ACCELDEV_MAX_DEVICES 256
#define ACCELDEV_NUM_BUFFERS 16

MODULE_LICENSE("GPL");




struct acceldev_device {
	struct pci_dev *pdev;
	struct cdev cdev;
	int idx;
	struct device *dev;
	void __iomem *bar;
	spinlock_t slock;
    struct acceldev_context *ctx[ACCELDEV_MAX_CONTEXTS];
    dma_addr_t contexts_config_dma;
    void *contexts_config_cpu;
};

struct acceldev_context {
    struct acceldev_device *dev;
};



static dev_t acceldev_devno;

static struct acceldev_device *acceldev_devices[ACCELDEV_MAX_DEVICES];
static DEFINE_MUTEX(acceldev_devices_lock);
static struct class acceldev_class = {
	.name = ACCELDEV_NAME,
};


/* Hardware handling. */

static inline void acceldev_iow(struct acceldev_device *dev, uint32_t reg, uint32_t val)
{
	iowrite32(val, dev->bar + reg);
//	printk(KERN_ALERT "acceldev %03x <- %08x\n", reg, val);
}

static inline uint32_t acceldev_ior(struct acceldev_device *dev, uint32_t reg)
{
	uint32_t res = ioread32(dev->bar + reg);
//	printk(KERN_ALERT "acceldev %03x -> %08x\n", reg, res);
	return res;
}

static irqreturn_t acceldev_isr(int irq, void *opaque)
{
	
	return IRQ_RETVAL(0);
}

/* Main device node handling.  */

static int acceldev_open(struct inode *inode, struct file *file)
{
	struct acceldev_device *dev = container_of(inode->i_cdev, struct acceldev_device, cdev);
	struct acceldev_context *ctx = kzalloc(sizeof *ctx, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
    int i = 0;
	for (; i < ACCELDEV_MAX_CONTEXTS; i++) {
        if (!dev->ctx[i]) {
            dev->ctx[i] = ctx;
            break;
        }
    }
    if (i == ACCELDEV_MAX_CONTEXTS) {
        kfree(ctx);
        return -EINVAL;
    }
    ctx->dev = dev;
    file->private_data = ctx;
	return nonseekable_open(inode, file);
}

static int acceldev_release(struct inode *inode, struct file *file)
{
	struct acceldev_context *ctx = file->private_data;
	struct acceldev_device *dev = ctx->dev;
    for (int i = 0; i < ACCELDEV_MAX_CONTEXTS; i++) {
        if (dev->ctx[i] == ctx) {
            dev->ctx[i] = NULL;
            break;
        }
    }
	kfree(ctx);
	return 0;
}

static const struct file_operations acceldev_file_ops = {
	.owner = THIS_MODULE,
	.open = acceldev_open,
	.release = acceldev_release,
}; 


static int acceldev_probe(struct pci_dev *pdev,
	const struct pci_device_id *pci_id)
{
	int err, i;

	/* Allocate our structure.  */
	struct acceldev_device *dev = kzalloc(sizeof *dev, GFP_KERNEL);
	if (!dev) {
		err = -ENOMEM;
		goto out_alloc;
	}
	pci_set_drvdata(pdev, dev);
	dev->pdev = pdev;

	/* Locks etc.  */
	spin_lock_init(&dev->slock);

	/* Allocate a free index.  */
	mutex_lock(&acceldev_devices_lock);
	for (i = 0; i < ACCELDEV_MAX_DEVICES; i++)
		if (!acceldev_devices[i])
			break;
	if (i == ACCELDEV_MAX_DEVICES) {
		err = -ENOSPC; // XXX right?
		mutex_unlock(&acceldev_devices_lock);
		goto out_slot;
	}
	acceldev_devices[i] = dev;
	dev->idx = i;
	mutex_unlock(&acceldev_devices_lock);

	/* Enable hardware resources.  */
	if ((err = pci_enable_device(pdev)))
		goto out_enable;

	if ((err = dma_set_mask_and_coherent(&(pdev->dev), DMA_BIT_MASK(64))))
		goto out_mask;
	pci_set_master(pdev);

	if ((err = pci_request_regions(pdev, "acceldev")))
		goto out_regions;

	/* Map the BAR.  */
	if (!(dev->bar = pci_iomap(pdev, 0, 0))) {
		err = -ENOMEM;
		goto out_bar;
	}

	/* Connect the IRQ line.  */
	if ((err = request_irq(pdev->irq, acceldev_isr, IRQF_SHARED, ACCELDEV_NAME, dev)))
		goto out_irq;

	acceldev_iow(dev, ACCELDEV_INTR, 1);
	acceldev_iow(dev, ACCELDEV_INTR_ENABLE, ACCELDEV_INTR_FENCE_WAIT | ACCELDEV_INTR_FEED_ERROR |
            ACCELDEV_INTR_CMD_ERROR | ACCELDEV_INTR_MEM_ERROR |
            ACCELDEV_INTR_SLOT_ERROR | ACCELDEV_INTR_USER_FENCE_WAIT);

    dev->contexts_config_cpu = dma_alloc_coherent(&pdev->dev, ACCELDEV_MAX_CONTEXTS * sizeof(struct acceldev_context_on_device_config), &dev->contexts_config_dma, GFP_KERNEL);
    if (!dev->contexts_config_cpu) {
        err = -ENOMEM;
        goto out_cdev;
    }
	memset(dev->contexts_config_cpu, 0, ACCELDEV_MAX_CONTEXTS * sizeof(struct acceldev_context_on_device_config));

    acceldev_iow(dev, ACCELDEV_CONTEXTS_CONFIGS, dev->contexts_config_dma & 0xFFFFFFFF);  
    acceldev_iow(dev, ACCELDEV_CONTEXTS_CONFIGS + 4, dev->contexts_config_dma >> 32);
    
	
	/* We're live.  Let's export the cdev.  */
	cdev_init(&dev->cdev, &acceldev_file_ops);
	if ((err = cdev_add(&dev->cdev, acceldev_devno + dev->idx, 1)))
		goto out_cdev;

	/* And register it in sysfs.  */
	dev->dev = device_create(&acceldev_class,
			&dev->pdev->dev, acceldev_devno + dev->idx, dev,
			"acceldev%d", dev->idx);
	if (IS_ERR(dev->dev)) {
		printk(KERN_ERR "acceldev: failed to register subdevice\n");
		/* too bad. */
		dev->dev = 0;
	}

	return 0;

out_cdev:
	acceldev_iow(dev, ACCELDEV_INTR_ENABLE, 0);
	free_irq(pdev->irq, dev);
out_irq:
	pci_iounmap(pdev, dev->bar);
out_bar:
	pci_release_regions(pdev);
out_regions:
out_mask:
	pci_disable_device(pdev);
out_enable:
	mutex_lock(&acceldev_devices_lock);
	acceldev_devices[dev->idx] = 0;
	mutex_unlock(&acceldev_devices_lock);
out_slot:
	kfree(dev);
out_alloc:
	return err;
}

static void acceldev_remove(struct pci_dev *pdev)
{
	struct acceldev_device *dev = pci_get_drvdata(pdev);
	if (dev->dev) {
		device_destroy(&acceldev_class, acceldev_devno + dev->idx);
	}
	cdev_del(&dev->cdev);
	acceldev_iow(dev, ACCELDEV_INTR_ENABLE, 0);
	for (int i = 0; i < ACCELDEV_MAX_CONTEXTS; i++) {
        if (dev->ctx[i]) {
            struct acceldev_context *ctx = dev->ctx[i];
            kfree(ctx);
        }
    }
    dma_free_coherent(&dev->pdev->dev, ACCELDEV_MAX_CONTEXTS * sizeof(struct acceldev_context), dev->contexts_config_cpu, dev->contexts_config_dma);
	free_irq(pdev->irq, dev);
	pci_iounmap(pdev, dev->bar);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	mutex_lock(&acceldev_devices_lock);
	acceldev_devices[dev->idx] = 0;
	mutex_unlock(&acceldev_devices_lock);
	kfree(dev);
}

static struct pci_device_id acceldev_pciids[] = {
	{ PCI_DEVICE(ACCELDEV_VENDOR_ID, ACCELDEV_DEVICE_ID) },
	{ 0 }
};


static struct pci_driver acceldev_pci_driver = {
	.name = ACCELDEV_NAME,
	.id_table = acceldev_pciids,
	.probe = acceldev_probe,
	.remove = acceldev_remove,
};


static int acceldev_init(void)
{
	int err;
	if ((err = alloc_chrdev_region(&acceldev_devno, 0, ACCELDEV_MAX_DEVICES, "acceldev")))
		goto err_chrdev;
	if ((err = class_register(&acceldev_class)))
		goto err_class;
	if ((err = pci_register_driver(&acceldev_pci_driver)))
		goto err_pci;
	return 0;

err_pci:
	class_unregister(&acceldev_class);
err_class:
	unregister_chrdev_region(acceldev_devno, ACCELDEV_MAX_DEVICES);
err_chrdev:
	return err;
}

static void acceldev_exit(void)
{
	pci_unregister_driver(&acceldev_pci_driver);
	class_unregister(&acceldev_class);
	unregister_chrdev_region(acceldev_devno, ACCELDEV_MAX_DEVICES);
}

module_init(acceldev_init);
module_exit(acceldev_exit);