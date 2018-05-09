/* FRMorp - USB IMG file dumper for SPMP8k
 *
 * Copyright (C) 2010, openschemes.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
 /* Revision History
 * REV		DATE			NOTE
 *	1			2/9/10		Initial Release
 *	1.1		2/10/10		Added makefile, selectable OS
 *  1.2     4/23.10     Fixed the defunct ERR statements that prevented the
 *                      tool from properly failing in the event of an error.
 *  1.3		5/09/18		Ported this tool to libusb 1.0, so we can compile it for Win64
 */
 
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libusb.h"

#define DEBUG
//Uncomment for Linux
//#define LIN

#define	FNAME1 "RedBoot.mmp"
#define FNAME2 "DRAM_Init1.mmp"
#define FNAME3 "DRAM_Init2.mmp"
#define FNAME4 "BOOT.IMG"
#define FNAME5 "SOFT.IMG"

#define SECTION_MAGIC 0xA0C7C1D4
#define BOOTFS_HEADMAGIC 0xE9D0CDCD

// Yes, you can change this to your VID/PID
static uint16_t usbvid = 0x04FC;
static uint16_t usbpid = 0x7201;

// Our USB command packet structure;
#pragma pack(1)
typedef struct CBW{
				uint32_t   sig;
				uint32_t   tag;
				uint32_t   xlen;
				uint8_t    flag;
				uint8_t    lun;
				uint8_t    blen;
				uint32_t   cmd;
				uint32_t   adr;
				uint32_t   unk[2];
}__attribute__((packed)) CBW_t ; 

#define ERR(msg) { \
	printf("error %s\n", msg); \
	goto done; \
}

uint32_t lswap(uint32_t data){
	return (((data&0xFF)<<24) + ((data&0xFF00)<<8) + ((data&0xFF0000)>>8) + ((data&0xFF000000)>>24));
}

