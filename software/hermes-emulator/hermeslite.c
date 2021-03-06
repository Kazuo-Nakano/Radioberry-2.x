/* 
* Copyright (C)
* 2017, 2018 - Johan Maas, PA3GSB
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*
* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
*
* Hermeslite Emulator. By using this emulator you have the possibility to connect to SDR programs like:
*	- pihpsdr
*	- linhpsdr
*	- Quisk
*	- PowerSDR
*	- Spark
*
*	Using the 'old HPSDR protocol'; also called protocol-1
*
*  This emulator works with the Radioberry radiocard.
*
*	http://www.pa3gsb.nl
*	  
*	2018 Johan PA3GSB
*
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <math.h>
#include <semaphore.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>

#include <pigpio.h>

char build_date[]=GIT_DATE;
char build_version[]=GIT_VERSION;

void runHermesLite(void);
void sendPacket(void);
void handlePacket(char* buffer);
void processPacket(char* buffer);
void fillDiscoveryReplyMessage(void);
int isValidFrame(char* data);
void fillPacketToSend(void);
void printIntroScreen(void);
void *packetreader(void *arg);
void *spiWriter(void *arg);
void put_tx_buffer(unsigned char  value);
unsigned char get_tx_buffer(void);

int sock_TCP_Server = -1;
int sock_TCP_Client = -1;

#define TX_MAX 4800 
#define TX_MAX_BUFFER (TX_MAX * 4)
unsigned char tx_buffer[TX_MAX_BUFFER];
int fill_tx = 0; 
int use_tx  = 0;
unsigned char drive_level;
unsigned char prev_drive_level;
int MOX = 0;
sem_t tx_empty;
sem_t tx_full;
sem_t mutex;

int tx_count =0;
void rx1_spiReader(unsigned char iqdata[]);
void rx2_spiReader(unsigned char iqdata[]);

static int rx1_spi_handler;
static int rx2_spi_handler;

unsigned char iqdata[6];
unsigned char tx_iqdata[6];

#define SERVICE_PORT	1024

int hold_nrx=0;
int nrx = 2; // n Receivers
int holdfreq = 0;
int holdfreq2 = 0;
int holdtxfreq = 0;
int freq = 4706000;
int freq2 = 1008000;
int txfreq = 3630000;

int att = 0;
int holdatt =128;
int holddither=128;
int dither = 0;
int rando = 0;
int sampleSpeed = 0;

unsigned char SYNC = 0x7F;
int last_sequence_number = 0;
uint32_t last_seqnum=0xffffffff, seqnum; 

unsigned char hpsdrdata[1032];
unsigned char broadcastReply[60];
#define TIMEOUT_MS      100     

int running = 0;
int fd;									/* our socket */

struct sockaddr_in myaddr;				/* our address */
struct sockaddr_in remaddr;				/* remote address */

socklen_t addrlen = sizeof(remaddr);	/* length of addresses */
int recvlen;							/* # bytes received */

struct timeval t20;
struct timeval t21;
float elapsed;

#define MAX11613_ADDRESS	0x34
unsigned char data[8];
unsigned int i2c_bus = 1;
int i2c_handler = 0;
int vswr_active = 0;

#define ADDR_ALEX 			0x21 		/* PCA9555 address 1 */
int i2c_alex_handler = 0;
int i2c_alex = 0;
int alex_manual = 0;
uint16_t i2c_alex_data = 0;
uint16_t i2c_data = 0;

float timedifference_msec(struct timeval t0, struct timeval t1)
{
    return (t1.tv_sec - t0.tv_sec) * 1000.0f + (t1.tv_usec - t0.tv_usec) / 1000.0f;
}

void initVSWR(){
	unsigned char config[1];
	config[0] = 0x07;
	if (i2cWriteDevice(i2c_handler, config, 1) == 0 ) {
		vswr_active = 1;
	}	
}

