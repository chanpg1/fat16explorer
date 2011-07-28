#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

// bootsector structure : unaltered (little endian)
// for casting over buf and potentially writing to a disk.
typedef struct {
	unsigned char jmp[3];			//0-2
	unsigned char oem[8];			//3-10
	unsigned char bytesInSector[2] ;		//11-12
	unsigned char sectorInCluster ;		//13
	unsigned char numResSector[2];		//14-15
	unsigned char numFatCopies;		//16
	unsigned char numRootDirs[2];		//17-18
	unsigned char numSectFS[2];		//19-20
	unsigned char mediaDesc;			//21
	unsigned char numSectFat[2];		//22-23
	unsigned char numSectTrac[2];		//24-25
	unsigned char numheads [2];		//26-27
	unsigned char numHiddenSect[2];		//28-29
	unsigned char bootstrap[480];		//30-509
	unsigned char sig[2];			//510-511  sig= 55 aa
}bootsector;

// for storing values for calculations
typedef struct{
	int bytesInSector;
	int sectorInCluster;
	int numResSector;
	int numFatCopies;
    	int numRootDirs;	    
	int numSectFS;
	int mediaDesc;
	int numSectFat;
	int numSectTrac;
	int numheads;
	int numHiddenSect;
}bootSectorValues;
// instances of above
bootsector* bs;
bootSectorValues* bsValues ;
// for retrieving data from disk at a time.
int bufsize = 512;
int bytesPerEntry = 32;  // bytes per directory entry
// parameters for calculating position of disk data structures
int bytesPerCluster;
int bytesPerFAT;
int reserveStartBytePos;  // set to zero if doesnt exist
int FAT0bytePosition;
int FAT1bytePosition;
int dirBytePosition;
int bootSize;
int reserveSize;
int FAT0bytePosition;
int FATSectorPosition;
int RootDirSectorPosition;
int bytesPerRootDir;
int sectorsPerRootDir;
int xtraRootDirByteCount;
int DataSectorPosition; // note that first cluster number is 2 at this address
int DataBytePosition;   // note that first cluster number is 2 at this address

// endof file mark used in fat cluster entry as sign of last cluster in file
// stored as val1 (second entry ) that has to be retrieed using 
// LEnd3ByteVal1
unsigned char eofMark[3];  // cluster mark for last cluster in chain
int startByteOfCurrentBlock; // keep track of what address is currently in buff
int fildes;  // fildescriptor opened in main so all can access
//void* buf;   // buff to hold cache block from fildes
int fd;

char* dotdir =   ".          ";
char* doubledot= "..         ";
char dir[255]="/";
// utility methods
// if we want to rewrite 2,3 or four byte arrays
void swapBytes(unsigned char* a, int length){
	unsigned char tmp;
	if (length == 2) {
	    tmp = a[0];
	    a[0] = a[1];
	    a[1] = tmp;
	}else if (length ==3){
	    tmp=a[0];
	    a[0]=a[2];
	    a[2]=tmp; 
	}else if (length == 4) {
		tmp=a[0];
		a[0]=a[3];
		a[3]=tmp;
		tmp=a[1];
		a[1]=a[2];
		a[2]=tmp;
	}

}
// given 2bytes in little endian, get the unsigned extended int value
int LEnd2ByteToInt(unsigned char* a){
	//printf("lend2byte received :   %02x %02x   \n",  a[0],a[1] );
	//return  ( ((int) a[1] )<<8 ) | a[0] ;	
	return    (int) (a[1] <<8  | a[0]) ;	
}
// given 3bytes in little endian get the unsigned extended int value
int LEnd3ByteToInt(unsigned char*a){
	return (int) ( a[2]<<(2*8) | (a[1]<<8) | a[0] 	);

}
// given 3bytes in little endian, get the first half value
// eg uv wx yz   ->  xuv and yzw  with the xuv being the zeroth element
int LEnd3ByteVal0(unsigned char * a){
	//return (int) ( a[2]<<(2*8) | (a[1]<<8) | a[0] 	)%4096;  // low order 12 bits is first entry
	//return  ( ((int)a[2])<<(2*8) | ((int)a[1])<<8 | (int)a[0] 	)&0x00000fff;  // low order 12 bits is first entry
//	printf("\nconverting lend3byte to val0\n");
//	printf("3values %x %x %x \n", a[0], a[1], a[2]);
//	printf("3values shifted and or %x  \n", a[2]<<(8*2) | a[1]<<8 | a[0]);	
//	printf("3val shift or and masked  %x  \n",( a[2]<<(8*2) | a[1]<<8 | a[0]) & 0x00000fff  );	
//	printf("middle bits shifted and masked %x  \n",(( a[2]<<(8*2) | a[1]<<8 | a[0]) & 0x00fff000 )>>12 );	
    	return  ( ((int)a[2])<<(2*8) | ((int)a[1])<<8 | (int)a[0] 	)&0x00000fff;  // low order 12 bits is first entry
}
// given 3bytes in little endian, get the first half value
// eg uv wx yz   ->  xuv and yzw  with the yzw being second element
int LEnd3ByteVal1(unsigned char* a){
	//return (int) ( a[2]<<(2*8) | (a[1]<<8) | a[0] 	)/4096;  // high order 12 bits is second entry ...little endian.....sigh.....
	return  (( ((int)a[2])<<(2*8) | ((int)a[1])<<8 | (int)a[0] 	)&0x00fff000)>>12;  // low order 12 bits is first entry

}
// given 4 byte little endian value get the integer value
int LEnd4ByteToInt(unsigned char *a){
	return (int) (a[3]<<(3*8) | a[2]<<(2*8) | a[1]<<(1*8) | a[0] );
	//return (int) ( a[0] );
}



