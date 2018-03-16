#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>



#define MAX_PACKET_SIZE 1024
#define HEADER_SIZE 32
#define MAX_PAYLOAD_SIZE 992
#define MAX_RETRANS_TIME 500
#define SYN 0
#define DATA 1
#define ACK 2
#define RETRANS 3
#define FILENAME 4

// function used to find timeout for the packet 
int check_time_out(int sock_fd){
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(sock_fd, &read_fds);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = MAX_RETRANS_TIME * 1000;
    int i = 0;
    if((i = select(sock_fd + 1, &read_fds, NULL, NULL, &tv)) == 0)
        return 1;
    else
        return 0;

}

struct packet
{   
    int type;
    int sequence_num;
    int max_num;
    int fin;
    int error;
    char data[MAX_PAYLOAD_SIZE];
    int data_size;
    int reuse_count;
};

int diff_ms(struct timeval t1, struct timeval t2){
    return (((t1.tv_sec - t2.tv_sec) * 1000000) + (t1.tv_usec - t2.tv_usec))/1000;
}

int main(int argc, char *argv[]){
    int sockfd;
    int port;
    int cwnd = 5120;
    struct packet packet_sent;
    int last = 0;
    int file_bufferLength;
    int last_seq_num = 0;
    
    // Check for correct argument length
	if (argc != 2) {
		fprintf(stderr, "Error: correct usage: ./server <portnumber>.\n");
		exit(1);
	}
    
    port = atoi(argv[1]);
    struct sockaddr_in serv_addr, cli_addr;
	socklen_t serv_len,cli_len;
	//create socket and bind 
	if((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) == -1){
		fprintf(stderr, "socket error.\n");
		exit(1);
	}
    memset((char *)&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(port);
    if ( (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr))) < 0) {
		perror("bind failed");
		exit(-1);
	}
	cli_len = sizeof(cli_addr);
	serv_len = sizeof(serv_addr);
	
	//return the new server sock_fd
	if ((getsockname(sockfd, (struct sockaddr *)&serv_addr, &serv_len)) < 0) {
		fprintf(stderr, "%s\n","failed to return the new sock_fd" );
		return -1;
	}
	printf("server listening .....\n");
    
    
    while(1) {
        int ret = 0;
        //get first syn
        while(1){
            if( recvfrom(sockfd, &packet_sent, sizeof(packet_sent), 0, (struct sockaddr *)&cli_addr, &cli_len) == - 1){
                printf("Error: failed to receive ACK.\n");
                exit(1);
            }
            if(packet_sent.type == SYN){
                fprintf(stderr, "Receiving packet %d\n",packet_sent.sequence_num);
                break;
            }
            else
                continue;
            }
        memset((char *) &packet_sent, 0, sizeof(packet_sent));
        //send synack 
        ret = 0;
        // get file name 
        while(1){
            sendto(sockfd, &packet_sent, sizeof(packet_sent), 0, (struct sockaddr *)&cli_addr, cli_len);
            if(!ret)
                fprintf(stdout, "Sending packet 0 SYN \n");
            else
                fprintf(stdout, "Sending packet 0 SYN Retransmission\n");
        

            if (check_time_out(sockfd)){
                fprintf(stderr, "packet timeout.\n");
                ret = 1;
                continue;
            }
            else{
                recvfrom(sockfd, &packet_sent, sizeof(packet_sent), 0, (struct sockaddr *) &serv_addr, &(serv_len));
                if(packet_sent.type == FILENAME)
                    break;
                else{
                    fprintf(stderr, "%s\n","did not receive expected filename.\n" );
                    ret = 1;
                }
            }

        }
        
        
        // READ THE FILE INTO FILE BUFFER 
        //---------------------------------------------------------
        // create file_buffer buffer to fopen and fread designated file
        char *file_buffer = NULL;
        FILE *fp = fopen(packet_sent.data, "r");
        if(!fp){
            packet_sent.error = 1;
            sendto(sockfd, &packet_sent, sizeof(packet_sent), 0, (struct sockaddr *)&cli_addr, cli_len);
            printf("404: file not found!\n");
            exit(1);
        }
        // read the file to a buffer and then send the data from buffer to client 
        if(fseek(fp, 0L, SEEK_END) == 0){
            long file_size = ftell(fp);
            if (file_size == -1){
                packet_sent.error = 1;
                sendto(sockfd, &packet_sent, sizeof(packet_sent), 0, (struct sockaddr *)&cli_addr, cli_len);
                printf("Error: file not found!\n");
                exit(1);
            }
            file_buffer = malloc(sizeof(char) * (file_size + 1));
            if (fseek(fp, 0L, SEEK_SET) != 0){
                packet_sent.error = 1;
                sendto(sockfd, &packet_sent, sizeof(packet_sent), 0, (struct sockaddr *)&cli_addr, cli_len);
                printf("Error: file not found!\n");
                exit(1);
            }
            
            // set file_buffer to file data
            file_bufferLength = fread(file_buffer, sizeof(char), file_size, fp);
            if (file_bufferLength == 0){
                packet_sent.error = 1;
                // send packet with error = 1 to represent no file found
                sendto(sockfd, &packet_sent, sizeof(packet_sent), 0, (struct sockaddr *)&cli_addr, cli_len);
                printf("Error: file not found!\n");
                exit(1);
            }
            file_buffer[file_bufferLength] = '\0';
        }
        fclose(fp);
        
        /* Setup to send back a response packet */
        memset((char *) &packet_sent, 0, sizeof(packet_sent));
        /* Determines how many packets we have to send based on the length of the requested file */
         //Iterating through the window
        //-2 for not yet sent
        //-1 for already ACK'ed
        //otherwise for not yet ACK'ed
        
        int wnd_size = cwnd / MAX_PACKET_SIZE;
        int* ACK_table = (int*) malloc(wnd_size*sizeof(int));
        int i;
        for (i = 0; i < wnd_size; i++)
            ACK_table[i] = -2;
        
        
        int total_packet = (file_bufferLength / MAX_PAYLOAD_SIZE) + 1;
        packet_sent.max_num = total_packet;
        packet_sent.sequence_num = 0;
        int window_start = 0, window_curr = 0;
        int window_curr_end = (wnd_size > total_packet? total_packet:wnd_size);
        struct packet *window;
        window = (struct packet *) malloc(wnd_size * sizeof(struct packet));
        
       
        struct timeval start;
        gettimeofday(&start, NULL);
        
       
        
        
        /* start the file transfer process */
        int offset = 0;
        while(1){
            // send the un sent packet 
            if (ACK_table[window_curr - window_start] == -2 && !last){
                packet_sent.type = 1;
                offset = window_curr*MAX_PAYLOAD_SIZE;
                packet_sent.sequence_num = (window_curr*MAX_PACKET_SIZE+1) % 30720;
                packet_sent.max_num = total_packet;
                packet_sent.reuse_count = (window_curr*MAX_PACKET_SIZE+1) / 30720;
                packet_sent.fin = 0;
                
                /* Check if this is the last packet among the paritioned packets */
                if (window_curr >= total_packet-1) {
                    last = 1;
                    last_seq_num = (window_curr*MAX_PACKET_SIZE+1) % 30720;
                }
                
                /* Set time table */
                struct timeval end;
                gettimeofday(&end, NULL);
                int time_diff = diff_ms(end, start);
                ACK_table[window_curr - window_start] = time_diff;
                
                //write the data into the sentpacket 
                int cur_data_size = ( (file_bufferLength - offset) < MAX_PAYLOAD_SIZE ? file_bufferLength - offset:MAX_PAYLOAD_SIZE);
                memcpy(packet_sent.data, file_buffer+offset, cur_data_size);
                packet_sent.data_size = cur_data_size ;
                memcpy(&(window[window_curr - window_start]), &packet_sent, sizeof(struct packet));
                window[window_curr - window_start].type = 3;
                /* Send the packet  */
                sendto(sockfd, &packet_sent, sizeof(packet_sent), 0, (struct sockaddr *)&cli_addr, cli_len);
                
                if(window_curr < window_curr_end)
                    window_curr++;
                char* status = "";
                if (packet_sent.type == 3)
                    status = "Retransmission";
                else if (packet_sent.fin == 1)
                    status = "FIN";
                printf("Sending packet %d %d %s\n", packet_sent.sequence_num, cwnd, status);
            }
            
            /* Resend */
            /*for (i = window_start; i<window_curr_end; i++){
                if (ACK_table[i-window_start] > 0){
                    struct timeval end;
                    gettimeofday(&end, NULL);
                    int time_diff = diff_ms(end, start);
                    packet_sent.time = time_diff - ACK_table[i-window_start];
                    if (time_diff - ACK_table[i-window_start] >= MAX_RETRANS_TIME){
                        if ((window[i - window_start]).sequence_num
                            == (window[i - window_start]).max_num)
                            (window[i - window_start]).fin = 1;
                        sendto(sockfd, &(window[i - window_start]), sizeof((window[i - window_start])), 0, (struct sockaddr *)&cli_addr, cli_len);
                        printf("Sending packet %d %d Retransmission\n", window[i - window_start].sequence_num, cwnd);
                        ACK_table[i-window_start] = diff_ms(end, start);
                    }
                }
            }*/

                if (ACK_table[0] >= 0){
                    struct timeval end;
                    gettimeofday(&end, NULL);
                    int time_diff = diff_ms(end, start);
                    int temp_time = time_diff - ACK_table[0];
                    if (temp_time  >= MAX_RETRANS_TIME){
                        if ((window[0]).sequence_num == (window[0]).max_num)
                            (window[0]).fin = 1;
                        sendto(sockfd, &(window[0]), sizeof((window[0])), 0, (struct sockaddr *)&cli_addr, cli_len);
                        printf("Sending packet %d %d Retransmission\n", window[0].sequence_num, cwnd);
                        ACK_table[0] = diff_ms(end, start);
                    }
                }
            
            
            // 3b
            if (ACK_table[0] == -1){
                //Move the time table and packet window to position 0
                for (i = 0; i<wnd_size-1; i++){
                    ACK_table[i] = ACK_table[i+1];
                    memcpy(&(window[i]), &(window[i+1]), sizeof(struct packet));
                }
                ACK_table[wnd_size-1] = -2;
                memset(&(window[wnd_size-1]), 0, sizeof(struct packet));
                window_start++;
                
                if (window_curr_end<total_packet)
                    window_curr_end++;
                //window_curr_end = window_start + 5;
            }
            
            
            // 3a) ACK(n) in [sendbase,sendbase+N]: mark pkt n as received
            
            fd_set inSet;
            struct timeval timeout;
            int received;
            
            FD_ZERO(&inSet);
            FD_SET(sockfd, &inSet);
            
            // MAX_RETRANS_TIME for specified time
            timeout.tv_sec = 0;
            timeout.tv_usec = 100000;
            
            // check for acks
            received = select(sockfd+1, &inSet, NULL, NULL, &timeout);
            
            // if timeout and no acks were received, break and maintain window position
            if(received < 1)
            {
                //printf("MAX_RETRANS_TIMEing for ACK timeout.\n");
                continue;
            }
            // otherwise, fetch the ack
            struct packet ack;
            recvfrom(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *) &cli_addr, &cli_len);
            
            if(ack.type == 2) {
                int pkt = (ack.sequence_num + ack.reuse_count*30720) / MAX_PACKET_SIZE;

                printf("No. %d\n", pkt);
                ACK_table[pkt-window_start] = -1;
                printf("Receiving packet %d\n", ack.sequence_num);
            }
            
            int finished = 0;
            if (last) {
                int n = 0;
                for(n=0; n < wnd_size; n++) {
                    if (ACK_table[n] > 0) {
                        finished = 0;
                        break;
                    }
                    else
                        finished = 1;
                }
                if (finished == 0){
                    /* Resend */
                    for (i = window_start; i<window_start+5; i++){
                        if (ACK_table[i-window_start] >= 0){
                            struct timeval end;
                            gettimeofday(&end, NULL);
                            int time_diff = diff_ms(end, start);
                            //packet_sent.time = time_diff - ACK_table[i-window_start];
                            if (time_diff - ACK_table[i-window_start] >= MAX_RETRANS_TIME){
                                // printf("(%d)\n", time_diff - ACK_table[i-window_start]);
                                packet_sent.type = 3;
                                if ((window[i - window_start]).sequence_num
                                    == (window[i - window_start]).max_num)
                                    (window[i - window_start]).fin = 1;
                                sendto(sockfd, &(window[i - window_start]), sizeof((window[i - window_start])), 0, (struct sockaddr *)&cli_addr, cli_len);
                                printf("Sending packet %d %d Retransmission\n", window[i - window_start].sequence_num, cwnd);
                                ACK_table[i-window_start] = diff_ms(end, start);
                            }
                        }
                    }

                }
                else {
                    //send fin and MAX_RETRANS_TIMEing for fin ACK
                    while (1) {
                        struct packet fin_packet;
                        memset((char *) &fin_packet, 0, sizeof(packet_sent));
                        fin_packet.fin = 1;
                        //send fin
                        
                        fin_packet.sequence_num = last_seq_num + packet_sent.data_size + HEADER_SIZE;
                        ///////
                        
                        sendto(sockfd, &fin_packet, sizeof(fin_packet), 0, (struct sockaddr *)&cli_addr, cli_len);
                        printf("Sending packet %d %d EOF\n", fin_packet.sequence_num, cwnd);
                        
                        received = select(sockfd+1, &inSet, NULL, NULL, &timeout);
                        
                        if(received < 1)
                        {
                            continue;
                        }

                        struct packet fin_ack;
                        recvfrom(sockfd, &fin_ack, sizeof(fin_ack), 0, (struct sockaddr *) &cli_addr, &cli_len);
                        
                        sendto(sockfd, &fin_ack, sizeof(fin_ack), 0, (struct sockaddr *)&cli_addr, cli_len);
                        printf("Sending packet %d %d FIN\n", fin_ack.sequence_num, cwnd);
                        
                        //fin = 2 means closing connection
                        if (fin_ack.fin == 2) {
                            printf("Transmission Finished.\nConnection Closed.\n");
                            close(sockfd);
                            exit(0);
                        }
                    }
                }
            }
        }
        exit(0);
    }
}