void initALEX(){
	
	int result = 0;
	
	unsigned char data[3];

	/* configure all pins as output */
	data[0] = 0x06;
	data[1] = 0x00;
	data[2] = 0x00;
	result = i2cWriteDevice(i2c_alex_handler, data, 3);
	
	if (result >= 0) {
		data[0] = 0x02;
		data[1] = 0x00;
		data[2] = 0x00;
		/* set all pins to low */
		result = i2cWriteDevice(i2c_alex_handler, data, 3);
	}
	
	if (result >= 0) {
		i2c_alex = 1;
		fprintf(stderr, "alex interface found and initialized \n");
	} else fprintf(stderr, "no alex interface found\n");
}

int main(int argc, char **argv)
{
	int yes = 1;
	
	printIntroScreen();	
	
	sem_init(&mutex, 0, 1);	
	sem_init(&tx_empty, 0, TX_MAX); 
    sem_init(&tx_full, 0, 0);    	
	
	if (gpioInitialise() < 0) {
		fprintf(stderr,"hpsdr_protocol (original) : gpio could not be initialized. \n");
		exit(-1);
	}
	
	gpioSetMode(13, PI_INPUT); 	//rx1 samples
	gpioSetMode(16, PI_INPUT);	//rx2 samples 
	gpioSetMode(20, PI_OUTPUT); 
	gpioSetMode(21, PI_OUTPUT); 
		
	i2c_handler = i2cOpen(i2c_bus, MAX11613_ADDRESS, 0);
	if (i2c_handler >= 0)  initVSWR();
	
	i2c_alex_handler = i2cOpen(i2c_bus, ADDR_ALEX, 0);
	if (i2c_alex_handler >= 0)  initALEX();
	
	
	rx1_spi_handler = spiOpen(0, 15625000, 49155);  //channel 0
	if (rx1_spi_handler < 0) {
		fprintf(stderr,"radioberry_protocol: spi bus rx1 could not be initialized. \n");
		exit(-1);
	}
	
	rx2_spi_handler = spiOpen(1, 15625000, 49155); 	//channel 1
	if (rx2_spi_handler < 0) {
		fprintf(stderr,"radioberry_protocol: spi bus rx2 could not be initialized. \n");
		exit(-1);
	}

	printf("init done \n");
		
	pthread_t pid1, pid2; 
	pthread_create(&pid1, NULL, packetreader, NULL); 
	pthread_create(&pid2, NULL, spiWriter, NULL);

	/* create a UDP socket */
	if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		perror("cannot create socket\n");
		return -1;
	}
	struct timeval timeout;      
    timeout.tv_sec = 0;
    timeout.tv_usec = TIMEOUT_MS;

	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,(char*)&timeout,sizeof(timeout)) < 0)
		perror("setsockopt failed\n");
		
	/* bind the socket to any valid IP address and a specific port */
	memset((char *)&myaddr, 0, sizeof(myaddr));
	myaddr.sin_family = AF_INET;
	myaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	myaddr.sin_port = htons(SERVICE_PORT);

	if (bind(fd, (struct sockaddr *)&myaddr, sizeof(myaddr)) < 0) {
		perror("bind failed");
		return -1;
	}
	
	if ((sock_TCP_Server = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		perror("socket tcp");
		return -1;
	}
	
	setsockopt(sock_TCP_Server, SOL_SOCKET, SO_REUSEADDR, (void *)&yes, sizeof(yes));

	int sndbufsize = 0xffff;
	int rcvbufsize = 0xffff;
	setsockopt(sock_TCP_Server, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbufsize, sizeof(int));
	setsockopt(sock_TCP_Server, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbufsize, sizeof(int));

	if (bind(sock_TCP_Server, (struct sockaddr *)&myaddr, sizeof(myaddr)) < 0)
	{
		perror("bind tcp");
		return -1;
	}
	
	listen(sock_TCP_Server, 1024);

	runHermesLite();
	
	if (rx1_spi_handler !=0)
		spiClose(rx1_spi_handler);
	if (rx2_spi_handler !=0)
		spiClose(rx2_spi_handler);
	
	if (sock_TCP_Client >= 0)
	{
		close(sock_TCP_Client);
	}

	if (sock_TCP_Server >= 0)
	{
		close(sock_TCP_Server);
	}
		
	gpioTerminate();
}