// 
// get a block of data into a buffer
int getBSdata (int fd){
	bsValues = (bootSectorValues* )malloc( sizeof(bootSectorValues));
	int rresult;
	int leastByteMask=0x000000FF;

	unsigned char* buf = (unsigned char*) malloc(bufsize);
	rresult =  read (fd, buf,bufsize );
	startByteOfCurrentBlock=0;
	if ( rresult >0){
    		printf("read %d bytes\n", rresult);
		bs = (bootsector*) buf;
//		printf(" %d %d %d \n",bs->jmp[0],bs->jmp[1],bs->jmp[3]);
		printf(" jmp to bootstrap  :  %02x %02x %02x \n",leastByteMask & bs->jmp[0],bs->jmp[1],bs->jmp[3]);
		printf(" OEM name          :  ",leastByteMask & bs->jmp[0],bs->jmp[1],bs->jmp[3]);
		int i ;
		for (i=0;i<8;i++) printf("%c",bs->oem[i]);
		printf("\n");
		printf("Bytes per Sector   :  %02x %02x  swapped %x  dec value %d \n",  
			bs->bytesInSector[0] ,
			bs->bytesInSector[1],
			LEnd2ByteToInt(bs->bytesInSector),
			LEnd2ByteToInt(bs->bytesInSector));
		bsValues->bytesInSector=LEnd2ByteToInt(bs->bytesInSector);
		printf("Sect per Cluster   :  %02x dec value %d \n",  bs->sectorInCluster, bs->sectorInCluster);
		bsValues->sectorInCluster=bs->sectorInCluster;
		printf("Reserved Sectors   :  %02x %02x  swapped %x  dec value %d  \n",  
			bs->numResSector[0] ,
			bs->numResSector[1],
			LEnd2ByteToInt(bs->numResSector),
			LEnd2ByteToInt(bs->numResSector));
		bsValues->numResSector=LEnd2ByteToInt(bs->numResSector);
		printf("Copies of FAT      :  %02x  \n",  bs->numFatCopies);
		bsValues->numFatCopies = bs->numFatCopies;
		printf("Entries in Root    :  %02x %02x swapped %x  dec value %d \n",  
			bs->numRootDirs[0] ,
			bs->numRootDirs[1],
			LEnd2ByteToInt(bs->numRootDirs),
			LEnd2ByteToInt(bs->numRootDirs)
			);
		bsValues->numRootDirs=LEnd2ByteToInt(bs->numRootDirs);
		printf("Qty FS Sectors     :  %02x %02x swapped %x  dec value %d \n",  
		      	bs->numSectFS[0] ,
			bs->numSectFS[1],
			LEnd2ByteToInt(bs->numSectFS),
			LEnd2ByteToInt(bs->numSectFS)
			);
		bsValues->numSectFS=LEnd2ByteToInt(bs->numSectFS);
		printf("Media Desc         :  %02x  \n",  bs->mediaDesc);
		//bsValues->mediaDesc=0x000000ff&bs->mediaDesc; // convert byte to int
		bsValues->mediaDesc=bs->mediaDesc; // convert unsigned char toe to int
		printf("Sectors per FAT    :  %02x %02x swapped %x  dec value %d \n",  
			bs->numSectFat[0] ,
			bs->numSectFat[1],
			LEnd2ByteToInt(bs->numSectFat),
			LEnd2ByteToInt(bs->numSectFat)
			);
		bsValues->numSectFat=LEnd2ByteToInt(bs->numSectFat);
		printf("Sectors per Track  :  %02x %02x swapped %x  dec value %d \n",  
			bs->numSectTrac[0],
			bs->numSectTrac[1],
			LEnd2ByteToInt(bs->numSectTrac),
			LEnd2ByteToInt(bs->numSectTrac)
			);
		bsValues->numSectTrac= LEnd2ByteToInt(bs->numSectTrac);
		printf("Number of Heads    :  %02x %02x swapped %x  dec value %d \n",  
		    	bs->numheads[0],
		    	bs->numheads[1],
			LEnd2ByteToInt(bs->numheads),
			LEnd2ByteToInt(bs->numheads)
			);
		bsValues->numheads= LEnd2ByteToInt(bs->numheads);
		printf("Numb of Hidden Sect:  %02x %02x swapped %x  dec value %d \n",
		  	bs->numHiddenSect[0],
		  	bs->numHiddenSect[1],
			LEnd2ByteToInt(bs->numHiddenSect),
			LEnd2ByteToInt(bs->numHiddenSect)
		    	);
		bsValues->numHiddenSect=LEnd2ByteToInt(bs->numHiddenSect);
		printf("Signature          :  %02x %02x  \n",  bs->sig[0] ,0x000000ff & (int) bs->sig[1]);
	
	//	dumpbytes((unsigned char *) buf);	
	// calc key position parameters from above inputs
	bytesPerCluster = bsValues->bytesInSector * bsValues->sectorInCluster;
	bytesPerFAT = bsValues->bytesInSector * bsValues->numSectFat;
	bootSize=512 ; // given as constant
	reserveSize=bsValues->numResSector * bsValues->bytesInSector;
	FAT0bytePosition = reserveSize ; 	
//	FAT0bytePosition = bootSize + reserveSize ; //double counting reserv includes boot	
//	FATSectorPosition = 1 + bsValues->numResSector;  // zero = boot sector , byteoffset = this * bytesInSector
	FATSectorPosition =  bsValues->numResSector;  // zero = boot sector , byteoffset = this * bytesInSector
	RootDirSectorPosition=FATSectorPosition + (bsValues->numFatCopies * bsValues->numSectFat);
	bytesPerRootDir = 32; // for each entry inthe root directory 32 bytes.
	sectorsPerRootDir = bytesPerRootDir*bsValues->numRootDirs / bsValues->bytesInSector;
	xtraRootDirByteCount = (bytesPerRootDir*bsValues->numRootDirs) % bsValues->bytesInSector;
	if (xtraRootDirByteCount >0) sectorsPerRootDir++;  // round up 
	DataSectorPosition = RootDirSectorPosition + sectorsPerRootDir;
	DataBytePosition = DataSectorPosition*bsValues->bytesInSector;
		// should be #5200 calculated by hand for test file
		// note this is address for Cluster 2 (there is no 0 or 1)
	printf("bytes per cluster    = %d\n", bytesPerCluster);
	printf("bytes per FAT        = %d\n", bytesPerFAT);
	printf("reserveSize          = %d\n", reserveSize);
	printf("FAT0 byte position   = %d\n", FAT0bytePosition);
	printf("FATSectorPosition    = %d\n", FATSectorPosition);
	printf("RootDirSectorPosition= %d\n", RootDirSectorPosition);
	printf("sectorsPerRootDir    = %d\n", sectorsPerRootDir);
	printf("DataAreaSectorPosition=%d\n", DataSectorPosition);
	printf("DataBytePosition     =%d   %x\n", DataBytePosition, DataBytePosition);

	}		
	free(buf);
}

