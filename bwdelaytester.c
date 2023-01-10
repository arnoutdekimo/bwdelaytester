 /*     +@              @         @                                                                
        @@              @        ,@+       .                                                       
        @@              @              @`  @  .@                                                   
        @@              @              @+  @  +@                                                   
   @@@@@@@    @@@@@`    @    ;@`  @`   @@ .@` @@    #@@@@#                                         
  @@   ,@@   @@`  #@`   @   @@+   @`   @@ #@+ @@   @@;  `@@                       ;                
  @     @@   @     @@   @ .@@     @`  .@@ @@@ @@`  @,    .@               @@     @@@               
 :@     @@  :@     @@   @#@@      @`  #@@ @,@ @@+ .@      @.              @@@    @@@@              
 :@     @@  :@@@@@@@@   @@@       @`  @'@ @`@ @+@ .@      @.             +@@@@  :@@@@@    @:       
 :@     @@  :@          @ @@      @`  @'@ @ @`@`@ .@      @.             @@@@@@ @@@@@@@   @@@      
 ;@     @@  :@          @  @@     @`  @`@'@ @+@`@ .@      @.             @@@@@@@@@@@@@@, ;@@@@     
  @@    @@   @@         @   @@`   @`  @ #@@ @@+ @  @@    @@             '@@@@@@@@@@@@@@@ @@@@@@    
  '@@@@@@@   ;@@@@@@'   @    @@`  @`  @ .@@ @@` @   @@@@@@              @@@@@@@@@@@@@@@@+@@@@@@@   
    ,@@,       ,@@,     @     #   @   @  @+ #@  @.   ,@@.               @@@@@@@@@@@@@@@@@@@@@@@@   
                                     .@  @, `@  @.                      @@@@@@@@@@@@@@@@@@@@@@@@@  
                                     ##  @` .@  ++                      @@@@@@@@@@@@@@@@@@@@@@@@@  
   .@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@   @   @   @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ 
 */
 
/**
 * Dekimo - Leuven 
 * Copyright(c) 2022 Dekimo, Inc. 
 *
 * @file bwdelaytester.c
 * @author Arnout Diels
 * @date 08 Nov 2022
 *
 * Bandwidth-delay tester
 * A simple IPv4 iperf-like application, which allows measuring bandwidth from a sender to a receiver, along with the perceived latency.
 * It has a very simple stdout/stderr periodic interface, which can easily be used by e.g. gnuplot to make realtime graphs from.
 *
 * Optionally an automated sweep can be configured to sweep a bandwidth range
 * At the end of the server execution, a latency histogram data is printed as well
 */

/* ***********************************************************************************************************************************************
 * Include Files
 * ***********************************************************************************************************************************************
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <getopt.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

/*
 * ***********************************************************************************************************************************************
 * Defines
 * ***********************************************************************************************************************************************
 */
#define BUFLEN                  2000         /* Max length of buffer for incoming packets. Must be larger than the maximum allowed packet size */
#define PORT                    8888         /* The port to which the UDP packets are sent */


#define MAX_OUT_OF_ORDER        10000        /* Maximum amount of packets to think possible in out of order, before deciding it really just was the remote restarting its counter */


/* BW-delay sweep-mode graph items */
#define BWDELAYGRAPHTICKS       100         /* Amount of SW sweep points to use. Ideally corresponds to BWBUCKETS, if entered max is GRAPHBWMAX. To avoid noise, better to have more ticks than resolution */
#define BWSWEEPTICKTIMESEC      1           /* Amount of time for each tick to attempt to send a fixed BW */
#define GRAPHMAXBW              3000        /* In Mbps. Worst worst case */
#define GRAPH_BWBUCKETS         300         /* Local resolution before downsampling in bw direction at the end. Resolution is GRAPHMAXBW/this */
#define GRAPH_BUCKETRES         (GRAPHMAXBW/GRAPH_BWBUCKETS)
#define BUCKET_CONTENT_THRESH   10          /* At least 10 samples (+- 1 sec) inside a bucket */

