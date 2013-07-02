#include <sys/ioctl.h>
#include <stdlib.h>
#include <stdio.h>
#include <iostream>
#include <fstream>
#include <fcntl.h>
#include <stdint.h>
#include <cstring>
#include <sys/time.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <cerrno>
#include <sys/types.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <assert.h>

#include <mutex>
#include <condition_variable>

#include "../dragon-module/dragon.h"

#define DRAGON_DEV_FILENAME "/dev/dragon0"

#define DRAGON_BUFFER_COUNT 4

#define LOOPS_COUNT 100

// get system time in milliseconds
unsigned long GetTickCount()
{
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (tv.tv_sec*1000+tv.tv_usec/1000);
}


#define TCPIP_PORT 32120
#define TCPIP_MAX_CLIENTS 1

#define PCIE_PULSE_WIDTH 1 // (127 max)

#define OUTPUT_BUFFER_TYPE uint32_t //it could be any type, just big enough to store output
#define OUTPUT_BUFFER_SIZE_BYTES (DRAGON_MAX_FRAME_LENGTH*sizeof(OUTPUT_BUFFER_TYPE)) //size of output buffer in bytes

OUTPUT_BUFFER_TYPE *Output_Read; //this is the real output data to be sent to client!
volatile bool NewDataReady=false; //this flag is set by PCIE thread when new output data is ready
unsigned int CollectedFramesCount = 0;
bool LastOutputDataChannel; //remembers which channel data is in output


#define DEFAULT_FRAME_COUNT 600
#define DEFAULT_DAC_DATA 0xFFFFFFFF //each byte for one of four 8-bit DACs

uint16_t FrameLength = DRAGON_MAX_FRAME_LENGTH; //how many points in one input (and ouput) frame we have
uint32_t FrameCount = DEFAULT_FRAME_COUNT; //how many frames we want to sum to get output data
uint32_t PcieDacData = 0xFFFFFFFF; //DAC data to be set on PCIE device
bool ActiveChannel = 0;
//same data, but received from client - to be set in appropriate time
uint16_t FrameLengthToSet = FrameLength;
uint32_t FrameCountToSet = DEFAULT_FRAME_COUNT;
uint32_t PcieDacDataToSet = DEFAULT_DAC_DATA;
bool ActiveChannelToSet = 0;

std::mutex gWaitMutex;
std::condition_variable gWaitCondition;

// thread for TCPIP exchange with client
void* SocketThread(void* ptr)
{
    int Sock_RetValue;
    unsigned int Sock_BytesWritten;
    int ClientSocket, ServerSocket;
    int r=-1;

    //start server
    struct sockaddr_in ServerAddress;
    memset(&ServerAddress, 0, sizeof(ServerAddress));
    ServerAddress.sin_family=AF_INET;
    ServerAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    ServerAddress.sin_port=htons(TCPIP_PORT);
    ServerSocket = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ServerSocket, SOL_SOCKET, SO_REUSEADDR, &r, sizeof(r));
    if(bind(ServerSocket, (struct sockaddr *) &ServerAddress, sizeof(ServerAddress))<0)
    {
        puts("cant bind port");
        return NULL;
    }
    listen(ServerSocket, TCPIP_MAX_CLIENTS);

    //accept client - read-write - disconnect
    while(1)
    {
        ClientSocket = accept(ServerSocket, NULL, NULL);
        puts("Client connected.\n");

        while(1)
        {
            std::unique_lock<std::mutex> lock(gWaitMutex);

            //wait for new data from PCIE thread
            while(!NewDataReady) gWaitCondition.wait(lock);
            NewDataReady=false;

            Sock_RetValue=send(ClientSocket, &LastOutputDataChannel, 1, MSG_NOSIGNAL);
            if(Sock_RetValue<1) break;
            Sock_RetValue=send(ClientSocket, &FrameLength, 2, MSG_NOSIGNAL);
            if(Sock_RetValue<2) break;
            Sock_RetValue=send(ClientSocket, &CollectedFramesCount, 4, MSG_NOSIGNAL);
            if(Sock_RetValue<4) break;
            Sock_RetValue=send(ClientSocket, &PcieDacData, 4, MSG_NOSIGNAL);
            if(Sock_RetValue<4) break;

            //send active channel
            Sock_BytesWritten=0;
            while(Sock_BytesWritten<OUTPUT_BUFFER_SIZE_BYTES)
            {
                Sock_RetValue=send(ClientSocket, Output_Read+Sock_BytesWritten, OUTPUT_BUFFER_SIZE_BYTES-Sock_BytesWritten, MSG_NOSIGNAL);
                if(Sock_RetValue<=0) break;
                else Sock_BytesWritten+=Sock_RetValue;
                printf("%d of %ld sent\n", Sock_BytesWritten, OUTPUT_BUFFER_SIZE_BYTES);
            }
            if(Sock_RetValue<=0) break;


            //read data from client
            Sock_RetValue=read(ClientSocket, &ActiveChannelToSet, 1);
            if(Sock_RetValue<1) break;
            Sock_RetValue=read(ClientSocket, &FrameLengthToSet, 2);
            if(Sock_RetValue<2) break;
            Sock_RetValue=read(ClientSocket, &FrameCountToSet, 4);
            if(Sock_RetValue<4) break;
            Sock_RetValue=read(ClientSocket, &PcieDacDataToSet, 4);
            if(Sock_RetValue<4) break;
        }

        puts("Client disconnected\n");
        if(ClientSocket>0) close(ClientSocket);
    }

    return NULL;
}


