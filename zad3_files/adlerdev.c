#include <linux/module.h>
#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/anon_inodes.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/file.h>
#include <linux/kref.h>
#include <linux/interrupt.h>

#include "adlerdev.h"

#define ADLERDEV_MAX_DEVICES 256
#define ADLERDEV_NUM_BUFFERS 16

MODULE_LICENSE("GPL");

struct adlerdev_buffer {
	struct list_head lh;
	struct adlerdev_context *ctx;
	void *data_cpu;
	dma_addr_t data_dma;
	size_t fill_size;
};

struct adlerdev_context {
	struct adlerdev_device *dev;
	int pending_buffers;
	wait_queue_head_t wq;
	uint32_t sum;
};

struct adlerdev_device {
	struct pci_dev *pdev;
	struct cdev cdev;
	int idx;
	struct device *dev;
	void __iomem *bar;
	spinlock_t slock;
	struct list_head buffers_free;
	struct list_head buffers_running;
	wait_queue_head_t free_wq;
	wait_queue_head_t idle_wq;
};

static dev_t adlerdev_devno;
static struct adlerdev_device *adlerdev_devices[ADLERDEV_MAX_DEVICES];
static DEFINE_MUTEX(adlerdev_devices_lock);
static struct class adlerdev_class = {
	.name = "adlerdev",
	.owner = THIS_MODULE,
};

/* Hardware handling. */

static inline void adlerdev_iow(struct adlerdev_device *dev, uint32_t reg, uint32_t val)
{
	iowrite32(val, dev->bar + reg);
//	printk(KERN_ALERT "adlerdev %03x <- %08x\n", reg, val);
}

static inline uint32_t adlerdev_ior(struct adlerdev_device *dev, uint32_t reg)
{
	uint32_t res = ioread32(dev->bar + reg);
//	printk(KERN_ALERT "adlerdev %03x -> %08x\n", reg, res);
	return res;
}

/* IRQ handler.  */

static irqreturn_t adlerdev_isr(int irq, void *opaque)
{
	struct adlerdev_device *dev = opaque;
	unsigned long flags;
	uint32_t istatus;
	struct adlerdev_buffer *buf;
	spin_lock_irqsave(&dev->slock, flags);
//	printk(KERN_ALERT "adlerdev isr\n");
	istatus = adlerdev_ior(dev, ADLERDEV_INTR) & adlerdev_ior(dev, ADLERDEV_INTR_ENABLE);
	if (istatus) {
		adlerdev_iow(dev, ADLERDEV_INTR, istatus);
		BUG_ON(list_empty(&dev->buffers_running));
		buf = list_entry(dev->buffers_running.next, struct adlerdev_buffer, lh);
		list_del(&buf->lh);
		buf->ctx->pending_buffers--;
		if (!buf->ctx->pending_buffers)
			wake_up(&buf->ctx->wq);
		buf->ctx->sum = adlerdev_ior(dev, ADLERDEV_SUM);
		buf->ctx = 0;
		list_add(&buf->lh, &dev->buffers_free);
		wake_up(&dev->free_wq);
		if (list_empty(&dev->buffers_running)) {
			/* No more buffers to run.  */
			wake_up(&dev->idle_wq);
		} else {
			/* Run the next buffer.  */
			buf = list_entry(dev->buffers_running.next, struct adlerdev_buffer, lh);
			adlerdev_iow(dev, ADLERDEV_DATA_PTR, buf->data_dma);
			adlerdev_iow(dev, ADLERDEV_SUM, buf->ctx->sum);
			adlerdev_iow(dev, ADLERDEV_DATA_SIZE, buf->fill_size);
		}
	}
	spin_unlock_irqrestore(&dev->slock, flags);
	return IRQ_RETVAL(istatus);
}

/* Main device node handling.  */

