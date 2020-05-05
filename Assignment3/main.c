
#include<linux/kernel.h>
#include<linux/init.h>
#include<linux/module.h>
#include<linux/slab.h>
#include<linux/usb.h>
#include<linux/blkdev.h>
#include<linux/genhd.h>
#include<linux/spinlock.h>
#include<linux/bio.h>
#include<linux/fs.h>
#include<linux/interrupt.h>
#include<linux/workqueue.h>
#include<linux/sched.h>

#define DEVICE_NAME "myDevice"
#define NR_OF_SECTORS 30273535
#define SECTOR_SIZE 512
#define CARD_CAPACITY  (NR_OF_SECTORS*SECTOR_SIZE)
#define bio_iovec_idx(bio, idx)	(&((bio)->bi_io_vec[(idx)]))
#define __bio_kmap_atomic(bio, idx, kmtype)				\
	(kmap_atomic(bio_iovec_idx((bio), (idx))->bv_page) +	\
		bio_iovec_idx((bio), (idx))->bv_offset)


#define __bio_kunmap_atomic(addr, kmtype) kunmap_atomic(addr)

#define toshiba_VID  0x0930
#define toshiba_PID  0x6544

#define BOMS_RESET                    0xFF
#define BOMS_RESET_REQ_TYPE           0x21
#define BOMS_GET_MAX_LUN              0xFE
#define BOMS_GET_MAX_LUN_REQ_TYPE     0xA1
#define READ_CAPACITY_LENGTH	      0x08
#define REQUEST_DATA_LENGTH           0x12
#define be_to_int32(buf) (((buf)[0]<<24)|((buf)[1]<<16)|((buf)[2]<<8)|(buf)[3])

	// ***********************************************Command Block Wrapper*******************************************
struct gendisk *disk_gend = NULL;
struct command_block_wrapper {
	uint8_t dCBWSignature[4];
	uint32_t dCBWTag;
	uint32_t dCBWDataTransferLength;
	uint8_t bmCBWFlags;
	uint8_t bCBWLUN;
	uint8_t bCBWCBLength;
	uint8_t CBWCB[16];
};

	// ***********************************************Command Status Wrapper*******************************************
struct command_status_wrapper {
	uint8_t dCSWSignature[4];
	uint32_t dCSWTag;
	uint32_t dCSWDataResidue;
	uint8_t bCSWStatus;
}; 



 static uint8_t cdb_length[256] = {
//	 0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
	06,06,06,06,06,06,06,06,06,06,06,06,06,06,06,06,  //  0
	06,06,06,06,06,06,06,06,06,06,06,06,06,06,06,06,  //  1
	10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,  //  2
	10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,  //  3
	10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,  //  4
	10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,  //  5
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  6
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  7
	16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,  //  8
	16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,  //  9
	12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,  //  A
	12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,  //  B
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  C
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  D
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  E
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  F
}; 


	// ***********************************************USB Device ID Table*******************************************
static struct usb_device_id usbdev_table [] = {
	{USB_DEVICE(toshiba_VID,toshiba_PID)},
	{} /*terminating entry*/	
};

struct usb_device *device;
uint8_t endpoint_in , endpoint_out ;

struct blkdev_private{
        int size;                      
        u8 *data;                        
        short users;                    // No. of users 
        short media_change;             // Flag for media change 
        spinlock_t lock;                // Spinlock for mutual exclusion 
        struct request_queue *queue;    // The device request queue 
        struct gendisk *gd;             // The gendisk structure 
           
};	


struct request *req;
static struct blkdev_private *p_blkdev = NULL;

// ***********************************************USB Disconnect Function*******************************************

static void usbdev_disconnect(struct usb_interface *interface)
{
	printk(KERN_INFO "USBDEV Device Removed\n");
	struct gendisk *disk_gend = p_blkdev->gd;
	del_gendisk(disk_gend);
	blk_cleanup_queue(p_blkdev->queue);
	kfree(p_blkdev);
	return;
}

// ***********************************************Status of USB device*******************************************

