#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <chrono>
#include <vector>
using namespace std;
#define BUF_SIZE 1500
#define MSS 1400
// set the time out tobe 100ms
#define TIME_OUT 100000
#define ACK_TIMEOUT 20000
// #define FIN_TIME_OUT 50000
#define TIMEOUT_TIMES 5
#define SYN     1
#define ACK     2
#define FIN     4
#define DATA    8
#define RST     16
#define EMPTY   32
#define QUEUELEN 2000

struct sockaddr_in si_me, si_other;
int s, slen;
unsigned long long base_seq;
char buf[BUF_SIZE];

struct sockaddr_in client_addr;
socklen_t client_addr_len = sizeof(client_addr);


typedef struct package{
    unsigned long long seq_num;
    unsigned long long ack_num;
    int flags; // bit0: SYNbit, bit1: ACKbit, bit2: FINbit, bit 3: Data bit, bit 4: RST
    int datasize;
    char data[MSS];
} package;

// decrease the size of ack package
typedef struct ack_package{
    unsigned long long seq_num;
    unsigned long long ack_num;
    int flags;
    int datasize;
} ack_package;

vector<package> queue;


unsigned long long create_connection(int sid);
package* receive_segment(int sid, unsigned long long last_ack, FILE* file);
int close_connection(package* finseg);

void diep(char *s) {
    perror(s);
    exit(1);
}

// input: send_base
// output: get the index of segment in package_queue
int get_seg_index(int send_base){
    // if it is the last package, then send_base - base_seq is not n * MSS
    // it is less than n * MSS
    if ((send_base - base_seq) % MSS)
        return (send_base - base_seq) / MSS + 1;
    return ((send_base - base_seq) / MSS) % QUEUELEN;
}

void reliablyReceive(unsigned short int myUDPport, char* destinationFile) {
    
    slen = sizeof (si_other);


    if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
        diep("socket");

    memset((char *) &si_me, 0, sizeof (si_me));
    si_me.sin_family = AF_INET;
    si_me.sin_port = htons(myUDPport);
    si_me.sin_addr.s_addr = htonl(INADDR_ANY);
    printf("Now binding\n");
    if (bind(s, (struct sockaddr*) &si_me, sizeof (si_me)) == -1)
        diep("bind");
    package empty;
    empty.flags = EMPTY;
    queue = vector<package>(QUEUELEN, empty);


	/* Now receive data and send acknowledgements */    

    int retval;
    unsigned long long lastack;
    lastack = create_connection(s);
    printf("build connection successfully\n");
    if (lastack <= 0)
        return ;
    package* fin = new package;
    FILE* file = fopen(destinationFile, "w");
    fin = receive_segment(s, lastack, file);
    retval = close_connection(fin);

    close(s);
    // FILE* file = fopen(destinationFile, "w");
    // for (int i = 0; i < queue.size(); i++){
    //     if (fwrite(queue[i].data, 1, queue[i].datasize, file) != queue[i].datasize){
    //         perror("write into file failed");
    //         exit(1);
    //     }
    // }
    fclose(file);
	printf("%s received.\n", destinationFile);
    return ;
}