static int adlerdev_open(struct inode *inode, struct file *file)
{
	struct adlerdev_device *dev = container_of(inode->i_cdev, struct adlerdev_device, cdev);
	struct adlerdev_context *ctx = kzalloc(sizeof *ctx, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->dev = dev;
	init_waitqueue_head(&ctx->wq);
	ctx->sum = ADLERDEV_SUM_INIT;
	ctx->pending_buffers = 0;
	file->private_data = ctx;
	return nonseekable_open(inode, file);
}

static int adlerdev_release(struct inode *inode, struct file *file)
{
	struct adlerdev_context *ctx = file->private_data;
	struct adlerdev_device *dev = ctx->dev;
	unsigned long flags;
	spin_lock_irqsave(&dev->slock, flags);
	while (ctx->pending_buffers) {
		spin_unlock_irqrestore(&dev->slock, flags);
		wait_event(ctx->wq, !ctx->pending_buffers);
		spin_lock_irqsave(&dev->slock, flags);
	}
	spin_unlock_irqrestore(&dev->slock, flags);
	kfree(ctx);
	return 0;
}

static ssize_t adlerdev_write(struct file *file, const char __user *buf,
		size_t len, loff_t *off)
{
	struct adlerdev_context *ctx = file->private_data;
	struct adlerdev_device *dev = ctx->dev;
	ssize_t res = 0;
	unsigned long flags;
	struct adlerdev_buffer *abuf;
	while (len) {
		size_t clen = min(len, PAGE_SIZE);
//		printk(KERN_ALERT "adlerdev write %zu\n", clen);
		/* Get a free buffer.  */
		spin_lock_irqsave(&dev->slock, flags);
		while (list_empty(&dev->buffers_free)) {
			spin_unlock_irqrestore(&dev->slock, flags);
			if (wait_event_interruptible(dev->free_wq, !list_empty(&dev->buffers_free)))
				return res ? res : -ERESTARTSYS;
			spin_lock_irqsave(&dev->slock, flags);
		}
		abuf = list_entry(dev->buffers_free.next, struct adlerdev_buffer, lh);
		list_del(&abuf->lh);
		spin_unlock_irqrestore(&dev->slock, flags);
		/* Got buffer, fill it.  */
		if (copy_from_user(abuf->data_cpu, buf, clen)) {
			/* Oops.  Put it back.  */
			spin_lock_irqsave(&dev->slock, flags);
			list_add(&abuf->lh, &dev->buffers_free);
			spin_unlock_irqrestore(&dev->slock, flags);
			return res ? res : -EFAULT;
		}
		abuf->fill_size = clen;
		abuf->ctx = ctx;
		ctx->pending_buffers++;
		/* Submit it.  */
		spin_lock_irqsave(&dev->slock, flags);
		if (list_empty(&dev->buffers_running)) {
			adlerdev_iow(dev, ADLERDEV_DATA_PTR, abuf->data_dma);
			adlerdev_iow(dev, ADLERDEV_SUM, abuf->ctx->sum);
			adlerdev_iow(dev, ADLERDEV_DATA_SIZE, abuf->fill_size);
		}
		list_add_tail(&abuf->lh, &dev->buffers_running);
		spin_unlock_irqrestore(&dev->slock, flags);
		/* And next chunk.  */
		res += clen;
		buf += clen;
		len -= clen;
	}
	return res;
}

static ssize_t adlerdev_read(struct file *file, char __user *buf,
		size_t len, loff_t *off)
{
	struct adlerdev_context *ctx = file->private_data;
	struct adlerdev_device *dev = ctx->dev;
	uint32_t sum;
	unsigned long flags;
	if (len != 4)
		return -EINVAL;
	spin_lock_irqsave(&dev->slock, flags);
	while (ctx->pending_buffers) {
		spin_unlock_irqrestore(&dev->slock, flags);
		if (wait_event_interruptible(ctx->wq, !ctx->pending_buffers))
			return -ERESTARTSYS;
		spin_lock_irqsave(&dev->slock, flags);
	}
	sum = ctx->sum;
	spin_unlock_irqrestore(&dev->slock, flags);
	if (copy_to_user(buf, &sum, 4))
		return -EFAULT;
	return 4;
}

static const struct file_operations adlerdev_file_ops = {
	.owner = THIS_MODULE,
	.open = adlerdev_open,
	.release = adlerdev_release,
	.write = adlerdev_write,
	.read = adlerdev_read,
};

/* PCI driver.  */

static int adlerdev_probe(struct pci_dev *pdev,
	const struct pci_device_id *pci_id)
{
	int err, i;
	struct list_head *lh, *tmp;

	/* Allocate our structure.  */
	struct adlerdev_device *dev = kzalloc(sizeof *dev, GFP_KERNEL);
	if (!dev) {
		err = -ENOMEM;
		goto out_alloc;
	}
	pci_set_drvdata(pdev, dev);
	dev->pdev = pdev;

	/* Locks etc.  */
	spin_lock_init(&dev->slock);
	init_waitqueue_head(&dev->free_wq);
	init_waitqueue_head(&dev->idle_wq);
	INIT_LIST_HEAD(&dev->buffers_free);
	INIT_LIST_HEAD(&dev->buffers_running);

	/* Allocate a free index.  */
	mutex_lock(&adlerdev_devices_lock);
	for (i = 0; i < ADLERDEV_MAX_DEVICES; i++)
		if (!adlerdev_devices[i])
			break;
	if (i == ADLERDEV_MAX_DEVICES) {
		err = -ENOSPC; // XXX right?
		mutex_unlock(&adlerdev_devices_lock);
		goto out_slot;
	}
	adlerdev_devices[i] = dev;
	dev->idx = i;
	mutex_unlock(&adlerdev_devices_lock);

	/* Enable hardware resources.  */
	if ((err = pci_enable_device(pdev)))
		goto out_enable;

	if ((err = pci_set_dma_mask(pdev, DMA_BIT_MASK(32))))
		goto out_mask;
	if ((err = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32))))
		goto out_mask;
	pci_set_master(pdev);

	if ((err = pci_request_regions(pdev, "adlerdev")))
		goto out_regions;

	/* Map the BAR.  */
	if (!(dev->bar = pci_iomap(pdev, 0, 0))) {
		err = -ENOMEM;
		goto out_bar;
	}

	/* Connect the IRQ line.  */
	if ((err = request_irq(pdev->irq, adlerdev_isr, IRQF_SHARED, "adlerdev", dev)))
		goto out_irq;

	/* Allocate some buffers.  */
	for (i = 0; i < ADLERDEV_NUM_BUFFERS; i++) {
		struct adlerdev_buffer *buf = kmalloc(sizeof *buf, GFP_KERNEL);
		if (!buf)
			goto out_cdev;
		if (!(buf->data_cpu = dma_alloc_coherent(&dev->pdev->dev,
				PAGE_SIZE,
				&buf->data_dma, GFP_KERNEL))) {
			kfree(buf);
			goto out_cdev;
		}
		buf->ctx = 0;
		list_add(&buf->lh, &dev->buffers_free);
	}

	adlerdev_iow(dev, ADLERDEV_INTR, 1);
	adlerdev_iow(dev, ADLERDEV_INTR_ENABLE, 1);
	
	/* We're live.  Let's export the cdev.  */
	cdev_init(&dev->cdev, &adlerdev_file_ops);
	if ((err = cdev_add(&dev->cdev, adlerdev_devno + dev->idx, 1)))
		goto out_cdev;

	/* And register it in sysfs.  */
	dev->dev = device_create(&adlerdev_class,
			&dev->pdev->dev, adlerdev_devno + dev->idx, dev,
			"adler%d", dev->idx);
	if (IS_ERR(dev->dev)) {
		printk(KERN_ERR "adlerdev: failed to register subdevice\n");
		/* too bad. */
		dev->dev = 0;
	}

	return 0;

