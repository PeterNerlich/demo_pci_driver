#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <asm/uaccess.h>

#define MY_VENDOR_ID 0x144a
#define MY_DEVICE_ID 0x9111

/*
int pci_read_config_byte(struct pci_def *dev, int offset, u8 *val);
int pci_read_config_word(struct pci_def *dev, int offset, u16 *val);
int pci_read_config_dword(struct pci_def *dev, int offset, u32 *val);
*/

static char hello_world[] = "Hello World\n";

static unsigned long ioport=0L, iolen=0L, memstart=0L, memlen=0L;

static dev_t mypci_dev_number;
static struct cdev *driver_object;
static struct class *mypci_class;
static struct device *mypci_dev;

static int driver_open( struct inode *devfile, struct file *instance )
{
	dev_info( mypci_dev, "driver_open called\n" );
	return 0;
}

static int driver_close( struct inode *devfile, struct file *instance )
{
	dev_info( mypci_dev, "driver_close called\n" );
	return 0;
}

static ssize_t driver_read( struct file *instance, char __user *user, size_t count, loff_t *offset )
{
	unsigned long not_copied, to_copy;

	to_copy = min(count, strlen(hello_world)+1);
	not_copied = raw_copy_to_user(user, hello_world, to_copy);
	*offset += to_copy - not_copied;
	return to_copy - not_copied;
}

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.read = driver_read,
	.open = driver_open,
	.release = driver_close,
};

static irqreturn_t pci_isr( int irq, void *dev_id )
{
	return IRQ_HANDLED;
}

static int device_init(struct pci_dev *pdev,
	const struct pci_device_id *id)
{
	if( pci_enable_device( pdev ) )
		return -EIO;
	ioport = pci_resource_start( pdev, 2 );
	iolen = pci_resource_len( pdev, 2 );
	if( request_region( ioport, iolen, pdev->dev.kobj.name )==NULL ) {
		dev_err(&pdev->dev,"I/O address conflict for device \"%s\"\n",
			pdev->dev.kobj.name);
		return -EIO;
	}
	memstart = pci_resource_start( pdev, 1 );
	memlen = pci_resource_len( pdev, 1 );
	if( request_mem_region(memstart,memlen,pdev->dev.kobj.name)==NULL ) {
		dev_err(&pdev->dev,"Memory address conflict for device\n");
		goto cleanup_ports;
	}
	if(request_irq(pdev->irq,pci_isr,IRQF_SHARED,
		"mypci",pdev)) {
		dev_err(&pdev->dev,"mypci: IRQ %d not free.\n",pdev->irq);
		goto cleanup_mem;
	}
	return 0;
cleanup_mem:
	release_mem_region( memstart, memlen );
cleanup_ports:
	release_region( ioport, iolen );
	return -EIO;
}

static void device_deinit( struct pci_dev *pdev )
{
	free_irq( pdev->irq, pdev );
	if( ioport )
		release_region( ioport, iolen );
	if( memstart )
		release_mem_region( memstart, memlen );
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
	if( alloc_chrdev_region(&mypci_dev_number,0,1,"mypci")<0 )
		return -EIO;
	driver_object = cdev_alloc(); /* Anmeldeobjekt reservieren */
	if( driver_object==NULL ) goto free_dev_number;
		driver_object->owner = THIS_MODULE;
	driver_object->ops = &fops;
	if( cdev_add(driver_object,mypci_dev_number,1) )
		goto free_cdev;
	/* Eintrag im Sysfs, damit Udev den Geraetedateieintrag erzeugt. */
	mypci_class = class_create( THIS_MODULE, "mypci" );
	if( IS_ERR( mypci_class ) ) {
		pr_err( "mypci: no udev support available\n");
		goto free_cdev;
	}
	mypci_dev = device_create( mypci_class, NULL, mypci_dev_number,
		NULL, "%s", "mypci" );

	if( pci_register_driver(&pci_drv)<0)  {
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
	pci_unregister_driver( &pci_drv );

	device_destroy( mypci_class, mypci_dev_number );  // remove the device file
	class_destroy( mypci_class );
	cdev_del( driver_object );
	unregister_chrdev_region( mypci_dev_number, 1 );
}

module_init(mod_init);
module_exit(mod_exit);
MODULE_LICENSE("GPL");
