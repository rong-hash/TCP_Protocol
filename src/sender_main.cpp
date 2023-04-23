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
#include <sys/stat.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>

// added library
#include <netdb.h>
#include <deque>
#include <iostream>
#include <chrono>

#define MSS 1400
#define MAX_RAND 3034
#define SYN 1
#define ACK 2
#define FIN 4
#define DATA 8
#define RST 16
#define EMPTY 32
#define MAX_SYN_TIMES 4
#define TIME_OUT 40000
#define TIMEOUT_TIMES 5
#define FR  1               // slow start
#define LP  2               // Congestion Avoidance
#define CO  3               // Congestion happens, timeout
#define DA  4               // Duplicate ACK happens
#define BIG_NUM 100000000
#define ALPHA 0.125
#define BETA 0.25
#define MAXSEG 250
#define MINTHR 125
#define QUEUELEN    2000

using namespace std;




void diep(char *s) {
    perror(s);
    exit(1);
}



// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

typedef struct package{
    unsigned long long seq_num;
    unsigned long long ack_num;
    int flags; // bit0: SYNbit, bit1: ACKbit, bit2: FINbit, bit 3: Data bit, bit 4: RST
    int datasize;
    char data[MSS];
} package;

// window starting point
unsigned long long base_seq;
// window ending point
unsigned long long next_sent;
unsigned long long int bytestoT;
unsigned long long int bytesTrans;
struct sockaddr_in si_other;
int s, slen;
// Congestion Avoidance threshold
int threshold;
// sending window size, cwnd
int seg_num;
long long cwnd;

int RTT_times = 0;
int estimate_rtt = 20000;
int devrtt = 10000;
int time_out;

int duptimes = 0;


struct addrinfo* recvaddr;

FILE *fp;
FILE *stream;

deque<package> package_queue;

unsigned long long send_base;

int fill_queue(unsigned long long int bytesToTransfer);

int establish_connection();

int congestion_control(int state, int newack, int dupack, int timeout);

int send_data();

int close_connection();

int get_seg_index();

int estimate_timeout(int rtt);


int reliablyTransfer(char* hostname, unsigned short int hostUDPport, char* filename, unsigned long long int bytesToTransfer) {
    //Open the file
    int rv;
    fp = fopen(filename, "rb");
    struct addrinfo hints;
    struct addrinfo *servinfo, *p;
    char UDPport[10];
    char recv_addr[INET6_ADDRSTRLEN];
    memset(&UDPport, 0, 10);
    memset(&hints, 0, sizeof hints); 
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;
    // convert portnum to str again
    sprintf(UDPport, "%d", hostUDPport);
    threshold = BIG_NUM;
    if (fp == NULL) {
        printf("Could not open file to send.");
        exit(1);
    }

	/* Determine how many bytes to transfer */

    slen = sizeof (si_other);

    if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1){
        fprintf(stderr, "create socket failed\n");
        close(s);
        exit(1);
    }

    memset((char *) &si_other, 0, sizeof (si_other));
    si_other.sin_family = AF_INET;
    si_other.sin_port = htons(hostUDPport);
    if (inet_aton(hostname, &si_other.sin_addr) == 0) {
        fprintf(stderr, "inet_aton() failed\n");
        close(s);
        exit(1);
    }

    // find addressinfo of receiver
    if ((rv = getaddrinfo(hostname, UDPport, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        close(s);
		return 1;
	}
    printf("successfully get the address\n");
    for(p = servinfo; p != NULL; p = p->ai_next) {
        break;
    }
    if (p == NULL)  {
        fprintf(stderr, "server: failed to bind\n");
        close(s);
        exit(1);
    }
    inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr),
			recv_addr, sizeof recv_addr);
	printf("client: connecting to %s\n", recv_addr);
    recvaddr = p;
    package empty;
    empty.flags = EMPTY;
    package_queue = deque<package>(QUEUELEN, empty);
	/* Send data and receive acknowledgements on s*/

    if (fill_queue(bytesToTransfer) == 1){
        close(s);
        return 1;
    }
    printf("successfully fill queue\n");
    if (establish_connection() == 1){
        close(s);
        return 1;
    }
    printf("successfully establish connection\n");
    if (send_data() == 1){
        close(s);
        return 1;
    }
    printf("successfully send data\n");
    if (close_connection() == 1){
        close(s);
        return 1;
    }

    printf("successfully close connection\n");
    printf("Closing the socket\n");
    close(s);
    fclose(fp);
    return 0;

}

