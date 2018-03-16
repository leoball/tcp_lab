#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <termios.h>
#include <unistd.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <signal.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>

const int MAX_PACKET_SIZE = 1024;
const int HEADER_SIZE = 32;
const int MAX_PAYLOAD_SIZE = 992;
const int MAX_RETRANS_TIME = 500;
const int WSIZE = 5;

int last_seq_num = 0;

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
    //Packet Types:
    //SYN and SYN ACK: 0
    //Datagram: 1
    //ACK: 2
    //Retransmission: 3
    //filename: 4 
    
    int type;
    int seq_num;
    int max_seq_num;
    int fin;
    int error;
    double time;
    char data[MAX_PAYLOAD_SIZE];
    int data_size;
    int seq_count;
};

int main(int argc, char *argv[]) {
    int sockfd;
    int rv;
    int numbytes;
    struct sockaddr_in serv_addr;
    socklen_t serv_len;
    int port;
    if(argc != 4 ){
        fprintf(stderr,"Error: The correct usage: client <server_hostname> <server_portnumber> <filename>.\n");
        exit(1);
    }
    
    port = atoi(argv[2]);
   //create a socket 
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sockfd < 0){
        fprintf(stderr,"Cannot create socket.\n");
        exit(1);
    }

    //get ip address for the host name 
    struct hostent * server = NULL;
    server = gethostbyname(argv[1]);
    if(!server){
        fprintf(stderr, "unfound host.\n");
        exit(1);
    }
    
    memset((char *) &serv_addr, 0, sizeof(serv_addr));//reset memory
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_len = sizeof(serv_addr);
    //connection successful 
    //client sends the first ack message to server 
    
    struct packet response_packet;
    memset((char *) &response_packet, 0, sizeof(response_packet));
    
    
    response_packet.type = 0;

    //retrans syn until get synack 
    int ret = 0;
    

    //send first syn
    while(1){
        if((numbytes = sendto(sockfd, &response_packet, sizeof(response_packet), 0, (struct sockaddr *)&serv_addr, serv_len)) == -1){
            fprintf(stderr, "%s\n","failed to send first Ack to server." );
            exit(1);
        }

        if(!ret)
            fprintf(stdout, "Sending packet SYN\n");
        else
            fprintf(stdout, "Sending packet Retransmission SYN\n");

        if (check_time_out(sockfd)){
            fprintf(stderr, "SYN packet timeout.\n");
            ret = 1;
            continue;
        }
        // no timeout, receive the syn ack 
        recvfrom(sockfd, &response_packet, sizeof(response_packet), 0, (struct sockaddr *) &serv_addr, &(serv_len));
        if(response_packet.type == 0){
            printf("Receiving packet SYN\n");
            break;
        }
        else{
            fprintf(stderr, "error: Did not receive expected SYN-ACK.");
            ret = 1;
        }
    }
    ret = 0;
    Packet_filename:
        memset((char *) &response_packet, 0, sizeof(response_packet));
        sprintf(response_packet.data, "%s", argv[3]);
        response_packet.type = 4;

        if((sendto(sockfd, &response_packet, sizeof(response_packet), 0, (struct sockaddr *)&serv_addr, serv_len)) == -1){
            fprintf(stderr, "send packet error.\n");
            exit(1);
        }
        if(!ret)
            fprintf(stdout, "Sending packet 0 \n");
        else
            fprintf(stdout, "Sending packet 0 Retransmission\n");
        

        if (check_time_out(sockfd)){
            fprintf(stderr, "packet timeout.\n");
            goto Packet_filename;
        }
        FILE *fp = fopen("received.data", "w+");
        if (!fp){
            printf("Error: cannot create received.data.\n");
            exit(1);
        }     
    
    //send to the server with name 
    char newFilename[MAX_PACKET_SIZE];
    
    
    struct packet *window;
    window = (struct packet *) malloc(WSIZE * sizeof(struct packet));
    int x;
    for (x = 0; x < WSIZE; x++)
        memset(&(window[x]), 0, sizeof(struct packet));
    
    int window_start = 0;
    
    while(1)
    {
        recvfrom(sockfd, &response_packet, sizeof(response_packet), 0, (struct sockaddr *) &serv_addr, &(serv_len));
        
        //find that file not found 
        if (response_packet.error == 1) {
            printf("Error 404: File Not Found!\n");
            return 0;
        }     
        char* retrans = "";
        if (response_packet.type == 3)
            retrans = "Retransmission";

        fprintf(stdout, "Receiving packet %d\n", response_packet.seq_num);
        

        int window_end =  (window_end > response_packet.max_seq_num?response_packet.max_seq_num : WSIZE);
        int window_curr = (response_packet.seq_num + response_packet.seq_count*30720) / MAX_PAYLOAD_SIZE;


        // 1. pkt # in [rcvbase, rcvbase+N-1]
        int front = window_curr - window_start;
        if ((front >= 0) && (front < WSIZE)){
            // send ACK
            struct packet ack_packet;
            memset((char *) &ack_packet, 0, sizeof(ack_packet));

            ack_packet.type = 2;
            ack_packet.seq_num = response_packet.seq_num;
            ack_packet.seq_count = response_packet.seq_count;

            sendto(sockfd, &ack_packet, sizeof(ack_packet), 0, (struct sockaddr *) &serv_addr, serv_len);
            printf("Sending packet %d %s\n", ack_packet.seq_num, retrans);
            
            memcpy(&(window[front]), &response_packet, sizeof(struct packet));
            
            while (1){

                int first_packet_in_window = (window[0].seq_num+window[0].seq_count*30720) / MAX_PAYLOAD_SIZE;
                if (first_packet_in_window == window_start) {
                    fwrite(window[0].data, sizeof(char), window[0].data_size, fp);
                    
                    // update window
                    int y = 0;
                    for (y = 0; y < WSIZE-1; y++){
                        memcpy(&(window[y]), &(window[y+1]), sizeof(struct packet));
                    }
                    //memset(&(window[WSIZE-1]), 0, sizeof(struct packet));
                    window_start++;
                }
                else
                    break;
            }
        }
        
        //memset((char *) &response_packet.data, 0, sizeof(response_packet.data));
        
        if (response_packet.fin == 1) {
            struct packet fin_packet;
            memset((char *) &fin_packet, 0, sizeof(fin_packet));
            fin_packet.fin = 2;
            fin_packet.seq_num = response_packet.seq_num;
            fd_set inSet;
        
            int attempt = 1;
            while (1) {
                sendto(sockfd, &fin_packet, sizeof(fin_packet), 0, (struct sockaddr *)& serv_addr, serv_len);
                printf("Sending packet %d FIN\n", response_packet.seq_num);
                
                int check_time = check_time_out(sockfd);

                if(check_time < 1)
                {
                    attempt++;
                    if (attempt == 3) {
                        printf("Transmission finished.\nConnection closed.\n");
                        close(sockfd);
                        exit(0);
                    }
                    continue;
                }
                // otherwise, fetch the ack
                struct packet ack;
                recvfrom(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *) &serv_addr, &(serv_len));
                
                if (ack.fin == 2) {
                    printf("Transmission Finished.\nConnection Closed.\n");
                    close(sockfd);
                    exit(0);
                }           
            }  
        }
    }
    exit(0);
}