/* Latency histogram buckets */
#define LATHIST_MAX_LATMS       20
#define LATHIST_QUEUECOUNT      100

#define xstr(s) str(s)
#define str(s) #s

/*
 * ***********************************************************************************************************************************************
 * Local Members
 * ***********************************************************************************************************************************************
 */

/*
 * The packet structure of the UDP content. Pretty simple
 */
typedef struct udppckt
{
    uint64_t ctr;
    uint64_t timestamp;
    /* Rest of the packet is dummy payload */
} bdt_pkt;

/*
 * Global program variables
 */
struct progsettings
{
    char *prgname;
    bool clientmode;            /* False: SERVER mode, TRUE: Client mode */

    char *dsthost;              /* In client mode, the destination IP to send to */
    uint64_t nsdelay;           /* In client mode, time to wait between packets (exclusive to targetbwmbps) */
    uint64_t targetbwmbps;      /* In client mode, the target bandwidth (exclusive to nsdelay) */
    int packetsize;             /* In client mode, dummy UDP payload size */

    int compensationlatency;    /* Pass latency to compensate for first packet */
    bool nonblockingmode;       /* Nonblocking socket -- allows for link oversaturation for locally generated traffic */
    char *sourceifbind;         /* If nonzero, string to specify interface to bind to (e.g. for VRF) */
    bool sweepmode;             /* If enabled, will sweep from 1/BWDELAYGRAPHTICKS to BWDELAYGRAPHTICKS/BWDELAYGRAPHTICKS ratio of bw */
    bool nonsyncedclocks;       /* Clocks on sender and receiver are not very accurately synced (< 0.1ms) (e.g. through PTP) */
} progsettings;

/*
 * Binning for sweep mode struct
 */
struct bwdelaypoint
{
    /* A point has a implicit BW range determined by its position in the array */
    double losspercent_cumul; /* 0..100, adding every time */
    int64_t min_delay;
    int64_t max_delay;
    int64_t avg_delay_cumul; /* Delays added, and divided at the end every time */

    uint64_t total_samples_for_this_bucket; /* Divide counters at the end */
} bwdelaypoints[GRAPH_BWBUCKETS];


uint64_t latencyhits[LATHIST_QUEUECOUNT];
/*
 * ***********************************************************************************************************************************************
 * Private Function Prototypes
 * ***********************************************************************************************************************************************
 */


/*
 * ***********************************************************************************************************************************************
 * Private Functions
 * ***********************************************************************************************************************************************
 */


static void die(char *s)
{
    perror(s);
    exit(1);
}

void sig_handler(int signum)
{
    /*
     * If we were the server, first print overall latency histogram data
     */
    if(!progsettings.clientmode) {
        fprintf(stderr, "## Printing latency histogram:\n");
        uint64_t totpkts = 0;
        for(int i = 0; i < LATHIST_QUEUECOUNT; i++) {
            totpkts += latencyhits[i];
        }

        uint64_t cumul = 0;
        for(int i = 0; i < LATHIST_QUEUECOUNT; i++) {
            cumul += latencyhits[i];
            fprintf(stderr, "%u %lu %lu\n", i*LATHIST_MAX_LATMS*1000/LATHIST_QUEUECOUNT ,latencyhits[i], cumul*100/totpkts);
        }
    }

    /*
     * If we were the server, in sweep mode, print our sweep-mode stats before quitting
     */
    fprintf(stderr, "## Printing sweep data:\n");
    if(!progsettings.clientmode && progsettings.sweepmode) {
        for(int i = 0; i < GRAPH_BWBUCKETS; i++) {
            if(bwdelaypoints[i].total_samples_for_this_bucket > BUCKET_CONTENT_THRESH) {
                fprintf(stderr, "%u %lf %ld %ld %ld %lu\n", \
                        GRAPHMAXBW*(i+1)/GRAPH_BWBUCKETS,\
                        bwdelaypoints[i].losspercent_cumul/(double)bwdelaypoints[i].total_samples_for_this_bucket, \
                        bwdelaypoints[i].min_delay,\
                        bwdelaypoints[i].max_delay,\
                        bwdelaypoints[i].avg_delay_cumul/(int64_t)bwdelaypoints[i].total_samples_for_this_bucket, \
                        bwdelaypoints[i].total_samples_for_this_bucket);
            }
        }
    }

    exit(1);
}