// utility to verify byte dump 
int dumpbytes(unsigned char* ch){
	int i ;
	for (i=0;i<512;i++){
		printf("%3d. %02x  %2c \n", i, ch[i], ch[i]);	
	}
	
}
// specific test to verify the littleEndian conversions to int
void testToInts(){
	int* a = (int*) malloc (sizeof(int));
	//*a=    0x78563412;  // stored as little endian, cast as int, should be 0x123454678
	//(char[] ) (*a) = {0x12,0x34,0x56,0x78};
	*a=0x12345678; // should be stored as 78563412 but print correctly as int shown
	printf("LE4btyesToInt  %x  swapsto %x\n",*a,LEnd4ByteToInt((char*)a));
	//char b[] = {0x12,0x34,0x56};
	char b[] = {0x56,0x34,0x12};
	printf("LE3bytesToInt  %x  swaps to %x\n",((*b)>>8)&0x00ffffff,LEnd3ByteToInt(b));
    	char c[] = {0x34,0x12};
	printf("LE2bytesToInt  %x  swaps to %x\n",(int)(*c>>16),LEnd3ByteToInt(c));

}
// calculate position of FAT0
int getFat0ByteOffset(int qtyResSectors, int bytesPerSector){
	int sizeOfBoot = 512;
	return sizeOfBoot*qtyResSectors*bytesPerSector;  // byte pos of fat0
}
// show details of a directory entry given a array of bytes that holds the 
// directory  entry,
// return the value of the directory's Cluster from FAT table
// 0xfff means no more clusters for this directory
// postive number points to another cluster that we have to list out
int showDirDetails(unsigned char* a){
	int i ;
	int attByte = 11; // 11th byte contains attribute bits
	int StClusterBytes = 26;
	int SizeBytes = 28;
	int subDirMask = 0x08;
	int deletedFile=0;
	char filename[11];
	filename[11]='\0';
	for (i=0; i<=10; i++){
	    	filename[i]=a[i];
	     
		if (i==0) {
		    if ( a[0]==0x05 ){ // first byte 05 means fileiname start with 5e
	    		printf("%c", 0xe5); 
		    }else if (a[0]==0xe5){
			deletedFile=1; // first byte 5e means file is deleted
	    		printf("%c",a[i]);
	   	    }else{ 
	    		printf("%c",a[i]);
	    	    }	
		}else{
	    		printf("%c", a[i]);
	    	}
	};
	int isDir = ( (((int)a[attByte])>>4)&0x00000001 );  //bool bit for directory attribute
	printf (" %x ", isDir );
	   // printf(" %x ", (((int)a[attByte])>>4)&0x00000001 );  //bool bit for directory attribute
	printf(" %x ", (((int)a[attByte])>>5)&0x00000001 );  //bool bit for directory attribute
	    // since directory has 3 bytes for cluster number, we have two possible cluster numbers
	    // value0 is the one to use
	    // value1 is assumed to be used for something else and is simply space holder
	    // as far as getting the cluster number...a
	int thisCluster = LEnd3ByteVal0(&a[StClusterBytes]);
	printf(" %6x ", thisCluster);
	// raw 3 bytes for inspection
	printf(" %.2x %.2x %.2x ",a[StClusterBytes],a[StClusterBytes+1],a[StClusterBytes+2]);
	
	printf(" %8x ", LEnd3ByteToInt ( &a[SizeBytes]));
	printf(" %8d ", LEnd3ByteToInt ( &a[SizeBytes]));
	if (deletedFile ) printf("    DELETED FILE ");
	printf("\n");
	if ((isDir)&&(!(strcmp(filename,dotdir)==0))&&(!(strcmp(filename,doubledot)==0))) {
		noBlank((char *)filename);
		strcat(dir,filename);
		strcat(dir,"/");
		getDirInfoAtByteAddr(getByteAddrFrClusterNumber(thisCluster));
//		printf("	fat next cluster value is %x \n",
//			getNextClusterNumber(thisCluster) );
		return getNextClusterNumber(thisCluster);
	}else{
		int nextCluster =  getNextClusterNumber(thisCluster);
		int j=0;
		while (isContinue(nextCluster)) {
		    	if ((j%12)==0){ 
			    	printf("\n\t");	
			}
			j++;
			printf("%3x ", nextCluster);
			nextCluster = getNextClusterNumber(nextCluster);
		}
		printf("\n");
	
	}
	

	return 0xfff;  // this is directory listing logic, we dont want to pass on 
			// filename next cluster blocks within this logic.
}
//////////////////////////////////////////////////////////////////////////
// same as showDirDetails but do not descend into subdirectories.  Just
// list out all files in current directory .
int showDirDetailsNoDescend(unsigned char* a){
	int i ;
	int attByte = 11; // 11th byte contains attribute bits
	int StClusterBytes = 26;
	int SizeBytes = 28;
	int subDirMask = 0x08;
	int deletedFile=0;
	char filename[11];
	filename[11]='\0';
	for (i=0; i<=10; i++){
	    	filename[i]=a[i];
	     
		if (i==0) {
		    if ( a[0]==0x05 ){ // first byte 05 means fileiname start with 5e
	    		printf("%c", 0xe5); 
		    }else if (a[0]==0xe5){
			deletedFile=1; // first byte 5e means file is deleted
	    		printf("%c",a[i]);
	   	    }else{ 
	    		printf("%c",a[i]);
	    	    }	
		}else{
	    		printf("%c", a[i]);
	    	}
	};
	int isDir = ( (((int)a[attByte])>>4)&0x00000001 );  //bool bit for directory attribute
	printf (" %x ", isDir );
	   // printf(" %x ", (((int)a[attByte])>>4)&0x00000001 );  //bool bit for directory attribute
	printf(" %x ", (((int)a[attByte])>>5)&0x00000001 );  //bool bit for directory attribute
	    // since directory has 3 bytes for cluster number, we have two possible cluster numbers
	    // value0 is the one to use
	    // value1 is assumed to be used for something else and is simply space holder
	    // as far as getting the cluster number...a
	int thisCluster = LEnd3ByteVal0(&a[StClusterBytes]);
	printf(" %6x ", thisCluster);
	// raw 3 bytes for inspection
	printf(" %.2x %.2x %.2x ",a[StClusterBytes],a[StClusterBytes+1],a[StClusterBytes+2]);
	
	printf(" %8x ", LEnd3ByteToInt ( &a[SizeBytes]));
	printf(" %8d ", LEnd3ByteToInt ( &a[SizeBytes]));
	if (deletedFile ) printf("    DELETED FILE ");
	printf("\n");
	if ((isDir)&&(!(strcmp(filename,dotdir)==0))&&(!(strcmp(filename,doubledot)==0))) {
		noBlank((char *)filename);
		strcat(dir,filename);
		strcat(dir,"/");
//		getDirInfoAtByteAddr(getByteAddrFrClusterNumber(thisCluster));
//		printf("	fat next cluster value is %x \n",
//			getNextClusterNumber(thisCluster) );
		return getNextClusterNumber(thisCluster);
	}
	return 0xfff;  // this is directory listing logic, we dont want to pass on 
			// filename next cluster blocks within this logic.
}


