#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include "uart16550.h"
#include "uart16550_hw.h"

MODULE_DESCRIPTION("Uart16550 driver");
MODULE_LICENSE("GPL");

#ifdef __DEBUG
#define dprintk(fmt, ...)                                                      \
	printk(KERN_DEBUG "%s:%d " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define dprintk(fmt, ...)                                                      \
	do {                                                                   \
	} while (0)
#endif

#define MODULE_NAME "uart16550"
#define EXIT_ON_ERROR(error)                                                   \
	if (error)                                                             \
	return error

irqreturn_t interrupt_handler(int, void *);
int uart16550_open(struct inode *, struct file *);
ssize_t uart16550_read(struct file *, char __user *, size_t, loff_t *);
int uart16550_release(struct inode *, struct file *);
long uart16550_unlocked_ioctl(struct file *, unsigned int, unsigned long);
ssize_t uart16550_write(struct file *, const char __user *, size_t, loff_t *);
static int contains(unsigned char, unsigned char *, size_t);

struct uart16550_dev {
	struct cdev cdev;
	DECLARE_KFIFO(outgoing, char, FIFO_SIZE);
	DECLARE_KFIFO(incoming, char, FIFO_SIZE);
	wait_queue_head_t wq_head_outgoing;
	wait_queue_head_t wq_head_incoming;
	u32 device_port;
};
static struct class *uart16550_class = NULL;
static const struct file_operations uart16550_fops = {
	.owner = THIS_MODULE,
	.open = uart16550_open,
	.read = uart16550_read,
	.write = uart16550_write,
	.release = uart16550_release,
	.unlocked_ioctl = uart16550_unlocked_ioctl,
};


/*
 * TODO: Populate major number from module options (when it is given).
 */
static int major = 42;
static int behavior = 0x03;
static struct uart16550_dev com1, com2;

module_param(major, int, 0);
module_param(behavior, int, 0);

int uart16550_open(struct inode *inode, struct file *filp)
{
	struct uart16550_dev *uart16550_dev;
	uart16550_dev = container_of(inode->i_cdev, struct uart16550_dev, cdev);

	filp->private_data = uart16550_dev;

	return 0;
}

ssize_t uart16550_read(struct file *filp, char __user *buff, size_t count,
		       loff_t *offp)
{
	// printk("SKDEBUG enter in read");
	int err, copied_chars = 0;
	struct uart16550_dev *uart16550_dev =
		(struct uart16550_dev *)filp->private_data;
	int chars_copied = 0;
	char *kernel_buff = kmalloc(count, GFP_KERNEL);
	if (NULL == kernel_buff) {
		return -ENOMEM;
	}

	uart16550_hw_force_interrupt_reemit(uart16550_dev->device_port);

	do {
		// printk("SKDEBUG waiting for spin lock in read");
		spin_lock_irq(&uart16550_dev->wq_head_incoming.lock);
		// printk("SKDEBUG lock taken in read");
		// printk("SKDEBUG start waiting read");
		err = wait_event_interruptible_locked_irq(
			uart16550_dev->wq_head_incoming,
			!kfifo_is_empty(&uart16550_dev->incoming));
		// printk("SKDEBUG finish waiting in read");
		int to_copy_chars =
			kfifo_out(&uart16550_dev->incoming, kernel_buff, count);
		*offp += to_copy_chars;
		spin_unlock_irq(&uart16550_dev->wq_head_incoming.lock);
		// printk("SKDEBUG lock released in read");
		int uncopied_chars =
			copy_to_user(buff, kernel_buff, to_copy_chars);
		copied_chars = to_copy_chars - uncopied_chars;
		// printk("SKDEBUG copied to user in read");
	} while (copied_chars == 0 && err != -ERESTARTSYS);
	kfree(kernel_buff);
	wake_up_locked(&uart16550_dev->wq_head_incoming);


	// printk("SKDEBUG leaving read with %d copied bytes and err %d",
	      // copied_chars, err);

	return copied_chars;
}

ssize_t uart16550_write(struct file *filp, const char __user *buff,
			size_t count, loff_t *offp)
{
	// printk("SKDEBUG enter in write");
	int err, bytes_copied;
	struct uart16550_dev *uart16550_dev =
		(struct uart16550_dev *)filp->private_data;
	char *kernel_buffer = kmalloc(count, GFP_KERNEL);
	if (NULL == kernel_buffer) {
		return -ENOMEM;
	}

	do {
		int uncopied_chars = copy_from_user(kernel_buffer, buff, count);

		// printk("SKDEBUG waiting for spin lock in write");
		spin_lock_irq(&uart16550_dev->wq_head_outgoing.lock);
		// printk("SKDEBUG lock taken in write");
		// printk("SKDEBUG start waiting in write");
		err = wait_event_interruptible_locked_irq(
			uart16550_dev->wq_head_outgoing,
			!kfifo_is_full(&uart16550_dev->outgoing));

		// printk("SKDEBUG finish waiting in write");
		size_t available = kfifo_avail(&uart16550_dev->outgoing);
		count = count < available ? count : available;

		bytes_copied = count - uncopied_chars;
		kfifo_in(&uart16550_dev->outgoing, kernel_buffer, bytes_copied);
		*offp += bytes_copied;

		spin_unlock_irq(&uart16550_dev->wq_head_outgoing.lock);
		// printk("SKDEBUG lock released in write");
	} while (bytes_copied == 0 && err != -ERESTARTSYS);
	kfree(kernel_buffer);
	uart16550_hw_force_interrupt_reemit(uart16550_dev->device_port);

	// printk("SKDEBUG leaving write with %d copied bytes and err %d",
	      // bytes_copied, err);

	return bytes_copied;
}

int uart16550_release(struct inode *inode, struct file *filp)
{
	return 0;
}

long uart16550_unlocked_ioctl(struct file *filp, unsigned int cmd,
			      unsigned long arg)
{
	if (cmd != UART16550_IOCTL_SET_LINE) {
		return -EINVAL;
	}

	struct uart16550_dev *uart16550_dev =
		(struct uart16550_dev *)filp->private_data;
	struct uart16550_line_info parameters =
		*((struct uart16550_line_info *)&arg);

	char baud[] = UART16550_BAUD_VALUES;
	// printk("SKDEBUG baud : %d", parameters.baud);
	if (!contains(parameters.baud, baud, UART16550_BAUD_VALUES_SIZE)) {
		return -EINVAL;
	}

	char len[] = UART16550_LEN_VALUES;
	// printk("SKDEBUG len : %d", parameters.len);
	if (!contains(parameters.len, len, UART16550_LEN_VALUES_SIZE)) {
		return -EINVAL;
	}

	char par[] = UART16550_PAR_VALUES;
	// printk("SKDEBUG par : %d", parameters.par);
	if (!contains(parameters.par, par, UART16550_PAR_VALUES_SIZE)) {
		return -EINVAL;
	}

	char stop[] = UART16550_STOP_VALUES;
	// printk("SKDEBUG stop : %d", parameters.stop);
	if (!contains(parameters.stop, stop, UART16550_STOP_VALUES_SIZE)) {
		return -EINVAL;
	}


	uart16550_hw_set_line_parameters(uart16550_dev->device_port,
					 parameters);

	return 0;
}

irqreturn_t interrupt_handler(int irq_no, void *data)
{
	int device_status;
	u32 device_port;

	struct uart16550_dev *uart16550_dev = (struct uart16550_dev *)data;
	device_port = uart16550_dev->device_port;
	/*
	 * TODO: Write the code that handles a hardware interrupt.
	 * TODO: Populate device_port with the port of the correct device.
	 */
	unsigned long a;

	device_status = uart16550_hw_get_device_status(device_port);

	while (uart16550_hw_device_can_send(device_status)) {
		// printk("SKDEBUG waiting for spin lock in interrupt for cam send");
		spin_lock_irqsave(&uart16550_dev->wq_head_outgoing.lock, a);
		// printk("SKDEBUG lock taken in interrupt for cas send");

		u8 byte_value;
		if (!kfifo_is_empty(&uart16550_dev->outgoing)) {
			// printk("SKDEBUG interrupt cppied one byte from outgoin");
			kfifo_out(&uart16550_dev->outgoing, &byte_value, 1);
			uart16550_hw_write_to_device(device_port, byte_value);
		} else {
			device_status =
				uart16550_hw_get_device_status(device_port);
			spin_unlock_irqrestore(
				&uart16550_dev->wq_head_outgoing.lock, a);
			// printk("SKDEBUG lock released in interrupt");

			break;
		}

		device_status = uart16550_hw_get_device_status(device_port);
		spin_unlock_irqrestore(&uart16550_dev->wq_head_outgoing.lock,
				       a);
		// printk("SKDEBUG lock released in interrupt");
	}

	while (uart16550_hw_device_has_data(device_status)) {
		// printk("SKDEBUG waiting for spin lock in interrupt in has data");
		spin_lock_irqsave(&uart16550_dev->wq_head_incoming.lock, a);
		// printk("SKDEBUG lock taken in intrrupt for has data");

		u8 byte_value;

		if (!kfifo_is_full(&uart16550_dev->incoming)) {
			// printk("SKDEBUG interrupt cppied one byte in incoming");
			byte_value = uart16550_hw_read_from_device(device_port);
			kfifo_in(&uart16550_dev->incoming, &byte_value, 1);
		} else {
			device_status =
				uart16550_hw_get_device_status(device_port);
			spin_unlock_irqrestore(
				&uart16550_dev->wq_head_incoming.lock, a);
			// printk("SKDEBUG lock relased in interrupt");

			break;
		}
		device_status = uart16550_hw_get_device_status(device_port);
		spin_unlock_irqrestore(&uart16550_dev->wq_head_incoming.lock,
				       a);
		// printk("SKDEBUG lock relased in interrupt");
	}

	wake_up_locked(&uart16550_dev->wq_head_incoming);

	return IRQ_HANDLED;
}

static int uart16550_init(void)
{
	int have_com1, have_com2;
	int err = 0;
	struct device *device;

	if (major >= MAX_MAJOR_COUNT || major < 0) {
		return -EINVAL;
	}

	have_com1 = behavior == OPTION_COM1 || behavior == OPTION_BOTH;
	have_com2 = behavior == OPTION_COM2 || behavior == OPTION_BOTH;

	err = register_chrdev_region(
		MKDEV(major, have_com1 ? MINOR_COM1 : MINOR_COM2),
		have_com1 + have_com2, MODULE_NAME);

	EXIT_ON_ERROR(err);


	/*
	 * Setup a sysfs class & device to make /dev/com1 & /dev/com2 appear.
	 */
	uart16550_class = class_create(THIS_MODULE, MODULE_NAME);

	if (have_com1) {
		/* Setup the hardware device for COM1 */
		err = uart16550_hw_setup_device(COM1_BASEPORT,
						THIS_MODULE->name);
		EXIT_ON_ERROR(err);

		/* Create the sysfs info for /dev/com1 */
		device = device_create(uart16550_class, NULL,
				       MKDEV(major, MINOR_COM1), NULL, "com1");
		if (IS_ERR(device)) {
			return PTR_ERR(device);
		}

		cdev_init(&com1.cdev, &uart16550_fops);
		err = cdev_add(&com1.cdev, MKDEV(major, MINOR_COM1), 1);

		EXIT_ON_ERROR(err);

		INIT_KFIFO(com1.outgoing);
		INIT_KFIFO(com1.incoming);
		init_waitqueue_head(&(com1.wq_head_outgoing));
		init_waitqueue_head(&(com1.wq_head_incoming));
		com1.device_port = COM1_BASEPORT;

		err = request_irq(COM1_IRQ, interrupt_handler, IRQF_SHARED,
				  THIS_MODULE->name, &com1);
		EXIT_ON_ERROR(err);
	}
	if (have_com2) {
		/* Setup the hardware device for COM2 */
		err = uart16550_hw_setup_device(COM2_BASEPORT,
						THIS_MODULE->name);
		EXIT_ON_ERROR(err);

		/* Create the sysfs info for /dev/com2 */
		device = device_create(uart16550_class, NULL,
				       MKDEV(major, MINOR_COM2), NULL, "com2");
		if (IS_ERR(device)) {
			return PTR_ERR(device);
		}

		cdev_init(&com2.cdev, &uart16550_fops);
		err = cdev_add(&com2.cdev, MKDEV(major, MINOR_COM2), 1);

		EXIT_ON_ERROR(err);

		INIT_KFIFO(com2.outgoing);
		INIT_KFIFO(com2.incoming);
		init_waitqueue_head(&(com2.wq_head_outgoing));
		init_waitqueue_head(&(com2.wq_head_incoming));
		com2.device_port = COM2_BASEPORT;

		err = request_irq(COM2_IRQ, interrupt_handler, IRQF_SHARED,
				  THIS_MODULE->name, &com2);
		EXIT_ON_ERROR(err);
	}

	return 0;
}

static void uart16550_cleanup(void)
{
	int have_com1, have_com2;
	/*
	 * TODO: Write driver cleanup code here.
	 * TODO: have_com1 & have_com2 need to be set according to the
	 *      module parameters.
	 */

	have_com1 = behavior == OPTION_COM1 || behavior == OPTION_BOTH;
	have_com2 = behavior == OPTION_COM2 || behavior == OPTION_BOTH;

	unregister_chrdev_region(
		MKDEV(major, have_com1 ? MINOR_COM1 : MINOR_COM2),
		have_com1 + have_com2);

	if (have_com1) {
		/* Reset the hardware device for COM1 */
		uart16550_hw_cleanup_device(COM1_BASEPORT);
		/* Remove the sysfs info for /dev/com1 */
		device_destroy(uart16550_class, MKDEV(major, 0));

		cdev_del(&com1.cdev);

		free_irq(COM1_IRQ, &com1);
		uart16550_hw_disable_interrupts(com1.device_port);
	}
	if (have_com2) {
		/* Reset the hardware device for COM2 */
		uart16550_hw_cleanup_device(COM2_BASEPORT);
		/* Remove the sysfs info for /dev/com2 */
		device_destroy(uart16550_class, MKDEV(major, 1));

		cdev_del(&com2.cdev);

		free_irq(COM2_IRQ, &com2);
		uart16550_hw_disable_interrupts(com2.device_port);
	}

	/*
	 * Cleanup the sysfs device class.
	 */
	class_destroy(uart16550_class);
}

static int contains(unsigned char element, unsigned char *array, size_t size)
{
	size_t i;
	for (i = 0; i < size; i++) {
		if (element == array[i]) {
			return 1;
		}
	}

	return 0;
}

module_init(uart16550_init) module_exit(uart16550_cleanup)