// create_connection
// use 3 way handshake to create connection with sender
// input: socketid
// output: report error if failed, or lastack if success
unsigned long long create_connection(int sid){
    int recv_len, flags;
    unsigned long long ack_num;
    package* seg = new package;
    while (1) {
        memset(buf, 0, BUF_SIZE);
        recv_len = recvfrom(sid, buf, BUF_SIZE, 0, (struct sockaddr*)&client_addr, &client_addr_len);
        if (recv_len <= 0){
            printf("Error in receiving messages.\n");
            continue;
        }
        seg = (package*) buf;
        // if the flag in the seg is not SYN, then send a RST segment back
        if (seg->flags != SYN){
            package errseg;
            errseg.flags = RST;
            if (sendto(s, (char*)(&errseg), sizeof(package), 0, (struct sockaddr*)&client_addr, client_addr_len) == -1){
                perror("sendto");
                exit(1);
            }
            continue;
        }
        // SYN seg received successfully
        else{
            printf("receive SYN segment successfully\n");
            // if receive the segment successfully, send segment back with flags ACK + SYN, ack_num = seq_num + datasized
            ack_num = seg->seq_num + seg->datasize;
            flags = SYN | ACK;
            package succseg;
            succseg.ack_num = ack_num;
            succseg.flags = flags;
            base_seq = seg->seq_num;
            if (sendto(s, (char*)(&succseg), sizeof(package), 0, (struct sockaddr*)&client_addr, client_addr_len) == -1){
                perror("sendto");
                exit(1);
            }
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = TIME_OUT;  // 100ms
            // set the max receiving message time
            if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv)) < 0) {
                perror("setsockopt failed");
                exit(EXIT_FAILURE);
            }
            while (recvfrom(sid, buf, BUF_SIZE, 0, (struct sockaddr*)&client_addr, &client_addr_len) <= 0){
                if (sendto(s, (char*)(&succseg), sizeof(package), 0, (struct sockaddr*)&client_addr, client_addr_len) == -1){
                    perror("sendto");
                    exit(1);
                }
            }
            break;
        }
    }

    return ack_num + MSS;
}
// receive_segment
// receive data and send the ack back
// input: socketid
// output: report error if failed, or finpack if success
package* receive_segment(int sid, unsigned long long last_ack, FILE* file){
    int recv_len;
    package* seg = new package;
    auto last = chrono::high_resolution_clock::now();
    package* finseg;
    int queue_index, base_index;
    // cancel the timeout
    while (1){
        memset(buf, 0, BUF_SIZE);
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;
        setsockopt(sid, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        recv_len = recvfrom(sid, buf, BUF_SIZE, 0, (struct sockaddr*)&client_addr, &client_addr_len);
        // if timeout, send the ack back
        if (recv_len <= 0){
            printf("recv failed\n");
            ack_package ackseg;
            ackseg.flags = ACK;
            ackseg.ack_num = last_ack;
            if (sendto(s, (char*) &ackseg, sizeof(ack_package), 0, (struct sockaddr*)&client_addr, client_addr_len) == -1){
                perror("send ack failed");
                exit(1);
            }
            last = chrono::high_resolution_clock::now();
            continue;
        }
        seg = (package*) buf;
        auto now = chrono::high_resolution_clock::now();
        // if receiving data, or receiving the ack / ack + syn in the starting stage
        if (seg->flags == DATA){
            auto duration = chrono::duration_cast<chrono::microseconds>(now - last);
            // if the received seq num is not inline
            // then send the ack back immediately
            queue_index = get_seg_index(seg->seq_num);
            if (queue[queue_index % QUEUELEN].flags != EMPTY || seg->seq_num < last_ack) 
                continue;
            package dataseg;
            dataseg.flags = seg->flags;
            dataseg.seq_num = seg->seq_num;
            dataseg.datasize = seg->datasize;
            memcpy(dataseg.data, seg->data, seg->datasize);
            queue[queue_index % QUEUELEN] = dataseg;
            base_index = get_seg_index(last_ack);
            printf("get %d seg, base index is %d\n", queue_index, base_index);
            while (queue[base_index % QUEUELEN].flags != EMPTY){
                fwrite(queue[base_index % QUEUELEN].data, 1, queue[base_index % QUEUELEN].datasize, file);
                queue[base_index % QUEUELEN].flags = EMPTY;
                last_ack = queue[base_index % QUEUELEN].seq_num + queue[base_index % QUEUELEN].datasize;
                base_index++;
            }
            ack_package ackseg;
            ackseg.flags = ACK;
            ackseg.ack_num = last_ack;
            if (sendto(s, (char*) &ackseg, sizeof(ack_package), 0, (struct sockaddr*)&client_addr, client_addr_len) == -1){
                perror("send ack failed");
                exit(1);
            }
        }
        else if (seg->flags == ACK)
            continue;
        else {
            finseg = seg;
            break;
        }
    }
    return finseg;
}

// close connection
// input finseg
// four way handshake to close the connection
// return 1 if failed, 0 if success
int close_connection(package* finseg){
    int retry = 0;
    int y = rand() % 10000;
    int n;
    package finack;
    package fin;
    package* recvseg = new package;
    finack.ack_num = finseg->seq_num + 1;
    finack.flags = ACK;
    finack.seq_num = y;
    fin.flags = FIN;
    fin.seq_num = y+ 1;

    while (retry < TIMEOUT_TIMES){
        // send close connection segment
        if (sendto(s, (char*) &finack, sizeof(package), 0, (struct sockaddr*)&client_addr, client_addr_len) == -1){
            fprintf(stderr, "send data failed\n");
            return 1;
        }
        if (sendto(s, (char*) &fin, sizeof(package), 0, (struct sockaddr*)&client_addr, client_addr_len) == -1){
            fprintf(stderr, "send data failed\n");
            return 1;
        }
        // set timeout 100ms
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = TIME_OUT;  // 100ms
        if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv)) < 0) {
            perror("setsockopt failed");
            exit(EXIT_FAILURE);
        }
        // receive reply
        n = recvfrom(s, (char *)recvseg, sizeof(package), 0, NULL, NULL);
        if (n > 0) {
            if (recvseg->flags == ACK ){
                printf("receive ACK successfully.\n");
                break;
            }
            else{
                printf("Receive seg flag not ACK, Retrying...\n");
                retry++;  // retry if wrong segment received
            }
        } 
        else {
            printf("Timeout, Retrying...\n");
            retry++;  // retry if timeout occurred
        }
    }
    if (retry == TIMEOUT_TIMES){
        perror("receiving last ack failed\n");
        exit(1);
    }
    return 0;
}

/*
 * 
 */
int main(int argc, char** argv) {

    unsigned short int udpPort;

    if (argc != 3) {
        fprintf(stderr, "usage: %s UDP_port filename_to_write\n\n", argv[0]);
        exit(1);
    }

    udpPort = (unsigned short int) atoi(argv[1]);

    reliablyReceive(udpPort, argv[2]);
    return 0;
}