//
// same as getDirInfoAtByteAddr but do not descend into subdirectories

int getDirInfoAtByteAddrNoDesc(int byteAddr){
    	
	int byteOffset= byteAddr;
//	printf("Address = 0x%x \n", byteOffset);
    	int rc = lseek(fd,byteOffset,SEEK_SET);
	startByteOfCurrentBlock=byteOffset;
	if (rc <0){
	    printf("lseek to position %d failed!\n", RootDirSectorPosition);
	    printf("rc = %d\n", rc);
	    return ;
	}
//	printf("address %d \n", rc);
	printf("Current Directory: %s \n", dir);
	printf(" FileName   D  A   Clstr  3ClstrBs    Size0x   SizeDec\n");
	int rresult;
	int count=0;

	unsigned char* buf = (unsigned char*) malloc(bufsize);
	rresult =  read (fd, buf,bufsize );
	while ( rresult >0){
	    	unsigned char* a = (unsigned char* ) buf;
		int i=0;
		int entriesPerbuf = bufsize/bytesPerEntry ;
		while (i<entriesPerbuf){
			if (a[i*bytesPerEntry]==0  )  {
			  // printf(" ----- End of Directory, 00 byte found in pos zero\n"); 
			  // printf(" ------files in directory = %d,  a[ %d ] = %d \n",i, 
			  //  i*bytesPerEntry, a[i*bytesPerEntry]); 
			    //dumpbytes(a);
			    printf("----END  of DIR ... going back up tree\n");
		      	    strcpy(dir,"/");	   
			    return 0; // first byte in dir entry means dir ends
			    // stop looking...
			}
			int nextCluster = showDirDetailsNoDescend(&a[i*bytesPerEntry]);
			// does this directory span more than one cluster

			i++;
			count++;
		}
		byteOffset=byteOffset+bufsize;
		rc =lseek(fd,byteOffset,SEEK_SET);
		//printf("address %d \n", rc);
		if (rc <0){
	    		printf("lseek to position %d failed!\n", RootDirSectorPosition);
	    		return ;
		}
		rresult=read(fd,buf,bufsize);
	}

	free (buf);
	return 1;

		//unsigned char *a;
		//dumpbytes((unsigned char*) buf); 	
}
/////////////////////////////////////////////////////////