void runHermesLite() {
	printf("runHermesLite \n");

	for (;;) {
		if (running) {
			sendPacket();
		} else {usleep(20000);}
	}
}


void *packetreader(void *arg) {
	
	int size, bytes_read, bytes_left;
	unsigned char buffer[2048];
	uint32_t *code0 = (uint32_t *) buffer; 
	
	while(1) {
		if (sock_TCP_Client >= 0) {
			// handle TCP protocol.
			bytes_read=0;
			bytes_left=1032;
			while (bytes_left > 0) {
              size = recvfrom(sock_TCP_Client, buffer+bytes_read, (size_t) bytes_left, 0, NULL, 0);
			  if (size < 0 && errno == EAGAIN) continue;
			  if (size < 0) break;
			  bytes_read += size;
			  bytes_left -= size;
			}
			handlePacket(buffer);
		} 
		else {
			// handle UDP protocol.
			recvlen = recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr *)&remaddr, &addrlen);
			if (recvlen > 0) handlePacket(buffer);
		}
	}
}

int att11 = 0;
int prevatt11 = 0;
int att523 = 0;
int prevatt523 = 0;

int count=0;
void handlePacket(char* buffer){
	uint32_t code;
	memcpy(&code, buffer, 4);
	switch (code)
	{
		default:
			fprintf(stderr, "Received packages not for me! \n");
			break;
		case 0x0002feef:
			fprintf(stderr, "Discovery packet received \n");
			fprintf(stderr, "IP-address %d.%d.%d.%d  \n", 
							remaddr.sin_addr.s_addr&0xFF,
                            (remaddr.sin_addr.s_addr>>8)&0xFF,
                            (remaddr.sin_addr.s_addr>>16)&0xFF,
                            (remaddr.sin_addr.s_addr>>24)&0xFF);
			fprintf(stderr, "Discovery Port %d \n", ntohs(remaddr.sin_port));
		
			fillDiscoveryReplyMessage();
		
			if (sendto(fd, broadcastReply, sizeof(broadcastReply), 0, (struct sockaddr *)&remaddr, addrlen) < 0) printf("error sendto");
			break;
		case 0x0004feef:
			fprintf(stderr, "SDR Program sends Stop command \n");
			running = 0;
			last_sequence_number = 0;
			if (sock_TCP_Client > -1)
			{
				close(sock_TCP_Client);
				sock_TCP_Client = -1;
				fprintf(stderr, "SDR Program sends TCP Stop command \n");
			} else fprintf(stderr, "SDR Program sends UDP Stop command \n");	
			break;
		case 0x0104feef:
		case 0x0304feef:
			fprintf(stderr, "Start Port %d \n", ntohs(remaddr.sin_port));
			running = 1;
			fprintf(stderr, "SDR Program sends UDP Start command \n");
			break;
		case 0x1104feef: 
			fprintf(stderr, "Connect the TCP client to the server\n");
			if (sock_TCP_Client < 0)
			{
				if((sock_TCP_Client = accept(sock_TCP_Server, NULL, NULL)) < 0)
				{
					fprintf(stderr, "*** ERROR TCP accept ***\n");
					perror("accept");
					return;
				}
				fprintf(stderr, "sock_TCP_Client: %d connected to sock_TCP_Server: %d\n", sock_TCP_Client, sock_TCP_Server);
				running = 1;
				fprintf(stderr, "SDR Program sends TCP Start command \n");
			}	
			break;
		case 0x0201feef:
			processPacket(buffer);
			break;		
	}
}	

