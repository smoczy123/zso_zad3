#include "asm-generic/fcntl.h"
#include "asm/page.h"
#include "linux/dma-mapping.h"
#include "linux/fs.h"
#include "linux/mm.h"
#include "linux/mm_types.h"
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

static uint32_t dma_handle_to_page_entry(dma_addr_t dma_handle) {
	return (uint32_t)(((dma_handle >> ACCELDEV_PAGE_SHIFT) << 4) | 1);
}


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
	int buffers[ACCELDEV_NUM_BUFFERS];
};

struct acceldev_buffer_data {
	void* page_table_cpu;
	dma_addr_t page_table_dma;
	size_t buffer_size;
	dma_addr_t* pages_dma;
	void** pages_cpu;
	struct pci_dev *pdev; 	
};

static void dealloc_page_table(struct acceldev_buffer_data *buf_data, struct pci_dev *pdev) {
	int i = 0;
	if (buf_data->page_table_cpu) {
		uint32_t *page_table = (uint32_t *)buf_data->page_table_cpu;
		for (i = 0; i < 1024; i++) {
   			if (page_table[i] && 1) {
				dma_free_coherent(&(pdev->dev), ACCELDEV_PAGE_SIZE, buf_data->pages_cpu[i], buf_data->pages_dma[i]);
   			} else {
				break;
			}
  		}
	  	dma_free_coherent(&(pdev->dev), 1024 * 32, buf_data->page_table_cpu, buf_data->page_table_dma);		
	}
}

static int alloc_page_table(struct acceldev_buffer_data *buf_data, struct pci_dev *pdev) {
	int err;
	if ((err = dma_set_mask_and_coherent(&(pdev->dev), DMA_BIT_MASK(64))))
		goto out_mask;
	buf_data->page_table_cpu = dma_alloc_coherent(&(pdev->dev), 1024 * 32, &buf_data->page_table_dma, GFP_KERNEL);
	uint32_t *page_table = (uint32_t *)buf_data->page_table_cpu;
	memset(buf_data->page_table_cpu, 0, 1024 * 32);
	if (!buf_data->page_table_cpu) {
  		err = -ENOMEM;
  		goto out_alloc;
 	}
	int page_count = buf_data->buffer_size / ACCELDEV_PAGE_SIZE;
	page_count += (buf_data->buffer_size % ACCELDEV_PAGE_SIZE) ? 1 : 0;
	buf_data->pages_dma = kmalloc(sizeof(dma_addr_t) * page_count, GFP_KERNEL);
	if (!buf_data->pages_dma) {
	 	err = -ENOMEM;
		goto out_alloc;
 	}	
	buf_data->pages_cpu = kmalloc(sizeof(void*) * page_count, GFP_KERNEL);
	if (!buf_data->pages_cpu) {
   		err = -ENOMEM;
   		goto out_alloc;
  	}	
	unsigned long allocated = 0;
	if ((err = dma_set_mask_and_coherent(&(pdev->dev), DMA_BIT_MASK(40))))
		goto out_alloc;
	for (int i = 0; i < page_count; i++) {
		buf_data->pages_cpu[i] = dma_alloc_coherent(&(pdev->dev), ACCELDEV_PAGE_SIZE, &buf_data->pages_dma[i], GFP_KERNEL);
		if (!buf_data->pages_cpu[i]) {
			err = -ENOMEM;
			goto out_alloc;
		}
		memset(buf_data->pages_cpu[i], 0, ACCELDEV_PAGE_SIZE);
		allocated += ACCELDEV_PAGE_SIZE;
		page_table[i] = dma_handle_to_page_entry(buf_data->pages_dma[i]);
	}
	dma_set_mask_and_coherent(&(pdev->dev), DMA_BIT_MASK(64));

	return 0;


out_alloc:
 	dealloc_page_table(buf_data, pdev);
out_mask:		
	dma_set_mask_and_coherent(&(pdev->dev), DMA_BIT_MASK(64));
	return err;

}



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



static vm_fault_t buffer_fault(struct vm_fault *vmf) {
	struct acceldev_buffer_data* buf = (struct acceldev_buffer_data*)(vmf->vma->vm_private_data);
	unsigned long offset = vmf->pgoff << ACCELDEV_PAGE_SHIFT;
	if (offset >= buf->buffer_size) {
		return VM_FAULT_SIGBUS; // Out of bounds access
	}
	unsigned long page_index = offset / ACCELDEV_PAGE_SIZE;
	void* page_addr = buf->pages_cpu[page_index];
	struct page* page = virt_to_page(page_addr);
	if (!page) 
  		return VM_FAULT_SIGBUS; // Page not found
	get_page(page);
	vmf->page = page;
	return 0;
}

static const struct vm_operations_struct buffer_vm_ops = {
 .fault = buffer_fault,
};

static int buffer_release(struct inode *inode, struct file *filp) {
	// TODO: WAIT FOR CONTEXT TO FINISH
 	struct acceldev_buffer_data* buf_data = (struct acceldev_buffer_data*)filp->private_data;
 	struct pci_dev *pdev = buf_data->pdev;
	if (buf_data) {
  		dealloc_page_table(buf_data, pdev); // Assuming pdev is not needed here
  		kfree(buf_data->pages_dma);
  		kfree(buf_data->pages_cpu);
  		kfree(buf_data);
 	}
 	return 0;
}

