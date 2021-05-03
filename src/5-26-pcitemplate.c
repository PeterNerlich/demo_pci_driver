/************************************************************************/
/* Quellcode zum Buch                                                   */
/*                     Linux Treiber entwickeln                         */
/* (3. Auflage) erschienen im dpunkt.verlag                             */
/* Copyright (c) 2004-2011 Juergen Quade und Eva-Katharina Kunst        */
/*                                                                      */
/* This program is free software; you can redistribute it and/or modify */
/* it under the terms of the GNU General Public License as published by */
/* the Free Software Foundation; either version 2 of the License, or    */
/* (at your option) any later version.                                  */
/*                                                                      */
/* This program is distributed in the hope that it will be useful,      */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of       */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the         */
/* GNU General Public License for more details.                         */
/*                                                                      */
/************************************************************************/
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/pci.h>
#include <linux/interrupt.h>

#define MY_VENDOR_ID 0x10b7 /* Hier Vendor-ID eintragen */
#define MY_DEVICE_ID 0x5157 /* Hier Geraete-ID eintragen */

static unsigned long ioport=0L, iolen=0L, memstart=0L, memlen=0L;

static dev_t mypci_dev_number;
static struct cdev *driver_object;
static struct class *mypci_class;
static struct device *mypci_dev;

static irqreturn_t pci_isr( int irq, void *dev_id )
{
	return IRQ_HANDLED;
}

static int device_init(struct pci_dev *pdev,
	const struct pci_device_id *id)
{
	if( pci_enable_device( pdev ) )
		return -EIO;
	ioport = pci_resource_start( pdev, 0 );
	iolen = pci_resource_len( pdev, 0 );
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
	if(request_irq(pdev->irq,pci_isr,IRQF_DISABLED|IRQF_SHARED,
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

	device_destroy( mypci_class, mypci_dev_number );
	class_destroy( mypci_class );
	cdev_del( driver_object );
	unregister_chrdev_region( mypci_dev_number, 1 );
}

module_init(mod_init);
module_exit(mod_exit);
MODULE_LICENSE("GPL");
