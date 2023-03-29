#include "usrp.h"

#include "common.h"
#include "main.h"
#include "hostfile.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <cmath>

#include <mutex>

struct CUSRPData
{
    uint32_t SequenceNum = 0;
    USRPHeader Header = {};                                 // TODO: Buffer...?

    USRPHeader HeaderHistory[8];
    int HeaderHistoryIdx = 0;

    mutable std::mutex Mutex;

    USRPHeader GetHeader() const
    {
        return Header;
    }
};

struct CUSRPAudio
{
    bool IsKeyed = false;
    uint32_t SequenceNum = 0;
    short AudioBuffer[USRP_VOICE_BUFFER_FRAME_SIZE];        // 8kHz 16-bit signed
    int AudioBufferWriteOff = 0;
    int AudioBufferReadOff = 0;
    int AudioFrames = 0;
   
    int Read(short* FrameData, size_t BytesSize)
    {
        const int READ_SIZE = USRP_VOICE_FRAME_SIZE * sizeof(short); 
        assert(BytesSize >= READ_SIZE);
        const int FRAMES_SIZE = BytesSize / sizeof(short);

        if( AudioFrames >= FRAMES_SIZE )
        {
            memcpy(FrameData, &AudioBuffer[AudioBufferReadOff], BytesSize);
            AudioBufferReadOff = (AudioBufferReadOff + FRAMES_SIZE) % USRP_VOICE_BUFFER_FRAME_SIZE;
            AudioFrames -= BytesSize / sizeof(short);
            return BytesSize;
        }
        return 0;
    }

    int Write(const short* FrameData, size_t BytesSize)
    {
        const int FRAMES_SIZE = BytesSize / sizeof(short);
        if( AudioFrames < USRP_VOICE_BUFFER_FRAME_SIZE * sizeof(short) )
        {
            memcpy(&AudioBuffer[AudioBufferWriteOff], FrameData, BytesSize);
            AudioBufferWriteOff = (AudioBufferWriteOff + FRAMES_SIZE) % USRP_VOICE_BUFFER_FRAME_SIZE;
            AudioFrames += BytesSize / sizeof(short);
            return BytesSize;
        }
        return 0;
    }
};
 

static USRPHeader CreateUSRPVoiceHeader(bool Ptt, uint32_t Talkgroup, uint32_t SequenceNum )
{
    USRPHeader Header = {};
    Header.Usrp[0] = 'U';
    Header.Usrp[1] = 'S';
    Header.Usrp[2] = 'R';
    Header.Usrp[3] = 'P';
    Header.SequenceNum = SequenceNum;
    Header.Memory = 0;
    Header.Ptt = Ptt ? 1 : 0;
    Header.Talkgroup = Talkgroup;
    Header.Type = USRP_TYPE_VOICE;
    Header.Mpxid = 0;
    Header.Reserved = 0;
    return Header;
}


static USRPHeader USRPHeaderToNetworkByteOrder(USRPHeader Header)
{
    Header.SequenceNum = htonl(Header.SequenceNum);
    Header.Ptt = htonl(Header.Ptt);
    return Header;
}


static USRPHeader USRPHeaderToHostByteOrder(USRPHeader Header)
{
    Header.SequenceNum = ntohl(Header.SequenceNum);
    Header.Ptt = ntohl(Header.Ptt);
    return Header;
}



CUSRP::CUSRP()
{
    InData = new CUSRPData;
    OutAudio = new CUSRPAudio;
}


CUSRP::~CUSRP()
{
    delete InData;
    delete OutAudio;
}