static int buffer_mmap(struct file *filp, struct vm_area_struct *vma) {
	
	vma->vm_private_data = filp->private_data;
	vma->vm_ops = &buffer_vm_ops;
	return 0;
}

static const struct file_operations acceldev_buffer_fops = {
 .owner = THIS_MODULE,
 .mmap = buffer_mmap,
 .release = buffer_release,
};

static int create_buffer(int size, struct pci_dev *pdev, enum acceldev_buffer_type type, struct acceldev_context *ctx, struct acceldev_ioctl_create_buffer_result *result) {
	int err;
	struct acceldev_buffer_data *buf_data = kmalloc(sizeof(struct acceldev_buffer_data), GFP_KERNEL);
	if (!buf_data) {
  		return -ENOMEM;
 	}
	buf_data->buffer_size = size;
	buf_data->pdev = pdev;
	if ((err = alloc_page_table(buf_data, pdev)) < 0) {
		kfree(buf_data);
		return err;
	}

	int bfd = anon_inode_getfd(ACCELDEV_NAME, &acceldev_buffer_fops, buf_data, O_RDWR);
	if (bfd < 0) {
  		dealloc_page_table(buf_data, pdev);
		kfree(buf_data->pages_dma);
  		kfree(buf_data->pages_cpu);
  		kfree(buf_data);
 	}
	if (type == BUFFER_TYPE_DATA) {
		for (int i = 0; i < ACCELDEV_NUM_BUFFERS; i++) {
			if (ctx->buffers[i] == 0) {
				ctx->buffers[i] = bfd;
				struct acceldev_ioctl_create_buffer_result res = {.buffer_slot = i};
				if (copy_to_user(result, &res, sizeof(struct acceldev_ioctl_create_buffer_result))) {
					dealloc_page_table(buf_data, pdev);
					kfree(buf_data->pages_dma);
  					kfree(buf_data->pages_cpu);
					kfree(buf_data);
					return -EFAULT; // Failed to copy data to user space
				}
				break;
			}
		}
	}
	return bfd;

}

/* Main device node handling.  */

static long acceldev_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
	switch (cmd) {
		case ACCELDEV_IOCTL_CREATE_BUFFER: {
			struct acceldev_ioctl_create_buffer* create_buf = kmalloc(sizeof(struct acceldev_ioctl_create_buffer), GFP_KERNEL);
			if (copy_from_user(create_buf, (void __user *)arg, sizeof(struct acceldev_ioctl_create_buffer))) {
				kfree(create_buf);
				return -EFAULT; // Failed to copy data from user space
			}
			
			if (!create_buf || 
				create_buf->size <= 0 || 
				create_buf->size > ACCELDEV_BUFFER_MAX_SIZE || 
				create_buf->type >= BUFFER_TYPES_COUNT) {
					kfree(create_buf);
				return -EINVAL; // Invalid arguments
			}
			struct acceldev_context *ctx = file->private_data;
			if (create_buf->type == BUFFER_TYPE_DATA) {
				int i;
				for (i = 0; i < ACCELDEV_NUM_BUFFERS; i++) {
					if (ctx->buffers[i] == 0) {
						break;
					}
				}
				if (i == ACCELDEV_NUM_BUFFERS) {
					kfree(create_buf);
					return -ENOSPC;
				}
			}

			int size = create_buf->size;
			enum acceldev_buffer_type type = create_buf->type;
			int bfd = create_buffer(size, ctx->dev->pdev, type, ctx, create_buf->result);
			kfree(create_buf);
			return bfd;
		}

		case ACCELDEV_IOCTL_RUN: {
			struct acceldev_ioctl_run* run_cmd = (struct acceldev_ioctl_run*)arg;
			if (!run_cmd ||
				run_cmd->cfd < 0 || 
				run_cmd->addr < 0 || 
				run_cmd->size <= 0 || 
				run_cmd->addr > ACCELDEV_BUFFER_MAX_SIZE ||
				run_cmd->size > ACCELDEV_BUFFER_MAX_SIZE - run_cmd->addr || 
				run_cmd->addr % ACCELDEV_USER_CMD_WORDS * sizeof(uint32_t) != 0 ||
				run_cmd->size % ACCELDEV_USER_CMD_WORDS * sizeof(uint32_t) != 0) {

				return -EINVAL; // Invalid arguments
			}


			return 0;
		}

		case ACCELDEV_IOCTL_WAIT: {



			return 0;
		}
		
	}

	return -ENOTTY; // Not a valid ioctl command
}

static int acceldev_open(struct inode *inode, struct file *file)
{
	struct acceldev_device *dev = container_of(inode->i_cdev, struct acceldev_device, cdev);
	struct acceldev_context *ctx = kmalloc(sizeof(struct acceldev_context), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	for (int i = 0; i < ACCELDEV_NUM_BUFFERS; i++) {
		ctx->buffers[i] = 0; // Initialize all buffer slots to 0		
	}
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
	.unlocked_ioctl = acceldev_ioctl,
	.compat_ioctl = acceldev_ioctl,
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