static int get_mass_storage_status(struct usb_device *device, uint8_t endpoint, uint32_t expected_tag)
{	
	int r;
	int size;	
	
	struct command_status_wrapper *csw;
	csw=(struct command_status_wrapper *)kmalloc(sizeof(struct command_status_wrapper),GFP_KERNEL);
	r=usb_bulk_msg(device,usb_rcvbulkpipe(device,endpoint),(void*)csw,13, &size, 1000);
	if(r<0)
		printk("error in status");
	
	if (csw->dCSWTag != expected_tag) {
		printk("   get_mass_storage_status: mismatched tags (expected %08X, received %08X)\n",
			expected_tag, csw->dCSWTag);
		return -1;
	}
	if (size != 13) {
		printk("   get_mass_storage_status: received %d bytes (expected 13)\n", size);
		return -1;
	}	
	printk(KERN_INFO "Mass Storage Status: %02X (%s)\n", csw->bCSWStatus, csw->bCSWStatus?"FAILED":"Success");
	return 0;
}  

// ***********************************************Sending Commands to USB Device*******************************************

static int send_command(struct usb_device *device,uint8_t endpoint,
                         uint8_t *cdb, uint8_t direction, int data_length, uint32_t *ret_tag)
{
	
	uint32_t tag = 1;
	int r;
	int size;
	uint8_t cdb_len;
	struct command_block_wrapper *cbw;
	cbw=(struct command_block_wrapper *)kmalloc(sizeof(struct command_block_wrapper),GFP_KERNEL);
	
	if (cdb == NULL) {
		return -1;
	}
	if (endpoint & USB_DIR_IN) {
		printk("send_mass_storage_command: cannot send command on IN endpoint\n");
		return -1;
	}	
	cdb_len = cdb_length[cdb[0]];
	if ((cdb_len == 0) || (cdb_len > sizeof(cbw->CBWCB))) {
		printk("Invalid command\n");
		return -1;
	}	

	memset(cbw, 0, sizeof(*cbw));
	cbw->dCBWSignature[0] = 'U';
	cbw->dCBWSignature[1] = 'S';
	cbw->dCBWSignature[2] = 'B';
	cbw->dCBWSignature[3] = 'C';
	*ret_tag = tag;
	cbw->dCBWTag = tag++;
	cbw->dCBWDataTransferLength = data_length;
	cbw->bmCBWFlags = direction;
	cbw->bCBWLUN =0;
	cbw->bCBWCBLength = cdb_len;
	memcpy(cbw->CBWCB, cdb, cdb_len);
	

	r = usb_bulk_msg(device,usb_sndbulkpipe(device,endpoint),(void*)cbw,31, &size,1000);
	if(r!=0)
		printk("Command transfer failed %d",r);
	
	return 0;
} 

// ***********************************************Read from Device*******************************************
static int device_read(sector_t initial_sector,sector_t nr_sect,char *page_address)
{
	int result;
	unsigned int size;
	uint8_t cdb[16];	// SCSI Command Descriptor block
	uint32_t expected_tag;
	size=0;
	memset(cdb,0,sizeof(cdb));
	cdb[0] = 0x28;	// Read(10)
	cdb[2]=(initial_sector>>24) & 0xFF;
	cdb[3]=(initial_sector>>16) & 0xFF;
	cdb[4]=(initial_sector>>8) & 0xFF;
	cdb[5]=(initial_sector>>0) & 0xFF;
	cdb[7]=(nr_sect>>8) & 0xFF;
	cdb[8]=(nr_sect>>0) & 0xFF;	// 1 block


	send_command(device,endpoint_out,cdb,USB_DIR_IN,(nr_sect*512),&expected_tag);
	result=usb_bulk_msg(device,usb_rcvbulkpipe(device,endpoint_in),(void*)(page_address),(nr_sect*512),&size, 5000);

	get_mass_storage_status(device, endpoint_in, expected_tag);  
	return 0;
}