out_cdev:
	adlerdev_iow(dev, ADLERDEV_INTR_ENABLE, 0);
	list_for_each_safe(lh, tmp, &dev->buffers_free) {
		struct adlerdev_buffer *buf = list_entry(lh, struct adlerdev_buffer, lh);
		dma_free_coherent(&dev->pdev->dev, PAGE_SIZE, buf->data_cpu, buf->data_dma);
		kfree(buf);
	}
	free_irq(pdev->irq, dev);
out_irq:
	pci_iounmap(pdev, dev->bar);
out_bar:
	pci_release_regions(pdev);
out_regions:
out_mask:
	pci_disable_device(pdev);
out_enable:
	mutex_lock(&adlerdev_devices_lock);
	adlerdev_devices[dev->idx] = 0;
	mutex_unlock(&adlerdev_devices_lock);
out_slot:
	kfree(dev);
out_alloc:
	return err;
}

static void adlerdev_remove(struct pci_dev *pdev)
{
	struct list_head *lh, *tmp;
	struct adlerdev_device *dev = pci_get_drvdata(pdev);
	if (dev->dev) {
		device_destroy(&adlerdev_class, adlerdev_devno + dev->idx);
	}
	cdev_del(&dev->cdev);
	adlerdev_iow(dev, ADLERDEV_INTR_ENABLE, 0);
	list_for_each_safe(lh, tmp, &dev->buffers_free) {
		struct adlerdev_buffer *buf = list_entry(lh, struct adlerdev_buffer, lh);
		dma_free_coherent(&dev->pdev->dev, PAGE_SIZE, buf->data_cpu, buf->data_dma);
		kfree(buf);
	}
	free_irq(pdev->irq, dev);
	pci_iounmap(pdev, dev->bar);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	mutex_lock(&adlerdev_devices_lock);
	adlerdev_devices[dev->idx] = 0;
	mutex_unlock(&adlerdev_devices_lock);
	kfree(dev);
}