static int GetSwitcherState(const dragon_buffer& buf, unsigned char *user_bufs[])
{
    return ((user_bufs[buf.idx][1] >> 5) & 1);
}

static int GetCurrentChannel(const dragon_buffer& buf, unsigned char *user_bufs[])
{
    return ((user_bufs[buf.idx][1] >> 7) & 1);
}


volatile bool ExitFlag=false;

void ExitHandler(int s)
{
    printf("Caught SIGINT\n");
    ExitFlag=true;
}

//main function, here main PCIE thread runs
int main(int argc, char** argv)
{
    int DragonDevHandle; //handle for opening PCIE device file
    dragon_buffer buf;
    size_t buf_count = DRAGON_BUFFER_COUNT;
    unsigned char* user_bufs[DRAGON_BUFFER_COUNT];
    unsigned long dtStart, dtEnd;
    unsigned int i, j, k, m, n;
    uint8_t *tmp_buf;
    int switcherState = -1;

    unsigned long FrameCounter=0; //count input frames; when it reaches FrameCount - new output data is ready

    bool Output_ChannelReadSelector=false; //used to switch output buffers (double-buffering)
    OUTPUT_BUFFER_TYPE *Output[2]; //double output buffers
    OUTPUT_BUFFER_TYPE *Output_Write; //pointers to part of output buffer to be filled in PCIE thread

    //start TCPIP server thread
    pthread_t SocketThreadHandle;
    //pthread_create(&SocketThreadHandle, NULL, SocketThread, NULL);

    //allocate memory for output buffers, assign pointers to parts of double-buffers
    Output[0] = (OUTPUT_BUFFER_TYPE*)malloc(OUTPUT_BUFFER_SIZE_BYTES);
    Output[1] = (OUTPUT_BUFFER_TYPE*)malloc(OUTPUT_BUFFER_SIZE_BYTES);
    memset(Output[0], 0, OUTPUT_BUFFER_SIZE_BYTES);
    memset(Output[1], 0, OUTPUT_BUFFER_SIZE_BYTES);
    Output_Read=Output[Output_ChannelReadSelector];
    Output_Write=Output[!Output_ChannelReadSelector];

    //signal(SIGINT, ExitHandler);

    //open PCIE device
    DragonDevHandle = open(DRAGON_DEV_FILENAME, O_RDWR);

    if(DragonDevHandle<0)
    {
        puts("Failed to open PCIE device!\n");
        return -1;
    }

    ioctl(DragonDevHandle, DRAGON_SET_ACTIVITY, 0);

    uint32_t dID=0;
    ioctl(DragonDevHandle, DRAGON_GET_ID, &dID);
    printf("Dragon ID: %u\n", dID);

    dragon_params p;
    ioctl(DragonDevHandle, DRAGON_QUERY_PARAMS, &p);
    //p.adc_type=0; // 0 for 8-bit, 1 for 12-bit
    //p.board_type=0; // 0 for red KNJN, 1 for new green
    p.channel=ActiveChannel;
    p.channel_auto=0;
    p.frames_per_buffer=(DRAGON_MAX_DATA_IN_BUFFER/FrameLength);
    p.frame_length=FrameLength;
    p.half_shift=0;
    //p.switch_period=FrameCount;
    p.pulse_mask=0x80000001;
    p.sync_offset=16;
    p.sync_width=1;
    p.dac_data=PcieDacData;
    ioctl(DragonDevHandle, DRAGON_SET_PARAMS, &p);

    printf("frames per buffer: %d\n", p.frames_per_buffer);

    if (!ioctl(DragonDevHandle, DRAGON_REQUEST_BUFFERS, &buf_count) )
    {
        printf("buf_count = %ld\n", buf_count);
    }
    else
    {
        printf("DRAGON_REQUEST_BUFFERS error\n");
        return -1;
    }

    ioctl(DragonDevHandle, DRAGON_SET_ACTIVITY, 1);

    for (i = 0; i < buf_count; ++i)
    {
        buf.idx = i;

        if (ioctl(DragonDevHandle, DRAGON_QUERY_BUFFER, &buf) )
        {
            printf("DRAGON_QUERY_BUFFER %d error\n", i);
            return -1;
        }

        user_bufs[i] = (unsigned char*)mmap(NULL, buf.len,
                                            PROT_READ | PROT_WRITE,
                                            MAP_SHARED, DragonDevHandle,
                                            buf.offset);

        if (!user_bufs[i])
        {
            printf("mmap buffer %d error\n", i);
            return -1;
        }


        if (ioctl(DragonDevHandle, DRAGON_QBUF, &buf) )
        {
            printf("DRAGON_QBUF error\n");
            return -1;
        }
    }

 
  
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(DragonDevHandle, &fds);

    dtStart = GetTickCount();
    int count = 0;

    for (;!ExitFlag;)
    {
        select(DragonDevHandle + 1, &fds, NULL, NULL, NULL);

        //Dequeue buffer

        if (ioctl(DragonDevHandle, DRAGON_DQBUF, &buf) )
        {
            printf("DRAGON_DQBUF error\n");
            continue;
        }

        tmp_buf=user_bufs[buf.idx];
        memset(tmp_buf, 0, buf.len);
        /*
        if (switcherState == -1)
        {
            switcherState = GetSwitcherState(buf, user_bufs);
        }
       
        int switcherStateCurrent = GetSwitcherState(buf, user_bufs);
        
        if (switcherState != switcherStateCurrent)
        {
            std::unique_lock<std::mutex> lock(gWaitMutex);
            printf("Polarization collected frame count = %ld\n", FrameCounter);
            CollectedFramesCount = FrameCounter;
            FrameCounter=0;
            switcherState = switcherStateCurrent;


            Output_ChannelReadSelector=!Output_ChannelReadSelector;
            Output_Read=Output[Output_ChannelReadSelector];
            Output_Write=Output[!Output_ChannelReadSelector];
            LastOutputDataChannel = GetCurrentChannel(buf, user_bufs);
            NewDataReady=true;         // indicate TCPIP thread that new data is ready

            //clear write-buffer
            memset(Output_Write, 0, OUTPUT_BUFFER_SIZE_BYTES);
            if (    FrameLength  !=  FrameLengthToSet ||
                    FrameCount   !=  FrameCountToSet  ||
                    PcieDacData  !=  PcieDacDataToSet ||
                    ActiveChannel != ActiveChannelToSet
                    )
            {
                FrameLength  =  FrameLengthToSet;
                FrameCount   =  FrameCountToSet;
                PcieDacData  =  PcieDacDataToSet;
                ActiveChannel = ActiveChannelToSet;

                ioctl(DragonDevHandle, DRAGON_SET_ACTIVITY, 0);

                p.frames_per_buffer=(DRAGON_MAX_DATA_IN_BUFFER/FrameLength);
                p.frame_length=FrameLength;
                p.switch_period=FrameCount;
                p.dac_data=PcieDacData;
                p.channel=ActiveChannel;
                printf("Active Channel: %d\n", ActiveChannel);

                ioctl(DragonDevHandle, DRAGON_SET_PARAMS, &p);

                ioctl(DragonDevHandle, DRAGON_SET_ACTIVITY, 1);
            }

            gWaitCondition.notify_one();
        }


        for(i=0; i<p.frames_per_buffer; i++)
        {
            m=0;
            for(j=0; j<p.frame_length/DRAGON_DATA_PER_PACKET; j++)
            {
                n=i*(p.frame_length*DRAGON_PACKET_SIZE_BYTES/DRAGON_DATA_PER_PACKET)+j*DRAGON_PACKET_SIZE_BYTES+4;
                if(p.adc_type==0) //8-bit
                    for(k=4; k<DRAGON_PACKET_SIZE_BYTES-4; k++)
                        Output_Write[m++] += tmp_buf[n++];
                else if(p.adc_type==1) //12-bit
                {
                    for(k=4; k<DRAGON_PACKET_SIZE_BYTES-4; k+=8)
                    {
                        // aa-ab-bb-cc-cd-dd-ee-e_
                        Output_Write[m++] += (tmp_buf[n]<<4)|(tmp_buf[n+1]>>4);
                        Output_Write[m++] += ((15&tmp_buf[n+1])<<8)|tmp_buf[n+2];
                        Output_Write[m++] += (tmp_buf[n+3]<<4)|(tmp_buf[n+4]>>4);
                        Output_Write[m++] += ((15&tmp_buf[n+4])<<8)|(tmp_buf[n+5]);
                        Output_Write[m++] += (tmp_buf[n+6]<<4)|(tmp_buf[n+7]>>4);
                        n+=8;
                    }
                }
            }
        }


        FrameCounter+=p.frames_per_buffer;
        */

        //Queue buffer
        if (ioctl(DragonDevHandle, DRAGON_QBUF, &buf) )
        {
            printf("DRAGON_QBUF error\n");
            return -1;
        }

        ++count;
        if (count % LOOPS_COUNT == 0)
        {
            dtEnd = GetTickCount();
            double  FPS = 1000*LOOPS_COUNT / (double)(dtEnd - dtStart);
            count = 0;
            dtStart = dtEnd;
            printf("FPS = %lf\n", FPS);
        }
    }

    close(DragonDevHandle);
    printf("Dragon closed\n");

    return 0;
}

