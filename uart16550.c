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

int uart16550_open(struct inode *, struct file *);
ssize_t uart16550_read(struct file *, char __user *, size_t , loff_t *);
int uart16550_release(struct inode *, struct file *);
long uart16550_unlocked_ioctl(struct file *, unsigned int , unsigned long);
ssize_t uart16550_write(struct file *,
			       const char __user *, size_t ,
			       loff_t *);

struct uart16550_dev {
	struct cdev cdev;
	DECLARE_KFIFO(outgoing, char, FIFO_SIZE);
	DECLARE_KFIFO(incoming, char, FIFO_SIZE);
	wait_queue_head_t wq_head;
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

ssize_t uart16550_read(struct file *filp, char __user *buff, size_t count, loff_t *offp)
{
	struct uart16550_dev *uart16550_dev = (struct uart16550_dev*) filp->private_data;
	int chars_copied = 0;
	char *kernel_buff = kmalloc(count, GFP_KERNEL);
	if(NULL == kernel_buff) {
		return -ENOMEM;
	}
	printk("SKDEBUG read acquiring lock to read %d bytes...", count);
	spin_lock_irq(&uart16550_dev->wq_head.lock);
	printk("SKDEBUG read lock acquired ...");
	printk("SKDEBUG read start waiting ...");
	int err = wait_event_interruptible_locked_irq(uart16550_dev->wq_head, !kfifo_is_empty(&uart16550_dev->outgoing));
	printk("SKDEBUG read finished waiting with err code %d. Number of available bytes is %d", err, kfifo_avail(&uart16550_dev->outgoing));

	int to_copy_chars = kfifo_out(&uart16550_dev->outgoing, kernel_buff, count);
	*offp += to_copy_chars;
	spin_unlock_irq(&uart16550_dev->wq_head.lock);
	printk("SKDEBUG read unlocked the lock ...");

	int uncopied_chars = copy_to_user(buff, kernel_buff, to_copy_chars);
	int copied_chars = to_copy_chars - uncopied_chars;
	printk("SKDEBUG read copied %i bytes", copied_chars);
	wake_up_locked(&uart16550_dev->wq_head);
	printk("SKDEBUG read sent wake up action ...");

	printk("SKDEBUG read returned ...");

	return copied_chars;

}

ssize_t uart16550_write(struct file *filp,
			       const char __user *buff, size_t count,
			       loff_t *offp)
{
	int bytes_copied;
	struct uart16550_dev *uart16550_dev = (struct uart16550_dev*) filp->private_data;
	char *kernel_buffer = kmalloc(count, GFP_KERNEL);
	if(NULL == kernel_buffer) {
		return -ENOMEM;
	}
	int uncopied_chars = copy_from_user(kernel_buffer, buff, count);

	printk("SKDEBUG write acquiring lock ...");
	spin_lock_irq(&uart16550_dev->wq_head.lock);
	printk("SKDEBUG write lock acquired ...");
	printk("SKDEBUG write start waiting ...");
	int err = wait_event_interruptible_locked_irq(uart16550_dev->wq_head, !kfifo_is_full(&uart16550_dev->outgoing));
	printk("SKDEBUG write finished waiting with err code %d...", err);
	size_t available = kfifo_avail(&uart16550_dev->outgoing);
	count = count <  available ? count : available;

	bytes_copied = count - uncopied_chars;
	kfifo_in(&uart16550_dev->outgoing, kernel_buffer, bytes_copied);
	*offp += bytes_copied;
	printk("SKDEBUG write copied %i bytes", bytes_copied);


	/*
	 * TODO: Write the code that takes the data provided by the
	 *      user from userspace and stores it in the kernel
	 *      device outgoing buffer.
	 * TODO: Populate bytes_copied with the number of bytes
	 *      that fit in the outgoing buffer.
	 */
	wake_up_locked(&uart16550_dev->wq_head);
	printk("SKDEBUG write sent wake up action ...");
	spin_unlock_irq(&uart16550_dev->wq_head.lock);
	printk("SKDEBUG write unlocked the lock ...");

	uart16550_hw_force_interrupt_reemit(uart16550_dev->device_port);

	printk("SKDEBUG write returning ..., outgoing is not empty :%d", !kfifo_is_empty(&uart16550_dev->outgoing));

	return bytes_copied;
}

int uart16550_release(struct inode *inode, struct file *filp)
{
	return 0;
}

long uart16550_unlocked_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	/*if (cmd != UART16550_IOCTL_SET_LINE) {
		return 0;
	}
	struct uart16550_dev *uart16550_dev = (struct uart16550_dev*) filp->private_data;
	struct uart16550_line_info parameters = *((struct uart16550_line_info*)&arg);

	uart16550_hw_set_line_parameters(uart16550_dev->device_port, parameters);
*/
	return 0;
}