static int nsleep(uint64_t nsleep)
{
#if 1
    struct timespec ts;
    int res;

    if (nsleep < 0)
    {
        errno = EINVAL;
        return -1;
    }

    ts.tv_sec = nsleep / 1000000000;
    ts.tv_nsec = (nsleep % 1000000000);

    do {
        res = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, &ts);
    } while (res && errno == EINTR);

    return res;
#else
    usleep(nsleep/1000);
#endif
}

static int busywait_nsleep(uint64_t nsleep)
{
    struct timespec time1;
    uint64_t localtstamp, now;
    clock_gettime(CLOCK_MONOTONIC , &time1);
    localtstamp = time1.tv_sec * 1000000000 + time1.tv_nsec;

    while(1) {
        clock_gettime(CLOCK_MONOTONIC , &time1);
        now = time1.tv_sec * 1000000000 + time1.tv_nsec;
        if(now - localtstamp > nsleep)
            break;
    }
}


/*
 * Fill in the packet content
 */
static void prepPacket(bdt_pkt *pkt, int *outlen)
{
    static uint64_t pktcounter;
    struct timespec time1;
    clock_gettime(progsettings.nonsyncedclocks ? CLOCK_MONOTONIC : CLOCK_REALTIME  , &time1);

    pkt->ctr = pktcounter++;
    pkt->timestamp = time1.tv_sec * 1000000 + time1.tv_nsec/1000;
    *outlen = progsettings.packetsize;
}

/*
 * Parse the actual packet.
 * Attempt to find
 *  - min max avg latency of the packets per interval
 *  - bandwidth (data received)          per interval
 *  - drops                              per interval
 */
