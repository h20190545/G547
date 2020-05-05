Ketan nirmal     2019H140070G

Pankaj chandnani 209H1400545G

.>>>>>>>>>USB BLOCK DRIVER CODE FOR READING AND WRITING FILES IN USB USING SCSI COMMANDS <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< 

step 0: open main.c and enter  vid and pid of your usb device

step 1: Go to the directory (assignment3) and give the command  $ make all

step 2: remove kernel drivers using commands   $ sudo rmmod uas 
                                               $ sudo rmmod usb_storage 

step 3: insert the module using  $ sudo insmod main.ko

step 4: check whether module is loaded using command  $ lsmod 

step 5: connect pendrive ,then again follow step 2 (as they may get installed again)

step 6: make folder inside media directory  $ sudo mkdir /media/pusb

step 7: use mount command for accessing pendrive  $ sudo mount -t vfat /dev/myDevice1 /media/pusb

step 8: go inside root  $ sudo -i

step 9: go inside pusb folder of media directory  $ cd /media/pusb/

step 10: command to see content of pendrive  $ ls

step 11: command to create new text file and writing into it  $ echo HelloWorld>test.txt

step 12: command to read content of file  $ cat test.txt

step 13: command to come outside of root  $ logout

step 14: remove pendrive and again connect ,check its content ,you can clearly see new file test.txt in it along with already existing files