void processPacket(char* buffer)
{
	//if (isValidFrame(buffer)) {
		seqnum=((buffer[4]&0xFF)<<24)+((buffer[5]&0xFF)<<16)+((buffer[6]&0xFF)<<8)+(buffer[7]&0xFF);
		if (seqnum != last_seqnum + 1) {
		  fprintf(stderr,"SEQ ERROR: last %ld, recvd %ld\n", (long) last_seqnum, (long) seqnum);
		}
		last_seqnum = seqnum;
	
		 MOX = ((buffer[11] & 0x01)==0x01) ? 1:0;
	
		if ((buffer[11] & 0xFE)  == 0x14) {
			att = (buffer[11 + 4] & 0x1F);
			att11 = att;
		}
		
		if ((buffer[523] & 0xFE)  == 0x14) {
			att = (buffer[523 + 4] & 0x1F);
			att523 = att;
		}
	
		if ((buffer[11] & 0xFE)  == 0x00) {
			nrx = (((buffer[11 + 4] & 0x38) >> 3) + 1);
			
			sampleSpeed = (buffer[11 + 1] & 0x03);
			
			dither = 0;
			if ((buffer[11 + 3] & 0x08) == 0x08)
				dither = 1; 
						
			rando = 0;
			if ((buffer[11 + 3] & 0x10) == 0x10)
				rando = 1;
		}
		
		if ((buffer[523] & 0xFE)  == 0x00) {
			
			dither = 0;
			if ((buffer[523 + 3] & 0x08) == 0x08)
				dither = 1; 
					
			rando = 0;
			if ((buffer[523 + 3] & 0x10) == 0x10)
				rando = 1;
		}
		if (prevatt11 != att11) 
		{
			att = att11;
			prevatt11 = att11;
		}
		if (prevatt523 != att523) 
		{
			att = att523;
			prevatt523 = att523;
		}
			
		if ((buffer[11] & 0xFE)  == 0x00) {
			nrx = (((buffer[11 + 4] & 0x38) >> 3) + 1);
		}
		if ((buffer[523] & 0xFE)  == 0x00) {
			nrx = (((buffer[523 + 4] & 0x38) >> 3) + 1);
		}
		if (hold_nrx != nrx) {
			hold_nrx=nrx;
			printf("aantal rx %d \n", nrx);
		}
		
		// select Command
		if ((buffer[11] & 0xFE) == 0x02)
        {
            txfreq = ((buffer[11 + 1] & 0xFF) << 24) + ((buffer[11+ 2] & 0xFF) << 16)
                    + ((buffer[11 + 3] & 0xFF) << 8) + (buffer[11 + 4] & 0xFF);
        }
        if ((buffer[523] & 0xFE) == 0x02)
        {
            txfreq = ((buffer[523 + 1] & 0xFF) << 24) + ((buffer[523+ 2] & 0xFF) << 16)
                    + ((buffer[523 + 3] & 0xFF) << 8) + (buffer[523 + 4] & 0xFF);
        }
		
		if ((buffer[11] & 0xFE) == 0x04)
        {
            freq = ((buffer[11 + 1] & 0xFF) << 24) + ((buffer[11+ 2] & 0xFF) << 16)
                    + ((buffer[11 + 3] & 0xFF) << 8) + (buffer[11 + 4] & 0xFF);
        }
        if ((buffer[523] & 0xFE) == 0x04)
        {
            freq = ((buffer[523 + 1] & 0xFF) << 24) + ((buffer[523+ 2] & 0xFF) << 16)
                    + ((buffer[523 + 3] & 0xFF) << 8) + (buffer[523 + 4] & 0xFF);
        }
		
		if ((buffer[11] & 0xFE) == 0x06)
        {
            freq2 = ((buffer[11 + 1] & 0xFF) << 24) + ((buffer[11+ 2] & 0xFF) << 16)
                    + ((buffer[11 + 3] & 0xFF) << 8) + (buffer[11 + 4] & 0xFF);
        }
        if ((buffer[523] & 0xFE) == 0x06)
        {
            freq2 = ((buffer[523 + 1] & 0xFF) << 24) + ((buffer[523+ 2] & 0xFF) << 16)
                    + ((buffer[523 + 3] & 0xFF) << 8) + (buffer[523 + 4] & 0xFF);
        }

        // select Command
        if ((buffer[523] & 0xFE) == 0x12)
        {
            drive_level = buffer[524];  
        }
		 //ALEX
		if (i2c_alex & (buffer[523] & 0xFE) == 0x12) {
			alex_manual = ((buffer[525] & 0x40) == 0x40) ? 1: 0;
			if (alex_manual) {
				i2c_alex_data = ((buffer[526] & 0x8F) << 8 ) | (buffer[527] & 0xFF);
			} else {
				//firmware does determine the filter.
				uint16_t hpf = 0, lpf = 0;
				
				if(freq < 1416000) hpf = 0x20; /* bypass */
				else if(freq < 6500000) hpf = 0x10; /* 1.5 MHz HPF */
				else if(freq < 9500000) hpf = 0x08; /* 6.5 MHz HPF */
				else if(freq < 13000000) hpf = 0x04; /* 9.5 MHz HPF */
				else if(freq < 20000000) hpf = 0x01; /* 13 MHz HPF */
				else hpf = 0x02; /* 20 MHz HPF */
				
				if(freq > 32000000) lpf = 0x10; /* bypass */
				else if(freq > 22000000) lpf = 0x20; /* 12/10 meters */
				else if(freq > 15000000) lpf = 0x40; /* 17/15 meters */
				else if(freq > 8000000) lpf = 0x01; /* 30/20 meters */
				else if(freq > 4500000) lpf = 0x02; /* 60/40 meters */
				else if(freq > 2400000) lpf = 0x04; /* 80 meters */
				else lpf = 0x08; /* 160 meters */
				
				i2c_alex_data = hpf << 8 | lpf;
			}
		}
			
		if ((holdatt != att) || (holddither != dither)) {
			holdatt = att;
			holddither = dither;
			printf("att =  %d ", att);printf("dither =  %d ", dither);printf("rando =  %d ", rando);
			printf("code =  %d \n", (((rando << 6) & 0x40) | ((dither <<5) & 0x20) |  (att & 0x1F)));
			printf("att11 = %d and att523 = %d\n", att11, att523);
		}
		if (holdfreq != freq) {
			holdfreq = freq;
			printf("frequency %d en aantal rx %d \n", freq, nrx);
		}
		if (holdfreq2 != freq2) {
			holdfreq2 = freq2;
			printf("frequency %d en aantal rx %d \n", freq2, nrx);
		}
		if (holdtxfreq != txfreq) {
			holdtxfreq = txfreq;
			printf("TX frequency %d\n", txfreq);
		}
		
		if(i2c_alex)
		{
			if(i2c_data != i2c_alex_data)
			{
				fprintf(stderr, "Set Alex data to output = %x \n", i2c_alex_data);
				i2c_data = i2c_alex_data;
				unsigned char ldata[3];
				ldata[0] = 0x02;
				ldata[1] = ((i2c_alex_data >> 8) & 0xFF);
				ldata[2] = (i2c_alex_data & 0xFF);
				i2cWriteDevice(i2c_alex_handler, ldata, 3);
			}
		}
		
		int frame = 0;
		for (frame; frame < 2; frame++)
		{
			int coarse_pointer = frame * 512 + 8;
			int j = 8;
			for (j; j < 512; j += 8)
			{
				int k = coarse_pointer + j;
				if (MOX) {
					sem_wait(&tx_empty);
					int i = 0;
					for (i; i < 4; i++){
						put_tx_buffer(buffer[k + 4 + i]);	
					}
					sem_post(&tx_full);
				}
			}
			
		}
	//}
}

