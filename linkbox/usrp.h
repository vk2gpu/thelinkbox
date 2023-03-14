#ifndef _USRP_H_
#define _USRP_H_

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

enum
{
    USRP_TYPE_VOICE = 0,
    USRP_TYPE_DTMF = 1,
    USRP_TYPE_TEXT = 2,
    USRP_TYPE_PING = 3,
    USRP_TYPE_TLV = 4,
    USRP_TYPE_VOICE_ADPCM = 5,
    USRP_TYPE_VOICE_ULAW = 6,

    USRP_TLV_TAG_BEGIN_TX = 0,
    USRP_TLV_TAG_AMBE = 1,
    USRP_TLV_TAG_END_TX = 2,
    USRP_TLV_TAG_TG_TUNE = 3,
    USRP_TLV_TAG_PLAY_AMBE = 4,
    USRP_TLV_TAG_REMOTE_CMD = 5,
    USRP_TLV_TAG_AMBE_49 = 6,
    USRP_TLV_TAG_AMBE_72 = 7,
    USRP_TLV_TAG_SET_INFO = 8,
    USRP_TLV_TAG_IMBE = 9,
    USRP_TLV_TAG_DSAMBE = 10,
    USRP_TLV_TAG_FILE_XFER = 11,

    USRP_VOICE_FRAME_SIZE = 160, // 20ms
    USRP_VOICE_FRAMES_MAX = 10,  // 200ms

    USRP_VOICE_BUFFER_FRAME_SIZE = USRP_VOICE_FRAME_SIZE * USRP_VOICE_FRAMES_MAX,
};


struct USRPHeader
{
	char Usrp[4];	        // "USRP"
	uint32_t SequenceNum;
	uint32_t Memory;
	uint32_t Ptt;
	uint32_t Talkgroup;
	uint32_t Type;
	uint32_t Mpxid;
	uint32_t Reserved;
};


typedef struct ClientInfo_TAG ClientInfo;

class CUSRP
{
public:
    CUSRP();
    ~CUSRP();

    int Init(const char *NodeName, char *AudioDevice, ClientInfo *pAudioC);
    bool PollCOS();

    int Read(short *OutData, int MaxRead);
    int Write(const short *FrameData, int SizeBytes);

private:
	static void* StaticRecvMain( void* pParam );
	void* RecvMain();

    struct CUSRPImpl* pImpl;
        
    pthread_t SendThread;
    pthread_t RecvThread;

    int PortIn = 0;
    int PortOut = 0;

    ClientInfo *AudioC = NULL;

    struct sockaddr_in InServerAddr;
    struct sockaddr_in InClientAddr;
    int InSock;
    socklen_t InAddrLen;

    struct CUSRPData* InData;
    struct CUSRPData* OutData;





};


#endif // _USRP_H_