// Stolen from Daniel Drake's memtool - thanks!!
static libusb_device_handle *find_and_open_usbdev()
{
	/*
	struct usb_bus *bus, *busses;
	struct usb_device *udev = NULL;
	libusb_device_handle *dev;

	usb_find_busses();
	usb_find_devices();
	busses = usb_get_busses();

	for (bus = busses; bus; bus = bus->next) {
		struct usb_device *dev;
		for (dev = bus->devices; dev; dev = dev->next)
			if (dev->descriptor.idProduct == usbpid &&
					dev->descriptor.idVendor == usbvid)
				udev = dev;
	}*/
	
	libusb_device_handle *dev = libusb_open_device_with_vid_pid(NULL, usbvid, usbpid);

	if (!dev) {
		printf("Can't find device %04x:%04x\n", usbvid, usbpid);
		return NULL;
	}

	//dev = usb_open(udev);
	if (!dev) printf("error opening device\n");

	return dev;
}
////////////////////////////////////////////////////////////////////////////
uint32_t readpage(libusb_device_handle *dev, uint32_t page, char* pdatbuff, uint32_t bufsize, char* prepbuff, uint32_t repsize){
		uint32_t result=0;
		CBW_t mycbw;
		
		//Setup our generic USB packet - we will change stuff later if needed
		mycbw.sig=lswap(0x55534243); //USBC
		mycbw.tag = 0;//lswap(0x87654321); //Packet "serial number"  must it be unique?
		mycbw.xlen = lswap(0x00100000);// Nand page length
		mycbw.flag = 0x80;  // Output to device.  Try E0 if 80 is causing stalls
		mycbw.lun=0;
		mycbw.blen=0xA;
		mycbw.cmd = lswap(0xC2130000);//Read page command
		mycbw.adr=lswap(page);
		mycbw.unk[0]=0;
		mycbw.unk[1]=0;

		//#ifdef DEBUG
		//	printf("sizeof buffer:%0x\n", sizeof(mycbw));
		//#endif 

		//Send read page command
		//result = usb_bulk_write(dev, 0x02,(char *) &mycbw, sizeof(mycbw), 1000);
		libusb_bulk_transfer(dev, 0x02,(char *) &mycbw, sizeof(mycbw), &result, 1000);
		if(result != sizeof(mycbw)) return -1;
		//Get Data
		mycbw.flag = 0x00;  // Input from device
		result = libusb_bulk_transfer(dev, 0x81, pdatbuff, bufsize, NULL, 1000);
		if(result < 0 ) return -2;
		//Get reply
		mycbw.flag = 0x00;  // Input from device
		result = libusb_bulk_transfer(dev, 0x81, prepbuff, repsize, NULL, 1000);
		if(result < 0) return -3;
		return 0;
}
////////////////////////////////////////////////////////////////////////////
uint32_t getBOOTFSfile(libusb_device_handle* dev, uint32_t* patptr, char* fname ) {
	
	FILE *fo = NULL;
	uint32_t patbuff[0x400];
	uint32_t datbuff[0x400];
	uint32_t repbuff[0xD];
	uint32_t dsize, dpgcnt;
	uint32_t i, result;

	//Search for the BOOTFS PAT table
	printf("----------------------------------\n");
	printf("Dumping %s...\n",fname);
	//search 16 pages for next PAT - page allocation table - marked by magic word
	i=(*patptr)+ 0x10;
	patbuff[0]=0;
	while(((*patptr)< i) && (patbuff[0] != 0x55AACC33)){
		result = readpage(dev, *patptr, (char *) &patbuff, sizeof(patbuff), (char *) &repbuff, sizeof(repbuff));
		if (result != 0) return -1;
		(*patptr)+=1;
	}
	if(*patptr==i) {
		printf("No magic page!");
		return -2;
	}
		
	#ifdef DEBUG
		printf("Found magic page at 0x%0X \n", (*patptr)-1);
	#endif
		
	//Parse file size from PAT 3rd longword
	dsize=patbuff[2];	
	//Count the number of entires allocated to file, starting at entry 4
	dpgcnt=4;
	while((dpgcnt<(sizeof(patbuff)/4)) && (patbuff[dpgcnt]!=0xFFFFFFFF)){
		#ifdef DEBUG
			if(dpgcnt%4==0) printf("\n");
			printf("0x%08X ", patbuff[dpgcnt]);
		#endif
		dpgcnt+=1;
	}
	printf("\n");

	printf("First page is 0x%0X, length is 0x%0X bytes in 0x%0X pages\n", patbuff[4], dsize, dpgcnt-4);

	if ((fo = fopen(fname, "wb")) == NULL) return -3;
	
	//Step through the file's entries in the PAT and fetch their data to file
	printf("Dumping %s\n",fname);
    for(i=4; i<dpgcnt;i++){
		#ifdef DEBUG
			//if(i%4==0) printf("\n");
			printf("\b\b\b\b\b\b0x%0X ", patbuff[i]);
		#endif	
		result = readpage(dev, patbuff[i], (char *) &datbuff, sizeof(datbuff), (char *) &repbuff, sizeof(repbuff));
		if (result != 0){
			fclose(fo);
      return -4;
    }
		//Write a page to file.  Last page can be shorter than 0x1000
		if (dsize > 0x1000){
			fwrite(datbuff, sizeof(datbuff), 1, fo);
		}else{
			fwrite(datbuff, dsize, 1, fo);
		}
		dsize-=0x1000;
	}
	
	fclose(fo);
	printf("\n%s dumped successfully\n",fname);	
	//return file size
	return patbuff[2];
}
////////////////////////////////////////////////////////////////////////////
uint32_t packBOOTimg(char* fin1, char* fin2, char* fin3, char* fout){
	FILE *fi = NULL;
	FILE *fo = NULL;
	uint32_t i, size1,size2, size3, dsize, buffoff, result, chksize, chksum;
	
	uint32_t* buff1; //buffer for redboot
	uint32_t* buff2; //buffer for dram init 1
	uint32_t* buff3; //buffer for dram init 2
	
	dsize=0;//Total data size
	chksum=0;
		
	if ((fi = fopen(fin1, "rb")) == NULL) return -1;
	//Get first filesize
  fseek(fi , 0 , SEEK_END);
  size1 = ftell(fi);
  rewind(fi);
  dsize+=size1;
  //First buffer has 0x20 bytes for overall header + 0xC bytes for file header
  buff1 = (uint32_t*) malloc (size1+0x2C);
  //Set pointer offset to the start of the file header longword
	buffoff=8;
	//Make header
	//Stuff section magic word
	*(buff1+buffoff)=lswap(SECTION_MAGIC);
	buffoff+=1;
	//Stuff filesize
	*(buff1+buffoff)=size1;	
	buffoff+=1;
	//Stuff order indicator
	*(buff1+buffoff)=0x0;	
	buffoff+=1;		
	//Read data into buffer, leaving room for headers
	result = fread(&buff1[buffoff], 1, size1, fi);
	if (result != size1) return -2;
	fclose(fi);
	fi=NULL;
	//Do partial checksum
	if((size1/4) > 0x200) {
		chksize=0x200;
	}else{
		chksize = size1/4;
	}
	for(i=0xb; i<chksize+0xb; i++){
		chksum+=(*(buff1+i));
	}
	#ifdef DEBUG
		printf("Section 1 chk size is %0X is %0X\n",chksize, chksum);
	#endif
	
	
	//File 2
	if ((fi = fopen(fin2, "rb")) == NULL) return -3;
	//Get first filesize
  fseek(fi , 0 , SEEK_END);
  size2 = ftell(fi);
  rewind(fi);
  dsize+=size2;
  //Add 0xC bytes for file header
  buff2 = (uint32_t*) malloc (size2+0xC);
  //Set pointer offset to the start of the file header longword
	buffoff=0;
	//Make header
	//Stuff section magic word
	*(buff2+buffoff)=lswap(SECTION_MAGIC);
	buffoff+=1;
	//Stuff filesize
	*(buff2+buffoff)=size2;	
	buffoff+=1;
	//Stuff order indicator
	*(buff2+buffoff)=0x200;	
	buffoff+=1;		
	//Read data into buffer, leaving room for headers
	result = fread(&buff2[buffoff], 1, size2, fi);
	if (result != size2) return -4;
	fclose(fi);
	fi=NULL;
		//Do partial checksum
	if((size2/4) > 0x200) {
		chksize=0x200;
	}else{
		chksize = size2/4;
	}
	for(i=3; i<chksize+3; i++){
		chksum+=(*(buff2+i));
	}
	#ifdef DEBUG
		printf("Section 2 chk size is %0X is %0X\n",chksize, chksum);
	#endif
	
	//File 3
	if ((fi = fopen(fin3, "rb")) == NULL) return -5;
	//Get first filesize
  fseek(fi , 0 , SEEK_END);
  size3 = ftell(fi);
  rewind(fi);
  dsize+=size3;
  //Add 0xC bytes for file header
  buff3 = (uint32_t*) malloc (size3+0xC);
  //Set pointer offset to the start of the file header longword
	buffoff=0;
	//Make header
	//Stuff section magic word
	*(buff3+buffoff)=lswap(SECTION_MAGIC);
	buffoff+=1;
	//Stuff filesize
	*(buff3+buffoff)=size3;	
	buffoff+=1;
	//Stuff order indicator
	*(buff3+buffoff)=0x400;	
	buffoff+=1;		
	//Read data into buffer, leaving room for headers
	result = fread(&buff3[buffoff], 1, size3, fi);
	if (result != size3) return -6;
	fclose(fi);
	fi=NULL;
		//Do partial checksum
	if((size3/4) > 0x200) {
		chksize=0x200;
	}else{
		chksize = size3/4;
	}
	for(i=3; i<chksize+3; i++){
		chksum+=(*(buff3+i));
	}
	#ifdef DEBUG
		printf("Section 3 chk size is %0X is %0X\n",chksize, chksum);
	#endif
	
	buffoff=0;
	//Build main header
	*(buff1+buffoff)=lswap(BOOTFS_HEADMAGIC);
	buffoff+=1;
	*(buff1+buffoff)=size1+size2+size3 + 3*0xC; //Data length with section	 headers, no main header
	buffoff+=1;
	*(buff1+buffoff)=0x3;	//3 sections
	buffoff+=1;
	*(buff1+buffoff)=chksum;
	buffoff+=1;
	*(buff1+buffoff)=lswap(0x06000200);
	buffoff+=1;
	*(buff1+buffoff)=lswap(0x31313030);
	buffoff+=1;
	*(buff1+buffoff)=0;
	buffoff+=1;
	*(buff1+buffoff)=0;
		
	//Rearrange order of 2/3
	if ((fo = fopen(fout, "wb")) == NULL) return -7;
	fwrite(buff1, size1+0x2c, 1, fo);
	fwrite(buff2, size2+0xc, 1, fo);
	fwrite(buff3, size3+0xc, 1, fo);
	fclose(fo);
	
	free(buff1);
	free(buff2);
	free(buff3);
	
	printf("BOOT.IMG dumped and checksum'd\n");
	return dsize;	
}

