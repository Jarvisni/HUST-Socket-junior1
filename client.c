#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <dirent.h>
#include <time.h>
#define CMD_RRQ (short)1
#define CMD_WRQ (short)2
#define CMD_DATA (short)3
#define CMD_ACK (short)4
#define CMD_ERROR (short)5
#define CMD_LIST (short)6
#define CMD_HEAD (short)7
#define SERVER_PORT 69
#define MODE "octet"
#define MAX_REQUEST_SIZE 1024
#define DATA_SIZE 512
#define LIST_BUF_SIZE (DATA_SIZE * 8)
#define PKT_MAX_RXMT 10
#define PKT_SND_TIMEOUT 12*1000*1000
#define PKT_RCV_TIMEOUT 3*1000*1000
#define PKT_TIME_INTERVAL 5*1000
#define LINE_BUF_SIZE 1024
#define LOG "log.txt"

clock_t start_c, end_c;
double time_all;

struct tftp_packet{
	ushort cmd;
	union{
		ushort code;
		ushort block;
		// For a RRQ and WRQ TFTP packet
		char filename[2];
	};
	char data[DATA_SIZE];
};

struct tftp_request{
	int size;
	struct sockaddr_in client;
	struct tftp_packet packet;
};
int sock;
struct sockaddr_in server;
socklen_t addr_len;
int blocksize = DATA_SIZE;
FILE* plog;
int resend;
int total_size;
time_t timeptr;

// Download.
void GetFile(char *remote_file, char *local_file, char* mode){
	struct tftp_packet snd_packet, rcv_packet;
	int next_block = 1;
	int recv_n;
	int total_bytes = 0;
	struct tftp_packet ack;
	struct sockaddr_in sender;

	int r_size = 0;
	int time_wait_data;
	ushort block = 1;

	// Send request.
	snd_packet.cmd = htons(CMD_RRQ);
	sprintf(snd_packet.filename, "%s%c%s%c%d%c", remote_file, 0, mode, 0, blocksize, 0);
	sendto(sock, &snd_packet, sizeof(struct tftp_packet), 0, (struct sockaddr*)&server, addr_len);

	FILE* fp = fopen(local_file, "w");
	if (fp == NULL) {
		printf("Create file \"%s\" error.\n", local_file);
		plog = fopen(LOG,"a+");
			fprintf(plog,"ERROR:Create file \"%s\" error.\n",local_file);
			fclose(plog);
		return;
	}
    
    start_c = clock();
    
	// Receive data.
	snd_packet.cmd = htons(CMD_ACK);
	do {
		for (time_wait_data = 0; time_wait_data < PKT_RCV_TIMEOUT * PKT_MAX_RXMT; time_wait_data += 10000) {
			// Try receive(Nonblock receive).
			r_size = recvfrom(sock, &rcv_packet, sizeof(struct tftp_packet), MSG_DONTWAIT,
				(struct sockaddr*)&sender,
				&addr_len);
			if (r_size > 0 && r_size < 4) {
				printf("Bad packet: r_size=%d\n", r_size);
			}
			if (r_size >= 4 && rcv_packet.cmd == htons(CMD_DATA) && rcv_packet.block == htons(block)) {
				printf("DATA: block=%d, data_size=%d\n", ntohs(rcv_packet.block), r_size - 4);
				// Send ACK.
				snd_packet.block = rcv_packet.block;
				sendto(sock, &snd_packet, sizeof(struct tftp_packet), 0, (struct sockaddr*)&sender, addr_len);
				fwrite(rcv_packet.data, 1, r_size - 4, fp);
				break;
			}
			else if(time_wait_data%(PKT_RCV_TIMEOUT) == 0) sendto(sock, &snd_packet, sizeof(struct tftp_packet), 0, (struct sockaddr*)&sender, addr_len);
			usleep(10000);
		}
		if (time_wait_data >= PKT_RCV_TIMEOUT * PKT_MAX_RXMT) {
			printf("Wait for DATA #%d timeout.\n", block);
			plog = fopen(LOG,"a+");
			fprintf(plog,"ERROR:Wait for DATA #%d timeout.\n",block);
			time(&timeptr);
			fprintf(plog,"[%s]>:Download %d blks,%d bytes. Time: %s",remote_file,block,(block-1)*512,ctime(&timeptr));
			fclose(plog);
			return;
		}
		block++;
	} while (r_size == blocksize + 4);
	
	end_c = clock();
	time_all = (double)(end_c - start_c) / CLOCKS_PER_SEC;
	double size_all=r_size+(block-1)*512;
	
	plog = fopen(LOG,"a+");
	time(&timeptr);
	fprintf(plog,"[%s]>:Download %d blks,%d bytes. Time: %s",remote_file,block,r_size+(block-1)*512,ctime(&timeptr));
	fprintf(plog, "\n***** Average download throughput = %.2lf kB/s *****\n", size_all / (1024 * time_all));
	fclose(plog);
	//printf("\nReceived %d bytes.\n", total_bytes);

	fclose(fp);
}