// fill the package queue
// return 0 if success, 1 if fail
// first package does not contain data, else contain data
int fill_queue(unsigned long long int bytesToTransfer){
    int data_size;
    unsigned long long seq = 0;
    // first package is SYN
    base_seq = seq;
    package first_pack;
    first_pack.flags = SYN;
    first_pack.datasize = MSS;
    first_pack.seq_num = seq;
    package_queue[0] = first_pack;
    seq += MSS;
    // second package is ACK
    package sec_pack;
    sec_pack.flags = ACK;
    sec_pack.datasize = MSS;
    sec_pack.seq_num = seq;
    package_queue[1] = sec_pack;
    seq += MSS;

    return 0;
}


// establish_connection
// return 0 if success 1 if failure
// three way handshake, first send SYN, second send ACK
int establish_connection(){
    // permit only five timeout

    int n;
    int retry = 0;
    package firstseg = package_queue[0];
    package* recvseg = new package;
    // send SYN segment

    while (retry < TIMEOUT_TIMES){
        if (sendto(s, (char*) &firstseg, sizeof(package), 0, recvaddr->ai_addr, recvaddr->ai_addrlen) == -1){
            fprintf(stderr, "send the first segment failed\n");
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
            if (recvseg->ack_num == package_queue[1].seq_num && recvseg->flags == (SYN | ACK)){
                printf("receive SYN/ACK successfully.\n");
                break;
            }
            else{
                printf("Retrying...\n");
                retry++;  // retry if wrong segment received
            }
        } 
        else {
            printf("Retrying...\n");
            retry++;  // retry if timeout occurred
        }
    }
    // if retry reach TIMEOUT_TIMES, then can't receive the ack successfully
    if (retry == TIMEOUT_TIMES){
        perror("cannot receive ack");
        exit(1);
    }
    // send ACK
    package secseg = package_queue[1];
    for (int i = 0; i < 3; i++){
        if (sendto(s, (char*) &secseg, sizeof(package), 0, recvaddr->ai_addr, recvaddr->ai_addrlen) == -1){
            fprintf(stderr, "send data failed\n");
            return 1;
        }
    }
    send_base = next_sent = package_queue[1].seq_num + package_queue[1].datasize;
    printf("seq0: %lld, seq1: %lld, seq2: %lld\n", package_queue[0].seq_num, package_queue[1].seq_num, package_queue[2].seq_num);
    return 0;
}

// congestion_control

// input: state -- 1. Fast_Recovery (FR), 2. Linear_Probing (LP), 3. Congestion(CO)

// output: state


int congestion_control(int state, int newack, int dupack, int timeout){
    int next_state;
    switch(state){
        // slow start state
        case FR:
            if (newack){
                duptimes = 0;
                seg_num += 1;
                // seg_num = min(seg_num, MAXSEG);
                next_state = FR;
                if (seg_num >= threshold){
                    cwnd = seg_num * sizeof(package);
                    next_state = LP;
                }
            }
            else if (dupack){
                if (duptimes >= 3){
                    duptimes = 0;
                    threshold = seg_num / 2;
                    // threshold = max(MINTHR, threshold);
                    seg_num = threshold + 3;
                    next_state = DA;
                }
                else
                    next_state = FR;
                duptimes++;
                seg_num += 1;
                // seg_num = min(seg_num, MAXSEG);
            }
            else if (timeout){
                threshold = seg_num / 2;
                threshold = max(1, threshold);
                seg_num = 1;
                next_state = FR;
            }
            else {
                perror("input error in congestion control");
                exit(1);
            }
            break;
        // congestion avoidance state
        case LP:
            if (newack){
                duptimes = 0;
                cwnd += (sizeof(package) * sizeof(package) / cwnd);
                seg_num = cwnd / sizeof(package);
                // seg_num = min(seg_num, MAXSEG);
                next_state = LP;
            }
            else if (dupack){
                if (duptimes >= 3){
                    duptimes = 0;
                    threshold = seg_num / 2;
                    threshold = max(1, threshold);
                    seg_num = threshold + 3;
                    next_state = DA;
                }
                else
                    next_state = LP;
                duptimes++;
                seg_num++;
                // seg_num = min(seg_num, MAXSEG);
                cwnd += sizeof(package);
            }
            else if (timeout){
                threshold = seg_num / 2;
                // threshold = max(MINTHR, threshold);
                threshold = max(1, threshold);
                seg_num = 1;
                next_state = FR;
            }
            else {
                perror("input error in congestion control");
                exit(1);
            }
            break;
        // fast recovery state
        case DA:
            if (newack){
                duptimes = 0;
                // seg_num += 1;
                // seg_num = min(seg_num, MAXSEG);
                seg_num = threshold + 3;
                cwnd = seg_num * sizeof(package);
                next_state = LP;
            }
            else if (dupack){
                duptimes++;
                seg_num += 1;
                // seg_num = min(seg_num, MAXSEG);
                next_state = DA;
            }
            else if (timeout){
                threshold = seg_num / 2;
                threshold = max(1, threshold);
                // threshold = max(MINTHR, threshold);
                seg_num = 1;
                next_state = FR;
            }
            else {
                perror("input error in congestion control");
                exit(1);
            }
            break;
        default:
            printf("congestion state error\n");
    }
    return next_state;
}

