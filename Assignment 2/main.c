#include<linux/kernel.h>
#include<linux/module.h>
#include<linux/usb.h>
#include <linux/slab.h>

#define be_to_int32(buf) (((buf)[0]<<24)|((buf)[1]<<16)|((buf)[2]<<8)|(buf)[3])
#define TOSHIBA_VID 0x0930
#define TOSHIBA_PID 0x6544
#define VID_PD_2 0x0781
#define PID_PD_2 0x5567

#define READ_CAPACITY_LENGTH          0x08
#define INQUIRY_LENGTH                0x24
#define BOMS_RESET                    0xFF
#define BOMS_RESET_REQ_TYPE           0x21


struct usbdev_private
{
	struct usb_device *device;
	unsigned char class;
	unsigned char subclass;
	unsigned char protocol;
	unsigned char ep_in;
	unsigned char ep_out;
};

// Section 5.1: Command Block Wrapper (CBW)
struct command_block_wrapper {
	uint8_t dCBWSignature[4];
	uint32_t dCBWTag;
	uint32_t dCBWDataTransferLength;
	uint8_t bmCBWFlags;
	uint8_t bCBWLUN;
	uint8_t bCBWCBLength;
	uint8_t CBWCB[16];
};

// Section 5.2: Command Status Wrapper (CSW)
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

//static struct usb_device *device;

static void usbdev_disconnect(struct usb_interface *interface)
{
	printk(KERN_INFO "USBDEV Device Removed\n");
	return;
}

static struct usb_device_id usbdev_table [] = {
	
	{USB_DEVICE(TOSHIBA_VID, TOSHIBA_PID)},
        {USB_DEVICE(VID_PD_2, PID_PD_2)},
	{} /*terminating entry*/	
};


static int send_mass_storage_command(struct usb_device *device, uint8_t endpoint, uint8_t *cdb, uint8_t direction, int data_length, uint32_t *ret_tag)
{
	static uint32_t tag = 1;
	uint8_t cdb_len;
	int i;
	struct command_block_wrapper *cbw;
	//int retval;
	int wrote_cnt;

	cbw=(struct command_block_wrapper *)kmalloc(sizeof(struct command_block_wrapper),GFP_KERNEL);//ALLOCATING HEAP SPACE TO CBW STRUCTURE
	

	cdb_len = cdb_length[cdb[0]];

	memset(cbw, 0, sizeof(*cbw));
	cbw->dCBWSignature[0] = 'U';
	cbw->dCBWSignature[1] = 'S';
	cbw->dCBWSignature[2] = 'B';
	cbw->dCBWSignature[3] = 'C';
	*ret_tag = tag;
	cbw->dCBWTag = tag++;
	cbw->dCBWDataTransferLength = data_length;
	cbw->bmCBWFlags = direction;


	cbw->bCBWLUN = 0;
	cbw->bCBWCBLength = cdb_len;
	memcpy(cbw->CBWCB, cdb, cdb_len);

	i = 0;
		

	    usb_bulk_msg(device, usb_sndbulkpipe(device, endpoint), (void*)cbw, 31, &wrote_cnt, 5000);
            printk(KERN_INFO"sent %d CDB bytes\n", cdb_len);
	    printk(KERN_INFO"sent %d bytes \n", wrote_cnt);

	
	return 0;
}