void sendPacket() {
	fillPacketToSend();
	
	if (sock_TCP_Client >= 0) {
		if (send(sock_TCP_Client, hpsdrdata, sizeof(hpsdrdata), 0) < 0) printf("error sendto");
	} else {
		if (sendto(fd, hpsdrdata, sizeof(hpsdrdata), 0, (struct sockaddr *)&remaddr, addrlen) < 0) printf("error sendto");
	}
}

int isValidFrame(char* data) {
	return (data[8] == SYNC && data[9] == SYNC && data[10] == SYNC && data[520] == SYNC && data[521] == SYNC && data[522] == SYNC);
}

void fillPacketToSend() {
		
		hpsdrdata[0] = 0xEF;
		hpsdrdata[1] = 0xFE;
		hpsdrdata[2] = 0x01;
		hpsdrdata[3] = 0x06;
		hpsdrdata[4] = ((last_sequence_number >> 24) & 0xFF);
		hpsdrdata[5] = ((last_sequence_number >> 16) & 0xFF);
		hpsdrdata[6] = ((last_sequence_number >> 8) & 0xFF);
		hpsdrdata[7] = (last_sequence_number & 0xFF);
		last_sequence_number++;

		int factor = (nrx - 1) * 6;
		int index=0;
		int frame = 0;
		for (frame; frame < 2; frame++) {
			int coarse_pointer = frame * 512; // 512 bytes total in each frame
			hpsdrdata[8 + coarse_pointer] = SYNC;
			hpsdrdata[9 + coarse_pointer] = SYNC;
			hpsdrdata[10 + coarse_pointer] = SYNC;
			hpsdrdata[11 + coarse_pointer] = 0x00; // c0
			hpsdrdata[12 + coarse_pointer] = 0x00; // c1
			hpsdrdata[13 + coarse_pointer] = 0x00; // c2
			hpsdrdata[14 + coarse_pointer] = 0x00; // c3
			hpsdrdata[15 + coarse_pointer] = 0x28; // c4 //v4.0 firmware version

			if (!MOX) {
				
				tx_count = 0; 
				
				sem_wait(&mutex); 
				
				gpioWrite(21, 0); 	// ptt off
				
				while (gpioRead(13) == 0) {}//wait for enough samples
				
				int i = 0;
				for (i=0; i< (504 / (8 + factor)); i++) {
					index = 16 + coarse_pointer + (i * (8 + factor));
					rx1_spiReader(iqdata);
					int j =0;
					for (j; j< 6; j++){
						hpsdrdata[index + j] = iqdata[j];
					}	
				}
				
				if (nrx == 2) {
					int i =0;
					for (i=0; i< (504 / (8 + factor)); i++) {
						index = 16 + coarse_pointer + (i * (8 + factor));
						//rx2_spiReader(iqdata); //for now only 1 rx slice...
						int j =0;
						for (j; j< 6; j++){
							hpsdrdata[index + j + 6] = hpsdrdata[index + j] ; //iqdata[j];
						}	
					}
				}
				
				sem_post(&mutex);
					
			} else {
				int j = 0;
				for (j; j < (504 / (8 + factor)); j++) {
					index = 16 + coarse_pointer + (j * (8 + factor));
					int i =0;
					for (i; i< 8; i++){
						hpsdrdata[index + i] = 0x00;
						if (nrx == 2){
							hpsdrdata[index + i + 6] = 0x00;
						}
					}
				}
			}
			if (MOX){
				gpioWrite(21, 1); ;	// ptt on
				
				if (vswr_active) {
					if (frame == 0) {
						//reading once per 2 frames!
						int result = i2cReadDevice(i2c_handler, data, 8);
						hpsdrdata[11 + coarse_pointer] = 0x08;
						hpsdrdata[14 + coarse_pointer] = (data[6] & 0x0F); 
						hpsdrdata[15 + coarse_pointer] = data[7];
					}
					if (frame == 1) {
						hpsdrdata[11 + coarse_pointer] = 0x10;
						hpsdrdata[12 + coarse_pointer] = (data[0] & 0x0F); 
						hpsdrdata[13 + coarse_pointer] = data[1];
					}
				}
				if (sampleSpeed ==0) usleep(670);  // 48K
				else if (sampleSpeed == 1) usleep(260); //96K
				else if (sampleSpeed == 2) usleep(20);	//192K
				else if (sampleSpeed == 3) usleep(1);	//384K
			}
		}
}