static int adlerdev_suspend(struct pci_dev *pdev, pm_message_t state)
{
	unsigned long flags;
	struct adlerdev_device *dev = pci_get_drvdata(pdev);
	spin_lock_irqsave(&dev->slock, flags);
	while (list_empty(&dev->buffers_free)) {
		spin_unlock_irqrestore(&dev->slock, flags);
		wait_event(dev->idle_wq, !list_empty(&dev->buffers_free));
		spin_lock_irqsave(&dev->slock, flags);
	}
	spin_unlock_irqrestore(&dev->slock, flags);
	adlerdev_iow(dev, ADLERDEV_INTR_ENABLE, 0);
	return 0;
}

static int adlerdev_resume(struct pci_dev *pdev)
{
	struct adlerdev_device *dev = pci_get_drvdata(pdev);
	adlerdev_iow(dev, ADLERDEV_INTR, 1);
	adlerdev_iow(dev, ADLERDEV_INTR_ENABLE, 1);
	return 0;
}

static struct pci_device_id adlerdev_pciids[] = {
	{ PCI_DEVICE(ADLERDEV_VENDOR_ID, ADLERDEV_DEVICE_ID) },
	{ 0 }
};

static struct pci_driver adlerdev_pci_driver = {
	.name = "adlerdev",
	.id_table = adlerdev_pciids,
	.probe = adlerdev_probe,
	.remove = adlerdev_remove,
	.suspend = adlerdev_suspend,
	.resume = adlerdev_resume,
};

/* Init & exit.  */

static int adlerdev_init(void)
{
	int err;
	if ((err = alloc_chrdev_region(&adlerdev_devno, 0, ADLERDEV_MAX_DEVICES, "adlerdev")))
		goto err_chrdev;
	if ((err = class_register(&adlerdev_class)))
		goto err_class;
	if ((err = pci_register_driver(&adlerdev_pci_driver)))
		goto err_pci;
	return 0;

err_pci:
	class_unregister(&adlerdev_class);
err_class:
	unregister_chrdev_region(adlerdev_devno, ADLERDEV_MAX_DEVICES);
err_chrdev:
	return err;
}

static void adlerdev_exit(void)
{
	pci_unregister_driver(&adlerdev_pci_driver);
	class_unregister(&adlerdev_class);
	unregister_chrdev_region(adlerdev_devno, ADLERDEV_MAX_DEVICES);
}

module_init(adlerdev_init);
module_exit(adlerdev_exit);
