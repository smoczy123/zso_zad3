#include "asm-generic/fcntl.h"
#include "asm/page.h"
#include "linux/dma-mapping.h"
#include "linux/fdtable.h"
#include "linux/fs.h"
#include "linux/gfp_types.h"
#include "linux/list.h"
#include "linux/mm.h"
#include "linux/mm_types.h"
#include "linux/printk.h"
#include "linux/slab.h"
#include "linux/spinlock.h"
#include "linux/spinlock_types.h"
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
    struct acceldev_context_on_device_config* contexts_config_cpu;
	struct list_head cmd_queue;
	uint32_t dev_fence_counter;
	wait_queue_head_t user_waits; 
};

struct acceldev_context {
    struct acceldev_device *dev;
	int buffers[ACCELDEV_NUM_BUFFERS];
	spinlock_t ctx_lock;
	int ctx_idx;
	uint32_t fence_counter;
	uint8_t status;
};

struct command {
	uint32_t cmd[5];
	struct list_head lh;
};

struct acceldev_buffer_data {
	void* page_table_cpu;
	dma_addr_t page_table_dma;
	size_t buffer_size;
	dma_addr_t* pages_dma;
	enum acceldev_buffer_type type;
	void** pages_cpu;
	struct file *ctx_file;
	int buffer_slot;
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
		kfree(buf_data->pages_cpu);
		kfree(buf_data->pages_dma);
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

static void write_command(struct acceldev_device *dev, uint32_t *cmd) {
	uint32_t reg = CMD_MANUAL_FEED;
	for (int i = 0; i < 5; i++) {
  		acceldev_iow(dev, reg, cmd[i]);
  		reg += 4;
 	}
}

static void schedule_wait(struct acceldev_device *dev) {
	uint32_t reg = CMD_MANUAL_FEED;
	acceldev_iow(dev, reg, ACCELDEV_DEVICE_CMD_TYPE_FENCE);
	acceldev_iow(dev, reg + 4, dev->dev_fence_counter);
	acceldev_iow(dev, reg + 8, 0);
	acceldev_iow(dev, reg + 12, 0);
	acceldev_iow(dev, reg + 16, 0);

}

static void run_queue(struct acceldev_device *dev) {
	uint32_t cmd_left;
	while (!list_empty(&dev->cmd_queue)) {
		cmd_left = acceldev_ior(dev, CMD_MANUAL_FREE);
		if (cmd_left > 3) {
			struct command *cmd = list_entry(dev->cmd_queue.next, struct command, lh);
			list_del(&cmd->lh);
			acceldev_iow(dev, ACCELDEV_CMD_FENCE_WAIT, 0);
			write_command(dev, cmd->cmd);
			kfree(cmd);
		} else if (cmd_left == 3) {
			schedule_wait(dev);
			acceldev_iow(dev, ACCELDEV_CMD_FENCE_WAIT, dev->dev_fence_counter);
			dev->dev_fence_counter += 2; // Always odd to never hit 0
			break;
		} else {
			break;
		}
	}
}

static int submit_command(struct acceldev_device *dev, uint32_t *cmd) {
	unsigned long flags;
	struct command *list_cmd = kmalloc(sizeof(struct command), GFP_KERNEL);
	if (!list_cmd) {
		return -ENOMEM;
	}
	spin_lock_irqsave(&dev->slock, flags);
	for (int i = 0; i < 5; i++) {
  		list_cmd->cmd[i] = cmd[i];
 	}
	kfree(cmd);
	list_add_tail(&list_cmd->lh, &dev->cmd_queue);
	run_queue(dev);
	spin_unlock_irqrestore(&dev->slock, flags);
	return 0;
}

static int write_bind_slot(struct acceldev_device *dev, int ctx_idx, int slot, dma_addr_t page_table) {
	uint32_t *cmd = kmalloc(sizeof(uint32_t) * 5,  GFP_KERNEL);
	uint32_t idx = (uint32_t)ctx_idx;
	cmd[0] = ACCELDEV_DEVICE_CMD_BIND_SLOT_HEADER(idx);
	cmd[1] = slot;
	cmd[2] = (uint32_t)page_table;
	cmd[3] = page_table >> 32;
	cmd[4] = 0;
	return submit_command(dev, cmd);	
}

static void write_to_ctx(struct acceldev_device *dev) {
	for (int i = 0; i < ACCELDEV_MAX_CONTEXTS; i++) {
  		if (dev->ctx[i]) {
			unsigned long flags;
			spin_lock_irqsave(&dev->ctx[i]->ctx_lock, flags);
			struct acceldev_context_on_device_config *ctx_config = &dev->contexts_config_cpu[i];
			dev->ctx[i]->fence_counter = ctx_config->fence_counter;
			dev->ctx[i]->status = ctx_config->status | dev->ctx[i]->status;
			spin_unlock_irqrestore(&dev->ctx[i]->ctx_lock, flags);
		}
 	}
}


static irqreturn_t acceldev_isr(int irq, void *opaque)
{
	struct acceldev_device *dev = opaque;
	unsigned long flags;
	uint32_t istatus;
	spin_lock_irqsave(&dev->slock, flags);
	istatus = acceldev_ior(dev, ACCELDEV_INTR) & acceldev_ior(dev, ACCELDEV_INTR_ENABLE);
	if (istatus) {
		acceldev_iow(dev, ACCELDEV_INTR, istatus);
		if (istatus & ACCELDEV_INTR_FENCE_WAIT) {
			run_queue(dev);
		}
		if (istatus & ACCELDEV_INTR_FEED_ERROR) {
			printk(KERN_ERR "acceldev: feed error\n");
		}
		
		if (istatus & ACCELDEV_INTR_CMD_ERROR) {
			printk(KERN_ERR "acceldev: command error\n");
		}
		if (istatus & ACCELDEV_INTR_MEM_ERROR) {
			printk(KERN_ERR "acceldev: memory error\n");
		}
		if (istatus & ACCELDEV_INTR_SLOT_ERROR) {
   			printk(KERN_ERR "acceldev: slot error\n");
  		}
		if (istatus & ACCELDEV_INTR_USER_FENCE_WAIT) {
			printk(KERN_ERR "acceldev: user fence triggered\n");
		}
		if (istatus & ACCELDEV_INTR_CMD_ERROR ||
			istatus & ACCELDEV_INTR_MEM_ERROR ||
   			istatus & ACCELDEV_INTR_SLOT_ERROR ||
  		 	istatus & ACCELDEV_INTR_USER_FENCE_WAIT) {
			write_to_ctx(dev);
			wake_up_interruptible_all(&dev->user_waits);
		}
	}
	spin_unlock_irqrestore(&dev->slock, flags);
	return IRQ_RETVAL(istatus);
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
	struct acceldev_context *ctx = buf_data->ctx_file->private_data;
	if (buf_data->buffer_slot >= 0) {
		write_bind_slot(ctx->dev, ctx->ctx_idx, buf_data->buffer_slot, 0);
		ctx->buffers[buf_data->buffer_slot] = 0;
	}
 	struct pci_dev *pdev = ctx->dev->pdev;
  	dealloc_page_table(buf_data, pdev);
	fput(buf_data->ctx_file);
  	kfree(buf_data);
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

static int create_buffer(int size, struct file *ctx_file, enum acceldev_buffer_type type, int slot, struct acceldev_ioctl_create_buffer_result *result) {
	int err;
	unsigned long flags;
	struct acceldev_context *ctx = ctx_file->private_data;
	struct acceldev_buffer_data *buf_data = kmalloc(sizeof(struct acceldev_buffer_data), GFP_KERNEL);
	if (!buf_data) {
  		err = -ENOMEM;
		goto out_buf_data;
  	}
	buf_data->buffer_size = size;
	buf_data->type = type;
	int idx = ctx->ctx_idx;
	if ((err = alloc_page_table(buf_data, ctx->dev->pdev)) < 0) {
		goto out_alloc;
	}
	buf_data->buffer_slot = -1;
	get_file(ctx_file);
	buf_data->ctx_file = ctx_file;

	int bfd = anon_inode_getfd(ACCELDEV_NAME, &acceldev_buffer_fops, buf_data, O_RDWR);
	if (bfd < 0) {
  		err = bfd;
		goto out_getfd;
 	}
	
	if (type == BUFFER_TYPE_DATA) {
		spin_lock_irqsave(&ctx->ctx_lock, flags);
		buf_data->buffer_slot = slot;
		ctx->buffers[slot] = bfd;
		spin_unlock_irqrestore(&ctx->ctx_lock, flags);
		struct acceldev_ioctl_create_buffer_result res = {.buffer_slot = slot};
		if (copy_to_user(result, &res, sizeof(struct acceldev_ioctl_create_buffer_result))) {
			err = -EFAULT;
			ctx->buffers[slot] = 0;
			goto out_copy;
		}
		if (write_bind_slot(ctx->dev, idx, slot, buf_data->page_table_dma)) {
			err = -ENOMEM;
			ctx->buffers[slot] = 0;
			goto out_copy;
		}
	}

	return bfd;

out_copy:
	close_fd(bfd);
out_getfd:
	dealloc_page_table(buf_data, ctx->dev->pdev);
	fput(ctx_file);
out_alloc:
 	kfree(buf_data);
out_buf_data:
	if (slot >= 0) {
  		spin_lock_irqsave(&ctx->ctx_lock, flags);
  		ctx->buffers[slot] = 0; // Mark slot as free
  		spin_unlock_irqrestore(&ctx->ctx_lock, flags);
 	}
	return err;


}

/* Main device node handling.  */

static long acceldev_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
	unsigned long flags;
	switch (cmd) {
		case ACCELDEV_IOCTL_CREATE_BUFFER: {
			struct acceldev_ioctl_create_buffer* create_buf = kmalloc(sizeof(struct acceldev_ioctl_create_buffer), GFP_KERNEL);
			if (!create_buf) {
				return -ENOMEM; // Memory allocation failed
			}
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
			int size = create_buf->size;
			enum acceldev_buffer_type type = create_buf->type;
			struct acceldev_context *ctx = file->private_data;
			spin_lock_irqsave(&ctx->ctx_lock, flags);
			int i = -1;
			if (create_buf->type == BUFFER_TYPE_DATA) {
				for (i = 0; i < ACCELDEV_NUM_BUFFERS; i++) {
					if (ctx->buffers[i] == 0) {
						ctx->buffers[i] = -1; // Mark as used
						break;
					}
				}
				if (i == ACCELDEV_NUM_BUFFERS) {
					spin_unlock_irqrestore(&ctx->ctx_lock, flags);
					kfree(create_buf);
					return -ENOSPC;
				}
			}
			spin_unlock_irqrestore(&ctx->ctx_lock, flags);
			int bfd = create_buffer(size, file, type, i, create_buf->result);
			kfree(create_buf);
			return bfd;
		}

		case ACCELDEV_IOCTL_RUN: {
			struct acceldev_ioctl_run* run_cmd = kmalloc(sizeof(struct acceldev_ioctl_run), GFP_KERNEL);
			if (!run_cmd) {
				return -ENOMEM;
			}
			if (copy_from_user(run_cmd, (void __user *)arg, sizeof(struct acceldev_ioctl_run))) {
				kfree(run_cmd);
				return -EFAULT;
			}
			struct acceldev_context *ctx = file->private_data;
			if (run_cmd->cfd < 0 || 
				run_cmd->addr < 0 || 
				run_cmd->size <= 0 || 
				run_cmd->addr > ACCELDEV_BUFFER_MAX_SIZE ||
				run_cmd->size > ACCELDEV_BUFFER_MAX_SIZE - run_cmd->addr || 
				run_cmd->addr % sizeof(uint32_t) != 0 ||
				run_cmd->size % sizeof(uint32_t) != 0) {

				printk(KERN_ERR "acceldev: Invalid arguments in run command\n");
				spin_lock_irqsave(&ctx->ctx_lock, flags);
				ctx->status = ACCELDEV_CONTEXT_STATUS_ERROR;
				spin_unlock_irqrestore(&ctx->ctx_lock, flags);
				return -EINVAL; // Invalid arguments
			}
			
			spin_lock_irqsave(&ctx->ctx_lock, flags);
			uint8_t status = ctx->status;
			spin_unlock_irqrestore(&ctx->ctx_lock, flags);
			if (acceldev_context_on_device_config_is_error(status)) {
				kfree(run_cmd);
				return -EIO;
			}
			


			struct file *buf_file = fget(run_cmd->cfd);
			struct acceldev_buffer_data *buf_data = (struct acceldev_buffer_data*)buf_file->private_data;
			struct acceldev_context *buf_ctx = buf_data->ctx_file->private_data;
			if (run_cmd->addr + run_cmd->size > buf_data->buffer_size || buf_data->type != BUFFER_TYPE_CODE) {
				fput(buf_file);
				kfree(run_cmd);
				spin_lock_irqsave(&ctx->ctx_lock, flags);
				ctx->status = ACCELDEV_CONTEXT_STATUS_ERROR;
				spin_unlock_irqrestore(&ctx->ctx_lock, flags);
				return -EINVAL;
   	       	}
			if (buf_ctx->ctx_idx != ctx->ctx_idx) {
				printk(KERN_ERR "acceldev: Context mismatch in run command\n");
				fput(buf_file);
				kfree(run_cmd);
				return -EINVAL;
			}
			uint32_t *cmd = kmalloc(sizeof(uint32_t) * 5, GFP_KERNEL);
			if (!cmd) {
				
				fput(buf_file);
				kfree(run_cmd);
				return -ENOMEM; // Memory allocation failed
			}
			cmd[0] = ACCELDEV_DEVICE_CMD_RUN_HEADER(ctx->ctx_idx);
			cmd[1] = buf_data->page_table_dma;
			cmd[2] = buf_data->page_table_dma >> 32;
			cmd[3] = run_cmd->addr;
			cmd[4] = run_cmd->size;
			if (submit_command(ctx->dev, cmd) < 0) {
				fput(buf_file);
				kfree(run_cmd);
				return -ENOMEM; // Failed to submit command
   			}
			fput(buf_file);
			kfree(run_cmd);
			return 0;
		}

		case ACCELDEV_IOCTL_WAIT: {
			struct acceldev_ioctl_wait *wait_cmd = kmalloc(sizeof(struct acceldev_ioctl_wait), GFP_KERNEL);
			if (!wait_cmd) {
				return -ENOMEM;
			}
			if (copy_from_user(wait_cmd, (void __user *)arg, sizeof(struct acceldev_ioctl_wait))) {
				kfree(wait_cmd);
				return -EFAULT;
			}
			struct acceldev_context *ctx = file->private_data;
			spin_lock_irqsave(&ctx->ctx_lock, flags);
			uint32_t current_counter = ctx->fence_counter;
			uint8_t status = ctx->status;
			printk(KERN_ERR "My counter: %u, wait: %u, ctx: %d\n", current_counter, wait_cmd->fence_wait, ctx->ctx_idx);
			uint32_t fence_wait = wait_cmd->fence_wait;
			while (current_counter < fence_wait && !acceldev_context_on_device_config_is_error(status)) {
				spin_unlock_irqrestore(&ctx->ctx_lock, flags);
				if (wait_event_interruptible(ctx->dev->user_waits, true)) {
					kfree(wait_cmd);
					return -EINTR; // Interrupted by signal
				}
				spin_lock_irqsave(&ctx->ctx_lock, flags);
				current_counter = ctx->fence_counter;
				status = ctx->status;				
			}
			if  (acceldev_context_on_device_config_is_error(ctx->status)) {
				spin_unlock_irqrestore(&ctx->ctx_lock, flags);
				printk(KERN_ERR "Error while waiting for: %d in context %d\n", fence_wait, ctx->ctx_idx);
				kfree(wait_cmd);
				return -EIO; // Context is in error state
			}
			spin_unlock_irqrestore(&ctx->ctx_lock, flags);
			printk(KERN_ERR "Finished waiting for: %d in context %d\n", fence_wait, ctx->ctx_idx);
   			kfree(wait_cmd);
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
	ctx->ctx_idx = i;
	ctx->fence_counter = 0;
	ctx->status = 0;
    ctx->dev = dev;
	spin_lock_init(&ctx->ctx_lock);
    file->private_data = ctx;
	return nonseekable_open(inode, file);
}

static int acceldev_release(struct inode *inode, struct file *file)
{
	struct acceldev_context *ctx = file->private_data;
	struct acceldev_device *dev = ctx->dev;
    for (int i = 0; i < ACCELDEV_MAX_CONTEXTS; i++) {
        if (dev->ctx[i] == ctx) {
			memset(&dev->contexts_config_cpu[i], 0, sizeof(struct acceldev_context_on_device_config));
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
	dev->dev_fence_counter = 1;
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
	INIT_LIST_HEAD(&dev->cmd_queue);
	init_waitqueue_head(&dev->user_waits);

	/* Connect the IRQ line.  */
	if ((err = request_irq(pdev->irq, acceldev_isr, IRQF_SHARED, ACCELDEV_NAME, dev)))
		goto out_irq;

    dev->contexts_config_cpu = dma_alloc_coherent(&pdev->dev, ACCELDEV_MAX_CONTEXTS * sizeof(struct acceldev_context_on_device_config), &dev->contexts_config_dma, GFP_KERNEL);
    if (!dev->contexts_config_cpu) {
        err = -ENOMEM;
        goto out_contexts;
    }
	memset(dev->contexts_config_cpu, 0, ACCELDEV_MAX_CONTEXTS * sizeof(struct acceldev_context_on_device_config));


	acceldev_iow(dev, ACCELDEV_INTR, 1);
	acceldev_iow(dev, ACCELDEV_INTR_ENABLE, ACCELDEV_INTR_FENCE_WAIT | ACCELDEV_INTR_FEED_ERROR |
            ACCELDEV_INTR_CMD_ERROR | ACCELDEV_INTR_MEM_ERROR |
            ACCELDEV_INTR_SLOT_ERROR | ACCELDEV_INTR_USER_FENCE_WAIT);
    acceldev_iow(dev, ACCELDEV_CONTEXTS_CONFIGS, dev->contexts_config_dma);  
    acceldev_iow(dev, ACCELDEV_CONTEXTS_CONFIGS + 4, dev->contexts_config_dma >> 32);
    acceldev_iow(dev, ACCELDEV_ENABLE, 1);
	
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
	dma_free_coherent(&pdev->dev, ACCELDEV_MAX_CONTEXTS * sizeof(struct acceldev_context_on_device_config), dev->contexts_config_cpu, dev->contexts_config_dma);
	acceldev_iow(dev, ACCELDEV_INTR_ENABLE, 0);
	acceldev_iow(dev, ACCELDEV_ENABLE, 0);
out_contexts:
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
	acceldev_iow(dev, ACCELDEV_INTR_ENABLE, 0);
	acceldev_iow(dev, ACCELDEV_ENABLE, 0);
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