//return a null terminated string given an array of char.
//truncates on first blank or null
int noBlank(char* input){
	int i =0;
	while ((i<11) && (input[i]!=' ') && (input[i]!='\0')){
	 	i++;
	}
	input[i]='\0';
}

int isContinue(int clusterValue){

	if ((clusterValue) <= 0x00000fef){ //valid next cluster, should continue
		return 1;
	}	
	return 0;
}


//////////////////////////////////////

int level;
int getDirInfoAtByteAddr(int byteAddr){
    	level++;
	int byteOffset= byteAddr;
//	printf("Address = 0x%x \n", byteOffset);
    	int rc = lseek(fd,byteOffset,SEEK_SET);
	startByteOfCurrentBlock=byteOffset;
	if (rc <0){
	    printf("lseek to position %d failed!\n", RootDirSectorPosition);
	    printf("rc = %d\n", rc);
	    return ;
	}
//	printf("address %d \n", rc);
	printf("Current Directory: %s \n", dir);
	printf(" FileName   D  A   Clstr  3ClstrBs    Size0x   SizeDec\n");
	int rresult;
	int count=0;

	unsigned char* buf = (unsigned char*) malloc(bufsize);
	rresult =  read (fd, buf,bufsize );
	while ( rresult >0){
	    	unsigned char* a = (unsigned char* ) buf;
		int i=0;
		int entriesPerbuf = (bufsize)/bytesPerEntry ;
		while ((i<(entriesPerbuf))&&(count < bsValues->sectorInCluster*bsValues->bytesInSector/32)){
		// only process up to max of dir entries, otherwise will roll over into 
		// next cluster is assigned for another use.	

		if (a[i*bytesPerEntry]==0  )  {
			  // printf(" ----- End of Directory, 00 byte found in pos zero\n"); 
			  // printf(" ------files in directory = %d,  a[ %d ] = %d \n",i, 
			  //  i*bytesPerEntry, a[i*bytesPerEntry]); 
			    //dumpbytes(a);
			    printf("----End of Directory--returning back up tree\n");
		      	    strcpy(dir,"/");	   
			    return 0; // first byte in dir entry means dir ends
			    // stop looking...
			}
			int nextCluster = showDirDetails(&a[i*bytesPerEntry]);
			// does this directory span more than one cluster
			if (isContinue(nextCluster)==1   ){
			    // if so, call ourselves to do the next cluster
				getDirInfoAtByteAddr(getByteAddrFrClusterNumber(nextCluster));
			};
			i++;
			count++;
		}
		byteOffset=byteOffset+bufsize;
		rc =lseek(fd,byteOffset,SEEK_SET);
		//printf("address %d \n", rc);
		if (rc <0){
	    		printf("lseek to position %d failed!\n", RootDirSectorPosition);
	    		return ;
		}
		rresult=read(fd,buf,bufsize);
	}
	level--;
	free (buf);
	return 1;

		//unsigned char *a;
		//dumpbytes((unsigned char*) buf); 	
}