// ***********************************************Write to the device*******************************************

 static int device_write(sector_t initial_sector,sector_t nr_sect,char *page_address)
{   //int i;
	int result;
	unsigned int size;
	uint8_t cdb[16];	// SCSI Command Descriptor Block
	uint32_t expected_tag;
	//printk(KERN_INFO "write command------>");
	memset(cdb,0,sizeof(cdb));
	cdb[0]=0x2A;
	cdb[2]=(initial_sector>>24)&0xFF;
	cdb[3]=(initial_sector>>16)&0xFF;
	cdb[4]=(initial_sector>>8)&0xFF;
	cdb[5]=(initial_sector>>0)&0xFF;
	cdb[7]=(nr_sect>>8)&0xFF;
	cdb[8]=(nr_sect>>0)&0xFF;	// 1 block
	cdb[8]=0x01;
	send_command(device,endpoint_out,cdb,USB_DIR_OUT,nr_sect*512,&expected_tag);
	result=usb_bulk_msg(device,usb_sndbulkpipe(device,endpoint_out),(void*)page_address,nr_sect*512,&size, 1000);
	
	get_mass_storage_status(device, endpoint_in, expected_tag); 
	return 0;

}  

static void device_transfer(sector_t sector,sector_t nsect, char *buffer, int write)
{
    unsigned long offset = sector*512;
    unsigned long nbytes = nsect*512;

    if ((offset + nbytes) > (NR_OF_SECTORS*512)) {
        printk (KERN_NOTICE "Beyond-end write (%ld %ld)\n", offset, nbytes);
        return;
    }
     if (write)
        device_write(sector,nsect,buffer);
    else
        device_read(sector,nsect,buffer);
    return; 
}  



static int transfer_on_request(struct request *req)
{
    int i;
    struct bio_vec bvec;
    struct req_iterator iter;
    sector_t sector_offset;
    sector_t sector = req->bio->bi_iter.bi_sector;
    sector_offset = 0;
	//Transfer of each segment
    rq_for_each_segment(bvec,req,iter){
    	char *buffer = __bio_kmap_atomic(req->bio, i, KM_USER0);
    	device_transfer(sector,((bvec.bv_len)/512),buffer, bio_data_dir(req->bio)==WRITE);
    	sector_offset += (bvec.bv_len)/512;
    	__bio_kunmap_atomic(req->bio, KM_USER0);
        printk(KERN_DEBUG "Sector: %llu \t Sector Offset: %llu \t  Length: %u sectors\n",\
		(unsigned long long)(sector), (unsigned long long)(sector_offset), (bvec.bv_len)/512);
    }
    return 0; // Always "succeed" 
}  

// ***********************************************Workqueue Structure*******************************************

static struct workqueue_struct *my_workqueue=NULL;
struct dev_work{   
	struct work_struct work; 	// work struct
	struct request *req;
 };

static void my_work_function(struct work_struct *work)
{
	struct dev_work *my_work=container_of(work,struct dev_work,work);
	
	transfer_on_request(my_work->req);  	//Scheduling delayed operation
	__blk_end_request_cur(my_work->req,0);
	kfree(my_work);
	
	return;
}

void req_handle(struct request_queue *q)  // Request Handle function, executed for each request 
{
	struct request *req;  // Local req structure
	int sectors_xferred;
	struct dev_work *my_work=NULL;
  
	

	while((req=blk_fetch_request(q)) != NULL)		//Fetching request from front of queue
	{
		if(req == NULL && !blk_rq_is_passthrough(req)) // Ignoring non fs request
		{
			printk(KERN_INFO "non FS request");
			__blk_end_request_all(req, -EIO);
			continue;
		}
		
		my_work=(struct dev_work *)kmalloc(sizeof(struct dev_work),GFP_ATOMIC);
		if(my_work==NULL){

			printk("Memory Allocation to deferred work failed");
			__blk_end_request_all(req, 0);
			continue;
		}

		my_work->req=req;
		INIT_WORK(&my_work->work,my_work_function);	//Initializing work item for workqueue
		queue_work(my_workqueue,&my_work->work);
		
	}	
} 

// ***********************************************Block Device Operation Functions*******************************************
static int blkdev_open(struct block_device *bdev, fmode_t mode)
{
	
    struct blkdev_private *dev = bdev->bd_disk->private_data;
    spin_lock(&dev->lock);
    if (! dev->users) 
        check_disk_change(bdev);	
    dev->users++;
    spin_unlock(&dev->lock);
    return 0;
}

static void blkdev_release(struct gendisk *gdisk, fmode_t mode)
{
	
    struct blkdev_private *dev = gdisk->private_data;
    spin_lock(&dev->lock);
    dev->users--;
    spin_unlock(&dev->lock);

    return 0;
}