// Upload a file to the server.
void SendFile(char *filename,char* mode){
	struct sockaddr_in sender;
	resend=0;
	struct tftp_packet rcv_packet, snd_packet;
	int r_size = 0;
	int time_wait_ack;
	resend =0;
	// Send request and wait for ACK#0.
	snd_packet.cmd = htons(CMD_WRQ);
	sprintf(snd_packet.filename, "%s%c%s%c%d%c", filename, 0, mode, 0, blocksize, 0);
	sendto(sock, &snd_packet, sizeof(struct tftp_packet), 0, (struct sockaddr*)&server, addr_len);
	for (time_wait_ack = 0; time_wait_ack < PKT_RCV_TIMEOUT; time_wait_ack += 20000) {
		// Try receive(Nonblock receive).
		r_size = recvfrom(sock, &rcv_packet, sizeof(struct tftp_packet), MSG_DONTWAIT,
			(struct sockaddr*)&sender,
			&addr_len);
		if (r_size > 0 && r_size < 4) {
			printf("Bad packet: r_size=%d\n", r_size);
		}
		if (r_size >= 4 && rcv_packet.cmd == htons(CMD_ACK) && rcv_packet.block == htons(0)) {
			break;
		}
		usleep(20000);
	}
	if (time_wait_ack >= PKT_RCV_TIMEOUT) {
		printf("Could not receive from server.\n");
		plog = fopen(LOG,"a+");
		time(&timeptr);
		fprintf(plog,"ERROR:Could not receive from server. Time: %s\n",ctime(&timeptr));
		fclose(plog);
		return;
	}

	FILE* fp = fopen(filename, "r");
	if (fp == NULL) {
		printf("File not exists!\n");
		return;
	}

	int s_size = 0;
	int rxmt;
	ushort block = 1;
	snd_packet.cmd = htons(CMD_DATA);
	
	start_c = clock();
	
	// Send data.
	do {
		memset(snd_packet.data, 0, sizeof(snd_packet.data));
		snd_packet.block = htons(block);
		s_size = fread(snd_packet.data, 1, blocksize, fp);

		for (rxmt = 0; rxmt < PKT_MAX_RXMT; rxmt++) {
			sendto(sock, &snd_packet, s_size + 4, 0, (struct sockaddr*)&sender, addr_len);
			printf("Sending block: %d\n", block);
			// Wait for ACK.
			for (time_wait_ack = 0; time_wait_ack < PKT_RCV_TIMEOUT; time_wait_ack += 10000) {
				// Try receive(Nonblock receive).
				r_size = recvfrom(sock, &rcv_packet, sizeof(struct tftp_packet), MSG_DONTWAIT,
					(struct sockaddr*)&sender,
					&addr_len);
				if (r_size > 0 && r_size < 4) {
					printf("Bad packet: r_size=%d\n", r_size);
				}
				if (r_size >= 4 && rcv_packet.cmd == htons(CMD_ACK) && rcv_packet.block == htons(block)) {
					break;
					resend+=time_wait_ack/PKT_RCV_TIMEOUT;
				}
				usleep(10000);
			}
			if (time_wait_ack < PKT_RCV_TIMEOUT) {
				// Send success.
				break;
			}
			else {
				// Retransmission.
				continue;
			}
		}
		if (rxmt >= PKT_MAX_RXMT) {
			printf("Could not receive from server.\n");
			plog = fopen(LOG,"a+");
			fprintf(plog,"ERROR:Could not receive from server! Transmission aborted\n");
			fclose(plog);
			return;
		}

		block++;
	} while (s_size == blocksize);
    
    end_c = clock();
    time_all = (double)(end_c - start_c) / CLOCKS_PER_SEC;
    double size_all=r_size+(block-1)*512;
    
	printf("\nUpload file finished!\n");
	plog = fopen(LOG,"a+");
	time(&timeptr);
	fprintf(plog,"[%s]:Upload %d blks,%d bytes,%d blks resend. Time: %s",filename,block,r_size+(block-1)*512,resend,ctime(&timeptr));
	fprintf(plog, "\n***** Average upload throughput = %.2lf kB/s *****\n", size_all / (1024 * time_all));
	fclose(plog);
	fclose(fp);

	return;
}