////////////////////////////////////////////////////////////////////////////
uint32_t getSOFTimg(libusb_device_handle *dev, uint32_t* patptr, char* fname ) {
	
	FILE *fo = NULL;
	uint32_t patbuff[0x400];
	uint32_t datbuff[0x400];
	uint32_t repbuff[0xD];
	uint32_t dsize, dpgcnt;
	uint32_t i, result;
	

	//Search for the SOFT PAT table
	printf("----------------------------------\n");
	printf("Dumping %s...\n",fname);
	//search 16 pages for next PAT - page allocation table - marked by magic word "ROFS"
	i=(*patptr)+ 0x10;
	patbuff[0]=0;
	while(((*patptr)< i) && (patbuff[0] != 0x524F4653)){
		result = readpage(dev, *patptr, (char *) &patbuff, sizeof(patbuff), (char *) &repbuff, sizeof(repbuff));
		if (result != 0) return -1;
		(*patptr)+=1;
	}
	if(*patptr==i) {
		printf("No magic page!");
		return -2;
	}
		
	#ifdef DEBUG
		printf("Found ROFS magic page at 0x%0X \n", (*patptr)-1);
	#endif
		
	//Parse file size from PAT 4th longword
	dsize=patbuff[3];	
	printf("Length is 0x%0X bytes\n", dsize);
	
	if ((fo = fopen(fname, "wb")) == NULL) return -3;
	
	printf("Dumping %s\n",fname);
	//Hardwired first page
	i=0x2080;
	while(dsize>0){
		#ifdef DEBUG
			printf("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b");
			printf("0x%08X bytes, %5.2f%%", patbuff[3]-dsize, (float)100*(patbuff[3]-dsize)/patbuff[3]);
		#endif	
		result = readpage(dev, i, (char *) &datbuff, sizeof(datbuff), (char *) &repbuff, sizeof(repbuff));
		if (result != 0){
			fclose(fo);
      return -4;
    }
		//Write a page to file.  Last page can be shorter than 0x1000
		if (dsize > 0x1000){
			fwrite(datbuff, sizeof(datbuff), 1, fo);
			dsize-=0x1000;
		}else{
			fwrite(datbuff, dsize, 1, fo);
			dsize-=dsize;
		}
		i+=1;
		if((i%0x100) == 0) fflush(fo);
	}
	
	fclose(fo);
	printf("\n%s dumped successfully\n",fname);	
	//return file size
	return patbuff[2];
}