int isByteInbuf(int targetByteNumber){
	return ((targetByteNumber>=startByteOfCurrentBlock)&&
	    (targetByteNumber<(startByteOfCurrentBlock+bufsize)));

}
// giveme the cluster number 
int isVal0EOFCluster(char* clusterEntry){
	return LEnd3ByteVal1(eofMark)==LEnd3ByteVal0(clusterEntry);
}

int isVal1EOFCluster(char* clusterEntry){
	return LEnd3ByteVal1(eofMark)==LEnd3ByteVal1(clusterEntry);
}
/**
  */

int getByteAddrFrClusterNumber(int clusterNumber){
    return DataBytePosition + ((clusterNumber-2)*bsValues->bytesInSector*bsValues->sectorInCluster);
// data area starts with cluster number 2 so adjust clusterNumer offset by 2	
}

int getNextClusterNumber(int clusterNumber){
    	int byteOffsetInFAT;
	byteOffsetInFAT = (clusterNumber/2); // floor function assumed for int divide
	byteOffsetInFAT = byteOffsetInFAT *3; // every 3 bytes is 2 cluster numbers

	unsigned char* buf = (unsigned char*) malloc(bufsize);
	int rresult;
    	int rc = lseek(fd,FAT0bytePosition+byteOffsetInFAT,SEEK_SET);
	startByteOfCurrentBlock=FAT0bytePosition;
	if ( rc >0){
		rresult =  read (fd, buf,bufsize );
		if (rresult <0 ){
		    printf("failed read from verifyFAT\n");
		    return;
		}
	    unsigned char* arr = (unsigned char *) buf;	
		// we should have clusternumber in a[0] ..a[2]
//		printf ("   triple %.2x %.2x %.2x , val0 %x, val1 %x \n",
//			arr[0], arr[1], arr[2], LEnd3ByteVal0(&arr[0]), LEnd3ByteVal1(&arr[0]));
		if (clusterNumber%2) {
		    return LEnd3ByteVal1(&arr[0]);
		}else{
		    return LEnd3ByteVal0(&arr[0]);
		}
	}

	free(buf);
//	if (isByteInbuf(clusterNumber)
}