static void parsePacket(bdt_pkt *pkt, int len)
{
    static uint64_t rcvdpktcounter;
    static int64_t localoffset;
    int64_t currentdelay;

    static uint64_t lasttstamp_doneprint;
    static int64_t packets_last_intv;
    static uint64_t drops_last_intv;
    static uint64_t drops_consq_last_intv;
    static int64_t mindelay_last_intv;
    static int64_t maxdelay_last_intv;
    static int64_t avgdelay_last_intv;
    static uint64_t bytes_last_intv;

    int intv_ms = 100*1000; /* duration of the interval. For now hardcoded at 0.1s */

    uint64_t localtstamp;
    struct timespec time1;
    clock_gettime(progsettings.nonsyncedclocks ? CLOCK_MONOTONIC : CLOCK_REALTIME , &time1);
    localtstamp = time1.tv_sec * 1000000 + time1.tv_nsec/1000;

    if(localoffset == 0 && progsettings.nonsyncedclocks) {
        /* Never calculated any offset set. Do it with the first incoming packet (assume it has +- 0 latency) */
        localoffset =  pkt->timestamp - localtstamp + progsettings.compensationlatency;
    }

    currentdelay = (localtstamp + localoffset) - pkt->timestamp; /* Our time will have advance more in case of delay */

    if(currentdelay < mindelay_last_intv) {
        mindelay_last_intv = currentdelay;
    }
    if(currentdelay > maxdelay_last_intv) {
        maxdelay_last_intv = currentdelay;
    }

    avgdelay_last_intv += currentdelay; /* Do division upon print */

    int latencybucket = currentdelay*LATHIST_QUEUECOUNT/LATHIST_MAX_LATMS/1000;
    if(latencybucket >= LATHIST_QUEUECOUNT) latencybucket = LATHIST_QUEUECOUNT-1;
    latencyhits[latencybucket]++;

    if(rcvdpktcounter < pkt->ctr) {
        drops_last_intv += (pkt->ctr - rcvdpktcounter);
        drops_consq_last_intv++;
        rcvdpktcounter = pkt->ctr;
    } else if (rcvdpktcounter > pkt->ctr) {
        /* Handle drops: Do not take into account out-of-order packets (counted as drops), but do support remote restarting */
        if(rcvdpktcounter - pkt->ctr >  pkt->ctr || pkt->ctr == 0) {
            fprintf(stderr, "WARNING: Remote restarted. Restarting here too\n");
            rcvdpktcounter = pkt->ctr;

            localoffset = 0;
            /* don't clear all the last-interval values -- will have one wrong measurement, which is ok since remote restarted */
            rcvdpktcounter++; /* Expect a new packet next time though */
            return;
        }
    }
    rcvdpktcounter++; /* Increase for next packet */

    bytes_last_intv += len;

    /*
     * Print data after every interval
     */
    packets_last_intv++;
    if(localtstamp > lasttstamp_doneprint + intv_ms) {
        lasttstamp_doneprint = localtstamp;
        avgdelay_last_intv = avgdelay_last_intv / packets_last_intv;

        /* Print gnuplot-friendly output */
        printf("%lu %lu %lu %lu %ld %ld %ld\n", bytes_last_intv*(1000000/intv_ms), packets_last_intv*(1000000/intv_ms), drops_last_intv, drops_consq_last_intv, mindelay_last_intv, maxdelay_last_intv, avgdelay_last_intv );

        /*
         * Sweep mode:
         * Update bucket for BWdelay graph if needed
         */
        if(progsettings.sweepmode) {
            /* First estimate incoming total BW */
            uint64_t incomingmbps = (bytes_last_intv+drops_last_intv*len)*(1000000/intv_ms)*8;
            double losspercent = 0;
            if(bytes_last_intv+drops_last_intv*len > 0) {
                losspercent = ((double)drops_last_intv*len*100)/((double)bytes_last_intv+drops_last_intv*len);
            }
            /* Now figure out which bucket it belongs to */
            int idx = ((incomingmbps + GRAPH_BUCKETRES/2) * GRAPH_BWBUCKETS) / ((uint64_t)GRAPHMAXBW*1000*1000);
            if(idx < GRAPH_BWBUCKETS) {
                bwdelaypoints[idx].avg_delay_cumul += avgdelay_last_intv;
                bwdelaypoints[idx].losspercent_cumul += losspercent;
                if(mindelay_last_intv < bwdelaypoints[idx].min_delay) bwdelaypoints[idx].min_delay = mindelay_last_intv;
                if(maxdelay_last_intv > bwdelaypoints[idx].max_delay) bwdelaypoints[idx].max_delay = maxdelay_last_intv;
                bwdelaypoints[idx].total_samples_for_this_bucket++;
            }
        }

        packets_last_intv = 0;
        drops_last_intv = 0;
        drops_consq_last_intv = 0;
        maxdelay_last_intv = currentdelay;
        mindelay_last_intv = currentdelay;
        avgdelay_last_intv = 0;
        bytes_last_intv = 0;
        fflush(stdout);

    }
}

/*
 * If the sender is in sweep mode, gradually decrease the latency to increase the bandwidth
 */
static void applypacketdelayincreaseifneeded(uint64_t *currentdelay, uint64_t maxdelay)
{
    static struct timespec lastchange;
    static int tickamount = 1;
    static bool seenonce = false;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC , &now);
    if(!seenonce) {
        seenonce = true;
        lastchange = now;
        *currentdelay = (maxdelay * BWDELAYGRAPHTICKS) / tickamount;
    }
    if(now.tv_sec > lastchange.tv_sec + BWSWEEPTICKTIMESEC) {
        tickamount++;
        lastchange = now;
        if(tickamount > BWDELAYGRAPHTICKS) {
            printf("Sweep ends\n");
            exit(0);
        }
        *currentdelay = (maxdelay * BWDELAYGRAPHTICKS) / tickamount;
    }

}