int CUSRP::Init(const char *NodeName, char *AudioDevice, ClientInfo *pAudioC)
{
    int Ret = 0;

    char AddressBuf[256];
    PortIn = 0;
    PortOut = 0;

    AudioC = pAudioC;

    // hack better parsing
    char *PortBegin = strstr(AudioDevice, ":");
    *PortBegin++ = '\0';
    sscanf(AudioDevice, "USRP/%s", AddressBuf);
    sscanf(PortBegin, "%u:%u", &PortOut, &PortIn);

    // todo error handling.
    LOG_NORM(("%s#%d: USRP \"%s:%u, output %u\"\n",__FUNCTION__,__LINE__,AddressBuf, PortIn, PortOut));

    // Create & open a named pipe.
    char PipeName[256];
    sprintf(PipeName, "/tmp/usrp_pipe_%s_%u_%u", NodeName, PortIn, PortOut);
    mkfifo(PipeName, O_RDWR);
    chmod(PipeName, 0666);

    if((pAudioC->Socket = open(PipeName,0666)) < 0) {
        LOG_ERROR(("%s#%d: open(\"%s\") failed: %s",__FUNCTION__,__LINE__,PipeName,
                        Err2String(errno)));
        Ret = ERR_AUDIO_DEV_OPEN;
    }


    // In
    if ((InSock = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        perror("Socket");
        return ERR_USRP_FAIL;
    }

    InAddr.i.sin_family = AF_INET;
    InAddr.i.sin_port = htons(PortIn);
    InAddr.i.sin_addr.s_addr = INADDR_ANY;
    bzero(&(InAddr.i.sin_zero),8);

    struct timeval InReadTimeout;
    InReadTimeout.tv_sec = 0;
    InReadTimeout.tv_usec = 10;

    if (bind(InSock, &InAddr.s, sizeof(IPAdrUnion)) == -1)
    {
        perror("Bind");
        return ERR_USRP_FAIL;
    }

    // Out
    struct hostent *OutHost;
    OutHost = GetHostByName(AddressBuf);

    if ((OutSock = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
    {
        perror("socket");
        return ERR_USRP_FAIL;
    }

    OutAddr.i.sin_family = AF_INET;
    OutAddr.i.sin_port = htons(PortOut);
    OutAddr.i.sin_addr = *((struct in_addr *)OutHost->h_addr);
    bzero(&(OutAddr.i.sin_zero),8);

    // Receive thread.
    if(pthread_create( &RecvThread, NULL, &CUSRP::StaticRecvMain, this ) < 0) {
        perror("socket");
        Ret = ERR_USRP_FAIL; // todo better error;
    }

    return Ret;
}


bool CUSRP::PollCOS()
{
    return !!InData->GetHeader().Ptt;
}


void CUSRP::KeyTx(int bKey)
{
    if( OutAudio->IsKeyed != bKey )
    {
        char OutSendData[1024];
        USRPHeader *UsrpHeader = reinterpret_cast<USRPHeader *>( OutSendData );
        USRPMetaData* UsrpMetaData = reinterpret_cast<USRPMetaData *>( UsrpHeader + 1 );

        OutAudio->IsKeyed = bKey;

        if( bKey )
        {
            UsrpHeader->Usrp[0] = 'U';
            UsrpHeader->Usrp[1] = 'S';
            UsrpHeader->Usrp[2] = 'R';
            UsrpHeader->Usrp[3] = 'P';
            UsrpHeader->SequenceNum = OutAudio->SequenceNum++;
            UsrpHeader->Memory = 0;
            UsrpHeader->Ptt = 1;
            UsrpHeader->Talkgroup = 0;
            UsrpHeader->Type = USRP_TYPE_TEXT;
            UsrpHeader->Mpxid = 0;
            UsrpHeader->Reserved = 0;

            UsrpMetaData->TLVTag = USRP_TLV_TAG_SET_INFO;
            UsrpMetaData->TLVLength = 13 + strlen("VK2GPU");
            strcpy( UsrpMetaData->Callsign, "VK2GPU");
            UsrpMetaData->DmrID = htonl(5052317) >> 8;
            UsrpMetaData->RepeaterID = 0;
            UsrpMetaData->Talkgroup = 0;
            UsrpMetaData->Timeslot = 0;
            UsrpMetaData->ColorCode = 0;

            *UsrpHeader = USRPHeaderToNetworkByteOrder( *UsrpHeader );

            ::sendto(OutSock, 
                OutSendData, sizeof(USRPHeader) + UsrpMetaData->TLVLength,
                0, &OutAddr.s, sizeof(USRPIPAdrUnion));
        }
        else
        {
            UsrpHeader->Usrp[0] = 'U';
            UsrpHeader->Usrp[1] = 'S';
            UsrpHeader->Usrp[2] = 'R';
            UsrpHeader->Usrp[3] = 'P';
            UsrpHeader->SequenceNum = OutAudio->SequenceNum++;
            UsrpHeader->Memory = 0;
            UsrpHeader->Ptt = 0;
            UsrpHeader->Talkgroup = 0;
            UsrpHeader->Type = USRP_TYPE_VOICE;
            UsrpHeader->Mpxid = 0;
            UsrpHeader->Reserved = 0;

            *UsrpHeader = USRPHeaderToNetworkByteOrder( *UsrpHeader );

            ::sendto(OutSock, 
                OutSendData, sizeof(USRPHeader),
                0, &OutAddr.s, sizeof(USRPIPAdrUnion));
        }
    }
}


int CUSRP::Read(short *OutData, int MaxRead)
{
    const int READ_SIZE = USRP_VOICE_FRAME_SIZE * sizeof(short); 
    assert(MaxRead >= READ_SIZE);

    // Check if we have data ready to read with immediate timeout.
    fd_set ReadFds;
    FD_ZERO(&ReadFds);
    FD_SET(AudioC->Socket, &ReadFds);

    timeval Timeval { 0L, 0L };
    int Ret = ::select(AudioC->Socket + 1, &ReadFds, NULL, NULL, &Timeval);
    if (Ret <= 0) {
        return 0;
    }

    return ::read(AudioC->Socket, OutData, MaxRead);
}


int CUSRP::Write(const short *FrameData, int SizeBytes)
{
    char OutSendData[1024];
    USRPHeader *UsrpHeader = reinterpret_cast<USRPHeader *>( OutSendData );

    if( OutAudio->IsKeyed )
    {
        int BytesSent = 0;
        OutAudio->Write(FrameData, SizeBytes);
        while( OutAudio->AudioFrames >= USRP_VOICE_FRAME_SIZE )
        {
            *UsrpHeader = CreateUSRPVoiceHeader( OutAudio->IsKeyed, 0, OutAudio->SequenceNum++ );
            *UsrpHeader = USRPHeaderToNetworkByteOrder( *UsrpHeader );
            
            OutAudio->Read(reinterpret_cast<short*>(UsrpHeader + 1), USRP_VOICE_FRAME_SIZE * 2);

            ::sendto(OutSock,
                OutSendData, sizeof(USRPHeader) + USRP_VOICE_FRAME_SIZE * 2,
                0, &OutAddr.s, sizeof(USRPIPAdrUnion));

            BytesSent += USRP_VOICE_FRAME_SIZE * 2;
        }
        return BytesSent;
    }
    return SizeBytes;
}


//static
void* CUSRP::StaticRecvMain( void* pParam )
{
    return ((CUSRP*)pParam)->RecvMain();
}


void* CUSRP::RecvMain()
{
    int InBytesRead;
    char InRecvData[1024];
    USRPHeader *UsrpHeader = reinterpret_cast<USRPHeader *>( InRecvData );
    short *UsrpVoiceFrame = reinterpret_cast<short *>(UsrpHeader + 1);

    do {
        // Check if we have data ready to read with timeout of a single audio frame (20ms)
        fd_set ReadFds;
        FD_ZERO(&ReadFds);
        FD_SET(InSock, &ReadFds);

        timeval Timeval { 0L, 20000L };
        if( InData->Header.Ptt )
        {
            // Bump to 500ms timeout.
            Timeval.tv_usec = 500000L;
        }

        int Ret = ::select(InSock + 1, &ReadFds, NULL, NULL, &Timeval);
        if (Ret <= 0) {
            const size_t CopySize = USRP_VOICE_FRAME_SIZE * sizeof(short);
            memset(UsrpVoiceFrame, 0, CopySize);
            ::write(AudioC->Socket, UsrpVoiceFrame, CopySize);
            InBytesRead = CopySize;
            continue;
        }
        else
        {
            // Read data.
            InBytesRead = ::recvfrom(InSock, InRecvData, 1024, 0,
                            (struct sockaddr *)&InAddr, &InAddrLen);
        }

        // Process data.
        std::lock_guard<std::mutex> Lock(InData->Mutex);
        *UsrpHeader = USRPHeaderToHostByteOrder( *UsrpHeader );

        // Since it's coming over UDP, we expect at least 32 bytes.
        if(InBytesRead >= 32) {
            if(memcmp(UsrpHeader->Usrp, "USRP", 4) != 0) {
                LOG_NORM(("%s#%d: USRP Packet invalid, size %i\n",__FUNCTION__,__LINE__, InBytesRead));
                continue;
            }

            InData->HeaderHistory[InData->HeaderHistoryIdx] = *UsrpHeader;
            InData->HeaderHistoryIdx = (InData->HeaderHistoryIdx + 1) % 8;
 
            InData->Header = *UsrpHeader;

            switch(UsrpHeader->Type)
            {
            case USRP_TYPE_VOICE:
                {
                    // Check sequence validity if PTT.
                    if(UsrpHeader->Ptt)
                    {
                        if((int)(UsrpHeader->SequenceNum - InData->SequenceNum) <= 0) {
                            LOG_NORM(("%s#%d: USRP Packet out of sequence (Is %u, Expecting > %u), size %i\n",__FUNCTION__,__LINE__, UsrpHeader->SequenceNum, InData->SequenceNum, InBytesRead));
                        }
                        InData->SequenceNum = UsrpHeader->SequenceNum;
                    }
                    else
                    {
                        // No PTT, reset sequence number.
                        InData->SequenceNum = 0;
                    }

                    const size_t CopySize = USRP_VOICE_FRAME_SIZE * sizeof(short);
                    ::write(AudioC->Socket, UsrpVoiceFrame, CopySize);
                }
                break;

            case USRP_TYPE_DTMF:
                LOG_NORM(("%s#%d: USRP_TYPE_DTMF unimplemented %i bytes\n",__FUNCTION__,__LINE__, InBytesRead));
                break;

            case USRP_TYPE_TEXT:
                {
                    USRPMetaData* metaData = (USRPMetaData*)(UsrpHeader + 1);
                    if( metaData->TLVTag == USRP_TLV_TAG_SET_INFO )
                    {
                        metaData->DmrID = htonl(metaData->DmrID) >> 8;
                        metaData->RepeaterID = htonl(metaData->RepeaterID);
                        metaData->Talkgroup = htonl(metaData->Talkgroup) >> 8;
                        LOG_NORM(("%s#%d: Set Info: DMR ID: %u, Callsign: %s, Repeater ID: %u, Talkgroup: %u\n",__FUNCTION__,__LINE__, metaData->DmrID, metaData->Callsign, metaData->RepeaterID, metaData->Talkgroup) );

                        // Initialize sequence number.
                        InData->SequenceNum = UsrpHeader->SequenceNum;
                    }
                }
                break;

            case USRP_TYPE_PING:
                LOG_NORM(("%s#%d: USRP_TYPE_PING unimplemented %i bytes\n",__FUNCTION__,__LINE__, InBytesRead));
                break;

            case USRP_TYPE_TLV:
                LOG_NORM(("%s#%d: USRP_TYPE_TLV unimplemented %i bytes\n",__FUNCTION__,__LINE__, InBytesRead));
                break;

            case USRP_TYPE_VOICE_ADPCM:
                LOG_NORM(("%s#%d: USRP_TYPE_VOICE_ADPCM unimplemented %i bytes\n",__FUNCTION__,__LINE__, InBytesRead));
                break;

            case USRP_TYPE_VOICE_ULAW:
                LOG_NORM(("%s#%d: USRP_TYPE_VOICE_ULAW unimplemented %i bytes\n",__FUNCTION__,__LINE__, InBytesRead));
                break;
            }
        }
    } while (InBytesRead > 0 && bRunning && !bShutdown);

    LOG_NORM(("%s#%d: RecvMain exit\n",__FUNCTION__,__LINE__));
    return NULL;
}