int main(int argc, char **argv){
	char cmd_line[LINE_BUF_SIZE];
	char *buf;
	char *arg;
	int i;
	char *local_file;
	char mode[10]="netascii";
	int done = 0;
	char *server_ip;
	unsigned short port = SERVER_PORT;

	addr_len = sizeof(struct sockaddr_in);	
	
	if(argc < 2){
		printf("%s server_ip [server_port]\n", argv[0]);
		return 0;
	}
	
	server_ip = argv[1];
	if(argc > 2){
		port = (unsigned short)atoi(argv[2]);
	}
	if((sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0){
		perror("socket:");
		return 0;
	}
	printf("Connected to %s:%d\n", server_ip, port);
	plog = fopen(LOG,"a+");
	time(&timeptr);
	fprintf(plog,"Connected to server %s on port %d at %s",server_ip,port,ctime(&timeptr));
	fclose(plog);
	server.sin_family = AF_INET;
	server.sin_port = htons(port);
	inet_pton(AF_INET, server_ip, &(server.sin_addr.s_addr));
	while(1){
	    printf("What to do?(down up mode)\n");
		printf("---> ");
		memset(cmd_line, 0, LINE_BUF_SIZE);
		buf = fgets(cmd_line, LINE_BUF_SIZE, stdin);
		
		arg = strtok (buf, " \t\n");
		if(arg == NULL){
			continue;
		}
		

		if(strcmp(arg, "down") == 0){
			arg = strtok (NULL, " \t\n");
			local_file = strtok (NULL, " \t\n");
			if(arg == NULL){
				printf("Error: missing arguments\n");
			}
			else{
				if(local_file == NULL){
					local_file = arg;
				}
				plog = fopen(LOG,"a+");
				time(&timeptr);
				fprintf(plog,"Read request for fiel <%s>,mode %s at %s",arg,mode,ctime(&timeptr));
				fclose(plog);
				GetFile(arg, local_file,mode);
				

			}
		}
		else if(strcmp(arg, "up") == 0){
			arg = strtok (NULL, " \t\n");
			if(arg == NULL){
				printf("Error: missing arguments\n");
			}
			else{
				plog = fopen(LOG,"a+");
				time(&timeptr);
				fprintf(plog,"Write request for fiel <%s>,mode %s at %s",arg,mode,ctime(&timeptr));
				fclose(plog);
				SendFile(arg,mode);
			}
		}
		else if(strcmp(arg, "mode") == 0){
			arg = strtok (NULL, " \t\n");
			if(arg == NULL){
				printf("Error: missing arguments\n");
			}
			strcpy(mode,arg);
			if(strcmp(mode,"netascii") && strcmp(mode,"octet")){
				printf("Invalid mode!\n");
				strcpy(mode,MODE);
			}
			else{
				printf("Mode changed to %s\n",mode);
			}
		}
		else if(strcmp(arg, "quit") == 0){
			exit(0);
		}
		else{
			printf("Unknow command.\n");
		}
		}
	return 0;
}