void copy3Bytes(unsigned char* fr, unsigned char* to){
	to[0]=fr[0];
	to[1]=fr[1];
	to[2]=fr[2];
}
/**
	Verify the first byte of the block has 


  */
int verifyFAT(int fd){
	int rresult;
	printf("byte address %d %x \n", FAT0bytePosition , FAT0bytePosition);
    	int rc = lseek(fd,FAT0bytePosition,SEEK_SET);
	startByteOfCurrentBlock=FAT0bytePosition;
	if ( rc >0){
	    unsigned char* buf = (unsigned char*) malloc(bufsize);
		rresult =  read (fd, buf,bufsize );
		if (rresult <0 ){
		    printf("failed read from verifyFAT\n");
		    return;
		}
		unsigned char* a=(unsigned char *) buf;
		if (a[0] == bsValues->mediaDesc) {
		    printf(" Media Desc matches %x \n", ((int) a[0])&0x000000ff);
		    copy3Bytes (&a[0],&eofMark[0]); 
		    printf(" setting eofMark to 0x%x or dec %d \n", 
		    LEnd3ByteVal1(eofMark), LEnd3ByteVal1(eofMark));
		}else{
			dumpbytes(buf);		
			printf("failed to seek from verifyFAT\n");
		}
		free(buf);
	}
}

int main(int argc, char **argv){
	int fc;
	unsigned char* buf = (unsigned char*) malloc(bufsize);
	buf = malloc (bufsize);

    if (argc == 2 ){

       	printf(" filename = %s\n", argv[1]);
    }else{
	printf("pls enter filename to be opened\n");
	return;
    }

    fd = open(argv[1], O_RDONLY  );
    if (fd < 0 ){
 	printf("file did not open\n");	
	return 0;
    }


    getBSdata(fd);
    printf("----------------Part2-----------------\n");
    printf("-------------getting FAT now ------------\n");
    verifyFAT(fd);
    int dirBytePos = RootDirSectorPosition * bsValues->bytesInSector;
	printf(" -----------show root directory enteries without descending into subdirs\n");
	getDirInfoAtByteAddrNoDesc( dirBytePos );

	printf("-----------Part3----descend into all subdirs---\n");


	
    level=0;
    getDirInfoAtByteAddr( dirBytePos );

    fc = close(fd);
    if (fc ==0){
	printf("file closed\n");
    }

    free (buf);
    free (bsValues);

//	testToInts();


}

