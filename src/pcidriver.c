#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
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
{
	printk(KERN_INFO "mypci driver_open\n");
	//dev_info( mypci_dev, "driver_open called\n" );
	return 0;
}

static int driver_close( struct inode *devfile, struct file *instance )
{
	printk(KERN_INFO "mypci driver_close\n");
	//dev_info( mypci_dev, "driver_close called\n" );
	return 0;
}

static ssize_t driver_read( struct file *instance, char __user *user, size_t count, loff_t *offset )
{
	printk(KERN_INFO "mypci driver_read\n");

	u8 status, timeout, length;
	u16 data;
	int volt;
	char outs[32];	// buffer for formatting output to ascii

	for (timeout = 0; timeout < 25; timeout++)
	{
		status = inb(ioport+0x80);	// read a/d status again
		if (status & 0x80 > 0) break;	// stop if not busy
		usleep_range(5000, 10000);	// sleep for a minimum of 5 and maximum of 10 milliseconds
	}
	if (status & 0x80 == 0)
	{	// if we left the loop because of timeout
		raw_copy_to_user(user, "[TIMEOUT] waiting for device (busy)\n", 37);
		return 37;	// that is the length of the string plus the null byte
	}
	
	for (timeout = 0; timeout < 25; timeout++)
	{
		status = inb(ioport+0x80);	// read a/d status again
		if (status & 0x10 > 0) break;	// stop if empty
		
		inw(ioport+0); 	// discard data
	}
	if (status & 0x80 == 0)
	{	// if we left the loop because of timeout
		raw_copy_to_user(user, "[TIMEOUT] discarding data\n", 27);
		return 27;	// that is the length of the string plus the null byte
	}

	outb(0xff, ioport+0x0e);	// trigger conversion (value can be anything, but register must be written)

	for (timeout = 0; timeout < 25; timeout++)
	{
		status = inb(ioport+0x80);	// read a/d status again
		if (status & 0x80 > 0) break;	// stop if not busy
		usleep_range(5000, 10000);	// sleep for a minimum of 5 and maximum of 10 milliseconds
	}
	if (status & 0x80 == 0)
	{	// if we left the loop because of timeout
		raw_copy_to_user(user, "[TIMEOUT] waiting for data (busy)\n", 35);
		return 35;	// that is the length of the string plus the null byte
	}

	data = inw(ioport+0) >> 4;	// read a/d input register, discarding the bits encoding the channel number
	/*	                  1     10
		Voltage = data × ——— × ————
		                  K    gain
		K    = 2047×16 = 32752
		gain = 1		*/
	volt = data * 10 / 32752;
	length = snprintf(outs, 32, "%d V\n", volt);	// format the read number to a string of max length 8 (including null character)
	raw_copy_to_user(user, &outs, length+1);
	return length+1;
}

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.read = driver_read,
	.open = driver_open,
	.release = driver_close,
};

static int device_init( struct pci_dev *pdev, const struct pci_device_id *id )
{
	printk(KERN_INFO "mypci device_init\n");

	if( pci_enable_device( pdev ) )
		return -EIO;
	ioport = pci_resource_start( pdev, 2 );
	iolen = pci_resource_len( pdev, 2 );
	if( request_region( ioport, iolen, pdev->dev.kobj.name )==NULL )
	{
		dev_err(&pdev->dev,"I/O address conflict for device \"%s\"\n",
			pdev->dev.kobj.name);
		return -EIO;
	}
	pci_write_config_byte(pdev, ioport+0x06, 0x0);  // set channel 0
	pci_write_config_byte(pdev, ioport+0x08, 0x0);  // set signal range to ±10V
	pci_write_config_byte(pdev, ioport+0x0A, 0x0);  // set trigger to software/polling

	mypci_dev = device_create( mypci_class, NULL, mypci_dev_number, NULL, "%s", "mypci" );
	return 0;
}

static void device_deinit( struct pci_dev *pdev )
{
	printk(KERN_INFO "mypci device_deinit\n");

	device_destroy( mypci_class, mypci_dev_number );  // remove the device file
	if( ioport )
		release_region( ioport, iolen );
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
{
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
{
	printk(KERN_INFO "mypci mod_exit\n");

	pci_unregister_driver( &pci_drv );

	device_destroy( mypci_class, mypci_dev_number );  // remove the device file
	class_destroy( mypci_class );
	cdev_del( driver_object );
	unregister_chrdev_region( mypci_dev_number, 1 );
}

module_init(mod_init);
module_exit(mod_exit);
MODULE_LICENSE("GPL");