/**
 * This function attempts to delay the next packet sending such that the target pps is reached on average
 * The function also attempts to minimize burst-size (to avoid buffer fillup)
 */
static void performInterPacketDelay(int32_t target_perpacketdelay_ns)
{
    /*
     * The easiest way to generate correctly-paced data is to check every time the timestamp, and see when to schedule the next send event
     * Note that this is not very CPU efficient, but for our desired speeds, it will suffice
     */

    static int64_t next_sendevent;
    struct timespec now;
    int64_t now_ns;
    int64_t nextdelay;

    clock_gettime(CLOCK_MONOTONIC , &now);
    now_ns = now.tv_nsec + (uint64_t)now.tv_sec*1000*1000*1000;

    if(!next_sendevent) {
        /* First time */
        next_sendevent = now_ns;
    }

    nextdelay = next_sendevent - now_ns;        /* Check howmuch time (if any) we still have to wait for this packet */
    next_sendevent += target_perpacketdelay_ns; /* Determine when the next packet should be sent */

    if(nextdelay <= 0) {
        /* We already passed the moment we needed to send it. Send now */
        return;
    } else {
        busywait_nsleep(nextdelay); /* Note: This might in practise wait too long. But then we'll catch up later */
    }
}

/**
 * ===============
 * Client mode
 * ===============
 */