int main(int argc, char *argv[])
{
	libusb_device_handle *dev;
//	FILE *fi = NULL;
//	FILE *fo = NULL;
	
	CBW_t mycbw;
	
	int result;
	uint32_t patptr, i,j, finsize;
	uint32_t foutsize=0;
	uint32_t buffoff=0;
	uint32_t* foutbuff=NULL;
		
	dev=NULL;
	
  printf("========================================\n");
  printf("FRMorp (Version 1.3), openschemes.com\n");
  printf("Updated and modified by JBtronics\n");
  printf("SPMP8k USB IMG Dumper\n");
  printf("========================================\n");
  
  if(sizeof(mycbw) != 31){
		printf("Compiler error: Size of CBW is %d and should be 31", sizeof(mycbw));
		printf("Go look for the data padding flags, and turn that crap off!\n");
		return -1;
	} 
	
	
	libusb_init(NULL);
	dev = find_and_open_usbdev();
	libusb_reset_device(dev);
	if(!dev) {
             printf("Couldn't open device\n");
             return -1;
             }
	printf("Device opened...\n");
	
	#ifdef LIN
		if(usb_detach_kernel_driver_np(dev, 0)) printf("Detached..\n");
    #endif
	  result = libusb_set_configuration(dev, 1);
	  if(result) {
			printf("Could not set configuration, err:%d\n",result);
		  	goto done;
	  }
	  printf("Configuration Set...\n");
	  result = libusb_claim_interface(dev, 0);
	  if(result){
	  	printf("Could not claim interface, err:%d\n",result);
	 		goto done;
    }
    printf("Interface Claimed...\n");

	//Phase 1 - dump out the 3 boot files		
	printf("Phase 1 - Dumping files...\n");
	patptr=0;
	result =  getBOOTFSfile(dev, &patptr, FNAME1);
	if(result<0){
                 printf("Couldn't dump File 1");
                 return -1;
    }
	foutsize+=result;
	//Don't move patptr, he will have been updated by the redboot search
	result =  getBOOTFSfile(dev, &patptr, FNAME2);
	if(result<0){
                 printf("Couldn't dump File 2");
                 return -1;
    }
	foutsize+=result;
	//Move the patptr
	patptr=0x12;
	result =  getBOOTFSfile(dev, &patptr, FNAME3);
	if(result<0){
                 printf("Couldn't dump File 3");
                 return -1;
    }
	foutsize+=result;
	
	printf("Packing %s %s %s into %s\n", FNAME1, FNAME2, FNAME3, FNAME4);
	//Pack the files from BOOTFS dump into an IMG file for FRMPro
	result = packBOOTimg(FNAME1, FNAME2, FNAME3, FNAME4);
	
	//Phase 3 - Dump the Software image, rather stupidly with no error checking
	patptr=0x2000;
	result = getSOFTimg(dev, &patptr, FNAME5);
	
	
done:
    if (foutbuff != NULL) free(foutbuff);
//	if (fo != NULL) fclose(fo);
	if(dev !=NULL){
		libusb_release_interface(dev, 0);
		//usb_reset(dev);
		libusb_close(dev);
	}
	printf("Done!\n");
	return 0;
}
