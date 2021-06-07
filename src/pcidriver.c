#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/math64.h>
#include <asm/uaccess.h>

// vendor and device ids as specified
#define MY_VENDOR_ID 0x144a
#define MY_DEVICE_ID 0x9111

// base address of the device and length of the memory region
static unsigned long ioport=0L, iolen=0L;

static dev_t mypci_dev_number;	// future device number of the device
static struct cdev *driver_object;
static struct class *mypci_class;
static struct device *mypci_dev;

static int driver_open( struct inode *devfile, struct file *instance )
{	// 'open' operation for the device file
	// we don't really have anything to do here
	printk(KERN_INFO "mypci driver_open\n");
	return 0;
}

static int driver_close( struct inode *devfile, struct file *instance )
{	// 'close' operation for the device file
	// we don't have anything to do here either
	printk(KERN_INFO "mypci driver_close\n");
	return 0;
}

static ssize_t driver_read( struct file *instance, char __user *user, size_t count, loff_t *offset )
{	// 'read' operation for the device file
	// we want this to trigger the measurement and give the result
	u8 status, timeout;
	s16 data;

	printk(KERN_INFO "mypci driver_read\n");

	for (timeout = 0; timeout < 25; timeout++)
	{	// waiting for the converter to become available
		printk(KERN_INFO "mypci driver_read wait for ready %d\n", timeout);
		// try a max of 25 times, else error out
		status = inb(ioport+0x08);	// read a/d status again
		if ((status & 0x80) != 0) break;	// stop if not busy
		usleep_range(10, 20);	// sleep for a minimum of 10 and a maximum of 20 microseconds
		// we use 'usleep_range' so we don't block the whole kernel
		// usleep ensures we are woken up at least at the last duration specified
		// but we might be called as early as the first duration specified if there's already another convenient interrupt going on
	}
	if ((status & 0x80) == 0)
	{	// if we left the loop not normally but because of timeout
		printk(KERN_INFO "mypci driver_read [TIMEOUT] waiting for device (busy)\n");
		return 0;
	}

	for (timeout = 0; timeout < 25; timeout++)
	{	// waiting for the converter to become empty
		printk(KERN_INFO "mypci driver_read wait for empty %d\n", timeout);
		// try a max of 25 times, else error out
		status = inb(ioport+0x08);	// read a/d status again
		if ((status & 0x10) == 0) break;	// stop if empty

		inw(ioport+0); 	// discard data
	}
	if ((status & 0x10) != 0)
	{	// if we left the loop because of timeout
		printk(KERN_INFO "mypci driver_read [TIMEOUT] discarding data\n");
		return 0;
	}

	outb(0xff, ioport+0x0e);	// trigger conversion (value can be anything, but register must be written)

	usleep_range(10, 20);	// sleep for a minimum of 10 and a maximum of 20 microseconds

	data = inw(ioport+0);	// read a/d input register
	printk(KERN_INFO "mypci driver_read raw data: %d\n", data);
	raw_copy_to_user(user, &data, 2);
	return 2;
}

// we declare which functions handle which file operations
static struct file_operations fops = {
	.owner = THIS_MODULE,
	.read = driver_read,
	.open = driver_open,
	.release = driver_close,
};

static int device_init( struct pci_dev *pdev, const struct pci_device_id *id )
{	// when the kernel found a device to use this driver for
	printk(KERN_INFO "mypci device_init\n");

	if( pci_enable_device( pdev ) )
		return -EIO;
	ioport = pci_resource_start( pdev, 2 );	// get the address of the memory region
	iolen = pci_resource_len( pdev, 2 );	// ...and the length!
	if( request_region( ioport, iolen, pdev->dev.kobj.name )==NULL )
	{	// uh oh! we requested the region to use but something went wrong.
		dev_err(&pdev->dev,"I/O address conflict for device \"%s\"\n",
			pdev->dev.kobj.name);
		return -EIO;
	}
	outb(0x0, ioport+0x06);  // set channel 0
	outb(0x0, ioport+0x08);  // set signal range to ±10V
	outb(0x0, ioport+0x0A);  // set trigger to software/polling

	// finally, create the device file that userspace can interact with
	mypci_dev = device_create( mypci_class, NULL, mypci_dev_number, NULL, "%s", "mypci" );
	return 0;
}

static void device_deinit( struct pci_dev *pdev )
{	// when the device disappeares
	printk(KERN_INFO "mypci device_deinit\n");

	device_destroy( mypci_class, mypci_dev_number );	// remove the device file
	if( ioport )
		release_region( ioport, iolen );	// release the memory region
}

static struct file_operations fops;

static struct pci_device_id pci_drv_tbl[] = {
	{ MY_VENDOR_ID, MY_DEVICE_ID, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ 0, }
};

static struct pci_driver pci_drv = {
	.name= "mypci",
	.id_table= pci_drv_tbl,
	.probe= device_init,
	.remove= device_deinit,
};

static int __init mod_init(void)
{	// when the module is loaded
	printk(KERN_INFO "mypci mod_init\n");

	if( alloc_chrdev_region(&mypci_dev_number, 0, 1, "mypci") < 0 )
		return -EIO;
	driver_object = cdev_alloc(); /* Anmeldeobjekt reservieren */
	if( driver_object == NULL ) goto free_dev_number;
		driver_object->owner = THIS_MODULE;
	driver_object->ops = &fops;
	if( cdev_add(driver_object, mypci_dev_number, 1) )
		goto free_cdev;
	/* Eintrag im Sysfs, damit Udev den Geraetedateieintrag erzeugt. */
	mypci_class = class_create( THIS_MODULE, "mypci" );
	if( IS_ERR( mypci_class ) )
	{
		pr_err( "mypci: no udev support available\n");
		goto free_cdev;
	}

	if( pci_register_driver(&pci_drv) < 0)  {
		device_destroy( mypci_class, mypci_dev_number );
		goto free_dev_number;
	}
	return 0;

free_cdev:
        kobject_put( &driver_object->kobj );
free_dev_number:
        unregister_chrdev_region( mypci_dev_number, 1 );
        return -EIO;
}

static void __exit mod_exit(void)
{	// when the module is unloaded
	printk(KERN_INFO "mypci mod_exit\n");

	pci_unregister_driver( &pci_drv );	// give up the driver

	device_destroy( mypci_class, mypci_dev_number );  // remove the device file
	class_destroy( mypci_class );
	cdev_del( driver_object );
	unregister_chrdev_region( mypci_dev_number, 1 );
}

module_init(mod_init);
module_exit(mod_exit);
MODULE_LICENSE("GPL");