void fillDiscoveryReplyMessage() {
	int i = 0;
	for (i; i < 60; i++) {
		broadcastReply[i] = 0x00;
	}
	i = 0;
	broadcastReply[i++] = 0xEF;
	broadcastReply[i++] = 0xFE;
	broadcastReply[i++] = 0x02;

	broadcastReply[i++] =  0x00; // MAC
	broadcastReply[i++] =  0x01;
	broadcastReply[i++] =  0x02;
	broadcastReply[i++] =  0x03;
	broadcastReply[i++] =  0x04;
	broadcastReply[i++] =  0x05;
	broadcastReply[i++] =  40;
	broadcastReply[i++] =  6; // hermeslite
									
}

void rx1_spiReader(unsigned char iqdata[]) {
		
	iqdata[0] = (sampleSpeed & 0x03);
	iqdata[1] = (((rando << 6) & 0x40) | ((dither <<5) & 0x20) |  (att & 0x1F));
	iqdata[2] = ((freq >> 24) & 0xFF);
	iqdata[3] = ((freq >> 16) & 0xFF);
	iqdata[4] = ((freq >> 8) & 0xFF);
	iqdata[5] = (freq & 0xFF);
			
	spiXfer(rx1_spi_handler, iqdata, iqdata, 6);
}

void rx2_spiReader(unsigned char iqdata[]) {
		
	iqdata[0] = (sampleSpeed & 0x03);
	iqdata[1] = (((rando << 6) & 0x40) | ((dither <<5) & 0x20) |  (att & 0x1F));
	iqdata[2] = ((freq2 >> 24) & 0xFF);
	iqdata[3] = ((freq2 >> 16) & 0xFF);
	iqdata[4] = ((freq2 >> 8) & 0xFF);
	iqdata[5] = (freq2 & 0xFF);
			
	spiXfer(rx2_spi_handler, iqdata, iqdata, 6);
}