irqreturn_t interrupt_handler(int irq_no, void *data)
{
	int device_status;
	u32 device_port;
	/*
	 * TODO: Write the code that handles a hardware interrupt.
	 * TODO: Populate device_port with the port of the correct device.
	 */

	device_status = uart16550_hw_get_device_status(device_port);

	while (uart16550_hw_device_can_send(device_status)) {
		u8 byte_value;
		/*
		 * TODO: Populate byte_value with the next value
		 *      from the kernel device outgoing buffer.
		 * NOTE: If the outgoing buffer is empty, the interrupt
		 *       will not occur again. When data becomes available,
		 *       the driver must either:
		 *   a) force the hardware to reissue the interrupt.
		 *      OR
		 *   b) send the data separately.
		 */
		uart16550_hw_write_to_device(device_port, byte_value);
		device_status = uart16550_hw_get_device_status(device_port);
	}

	while (uart16550_hw_device_has_data(device_status)) {
		u8 byte_value;



		byte_value = uart16550_hw_read_from_device(device_port);
		/*
		 * TODO: Store the read byte_value in the kernel device
		 *      incoming buffer.
		 */
		device_status = uart16550_hw_get_device_status(device_port);
	}

	return IRQ_HANDLED;
}

static int uart16550_init(void)
{
	int have_com1, have_com2;
	int err = 0;
	struct device *device;

	/*
	 * TODO: Write driver initialization code here.
	 * TODO: Check return values of functions used. Fail gracefully.
	 */

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
		device = device_create(uart16550_class, NULL, MKDEV(major, 0),
				       NULL, "com1");
		if (IS_ERR(device)) {
			return PTR_ERR(device);
		}

		cdev_init(&com1.cdev, &uart16550_fops);
		err = cdev_add(&com1.cdev, MKDEV(major, MINOR_COM1), 1);

		EXIT_ON_ERROR(err);

		INIT_KFIFO(com1.outgoing);
		INIT_KFIFO(com1.incoming);
		init_waitqueue_head(&(com1.wq_head));
		com1.device_port = 0x3f8;

	}
	if (have_com2) {
		/* Setup the hardware device for COM2 */
		err = uart16550_hw_setup_device(COM2_BASEPORT,
						THIS_MODULE->name);
		EXIT_ON_ERROR(err);

		/* Create the sysfs info for /dev/com2 */
		device = device_create(uart16550_class, NULL, MKDEV(major, 1),
				       NULL, "com2");
		if (IS_ERR(device)) {
			return PTR_ERR(device);
		}

		cdev_init(&com2.cdev, &uart16550_fops);
		err = cdev_add(&com2.cdev, MKDEV(major, MINOR_COM2), 1);

		EXIT_ON_ERROR(err);

		INIT_KFIFO(com2.outgoing);
		INIT_KFIFO(com2.incoming);
		init_waitqueue_head(&(com2.wq_head));
		com2.device_port = 0x2f8;
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
		MKDEV(major, have_com1 ? OPTION_COM1 : OPTION_COM2),
		have_com1 + have_com2);

	if (have_com1) {
		/* Reset the hardware device for COM1 */
		uart16550_hw_cleanup_device(COM1_BASEPORT);
		/* Remove the sysfs info for /dev/com1 */
		device_destroy(uart16550_class, MKDEV(major, 0));

		cdev_del(&com1.cdev);
	}
	if (have_com2) {
		/* Reset the hardware device for COM2 */
		uart16550_hw_cleanup_device(COM2_BASEPORT);
		/* Remove the sysfs info for /dev/com2 */
		device_destroy(uart16550_class, MKDEV(major, 1));

		cdev_del(&com2.cdev);
	}

	/*
	 * Cleanup the sysfs device class.
	 */
	class_destroy(uart16550_class);
}

module_init(uart16550_init) module_exit(uart16550_cleanup)