static void runClient()
{
    struct sockaddr_in si_other;
    int s, i, slen=sizeof(si_other);
    int ret;
    char buf[BUFLEN];
    int buflen;
    bdt_pkt *pkt_p = (bdt_pkt*)buf;
    uint64_t currentdelay_ns;
    uint64_t maxdelay_ns;

    if ( (s=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    {
        die("socket");
    }

    memset((char *) &si_other, 0, sizeof(si_other));
    si_other.sin_family = AF_INET;
    si_other.sin_port = htons(PORT);

    if (inet_aton(progsettings.dsthost , &si_other.sin_addr) == 0)
    {
        fprintf(stderr, "inet_aton() failed\n");
        exit(1);
    }

    /*
     * OPTIONAL
     * Set nonblocking mode
     * This allows the sendto call to return immediately.
     *
     * This is useful in case linux provides actual backpressure from the interface side to the application,
     *  disallowing it to send too much traffic over an interface.
     * With non-blocking mode, you can generate more traffic than the interface allows, but this does not
     *  neccesairly provide a better throughput. Optimizing the qdisc hiearchy is often a better solution.
     */
    bool blocking = !progsettings.nonblockingmode;
    int flags = fcntl(s, F_GETFL, 0);
    flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    if(fcntl(s, F_SETFL, flags) != 0) {
        fprintf(stderr, "Failed to set blocking state\n");
        exit(1);
    }

    /*
     * OPTIONAL
     * Set bind to interface if specified
     */
    if(progsettings.sourceifbind) {
        if(setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, progsettings.sourceifbind, strlen(progsettings.sourceifbind)+1)) {
            fprintf(stderr, "Failed to bind to interface\n");
            exit(1);
        }
    }

    /*
     * OPTIONAL
     * Send the first two packets slowly, then start the actual test
     * This is only relevant in non-timesynced mode
     * The idea is that the first packets have +- ideal latency, and this allows a form of clock sync between sender/receiver
     *  onto which future latencies can be calculated.
     * Use PTP or something to sync the sender/receiver to avoid this.
     */
    if(progsettings.nonsyncedclocks) {
        for(int i = 0; i < 2; i++) {
            prepPacket(pkt_p, &buflen);
            if ((ret = sendto(s, buf, buflen , 0 , (struct sockaddr *) &si_other, slen))==-1)
            {
                if(errno != EWOULDBLOCK) {
                    die("sendto()");
                } else {
                    /* The non-blocking queue is currently full. Unexpected at this stage */
                }
            }
            nsleep((uint64_t)100*1000*1000);
        }
    }

    /*
     * Set the interpacket delay. If going to sweep, also remember the maximum.
     */
    if(progsettings.targetbwmbps) {
        currentdelay_ns = (int64_t)1000*1000*1000/(progsettings.targetbwmbps*1000*1000/progsettings.packetsize/8);
    } else {
        currentdelay_ns = progsettings.nsdelay;
    }
    maxdelay_ns = currentdelay_ns; /* In sweep mode, we vary currentdelay over time from 0 till max */

    /*
     * Start actual test
     * Note that the client is pretty basic. It just has to fill in the packets correctly, and send them at the appropriate time
     */
    while(1)
    {
        /* Prep packet contents */
        prepPacket(pkt_p, &buflen);

        /* Send the message (blocking/nonblocking depending on the mode) */
        if (sendto(s, buf, buflen , 0 , (struct sockaddr *) &si_other, slen)==-1)
        {
            if(errno != EWOULDBLOCK) {
                die("sendto()");
            } else {
                /*
                 * The non-blocking queue is currently full (only possible in non-blocking mode)
                 * We could now immediately retry the send in a loop, but that's basically the same as doing the blocking call
                 * Instead, we will just ignore this, which will trigger a drop at the receiver end.
                 */
            }
        }

        /* Wait an adequate time for the next packet, thereby achieving the target bandwidth */
        if(progsettings.sweepmode) applypacketdelayincreaseifneeded(&currentdelay_ns, maxdelay_ns);
        performInterPacketDelay(currentdelay_ns);
    }

    close(s);
}

/*
 * ===============
 * Server mode
 * ===============
 */
static void runServer()
{
    struct sockaddr_in si_me, si_other;
    int s, i, slen = sizeof(si_other) , recv_len;
    char buf[BUFLEN];
    bdt_pkt *pkt_p = (bdt_pkt*)buf;
    bool firstpktseen = false;

    /*create a UDP socket*/
    if ((s=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    {
        die("socket");
    }
    memset((char *) &si_me, 0, sizeof(si_me));

    si_me.sin_family = AF_INET;
    si_me.sin_port = htons(PORT);
    si_me.sin_addr.s_addr = htonl(INADDR_ANY);

    /*bind socket to port*/
    if( bind(s , (struct sockaddr*)&si_me, sizeof(si_me) ) == -1)
    {
        die("bind");
    }

    /*
     * OPTIONAL
     * Set bind to interface if specified
     */
    if(progsettings.sourceifbind) {
        if(setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, progsettings.sourceifbind, strlen(progsettings.sourceifbind)+1)) {
            fprintf(stderr, "Failed to bind to interface\n");
            exit(1);
        }
    }

    printf("# bytes_last_intv packets_last_intv drops_last_intv drops_consq_last_intv mindelayus_last_intv maxdelayus_last_intv avgdelayus_last_intv \n");

    while(1)
    {

        /* Do a blocking receive. */
        if ((recv_len = recvfrom(s, buf, BUFLEN, 0, (struct sockaddr *) &si_other, &slen)) == -1)
        {
            die("recvfrom()");
        }

        /* Print something that we saw an incoming connection the first time */
        if(!firstpktseen) {
            fprintf(stderr, "First packet seen from %s:%d\n", inet_ntoa(si_other.sin_addr), ntohs(si_other.sin_port));
            firstpktseen = true;
        }

        /* Parse the packet, and extract relevant data from it */
        parsePacket(pkt_p, recv_len);

    }

    close(s);
}


static void print_usage_and_exit() {
    printf("\n");
    printf("Bandwidth-delay tester. \n");
    printf("\n");
    printf(" Send IPv4 UDP packets containing timestamping information from sender to receiver, to determine received bandwidth and latency\n");
    printf("\t\n");
    printf("\tClient Usage: %s -c <dstip> -p <packet size> [-d <interpacket_delay_ns> | -b <bandwidth_mbps>] [-s (sweepmode)] [-n (nonblocking mode)] [-i sourceinterfacebind] [-l <compensationlatencyms] [-a (async)]\n",  progsettings.prgname);
    printf("\tServer Usage: %s  [-i sourceinterfacebind] [-s (sweepmode)] [-l <compensationlatencyms] [-a (async)]\n",  progsettings.prgname);
    printf("\t \n");
    printf("\t \n");
    printf("\t -The targetbandwidth can be supplied either with -d or -b\n");
    printf("\t \n");
    printf("\t -Async mode is only needed if sender and receiver clocks (CLOCK_REALTIME) are not synced (ideally < 0.1ms), e.g. through PTP\n");
    printf("\t\t In this mode, the absolute latency measurement will only be an estimate, and can be furthered tuned with -l\n");
    printf("\t \n");
    printf("\t -Sweep mode at the client allows it to sweep the bandwidth from 0Mbps till the filled in amount.\n");
    printf("\t -Sweep mode at the server will, upon exit, print some statistics per speed range (for histogram usage).\n");
    printf("\t \n");
    printf("\t -Optionally the target interface can be bound to using -i\n");
    printf("\t -Optionally the sender socket can be placed in non-blocking mode. (not necessairly useful)\n");
    printf("\t \n");
    printf("\t -The UDP port is hardcoded to " xstr(PORT) "\n");
    printf("\t \n");
    printf("\t Example client and server to send 1kB packets from 192.168.1.2 to 192.168.1.1, devices being time synced: \n");
    printf("\t \n");
    printf("\t\t CLIENT:  %s -c 192.168.1.1 -p 1000 -b 1000 \n",  progsettings.prgname);
    printf("\t\t SERVER:  %s  \n", progsettings.prgname);
    printf("\t \n");
    exit(EXIT_FAILURE);
}

static void post_parse_argscheck()
{
    /* Check that we have a consistent config */
    if(progsettings.clientmode) {
        if(progsettings.packetsize > BUFLEN || progsettings.packetsize < sizeof(bdt_pkt)) {
            if(progsettings.packetsize != 0)
                printf("Unsupported packet size\n");
            print_usage_and_exit();
            exit(EXIT_FAILURE);
        }

        if((progsettings.nsdelay && progsettings.targetbwmbps) || (!progsettings.nsdelay && !progsettings.targetbwmbps)) {
            /* Mutually exclusive */
            printf("Supply either delay or target bandwidth, but not both\n");
            print_usage_and_exit();
            exit(EXIT_FAILURE);
        }
    }
    return;
}

/*
 * ***********************************************************************************************************************************************
 * Public Functions
 * ***********************************************************************************************************************************************
 */

/*
 * Main function.
 * The application can run in both client and server mode.
 */
int main(int argc, char *argv[])
{
    int option = 0;
    signal(SIGINT,sig_handler);

    /*
     * Parse options. Sanity check is done at the end
     */
    progsettings.prgname = argv[0];
    while ((option = getopt(argc, argv,"c:d:p:l:ni:sab:")) != -1) {
        switch (option) {
        case 'c':
            progsettings.clientmode = true;
            progsettings.dsthost = optarg;
            break;
        case 'd':
            progsettings.nsdelay = atoi(optarg);
            break;
        case 'b':
            progsettings.targetbwmbps = atoi(optarg);
            break;
        case 'p':
            progsettings.packetsize = atoi(optarg);
            break;
        case 'l':
            progsettings.compensationlatency = atoi(optarg);
            break;
        case 'n':
            progsettings.nonblockingmode = true;
            break;
        case 'i':
            progsettings.sourceifbind = optarg;
            break;
        case 's':
            progsettings.sweepmode = true;
            break;
        case 'a':
            progsettings.nonsyncedclocks = true;
            break;
        default:
            print_usage_and_exit();
            exit(EXIT_FAILURE);
            break;
        }
    }
    /* Sanity check */
    post_parse_argscheck();

    /*
     * Start server/client mode
     */
    if(progsettings.clientmode) {
        runClient();
    } else {
        runServer();
    }

    return 0;
}

/* End of file bwdelaytester.c */