// input: send_base
// output: get the index of segment in package_queue
int get_seg_index(int seq_num){
    // if it is the last package, then send_base - base_seq is not n * MSS
    // it is less than n * MSS
    if ((seq_num - base_seq) % MSS)
        return (seq_num - base_seq) / MSS + 1;
    return ((seq_num - base_seq) / MSS) % QUEUELEN;
}



// estimate_timeout
// input: SampleRTT
// output: timeout value in this cycle
// functionality: calculate the timeout value in this cycle based on sample RTT and estimated RTT
int estimate_timeout(int rtt){
    int timeout;
    devrtt = (1 - BETA) * devrtt + BETA * abs(rtt - estimate_rtt);
    timeout = estimate_rtt + 4* devrtt;
    estimate_rtt = (1 - ALPHA) * estimate_rtt + ALPHA * rtt;
    printf("timeout is %d\n", timeout);
    printf("devrtt is %d\n", devrtt);
    printf("estimate rtt is %d\n", estimate_rtt);
    printf("rtt is %d\n", rtt);
    return timeout;
}

// fill the buffer before sending the data
int fill_buf(unsigned long long int seq){
    int seg_index = get_seg_index(seq);
    int queue_index = seg_index % QUEUELEN;
    int read_bytes = min((unsigned long long)MSS, bytestoT);
    // clear the buffer
    memset(package_queue[queue_index].data, 0, MSS);
    // fill the data, seqnum, flag and datasize
    int actual_read = fread(package_queue[queue_index].data, 1, read_bytes, fp);
    package_queue[queue_index].datasize = actual_read;
    package_queue[queue_index].seq_num = seq;
    package_queue[queue_index].flags = DATA;
    bytestoT -= actual_read;
    return actual_read;
}


// input: None
// output: 0 if success 1 if failure

// functionality: send data to the receiver
int send_data(){
    int seg_index, base_index;
    seg_index = get_seg_index(send_base);
    base_index = get_seg_index(send_base);
    stream = fopen("mylog.txt", "w");
    int n;
    package* recvseg = new package;
    // initialize the segment sent number to be 1
    seg_num = 1;
    cwnd = sizeof(package);
    int congestion_state = FR;
    threshold = 64;
    auto last = chrono::high_resolution_clock::now();
    int read_bytes = fill_buf(send_base);
    printf("fill the buf with %d bytes\n", read_bytes);
    if (read_bytes == 0){
        printf("no data in the file, or no data need to be transferred\n");
        return 0;
    }
    int send_size = sendto(s, (char*) &package_queue[seg_index % QUEUELEN], sizeof(package), 0, recvaddr->ai_addr, recvaddr->ai_addrlen);
    // flag of timeout, duplicate ack, new ack
    int to = 0, na = 0, da = 0;             
    next_sent = package_queue[seg_index % QUEUELEN].seq_num + package_queue[seg_index % QUEUELEN].datasize;
    if (send_size == -1){
        fprintf(stderr, "send the first data failed\n");
        return 1;
    }
    while (send_base < next_sent){
        // set timeout 100ms
        struct timeval tv;
        time_out = TIME_OUT;
        tv.tv_sec = 0;
        tv.tv_usec = time_out;  // 100ms
        // reset the flag of new ack and timeout and duplicate ack
        na = 0;
        to = 0;
        da = 0;
        if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv)) < 0) {
            perror("setsockopt failed");
            exit(EXIT_FAILURE);
        }
        // receive reply
        n = recvfrom(s, (char *)recvseg, sizeof(package), 0, NULL, NULL);
        auto now = chrono::high_resolution_clock::now();
        auto duration = chrono::duration_cast<chrono::microseconds>(now - last);
        // refresh the timer
        last = chrono::high_resolution_clock::now();
        if (n > 0) {
            // no connection established in receiver
            if (recvseg->flags & RST){
                fprintf(stderr, "No connection established in receiver\n");
                return 1;
            }
            if (recvseg->ack_num > send_base){
                send_base = recvseg->ack_num;
                na = 1;
            }
            // receive duplicate ACK
            else{
                fprintf(stream, "get %d duplicate ack\n", duptimes);
                da = 1;
                // if more than 3 duplicate ack received, then recet dup ack
                // and send the segment immediately.
                if (duptimes >= 3 && congestion_state != DA){
                    // duptimes = 0;
                    seg_index = get_seg_index(send_base);
                    fprintf(stream, "fast retransmit, send %d segment\n", seg_index);
                    if (sendto(s, (char*) &package_queue[seg_index % QUEUELEN], sizeof(package), 0, recvaddr->ai_addr, recvaddr->ai_addrlen) == -1){
                        fprintf(stderr, "send data failed\n");
                        return 1;
                    }
                }
            }
        } 
        else {
            fprintf(stream, "cannot receive ack in this iteration\n");
            to = 1;
            base_index = get_seg_index(send_base);
            sendto(s, (char*) &package_queue[base_index % QUEUELEN], sizeof(package), 0, recvaddr->ai_addr, recvaddr->ai_addrlen);
        }
        // find if there is congestion, based on which we get sent segment number in this cycle
        congestion_state = congestion_control(congestion_state, na, da, to);
        seg_index = get_seg_index(next_sent);
        base_index = get_seg_index(send_base);
        fprintf(stream, "send base index: %d, to send index: %d, cwnd: %d, queue_size: %ld, congestion_state: %d\n", base_index, seg_index, seg_num, package_queue.size(), congestion_state);
        while (next_sent < send_base + sizeof(package) * seg_num){
            read_bytes = fill_buf(next_sent);
            if (read_bytes == 0)
                break;
            seg_index = get_seg_index(next_sent);
            fprintf(stream, "send the %dth seg\n", seg_index);
            if (sendto(s, (char*) &package_queue[seg_index % QUEUELEN], sizeof(package), 0, recvaddr->ai_addr, recvaddr->ai_addrlen) == -1){
                fprintf(stderr, "send data failed\n");
                return 1;
            }
            next_sent += read_bytes;
        }
    }
    return 0;
}