void *spiWriter(void *arg) {
	
	gettimeofday(&t20, 0);
	
	while(1) {
		
		sem_wait(&tx_full);
		sem_wait(&mutex);
		
		if (tx_count % 4800 ==0) {
			//set the tx freq.
			tx_iqdata[0] = 0x00;
			tx_iqdata[1] = 0x00;
			tx_iqdata[2] = ((txfreq >> 24) & 0xFF);
			tx_iqdata[3] = ((txfreq >> 16) & 0xFF);
			tx_iqdata[4] = ((txfreq >> 8) & 0xFF);
			tx_iqdata[5] = (txfreq & 0xFF);
						
			if (MOX) spiXfer(rx2_spi_handler, tx_iqdata, tx_iqdata, 6);
		}		
		tx_iqdata[0] = 0;
		tx_iqdata[1] = drive_level / 6.4;  // convert drive level from 0-255 to 0-39 )
		if (prev_drive_level != drive_level) {
			printf("drive level %d - corrected drive level %d \n", drive_level, tx_iqdata[1]);
			prev_drive_level = drive_level; 
		}
		int i = 0;
		for (i; i < 4; i++){			
			tx_iqdata[2 + i] = get_tx_buffer(); //MSB is first in buffer..
		}
		
		if (MOX) spiXfer(rx1_spi_handler, tx_iqdata, tx_iqdata, 6);
		
		sem_post(&mutex);
		sem_post(&tx_empty); 
		
		if (gpioRead(20) == 1) usleep(20); // wait 1/48000 of a second.
		
		tx_count ++;
		if (tx_count == 48000) {
			tx_count = 0;
			gettimeofday(&t21, 0);
			float elapsd = timedifference_msec(t20, t21);
			printf("Code tx mode spi executed in %f milliseconds.\n", elapsd);
			gettimeofday(&t20, 0);
		}
	}
}

void put_tx_buffer(unsigned char  value) {
    tx_buffer[fill_tx] = value;    
    fill_tx = (fill_tx + 1) % TX_MAX_BUFFER; 
}

unsigned char get_tx_buffer() {
    int tmp = tx_buffer[use_tx];   
    use_tx = (use_tx + 1) % TX_MAX_BUFFER;  	
    return tmp;
}

void printIntroScreen() {
	fprintf(stderr,"\n");
	fprintf(stderr,	"====================================================================\n");
	fprintf(stderr,	"====================================================================\n");
	fprintf(stderr, "\t\t\t Radioberry V2.0 beta 2.\n");
	fprintf(stderr,	"\n");
	fprintf(stderr, "\t Emulator build date %s version %s \n", build_date ,build_version);
	fprintf(stderr,	"\n\n");
	fprintf(stderr, "\t\t\t Have fune Johan PA3GSB\n");
	fprintf(stderr, "\n\n");
	fprintf(stderr, "\n\tReport bugs to <pa3gsb@gmail.com>.\n");
	fprintf(stderr, "====================================================================\n");
	fprintf(stderr, "====================================================================\n");
}

//end of source.