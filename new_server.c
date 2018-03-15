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

#define MTU 1024
#define HEADERSIZE 52
#define DATASIZE 972
#define WAIT 500

struct packet_info
{
    //Packet Types:
    //Datagram: 1
    //ACK: 2
    //Retransmission: 3

    int type;
    int seq_no;
    int max_no;
    
    //Fin:
    //0: Middle of file
    //1: Finished
    //2: SYN Packet

    int fin;
    int error;
    double time;
    char data[DATASIZE];
    int data_size;
    int seq_count;
};

void *transform_addr(struct sockaddr *sa)
{
    return &(((struct sockaddr_in*)sa)->sin_addr);
}

int diff_ms(struct timeval t1, struct timeval t2)
{
    return (((t1.tv_sec - t2.tv_sec) * 1000000) + (t1.tv_usec - t2.tv_usec))/1000;
}

int main(int argc, char *argv[])
{
    int sockfd = -1, n2;
    struct addrinfo hints, *servinfo, *p;
    int rv;
    long numbytes;
    struct sockaddr_storage their_addr;
    socklen_t addr_len;
    char s[INET6_ADDRSTRLEN];
    int cwnd = 5120;
    struct packet_info response_packet;
    char filename[MTU];
    int last = 0;
    int sequence_counter = 0;
    
    // extract argument parameters
    if (argc != 2)
    {
        fprintf(stderr,"usage: server <portnumber>\n");
        exit(1);
    }
    
    char *port = argv[1];
    
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; //Use IPv4
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE; //Use my IP
    
    if ((rv = getaddrinfo(NULL, port, &hints, &servinfo)) != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }
    
    // loop through all the results and bind to the first we can
    for(p = servinfo; p != NULL; p = p->ai_next)
    {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
        {
            perror("sender: socket");
            continue;
        }
        
        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1)
        {
            close(sockfd);
            perror("sender: bind");
            continue;
        }
        
        break;
    }
    
    if (p == NULL)
    {
        fprintf(stderr, "Error: sender failed to bind socket\n");
        return 2;
    }
    
    //freeaddrinfo(servinfo);
    while(1) {
        
        printf("Waiting for requested filename...\n");
        
        addr_len = sizeof(their_addr);
        
        if ((numbytes = recvfrom(sockfd, &filename, sizeof(filename), 0, (struct sockaddr *)&their_addr, &addr_len)) == -1)
        {
            printf("Error: failed to receive filename\n");
            exit(1);
        }
        
        printf("Received filename from %s\n", inet_ntop(their_addr.ss_family, transform_addr((struct sockaddr *)&their_addr), s, sizeof s));
        filename[numbytes] = '\0';
        printf("Requested filename: \"%s\"\n", filename);
        
        // buf contains requested file name; if it exists, send back packets (up to 1 KB at a time) to receiver
        
        // create source buffer to fopen and fread designated file
        char *source = NULL;
        FILE *fp = fopen(filename, "r");
        int sourceLength;
        
        if (fp==NULL)
        {
            response_packet.error = 1;
            // send packet with error = 1 to represent no file found
            sendto(sockfd, &response_packet, sizeof(response_packet), 0, (struct sockaddr *)&their_addr, addr_len);
            printf("Error: file not found!\n");
            exit(1);
        }
        
        if (fseek(fp, 0L, SEEK_END) == 0){
            // set fsize to file size
            long fsize = ftell(fp);
            
            if (fsize == -1)
            {
                response_packet.error = 1;
                // send packet with error = 1 to represent no file found
                sendto(sockfd, &response_packet, sizeof(response_packet), 0, (struct sockaddr *)&their_addr, addr_len);
                printf("Error: file not found!\n");
                exit(1);
            }
            
            // allocate source buffer to filesize
            source = malloc(sizeof(char) * (fsize + 1));
            
            // return to front of file
            if (fseek(fp, 0L, SEEK_SET) != 0)
            {
                response_packet.error = 1;
                // send packet with error = 1 to represent no file found
                sendto(sockfd, &response_packet, sizeof(response_packet), 0, (struct sockaddr *)&their_addr, addr_len);
                printf("Error: file not found!\n");
                exit(1);
            }
            
            // set source to file data
            sourceLength = fread(source, sizeof(char), fsize, fp);
            
            // check file source for fread errors
            if (sourceLength == 0)
            {
                response_packet.error = 1;
                // send packet with error = 1 to represent no file found
                sendto(sockfd, &response_packet, sizeof(response_packet), 0, (struct sockaddr *)&their_addr, addr_len);
                printf("Error: file not found!\n");
                exit(1);
            }
            
            // NULL-terminate the source
            source[sourceLength] = '\0';
        }
        
        // close file
        fclose(fp);
        
        /* Setup to send back a response packet */
        memset((char *) &response_packet, 0, sizeof(response_packet));
        
        /* Determines how many packets we have to send based on the length of the requested file */
        int window_size = cwnd / MTU;
        int no_of_pkt = (sourceLength + (DATASIZE-1)) / DATASIZE;
        response_packet.max_no = no_of_pkt;
        response_packet.seq_no = 0;
        int start_of_seq = 0;
        int current_pkt = 0;
        int end_of_seq;
        if (window_size > no_of_pkt)
            end_of_seq = no_of_pkt;
        else
            end_of_seq = window_size;
        struct packet_info *packet_window;
        packet_window = (struct packet_info *) malloc(window_size * sizeof(struct packet_info));
        int* time_table = (int*) malloc(window_size*sizeof(int));
        int i;
        struct timeval start;
        gettimeofday(&start, NULL);
        
        //Iterating through the window
        //-2 for not yet sent
        //-1 for already ACK'ed
        //otherwise for not yet ACK'ed
        
        for (i = 0; i<window_size; i++)
            time_table[i] = -2;
        
        //Send SYN packet
        while(1) {
            struct packet_info sync_packet;
            memset((char *) &sync_packet, 0, sizeof(sync_packet));
            sync_packet.type = 1;
            sync_packet.seq_no = 0;
            sync_packet.fin = 2;
            sendto(sockfd, &sync_packet, sizeof(sync_packet), 0, (struct sockaddr *)&their_addr, addr_len);
            
            printf("Sending packet %d %d SYN\n", sync_packet.seq_no, cwnd);
            
            struct packet_info syn_received;
            fd_set inSet;
            struct timeval timeout;
            int received;
            int max_retry = 0;
            FD_ZERO(&inSet);
            FD_SET(sockfd, &inSet);
            
            // wait for specified time
            timeout.tv_sec = 0;
            timeout.tv_usec = 100000;
            
            received = select(sockfd+1, &inSet, NULL, NULL, &timeout);

            if(received < 1)
            {
                if (max_retry > 2) {
                    printf("Reached Maximum Retry Times, stop transmission.\n");
                    max_retry++;
                    break;
                }
                printf("Retransmit SYN packet!\n");
                continue;
            }

            recvfrom(sockfd, &syn_received, sizeof(syn_received), 0, (struct sockaddr *) &their_addr, &addr_len);
            if(syn_received.seq_no == 0 && syn_received.fin == 2) {
                printf("Receiving packet 0\n");
                break;
            }
        }
        
        /* Create partitioned packets */
        int offset = 0;
        while(1){
            /* If next available seq # in window, send packet */
            if (time_table[current_pkt - start_of_seq] == -2 && !last){
                
                /* Packet type is Datagram */
                int seq_num = (current_pkt*DATASIZE+1) % 30720;
                offset = current_pkt*DATASIZE;
                sequence_counter = offset / 30720;
                response_packet.type = 1;
                response_packet.seq_no = seq_num;
                response_packet.max_no = no_of_pkt;
                response_packet.seq_count = sequence_counter;
                response_packet.fin = 0;
                
                /* Check if this is the last packet among the paritioned packets */
                if (current_pkt >= no_of_pkt-1) {
                    last = 1;
                }
                
                /* Set time table */
                struct timeval end;
                gettimeofday(&end, NULL);
                int time_diff = diff_ms(end, start);
                time_table[current_pkt - start_of_seq] = time_diff;
                
                /* Save */
                if (sourceLength - offset < DATASIZE) {
                    memcpy(response_packet.data, source+offset, sourceLength - offset);
                    response_packet.data_size = sourceLength - offset;
                }
                else {
                    memcpy(response_packet.data, source+offset, DATASIZE);
                    response_packet.data_size = DATASIZE;
                }
                memcpy(&(packet_window[current_pkt - start_of_seq]), &response_packet, sizeof(struct packet_info));
                packet_window[current_pkt - start_of_seq].type = 3;
                
                
                /* Send */
                sendto(sockfd, &response_packet, sizeof(response_packet), 0, (struct sockaddr *)&their_addr, addr_len);
                
                if (current_pkt < end_of_seq)
                    current_pkt++;
                char* status = "";
                if (response_packet.type == 3)
                    status = "Retransmission";
                else if (response_packet.fin == 1)
                    status = "FIN";
                printf("Sending packet %d %d %s\n", response_packet.seq_no, cwnd, status);
            }
            
            /* Resend */
            for (i = start_of_seq; i<end_of_seq; i++){
                if (time_table[i-start_of_seq] > 0){
                    struct timeval end;
                    gettimeofday(&end, NULL);
                    int time_diff = diff_ms(end, start);
                    response_packet.time = time_diff - time_table[i-start_of_seq];
                    if (time_diff - time_table[i-start_of_seq] >= WAIT){
                        // printf("(%d)\n", time_diff - time_table[i-start_of_seq]);
                        if ((packet_window[i - start_of_seq]).seq_no
                            == (packet_window[i - start_of_seq]).max_no)
                            (packet_window[i - start_of_seq]).fin = 1;
                        sendto(sockfd, &(packet_window[i - start_of_seq]), sizeof((packet_window[i - start_of_seq])), 0, (struct sockaddr *)&their_addr, addr_len);
                        printf("Sending packet %d %d Retransmission\n", packet_window[i - start_of_seq].seq_no, cwnd);
                        time_table[i-start_of_seq] = diff_ms(end, start);
                    }
                }
            }
            
            // 3b
            if (time_table[0] == -1){
                //Move the time table and packet window to position 0
                for (i = 0; i<window_size-1; i++){
                    time_table[i] = time_table[i+1];
                    memcpy(&(packet_window[i]), &(packet_window[i+1]), sizeof(struct packet_info));
                }
                time_table[window_size-1] = -2;
                memset(&(packet_window[window_size-1]), 0, sizeof(struct packet_info));
                start_of_seq++;
                
                if (end_of_seq<no_of_pkt)
                    end_of_seq++;
                window_size = end_of_seq - start_of_seq;
            }
            
            
            // 3a) ACK(n) in [sendbase,sendbase+N]: mark pkt n as received
            
            fd_set inSet;
            struct timeval timeout;
            int received;
            
            FD_ZERO(&inSet);
            FD_SET(sockfd, &inSet);
            
            // wait for specified time
            timeout.tv_sec = 0;
            timeout.tv_usec = 100000;
            
            // check for acks
            received = select(sockfd+1, &inSet, NULL, NULL, &timeout);
            
            // if timeout and no acks were received, break and maintain window position
            if(received < 1)
            {
                //printf("Waiting for ACK timeout.\n");
                continue;
            }
            // otherwise, fetch the ack
            struct packet_info ack;
            recvfrom(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *) &their_addr, &addr_len);
            // printf("%x\t%d\n", received_crc, current_pkt);
//            for (i = start_of_seq; i<end_of_seq; i++){
//                 printf("%d\t", time_table[i-start_of_seq]);
//            }
            if(ack.type == 2) {
                int pkt = (ack.seq_no + ack.seq_count*30720) / DATASIZE;
                time_table[pkt-start_of_seq] = -1;
                printf("Received ACK %d\n", ack.seq_no);
            }
            
            int finished = 0;
            if (last) {
                int n = 0;
                for(n=0; n < window_size; n++) {
                    if (time_table[n] > 0) {
                        finished = 0;
                        break;
                    }
                    else
                        finished = 1;
                }
                if (finished == 0){
                    /* Resend */
                    for (i = start_of_seq; i<start_of_seq+5; i++){
                        if (time_table[i-start_of_seq] >= 0){
                            struct timeval end;
                            gettimeofday(&end, NULL);
                            int time_diff = diff_ms(end, start);
                            response_packet.time = time_diff - time_table[i-start_of_seq];
                            if (time_diff - time_table[i-start_of_seq] >= WAIT){
                                // printf("(%d)\n", time_diff - time_table[i-start_of_seq]);
                                response_packet.type = 3;
                                if ((packet_window[i - start_of_seq]).seq_no
                                    == (packet_window[i - start_of_seq]).max_no)
                                    (packet_window[i - start_of_seq]).fin = 1;
                                sendto(sockfd, &(packet_window[i - start_of_seq]), sizeof((packet_window[i - start_of_seq])), 0, (struct sockaddr *)&their_addr, addr_len);
                                printf("Sending packet %d %d Retransmission\n", packet_window[i - start_of_seq].seq_no, cwnd);
                                time_table[i-start_of_seq] = diff_ms(end, start);
                            }
                        }
                    }

                }
                else {
                    //send fin and waiting for fin ACK
                    while (1) {
                        struct packet_info fin_packet;
                        fin_packet.fin = 1;
                        //send fin
                        sendto(sockfd, &fin_packet, sizeof(fin_packet), 0, (struct sockaddr *)&their_addr, addr_len);
                        
                        received = select(sockfd+1, &inSet, NULL, NULL, &timeout);
                        
                        if(received < 1)
                        {
                            continue;
                        }

                        struct packet_info fin_ack;
                        recvfrom(sockfd, &fin_ack, sizeof(fin_ack), 0, (struct sockaddr *) &their_addr, &addr_len);
                        
                        sendto(sockfd, &fin_ack, sizeof(fin_ack), 0, (struct sockaddr *)&their_addr, addr_len);
                        
                        //fin = 2 means closing connection
                        if (fin_ack.fin == 2) {
                            printf("Transmission Finished.\nConnection Closed.\n");
                            close(sockfd);
                            return 0;
                        }
                    }
                }
            }
        }
        return 0;
    }
}