// close_connection
// input: none
// output: 0 if success, 0 if failure

// functionality: close the connection, 4-way handshake
int close_connection(){
    int n;
    int retry = 0;
    int y;
    package fin;
    package* recvseg = new package;
    fin.flags = FIN;
    fin.datasize = 0;
    fin.seq_num = send_base;

    while (retry < TIMEOUT_TIMES){
        // send close connection segment
        if (sendto(s, (char*) &fin, sizeof(package), 0, recvaddr->ai_addr, recvaddr->ai_addrlen) == -1){
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
            if (recvseg->ack_num == send_base + 1 && recvseg->flags == ACK){
                y = recvseg->seq_num;
                printf("receive ACK successfully.\n");
                break;
            }
            else{
                printf("Retrying to receive finack...\n");
                retry++;  // retry if wrong segment received
            }
        } 
        else {
            printf("Timeout, Retrying to receive finack...\n");
            retry++;  // retry if timeout occurred
        }
    }
    if (retry == TIMEOUT_TIMES){
        fprintf(stderr, "cannot receive ACK\n");
        return 1;
    }
    n = 0;
    retry = 0;
    // receive fin from receiver
    while (true){
        n = recvfrom(s, (char *)recvseg, sizeof(package), 0, NULL, NULL);
        if (n > 0 && recvseg->flags == FIN )
            break;
        else 
            retry++;
        if (retry >= TIMEOUT_TIMES){
            fprintf(stderr, "receive FIN failed\n");
            return 1;
        }
    }
    // send ack to receiver's fin
    // easy to lose segment, so send several times
    package ack;
    ack.flags = ACK;
    ack.datasize = 0;
    ack.ack_num = y+2;
    int times = 5;
    int i = 0;
    while (i < times){
        if (sendto(s, (char*) &ack, sizeof(package), 0, recvaddr->ai_addr, recvaddr->ai_addrlen) == -1){
            fprintf(stderr, "send data failed\n");
            return 1;
        }
        i++;
    }
    return 0;
}



/*
 * 
 */
int main(int argc, char** argv) {

    unsigned short int udpPort;
    unsigned long long int numBytes;

    if (argc != 5) {
        fprintf(stderr, "usage: %s receiver_hostname receiver_port filename_to_xfer bytes_to_xfer\n\n", argv[0]);
        exit(1);
    }
    udpPort = (unsigned short int) atoi(argv[2]);
    numBytes = atoll(argv[4]);
    bytestoT = numBytes;
    bytesTrans = 0;

    if (reliablyTransfer(argv[1], udpPort, argv[3], numBytes) == 1){
        fprintf(stderr, "send file failed\n");
        return 1;
    }
    


    return (EXIT_SUCCESS);
}