static struct block_device_operations blkdev_ops =
{
	.owner= THIS_MODULE,
	.open=blkdev_open,
	.release=blkdev_release
};

// ***********************************************USB Probe Function*******************************************

static int usbdev_probe(struct usb_interface *interface, const struct usb_device_id *id)
{	int i, type;
	unsigned char epAddr;
	int device_major=0;
	
	struct usb_endpoint_descriptor *ep_desc;
	if(id->idProduct == toshiba_PID && id->idVendor==toshiba_VID)
	{
		printk(KERN_INFO "“Known USB drive detected”\n");
	}

	device=interface_to_usbdev(interface);

	for(i=0;i<interface->cur_altsetting->desc.bNumEndpoints;i++)
	{
		ep_desc = &interface->cur_altsetting->endpoint[i].desc;
		epAddr = ep_desc->bEndpointAddress;
		type=usb_endpoint_type(ep_desc);
		if(type==2){
		if(epAddr & 0x80)
		{		
			printk(KERN_INFO "EP %d is Bulk IN\n", i);
			endpoint_in=ep_desc->bEndpointAddress;
			//printk("endpoint_in : %x",endpoint_in);
			
		}
		else
		{	
			endpoint_out=ep_desc->bEndpointAddress;
			printk(KERN_INFO "EP %d is Bulk OUT\n", i); 
			//printk("endpoint_out : %x",endpoint_out);
		}
		}
		if(type==3)
		{
		if(epAddr && 0x80)
			printk(KERN_INFO "EP %d is Interrupt IN\n", i);
		else
			printk(KERN_INFO "EP %d is Interrupt OUT\n", i);
		}
		}

		

	
	device_major = register_blkdev(0, "USB DISK");  //Block device registration, which will return major no.
	if (device_major < 0) 
		printk(KERN_WARNING "USB_Disk: unable to get major number\n");
	
	p_blkdev = kmalloc(sizeof(struct blkdev_private),GFP_KERNEL); // Private struct allocation
	
	if(!p_blkdev)
	{
		printk("ENOMEM  at %d\n",__LINE__);
		return 0;
	}
	memset(p_blkdev, 0, sizeof(struct blkdev_private)); 

	spin_lock_init(&p_blkdev->lock);  // Holding the lock during initializing and manipulating req queue

	p_blkdev->queue = blk_init_queue(req_handle, &p_blkdev->lock);  // Queue initialization, and relating the request handle fun 
  
	disk_gend = p_blkdev->gd = alloc_disk(2); // Allocation for gendisk struct
	if(!disk_gend)
	{
		kfree(p_blkdev);
		printk(KERN_INFO "alloc_disk failed\n");
		return 0;
	}

	//  Initialization of gendisk structure members
	disk_gend->major =device_major;
	disk_gend->first_minor = 0;
	disk_gend->fops = &blkdev_ops;		//File operation structure
	disk_gend->queue = p_blkdev->queue;
	disk_gend->private_data = p_blkdev;
	strcpy(disk_gend->disk_name, DEVICE_NAME);
	set_capacity(disk_gend,NR_OF_SECTORS); // Setting Capacity of our device  
	add_disk(disk_gend);  			// Finally adding the disk after all initializations

return 0;
}

static struct usb_driver usbdev_driver = {
	name: "usbdev",  	             //name of the device
	probe: usbdev_probe,		    //Probe function, called when usb device is detected
	disconnect: usbdev_disconnect,     //Disconnect function, called on removal of usb device
	id_table: usbdev_table,		   //Table for all the supported usb devices
};

int block_init(void)
{
	usb_register(&usbdev_driver);
	printk(KERN_NOTICE "UAS READ Capacity Driver Inserted\n");
	my_workqueue=create_workqueue("my_workqueue");  // Creation of workqueue
	return 0;	
}

void block_exit(void)
{ 
	usb_deregister(&usbdev_driver);
	printk("Device driver unregister");
	flush_workqueue(my_workqueue);  // to exit the work done
	destroy_workqueue(my_workqueue);
	return;
}


module_init(block_init);
module_exit(block_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pankaj Chandnani, Ketan Nirmal");
MODULE_DESCRIPTION("DISK");