static int test_mass_storage(struct usb_device *device, uint8_t endpoint_in, uint8_t endpoint_out)
{
	int r1=0;
	//uint8_t lun;
	uint32_t expected_tag,max_lba, block_size;
	//uint32_t i;

	uint32_t device_size;
	uint8_t cdb[16];	// SCSI Command Descriptor Block
	uint8_t *buffer=(uint8_t *)kmalloc(64*sizeof(uint8_t),GFP_KERNEL); // ALLOCATING HEAP SPACE BUFFER VARIABLE	
	//unsigned char *data;
	int read_cnt;
	
	   printk("Reset mass storage device");
	r1 = usb_control_msg(device,usb_sndctrlpipe(device,0),BOMS_RESET,BOMS_RESET_REQ_TYPE,0,0,NULL,0,1000);
	if(r1<0)
		printk("error code: %d",r1);
	else
		printk("successful Reset");



///////////////////////////////////////////// Read capacity SCSI COMMAND//////////////////////////////////////////
	printk("Reading Capacity:\n");
	memset(buffer, 0, sizeof(buffer));
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x25;	// Read Capacity
	

	send_mass_storage_command(device, endpoint_out, cdb,USB_DIR_IN, READ_CAPACITY_LENGTH, &expected_tag);

        usb_bulk_msg(device, usb_rcvbulkpipe(device, endpoint_in),(void*)buffer, READ_CAPACITY_LENGTH, &read_cnt, 5000);
		


	printk("   received %d bytes\n", read_cnt);
        printk(KERN_INFO"value of &buffer[0] %d\n",buffer[0]);
	printk("&buffer[4]: %x",&buffer[4]);

	max_lba = be_to_int32(&buffer[0]);
	block_size = be_to_int32(&buffer[4]);
	device_size = ((long)(max_lba+1))*block_size/(1024*1024);
/////////////////////////////////// PRINTING SIZE OF PENDRIVE IN MB///////////////////////////////////////////////////////////////////////////
	printk("   Max LBA: %x, Block Size: %x ( PD SIZE %d MB)\n", max_lba, block_size, device_size);
	printk("PD Size is %d MB", device_size);
	
	

	return 0;
}

///////////////////////////////////////////PROBE FUNCTION PRINTING CONTENT OF INTERFACE DESCRIPTOR/////////////////////////////////////////////
////////////////// VID PID SUBCLASS CLASS ENDPOINTS AND THERE TYPE ////////////////////////////////////////////////////////////////////////////
static int usbdev_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	int i;
	unsigned char epAddr, epAttr,endpoint_in, endpoint_out;
	struct usb_endpoint_descriptor *ep_desc;
	
	 if(id->idVendor == TOSHIBA_VID && id->idProduct == TOSHIBA_PID)
	{
		printk(KERN_INFO "Known USB drive detected \n");
	}
        else if(id->idVendor == VID_PD_2 && id->idProduct == PID_PD_2)
		{
			printk(KERN_INFO "Known USB drive detected \n");
		}

	
	struct usb_device *device = interface_to_usbdev(interface);
	
	printk(KERN_INFO "Vendor ID = %04X\n",  id->idVendor);
	printk(KERN_INFO "Product ID = %04X\n", id->idProduct);	
	printk(KERN_INFO "USB DEVICE CLASS : %x", interface->cur_altsetting->desc.bInterfaceClass);
	printk(KERN_INFO "USB DEVICE SUB CLASS : %x", interface->cur_altsetting->desc.bInterfaceSubClass);
	printk(KERN_INFO "USB DEVICE Protocol : %x", interface->cur_altsetting->desc.bInterfaceProtocol);
	printk(KERN_INFO "No. of Altsettings = %d\n",interface->num_altsetting);
	printk(KERN_INFO "No. of Endpoints = %d\n", interface->cur_altsetting->desc.bNumEndpoints);

	for(i=0;i<interface->cur_altsetting->desc.bNumEndpoints;i++)
	{
		ep_desc = &interface->cur_altsetting->endpoint[i].desc;
		epAddr = ep_desc->bEndpointAddress;
		epAttr = ep_desc->bmAttributes;
	
		if((epAttr & 0x2)==0x2)
	
		{
			if(epAddr & 0x80)
				{
				endpoint_in = epAddr;
				printk(KERN_INFO "EP %d and endpoint is Bulk IN\n", i);
				
				}
			else
				{
				endpoint_out = epAddr;
				printk(KERN_INFO "EP %d and endpoint is Bulk OUT\n", i);
				
				}	
		}

	}

		test_mass_storage(device, endpoint_in, endpoint_out);

return 0;
}



///////////////////////////////////////////////Operations structure/////////////////////////////////////////////////////////////////////////
static struct usb_driver usbdev_driver = {
	name: "usbdev",  //name of the device
	probe: usbdev_probe, // Whenever Device is plugged in
	disconnect: usbdev_disconnect, // When we remove a device
	id_table: usbdev_table, //  List of devices served by this driver
};


int device_init(void)
{
	usb_register(&usbdev_driver);
	printk(KERN_NOTICE "UAS READ Capacity Driver Inserted\n");
	return 0;
}

void device_exit(void)
{
	usb_deregister(&usbdev_driver);
	printk(KERN_NOTICE "Leaving Kernel\n");

}

module_init(device_init);
module_exit(device_exit);
MODULE_LICENSE("GPL");

