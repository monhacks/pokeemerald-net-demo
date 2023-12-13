#include "global.h"
#include "string_util.h"
#include "text.h"
#include "menu.h"
#include "main.h"
#include "task.h"
#include "link.h"
#include "data.h"
#include "event_data.h"
#include "script.h"
#include "field_message_box.h"
#include "libgcnmultiboot.h"
#include "gpu_regs.h"
#include "net_conn.h"
#include "constants/network.h"
#include "battle_tower.h"
#include "constants/trainers.h"
#include "constants/moves.h"
#include "shop.h"
#include "constants/items.h"
#include "field_message_box.h"

// Player name + 1 + Gender + Special Warp Flag + Trainer ID)
#define PLAYER_INFO_LENGTH (PLAYER_NAME_LENGTH + 1 + 1 + 1 + TRAINER_ID_LENGTH)

#define WELCOME_MSG_LENGTH 48

#define NET_GAME_NAME_LENGTH 20
static const u8 sNetGameName[] = _("Emerald Net Demo 1.0");

#define NET_SERVER_ADDR_LENGTH 14
static const u8 sNetServerAddr[] = _("127.0.0.1:9000");

#define SERVER_NAME_LENGTH 13
static const u8 sServerName[] = _("INCOMMING...\n");

#define DOWNLOAD_TRAINER_PARTY_SIZE 3
#define DOWNLOAD_TRAINER_POKEMON_SIZE 16

#define DOWNLOAD_MART_SIZE 6

static const u8 sDot[] = _("·");
static const u8 sWaitingMessage[] = _("Connecting To Server:");

static const u8 trainerName[] = _("NET");

static void Task_StartNetworkTask(u8 taskId);
static void Task_NetworkTaskLoop(u8 taskId);
static bool32 CheckLinkCanceled(u8 taskId);
static void NetConnResetSerial(void);
static void NetConnEnableSerial(void);
static void NetConnDisableSerial(void);

// NET_CONN_START_LINK_FUNC
static void SetupForLinkTask();
static void Task_LinkupCancel(u8 taskId);
static void Task_LinkupProcess(u8 taskId);
static void Task_EndLinkupConnection(u8 taskId);

// NET_CONN_START_BATTLE_FUNC <---- This example has some comments to explain what is happening 
static void SetupForDownloadBattleTask();
static void Task_DownloadBattleCancel(u8 taskId);
static void Task_DownloadBattleProcess(u8 taskId);
static void Task_EndDownloadBattleConnection(u8 taskId);

// NET_CONN_START_MART_FUNC
static void SetupForOnlineMartTask();
static void Task_OnlineMartCancel(u8 taskId);
static void Task_OnlineMartProcess(u8 taskId);
static void Task_EndOnlineMartConnection(u8 taskId);

// NET_CONN_START_EGG_FUNC
static void SetupForGiftEggTask();
static void Task_GiftEggCancel(u8 taskId);
static void Task_GiftEggProcess(u8 taskId);
static void Task_EndGiftEggConnection(u8 taskId);

static void DoTransferDataBlock(u8 taskId);
static void DoReceiveDataBlock(u8 taskId);

void xfer16(u16 data1, u16 data2, u8 taskId);
void xfer32(u32 data, u8 taskId);
u32 recv32(u8 taskId);
void waitForTransmissionFinish(u8 taskId, u16 readOrWriteFlag); // i.e JOY_READ or JOY_WRITE

// WARNING! configureSendRecvMgrChunked has only been tested sending multiples of 16 bytes. 
// It should work correctly sending any amount of data, this is just and FYI you will be running untested code if you don't send mutiples of 16 

/*
* In the chunked version messages are split into chunks and verified separately. This adds some overhead 
* as more verification messages need to be sent. However it reduces the amount of data we need to discard if there is an issue.
* As a rule of thumb if you are sending 16 bytes or less don't use chunked. If you are sending more, then chunk to 16 byte blocks.
*/
void configureSendRecvMgr(u16 cmd, vu32 * dataStart, u16 length, u8 state, u8 nextProcessStep);
void configureSendRecvMgrChunked(u16 cmd, vu32 * dataStart, u16 length, u8 state, u8 nextProcessStep, u8 chunkSize);

enum {
    NET_CONN_STATE_INIT = 0,
    NET_CONN_STATE_SEND,
    NET_CONN_STATE_RECEIVE,
    NET_CONN_STATE_PROCESS,
    NET_CONN_STATE_ERROR,
    NET_CONN_STATE_DONE
};

struct SendRecvMgr
{
    u8 state;              // The state of the current connection (e.g is is sending data, receiving data e.t.c)
    vu32 *dataStart;       // Payload source or destination
    u16 length;            // Length of payload
    u16 cmd;               // The command to send to the wii
    bool8 allowCancel;     // If 'B' can be pressed to end the current conneciton
    TaskFunc onProcess;    // The connection loop will branch to this after each action to check what to do next
    TaskFunc onCancel;     // What to do if the player cancels the connection (or there is an error/connection lost)
    TaskFunc onFinish;     // What to do when the connection ends in the expected way
    u8 nextProcessStep;    // State specific to the current network task that's running i.e (so onProcess change behaviour depending on the current step) 
    s8 retriesLeft;        // Number of times left to retry the current connection
    u8 retryPoint;         // Where to pick up from when doing a retry 
    bool8 disableChecks;   // If true all msg check bytes will be ignored. Turning this off is a bad idea unless you're A) only sending a 32 bit value /  B) need speed over accuracy and can ignore junk
    u8 repeatedStepCount;  // Times we've sucessfully sequentially done a process step. Allows for steps the need to run x many times / large transfers that need to be spit 
};
static struct SendRecvMgr sSendRecvMgr;

static void (* const sNetConnFunctions[])(void) =
{
    [NET_CONN_START_LINK_FUNC]    = SetupForLinkTask,
    [NET_CONN_START_BATTLE_FUNC]  = SetupForDownloadBattleTask,
    [NET_CONN_START_MART_FUNC]    = SetupForOnlineMartTask,
    [NET_CONN_START_EGG_FUNC]     = SetupForGiftEggTask
};

void CallNetworkFunction(void)
{
    if (sSendRecvMgr.state == 0)
        CpuFill32(0, &sSendRecvMgr, sizeof(sSendRecvMgr));

    sNetConnFunctions[gSpecialVar_0x8004]();

    if (FindTaskIdByFunc(Task_NetworkTaskLoop) == TASK_NONE)
        CreateTask(Task_NetworkTaskLoop, 80);
}

static void Task_NetworkTaskLoop(u8 taskId)
{
    if (CheckLinkCanceled(taskId) == TRUE)
        sSendRecvMgr.state = NET_CONN_STATE_ERROR;

    switch (sSendRecvMgr.state)
    {
        case NET_CONN_STATE_INIT:
            if (REG_RCNT != R_JOYBUS)
                NetConnResetSerial();

            SetSuppressLinkErrorMessage(TRUE);
            xfer16(NET_CONN_BCLR_REQ, 0, taskId);
            sSendRecvMgr.state = NET_CONN_STATE_PROCESS;
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- INIT");
            break;
        case NET_CONN_STATE_SEND:
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- SEND");
            DoTransferDataBlock(taskId);
            break;
        case NET_CONN_STATE_RECEIVE:
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- RECEIVE");
            DoReceiveDataBlock(taskId);
            break;
        case NET_CONN_STATE_PROCESS: 
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- PROCESS");
            gTasks[taskId].func = sSendRecvMgr.onProcess;
            break;
        case NET_CONN_STATE_ERROR:
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- ERROR");
            JOY_TRANS = 0;
            if (--sSendRecvMgr.retriesLeft < 0)
            {
                gTasks[taskId].func = sSendRecvMgr.onCancel;
            }
            else
            {   
                sSendRecvMgr.state = 0;
                sSendRecvMgr.nextProcessStep = sSendRecvMgr.retryPoint;
                if (sSendRecvMgr.repeatedStepCount > 0) sSendRecvMgr.repeatedStepCount--;
                NetConnResetSerial();
            }
            DebugPrintfLevel(MGBA_LOG_DEBUG, "Looping with retries left %d",  sSendRecvMgr.retriesLeft);
            break;
        case NET_CONN_STATE_DONE:
        default:
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- DONE");
            gTasks[taskId].func = sSendRecvMgr.onFinish;
            break;
    }
}

static bool32 CheckLinkCanceled(u8 taskId)
{
    if (((JOY_NEW(B_BUTTON)) || JOY_HELD(B_BUTTON)) && sSendRecvMgr.allowCancel == TRUE)
    {
        sSendRecvMgr.retriesLeft = RETRIES_LEFT_CANCEL + 1;
        gTasks[taskId].func = sSendRecvMgr.onCancel;
        return TRUE;
    }
    return FALSE;
}

static void NetConnDisableSerial(void)
{
    sSendRecvMgr.state = NET_CONN_STATE_INIT;

    // I have no idea if his is the proper way to end the link  ¯\_(ツ)_/¯
    DisableInterrupts(INTR_FLAG_TIMER3 | INTR_FLAG_SERIAL);
    REG_SIOCNT = SIO_MULTI_MODE;
    REG_TMCNT_H(3) = 0;
    REG_IF = INTR_FLAG_TIMER3 | INTR_FLAG_SERIAL;
    JOY_TRANS = 0;

    // This seems to cause a moment of lag with mgba in TCP mode but not integrated. So possibly a latency issue.  
    REG_RCNT = 0; 
}

static void NetConnEnableSerial(void)
{
    DisableInterrupts(INTR_FLAG_TIMER3 | INTR_FLAG_SERIAL);
    REG_RCNT = R_JOYBUS;
    EnableInterrupts(INTR_FLAG_VBLANK | INTR_FLAG_VCOUNT | INTR_FLAG_TIMER3 | INTR_FLAG_SERIAL); // These may not all be needed? needs checking
}

static void NetConnResetSerial(void) 
{
    NetConnDisableSerial();
    NetConnEnableSerial();
}

static void DoTransferDataBlock(u8 taskId)
{
    u16 i = 0;
    u8 * dataStart;
    u16 checkBytes = 0xFFFF;
    u8 transBuff[4];
    u32 resBuff = 0;

    JOY_CNT |= JOY_RW;
    for (i = 0; JOY_CNT&JOY_READ && i <= MAX_CONNECTION_LOOPS; i++) 
    { 
        /* Wait for the connection to become ready */ 
        if (CheckLinkCanceled(taskId) == TRUE || i == MAX_CONNECTION_LOOPS)
        {
            sSendRecvMgr.state = NET_CONN_STATE_ERROR;
            return;
        }

    }

    DebugPrintfLevel(MGBA_LOG_DEBUG, "Check bytes Before %x", checkBytes);

    xfer16(sSendRecvMgr.cmd, (u16) sSendRecvMgr.length, taskId);
    checkBytes ^= sSendRecvMgr.cmd;
    checkBytes ^= (u16) sSendRecvMgr.length;

    DebugPrintfLevel(MGBA_LOG_DEBUG, "Check bytes Post CMD %x", checkBytes);

    if (sSendRecvMgr.state == NET_CONN_STATE_ERROR)
        return;

    dataStart = (u8 *) sSendRecvMgr.dataStart;

    for(i = 0; i < sSendRecvMgr.length; i+=4)
    {
        transBuff[0] = dataStart[i];
        transBuff[1] = i + 1 < sSendRecvMgr.length ? dataStart[i + 1] : 0;
        transBuff[2] = i + 2 < sSendRecvMgr.length ? dataStart[i + 2] : 0;
        transBuff[3] = i + 3 < sSendRecvMgr.length ? dataStart[i + 3] : 0;

        xfer32((u32) (transBuff[0] + (transBuff[1] << 8) + (transBuff[2] << 16) + (transBuff[3] << 24)), taskId);

        if (sSendRecvMgr.state == NET_CONN_STATE_ERROR)
            return;

        checkBytes ^= (u16) (transBuff[0] + (transBuff[1] << 8));
        checkBytes ^= (u16) (transBuff[2] + (transBuff[3] << 8));

        DebugPrintfLevel(MGBA_LOG_DEBUG, "Check bytes %d Post Send %x", i, checkBytes);

    }
    
    resBuff = recv32(taskId);
    DebugPrintfLevel(MGBA_LOG_DEBUG, "Response from wii %x", resBuff);
    if (sSendRecvMgr.disableChecks || (resBuff ^ (u32) ((NET_CONN_CHCK_RES << 16) | (checkBytes & 0xFFFF))) == 0)
    {
        JOY_TRANS = 0;
        sSendRecvMgr.state = NET_CONN_STATE_PROCESS;
    }
    else
    {
        for (i = 0; i < 300; i++) {}
        DebugPrintfLevel(MGBA_LOG_DEBUG, "Check did not match expecting %x%x, returned %x. Will Retry:",  NET_CONN_CHCK_RES, checkBytes, JOY_RECV);
    }
}

static void DoReceiveDataBlock(u8 taskId)
{
    u16 i = 0;
    u8 * dataStart;
    u16 checkBytes = 0xFFFF;
    u8 transBuff[4];
    u32 resBuff = 0;

    JOY_CNT |= JOY_RW;
    for (i = 0; JOY_CNT&JOY_READ && i <= MAX_CONNECTION_LOOPS; i++) 
    { 
        /* Wait for the connection to become ready */ 
        if (CheckLinkCanceled(taskId) == TRUE || i == MAX_CONNECTION_LOOPS)
        {
            sSendRecvMgr.state = NET_CONN_STATE_ERROR;
            return;
        }

    }

    DebugPrintfLevel(MGBA_LOG_DEBUG, "Receive Check bytes Before %x", checkBytes);

    xfer16(sSendRecvMgr.cmd, (u16) sSendRecvMgr.length, taskId);

    DebugPrintfLevel(MGBA_LOG_DEBUG, "Check bytes Post CMD %x", checkBytes);

    if (sSendRecvMgr.state == NET_CONN_STATE_ERROR)
        return;

    dataStart = (u8 *) sSendRecvMgr.dataStart;

    for(i = 0; i < sSendRecvMgr.length; i+=4)
    {
        resBuff = recv32((u32) (taskId));

        transBuff[0] = (resBuff >> 24) & 0xFF;
        transBuff[1] = (resBuff >> 16) & 0xFF;
        transBuff[2] = (resBuff >> 8) & 0xFF;
        transBuff[3] = resBuff & 0xFF;

        dataStart[i] = transBuff[0];
        if (i + 1 < sSendRecvMgr.length) dataStart[i + 1] = transBuff[1];
        if (i + 2 < sSendRecvMgr.length) dataStart[i + 2] = transBuff[2];
        if (i + 3 < sSendRecvMgr.length) dataStart[i + 3] = transBuff[3];

        DebugPrintfLevel(MGBA_LOG_DEBUG, "Got Values From Wii %x %x %x %x", transBuff[0], transBuff[1], transBuff[2], transBuff[3]);

        if (sSendRecvMgr.state == NET_CONN_STATE_ERROR)
            return;

        checkBytes ^= (u16) (transBuff[0] + (transBuff[1] << 8));
        checkBytes ^= (u16) (transBuff[2] + (transBuff[3] << 8));

        DebugPrintfLevel(MGBA_LOG_DEBUG, "Check bytes %d Post Send %x", i, checkBytes);
    }

    resBuff = recv32(taskId);
    DebugPrintfLevel(MGBA_LOG_DEBUG, "Response from wii %x", resBuff);

    
    if (sSendRecvMgr.disableChecks || (resBuff ^ (u32) ((NET_CONN_CHCK_RES << 16) | (checkBytes & 0xFFFF))) == 0)
    {
        JOY_RECV = 0;
        sSendRecvMgr.state = NET_CONN_STATE_PROCESS;
    }
    else
    {
        for (i = 0; i < 300; i++) {}
        DebugPrintfLevel(MGBA_LOG_DEBUG, "Check did not match expecting %x%x, returned %x. Will Retry:",  NET_CONN_CHCK_RES, checkBytes, JOY_RECV);
    }
}

/**
* The way joybus coms work is that the wii is always master and the GBA always slave
* This means that the GBA cannot initiate a transfer, only respond with up to 4 bytes
* each time the wii sends a 4 byte message
*/
void xfer16(u16 data1, u16 data2, u8 taskId) 
{
    JOY_CNT |= JOY_RW;
    JOY_TRANS_L = data1;
    JOY_TRANS_H = data2;
    waitForTransmissionFinish(taskId, JOY_READ);
}

void xfer32(u32 data, u8 taskId) 
{
    JOY_CNT |= JOY_RW;
    JOY_TRANS = data;
    waitForTransmissionFinish(taskId, JOY_READ);
}

u32 recv32(u8 taskId) 
{
    JOY_CNT |= JOY_RW;
    waitForTransmissionFinish(taskId, JOY_WRITE);
    return JOY_RECV;
}

void waitForTransmissionFinish(u8 taskId, u16 readOrWriteFlag)
{
    u16 i = 0;

    for (i = 0; (JOY_CNT&readOrWriteFlag) == 0; i++)
    {
        if (i > MAX_CONNECTION_LOOPS || CheckLinkCanceled(taskId) == TRUE)
        {
            sSendRecvMgr.state = NET_CONN_STATE_ERROR;
            return;
        }
    }
}

void configureSendRecvMgr(u16 cmd, vu32 * dataStart, u16 length, u8 state, u8 nextProcessStep)
{
    sSendRecvMgr.cmd             = cmd;
    sSendRecvMgr.dataStart       = dataStart;
    sSendRecvMgr.length          = length; 
    sSendRecvMgr.state           = state;
    
    if (sSendRecvMgr.nextProcessStep == nextProcessStep)
    {
        sSendRecvMgr.repeatedStepCount++;
    } 
    else 
    {
        sSendRecvMgr.repeatedStepCount = 0;
    }
    
    sSendRecvMgr.nextProcessStep = nextProcessStep;
}   

void configureSendRecvMgrChunked(u16 cmd, vu32 * dataStart, u16 length, u8 state, u8 nextProcessStep, u8 chunkSize)
{
    if (length <= chunkSize || chunkSize <= 0)
    {
        DebugPrintfLevel(MGBA_LOG_DEBUG, "--- TO SMALL TO CHUNK");
        configureSendRecvMgr(cmd, dataStart, length, state, nextProcessStep);
    }
    else if (chunkSize * (sSendRecvMgr.repeatedStepCount + 1) < length)
    {
        DebugPrintfLevel(MGBA_LOG_DEBUG, "--- %x PART OF CHUNK", sSendRecvMgr.repeatedStepCount);
        DebugPrintfLevel(MGBA_LOG_DEBUG, "CMD: %x", cmd + ((chunkSize * sSendRecvMgr.repeatedStepCount)/MINIMUM_CHUNK_SIZE));
        DebugPrintfLevel(MGBA_LOG_DEBUG, "Writing to address: %x + %x * %x = %x", dataStart, chunkSize, sSendRecvMgr.repeatedStepCount, (dataStart + (chunkSize * sSendRecvMgr.repeatedStepCount)));
        configureSendRecvMgr(cmd + ((chunkSize * sSendRecvMgr.repeatedStepCount)/MINIMUM_CHUNK_SIZE), (vu32 *) dataStart + (chunkSize/4 * sSendRecvMgr.repeatedStepCount), chunkSize, state, sSendRecvMgr.nextProcessStep);
    }
    else if (chunkSize * (sSendRecvMgr.repeatedStepCount + 1) == length)
    {
        DebugPrintfLevel(MGBA_LOG_DEBUG, "--- FINAL PART OF FULL CHUNK");
        DebugPrintfLevel(MGBA_LOG_DEBUG, "CMD: %x", cmd + ((chunkSize * sSendRecvMgr.repeatedStepCount)/MINIMUM_CHUNK_SIZE));
        DebugPrintfLevel(MGBA_LOG_DEBUG, "Writing to address: %x + %x * %x = %x", dataStart, chunkSize, sSendRecvMgr.repeatedStepCount, (dataStart + (chunkSize * sSendRecvMgr.repeatedStepCount)));
        configureSendRecvMgr(cmd + ((chunkSize * sSendRecvMgr.repeatedStepCount)/MINIMUM_CHUNK_SIZE), (vu32 *) dataStart + (chunkSize/4 * sSendRecvMgr.repeatedStepCount), chunkSize, state, nextProcessStep);
    }
    else
    {
        DebugPrintfLevel(MGBA_LOG_DEBUG, "--- FINAL PART OF PARTIAL CHUNK");
        DebugPrintfLevel(MGBA_LOG_DEBUG, "CMD: %x", cmd + ((chunkSize * sSendRecvMgr.repeatedStepCount)/MINIMUM_CHUNK_SIZE));
        DebugPrintfLevel(MGBA_LOG_DEBUG, "Writing to address: %x", (dataStart + (chunkSize * sSendRecvMgr.repeatedStepCount)));
        configureSendRecvMgr(cmd + ((chunkSize * sSendRecvMgr.repeatedStepCount)/MINIMUM_CHUNK_SIZE), (vu32 *) dataStart + (chunkSize/4 * sSendRecvMgr.repeatedStepCount), length % chunkSize, state, nextProcessStep);
    }
}


/**
*   =====================================================================
*   === NET_CONN_START_LINK_FUNC                                      ===
*   =====================================================================
*/
enum {
    LINKUP_SEND_PLAYER_DATA = 0,
    LINKUP_APPEND_GAME_NAME,
    LINKUP_USE_DATA_AS_PLAYER_INFO,
    LINKUP_SEND_NETWORK_INFO,
    LINKUP_USE_DATA_AS_NETWORK_INFO,
    LINKUP_WAIT_FOR_SERVER_TO_CONNECT,
    LINKUP_REQUEST_NETWORK_STATUS,
    LINKUP_HANDLE_NETWORK_STATUS_OUTCOME,
    LINKUP_RECEIVE_WELCOME_MESSAGE,
    LINKUP_FINISH
};

static void SetupForLinkTask()
{
    sSendRecvMgr.allowCancel       = TRUE;
    sSendRecvMgr.retriesLeft       = MAX_CONNECTION_RETRIES;
    sSendRecvMgr.onFinish          = Task_EndLinkupConnection;
    sSendRecvMgr.onCancel          = Task_LinkupCancel;
    sSendRecvMgr.onProcess         = Task_LinkupProcess;
    sSendRecvMgr.nextProcessStep   = LINKUP_SEND_PLAYER_DATA;
    sSendRecvMgr.disableChecks     = FALSE;
    sSendRecvMgr.repeatedStepCount = 0;
}

static void Task_LinkupCancel(u8 taskId)
{
    DebugPrintfLevel(MGBA_LOG_DEBUG, "--- LINKUP BEING CANCELED");
    gSpecialVar_0x8003 = NET_STAT_OFFLINE;
    Task_EndLinkupConnection(taskId);
}

static void Task_LinkupProcess(u8 taskId)
{
    switch (sSendRecvMgr.nextProcessStep)
    {
        case LINKUP_SEND_PLAYER_DATA:
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- LINKUP_SEND_PLAYER_DATA");
            configureSendRecvMgr(NET_CONN_SEND_REQ, (vu32 *) &gSaveBlock2Ptr->playerName[0], PLAYER_INFO_LENGTH, NET_CONN_STATE_SEND, LINKUP_APPEND_GAME_NAME);
            break;

        case LINKUP_APPEND_GAME_NAME:
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- LINKUP_APPEND_GAME_NAME");
            configureSendRecvMgr(NET_CONN_SCH2_REQ, (vu32 *) &sNetGameName[0], NET_GAME_NAME_LENGTH, NET_CONN_STATE_SEND, LINKUP_USE_DATA_AS_PLAYER_INFO);
            break;

        case LINKUP_USE_DATA_AS_PLAYER_INFO:
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- LINKUP_USE_DATA_AS_PLAYER_INFO");
            configureSendRecvMgr(NET_CONN_PINF_REQ, 0, 0, NET_CONN_STATE_SEND, LINKUP_SEND_NETWORK_INFO);
            break;

        case LINKUP_SEND_NETWORK_INFO:
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- LINKUP_SEND_NETWORK_INFO");
            sSendRecvMgr.retryPoint = LINKUP_SEND_NETWORK_INFO;
            configureSendRecvMgr(NET_CONN_SEND_REQ, (vu32 *) &sNetServerAddr[0], NET_SERVER_ADDR_LENGTH, NET_CONN_STATE_SEND, LINKUP_USE_DATA_AS_NETWORK_INFO);
            break;

        case LINKUP_USE_DATA_AS_NETWORK_INFO:
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- LINKUP_USE_DATA_AS_NETWORK_INFO");
            configureSendRecvMgr(NET_CONN_CINF_REQ, 0, 0, NET_CONN_STATE_SEND, LINKUP_WAIT_FOR_SERVER_TO_CONNECT);
            break;


        case LINKUP_WAIT_FOR_SERVER_TO_CONNECT:
            if (sSendRecvMgr.repeatedStepCount == 0)
            {
                DebugPrintfLevel(MGBA_LOG_DEBUG, "--- LINKUP_WAIT_FOR_SERVER_TO_CONNECT");
                LoadMessageBoxAndFrameGfx(0, TRUE);
                VBlankIntrWait(); VBlankIntrWait();
                AddTextPrinterParameterized(0, FONT_NORMAL, sWaitingMessage, 0, 1, 0, NULL);
                sSendRecvMgr.repeatedStepCount++;
            }
            else if (sSendRecvMgr.repeatedStepCount <= 60)
            {
                VBlankIntrWait(); VBlankIntrWait();
                if (sSendRecvMgr.repeatedStepCount % 10 == 0)
                {
                    AddTextPrinterParameterized(0, FONT_NORMAL, sDot, ((sSendRecvMgr.repeatedStepCount - 1) * 8) / 10, 16, 0, NULL);
                }
                sSendRecvMgr.repeatedStepCount++;
            }
            else
            {
                sSendRecvMgr.nextProcessStep = LINKUP_REQUEST_NETWORK_STATUS;
                sSendRecvMgr.repeatedStepCount = 0;
            }
            break;

        case LINKUP_REQUEST_NETWORK_STATUS:
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- LINKUP_REQUEST_NETWORK_STATUS");
            sSendRecvMgr.disableChecks = TRUE; 
            sSendRecvMgr.retryPoint = LINKUP_REQUEST_NETWORK_STATUS;
            configureSendRecvMgr(NET_CONN_LIFN_REQ, (vu32 *) &gStringVar3[0], 4, NET_CONN_STATE_RECEIVE, LINKUP_HANDLE_NETWORK_STATUS_OUTCOME);
            break;

        case LINKUP_HANDLE_NETWORK_STATUS_OUTCOME:
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- LINKUP_HANDLE_NETWORK_STATUS_OUTCOME");
            if (!(NET_CONN_LIFN_REQ >> 8 == gStringVar3[0] && (NET_CONN_LIFN_REQ & 0x00FF) == gStringVar3[1]))
            {
                DebugPrintfLevel(MGBA_LOG_DEBUG, "--- UNEXPECTED RESULT %x ", (u16) (gStringVar3[0] | gStringVar3[1] << 8));
                sSendRecvMgr.nextProcessStep = LINKUP_REQUEST_NETWORK_STATUS;
            }
            else if (gStringVar3[3] >= NETWORK_MIN_ERROR)
            {
                gSpecialVar_0x8003 = NET_STAT_ATTACHED_NO_INTERNET;
                sSendRecvMgr.nextProcessStep = LINKUP_FINISH;
            }
            else if (gStringVar3[3] == NETWORK_CONNECTION_SUCCESS && gStringVar3[2] == NETWORK_STATE_WAITING)
            {
                gSpecialVar_0x8003 = NET_STAT_ONLINE;
                sSendRecvMgr.nextProcessStep = LINKUP_RECEIVE_WELCOME_MESSAGE;
            }
            else
            {
                sSendRecvMgr.nextProcessStep = LINKUP_WAIT_FOR_SERVER_TO_CONNECT;
            }
            break;

        case LINKUP_RECEIVE_WELCOME_MESSAGE:
            sSendRecvMgr.disableChecks = FALSE;
            if (sSendRecvMgr.repeatedStepCount == 0)
            {
                DebugPrintfLevel(MGBA_LOG_DEBUG, "--- Network Status is %x %x %x %x", gStringVar3[0], gStringVar3[1], gStringVar3[2], gStringVar3[3]);
                DebugPrintfLevel(MGBA_LOG_DEBUG, "--- LINKUP_RECEIVE_WELCOME_MESSAGE");
                sSendRecvMgr.retryPoint = LINKUP_RECEIVE_WELCOME_MESSAGE;
                StringCopy(gStringVar3, sServerName);
            }
            configureSendRecvMgrChunked(NET_CONN_RCHF0_REQ, (vu32 *) &gStringVar3[SERVER_NAME_LENGTH], WELCOME_MSG_LENGTH, NET_CONN_STATE_RECEIVE, LINKUP_FINISH, MINIMUM_CHUNK_SIZE);
            break;

        case LINKUP_FINISH:
        default:
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- LINKUP_FINISH");
            gStringVar3[SERVER_NAME_LENGTH + WELCOME_MSG_LENGTH] = 0xFF; // Force the message to end if they never never terminated it 
            sSendRecvMgr.state = NET_CONN_STATE_DONE;
            break;
    }

    gTasks[taskId].func = Task_NetworkTaskLoop;
}

static void Task_EndLinkupConnection(u8 taskId) 
{
    gSpecialVar_Result = 5;
    NetConnDisableSerial();
    StopFieldMessage();
    ScriptContext_Enable();
    DestroyTask(taskId);
}

/**
*   =====================================================================
*   === NET_CONN_DOWNLOAD_BATTLE                                      ===
*   =====================================================================
*/
enum {
    DOWNLOAD_BATTLE_SEND_REQUEST = 0,
    DOWNLOAD_BATTLE_TRANSMIT_REQUEST,
    DOWNLOAD_BATTLE_WAIT_FOR_SERVER,
    DOWNLOAD_BATTLE_RECIEVE_DATA,
    DOWNLOAD_BATTLE_FINISH
};

// NET_CONN_FETCH_BATTLE_FUNC
static void SetupForDownloadBattleTask();
static void Task_DownloadBattleCancel(u8 taskId);
static void Task_DownloadBattleProcess(u8 taskId);
static void Task_EndDownloadBattleConnection(u8 taskId);

static void SetupForDownloadBattleTask()
{
    sSendRecvMgr.allowCancel       = TRUE; 
    sSendRecvMgr.retriesLeft       = MAX_CONNECTION_RETRIES;
    sSendRecvMgr.onFinish          = Task_EndDownloadBattleConnection;
    sSendRecvMgr.onCancel          = Task_DownloadBattleCancel;
    sSendRecvMgr.onProcess         = Task_DownloadBattleProcess;
    sSendRecvMgr.nextProcessStep   = DOWNLOAD_BATTLE_SEND_REQUEST;
    sSendRecvMgr.disableChecks     = FALSE;
    sSendRecvMgr.repeatedStepCount = 0;
    gSpecialVar_0x8003 = 0;
}

static void Task_DownloadBattleCancel(u8 taskId)
{
    Task_EndDownloadBattleConnection(taskId);
}

static void Task_DownloadBattleProcess(u8 taskId)
{
    switch (sSendRecvMgr.nextProcessStep)
    {
        case DOWNLOAD_BATTLE_SEND_REQUEST: // Puts request data on the wii  (at address channel 2)
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- DOWNLOAD_BATTLE_SEND_REQUEST");
            gStringVar3[0] = 'B'; gStringVar3[1] = 'A'; gStringVar3[2] = '_'; gStringVar3[3] = '1'; // The '1' at the end is for if we want multiple downloadable trainers
            configureSendRecvMgr(NET_CONN_SCH2_REQ, (vu32 *) &gStringVar3[0], 4, NET_CONN_STATE_SEND, DOWNLOAD_BATTLE_TRANSMIT_REQUEST);
            break;

        case DOWNLOAD_BATTLE_TRANSMIT_REQUEST: // Sends request data to the server (from address channel 2)
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- DOWNLOAD_BATTLE_TRANSMIT_REQUEST");
            sSendRecvMgr.disableChecks = TRUE; // The wii code currently dosn't support transmit checks so they need to be off for transmit
            sSendRecvMgr.retryPoint = DOWNLOAD_BATTLE_TRANSMIT_REQUEST;
            CpuFill32(0, &gStringVar3, sizeof(gStringVar3));  
            configureSendRecvMgr(NET_CONN_TCH2_REQ, 0, 4, NET_CONN_STATE_SEND, DOWNLOAD_BATTLE_WAIT_FOR_SERVER);
            break;

        case DOWNLOAD_BATTLE_WAIT_FOR_SERVER: // Wait for data to be pulled from the server
            // TODO: This is just a delay, and hopes the wii has pulled all the data by the time the delay ends
            // We need a way to query the wii and find out if it's actually finished pulling data from the server
            if (sSendRecvMgr.repeatedStepCount == 0)
            {
                DebugPrintfLevel(MGBA_LOG_DEBUG, "--- DOWNLOAD_BATTLE_WAIT_FOR_SERVER");
                LoadMessageBoxAndFrameGfx(0, TRUE);
                VBlankIntrWait(); VBlankIntrWait();
                AddTextPrinterParameterized(0, FONT_NORMAL, sWaitingMessage, 0, 1, 0, NULL);
                sSendRecvMgr.repeatedStepCount++;
            }
            else if (sSendRecvMgr.repeatedStepCount <= 40)
            {
                VBlankIntrWait(); VBlankIntrWait();
                if (sSendRecvMgr.repeatedStepCount % 10 == 0)
                {
                    AddTextPrinterParameterized(0, FONT_NORMAL, sDot, ((sSendRecvMgr.repeatedStepCount - 1) * 8) / 10, 16, 0, NULL);
                }
                sSendRecvMgr.repeatedStepCount++;
            }
            else
            {
                sSendRecvMgr.nextProcessStep = DOWNLOAD_BATTLE_RECIEVE_DATA;
                sSendRecvMgr.repeatedStepCount = 0;
            }
            break;

        case DOWNLOAD_BATTLE_RECIEVE_DATA: // Pull back the data from the wii (reading at address chanel 0x0F and writing to gStringVar3)
            sSendRecvMgr.disableChecks = FALSE;
            if (sSendRecvMgr.repeatedStepCount == 0)
            {
                DebugPrintfLevel(MGBA_LOG_DEBUG, "--- DOWNLOAD_BATTLE_RECIEVE_DATA");
                sSendRecvMgr.retryPoint = DOWNLOAD_BATTLE_RECIEVE_DATA;
            }
            configureSendRecvMgrChunked(NET_CONN_RCHF0_REQ, (vu32 *) &gStringVar3[0], DOWNLOAD_TRAINER_POKEMON_SIZE * DOWNLOAD_TRAINER_PARTY_SIZE, NET_CONN_STATE_RECEIVE, DOWNLOAD_BATTLE_FINISH, MINIMUM_CHUNK_SIZE);
            break;


        case DOWNLOAD_BATTLE_FINISH: // Process the data (create the ereader data from what is now stored in gStringVar3)
        default:
        {
            u8 i;
            u8 offset = 0;

            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- DOWNLOAD_BATTLE_FINISH");
            FillEReaderTrainerWithPlayerData();
            StringFill(gSaveBlock2Ptr->frontier.ereaderTrainer.name, CHAR_SPACER, PLAYER_NAME_LENGTH);
            StringCopy_PlayerName(gSaveBlock2Ptr->frontier.ereaderTrainer.name, trainerName);
            gSaveBlock2Ptr->frontier.ereaderTrainer.facilityClass = FACILITY_CLASS_RS_BRENDAN;

            gSpecialVar_0x8003 = 1;

            for (i = 0; i < PARTY_SIZE; i++)
            {
                if (i < DOWNLOAD_TRAINER_PARTY_SIZE) 
                {
                    offset = DOWNLOAD_TRAINER_POKEMON_SIZE * i;

                    gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].species  = (u16) (gStringVar3[ 0 + offset] | gStringVar3[ 1 + offset] << 8);
                    gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].level    =        gStringVar3[ 2 + offset]                                 ;
                    gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].heldItem = (u16) (gStringVar3[ 3 + offset] | gStringVar3[ 4 + offset] << 8);
                    gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].moves[0] = (u16) (gStringVar3[ 5 + offset] | gStringVar3[ 6 + offset] << 8);
                    gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].moves[1] = (u16) (gStringVar3[ 7 + offset] | gStringVar3[ 8 + offset] << 8);
                    gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].moves[2] = (u16) (gStringVar3[ 9 + offset] | gStringVar3[10 + offset] << 8);
                    gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].moves[3] = (u16) (gStringVar3[11 + offset] | gStringVar3[12 + offset] << 8);

                    StringFill(gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].nickname, CHAR_SPACER, POKEMON_NAME_LENGTH);

                    gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].nickname[0] = gStringVar3[13 + offset];
                    gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].nickname[1] = gStringVar3[14 + offset];
                    gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].nickname[2] = gStringVar3[15 + offset];
                    gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].nickname[3] = 0xFF;

                    // Basic validation to make sure we got something sensible back from the server/wii (ideally this would be a checksum from the server)
                    if (gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].species > NUM_SPECIES || 
                        gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].level > MAX_LEVEL || 
                        gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].heldItem > ITEMS_COUNT || 
                        gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].moves[0] > MOVES_COUNT || 
                        gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].moves[1] > MOVES_COUNT || 
                        gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].moves[2] > MOVES_COUNT || 
                        gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].moves[3] > MOVES_COUNT )
                    {
                        gSpecialVar_0x8003 = 0;
                    }
                }
                else
                {
                    gSaveBlock2Ptr->frontier.ereaderTrainer.party[i].species = 0;
                }

                // Basic validation to make sure we got something sensible back from the server/wii
                if (gSaveBlock2Ptr->frontier.ereaderTrainer.party[0].species == 0)
                    gSpecialVar_0x8003 = 0;

            }


            sSendRecvMgr.state = NET_CONN_STATE_DONE;
            break;
        }
    }

    gTasks[taskId].func = Task_NetworkTaskLoop;
}

static void Task_EndDownloadBattleConnection(u8 taskId) 
{
    NetConnDisableSerial();
    StopFieldMessage();
    ScriptContext_Enable();
    DestroyTask(taskId);
}

/**
*   =====================================================================
*   === NET_CONN_ONLINE_MART                                          ===
*   =====================================================================
*/
enum {
    DOWNLOAD_MART_SEND_REQUEST = 0,
    DOWNLOAD_MART_TRANSMIT_REQUEST,
    DOWNLOAD_MART_WAIT_FOR_SERVER,
    DOWNLOAD_MART_RECEIVE_DATA,
    DOWNLOAD_MART_FINISH
};

// NET_CONN_ONLINE_MART_FUNC
static void SetupForOnlineMartTask();
static void Task_OnlineMartCancel(u8 taskId);
static void Task_OnlineMartProcess(u8 taskId);
static void Task_EndOnlineMartConnection(u8 taskId);

static void SetupForOnlineMartTask()
{
    sSendRecvMgr.allowCancel       = TRUE;
    sSendRecvMgr.retriesLeft       = MAX_CONNECTION_RETRIES;
    sSendRecvMgr.onFinish          = Task_EndOnlineMartConnection;
    sSendRecvMgr.onCancel          = Task_OnlineMartCancel;
    sSendRecvMgr.onProcess         = Task_OnlineMartProcess;
    sSendRecvMgr.nextProcessStep   = DOWNLOAD_MART_SEND_REQUEST;
    sSendRecvMgr.disableChecks     = FALSE;
    sSendRecvMgr.repeatedStepCount = 0;
    gSpecialVar_0x8003 = 0;
}

static void Task_OnlineMartCancel(u8 taskId)
{
    Task_EndLinkupConnection(taskId);
}

static void Task_OnlineMartProcess(u8 taskId)
{
    switch (sSendRecvMgr.nextProcessStep)
    {
        case DOWNLOAD_MART_SEND_REQUEST:
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- DOWNLOAD_MART_SEND_REQUEST");
            gStringVar3[0] = 'M'; gStringVar3[1] = 'A'; gStringVar3[2] = '_'; gStringVar3[3] = '1';
            configureSendRecvMgr(NET_CONN_SCH2_REQ, (vu32 *) &gStringVar3[0], 4, NET_CONN_STATE_SEND, DOWNLOAD_MART_TRANSMIT_REQUEST);
            break;

        case DOWNLOAD_MART_TRANSMIT_REQUEST:
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- DOWNLOAD_MART_TRANSMIT_REQUEST");
            sSendRecvMgr.disableChecks = TRUE; 
            sSendRecvMgr.retryPoint = DOWNLOAD_MART_TRANSMIT_REQUEST;
            CpuFill32(0, &gStringVar3, sizeof(gStringVar3)); 
            configureSendRecvMgr(NET_CONN_TCH2_REQ, 0, 4, NET_CONN_STATE_SEND, DOWNLOAD_MART_WAIT_FOR_SERVER);
            break;

        case DOWNLOAD_MART_WAIT_FOR_SERVER:
            if (sSendRecvMgr.repeatedStepCount == 0)
            {
                DebugPrintfLevel(MGBA_LOG_DEBUG, "--- DOWNLOAD_MART_WAIT_FOR_SERVER");
                LoadMessageBoxAndFrameGfx(0, TRUE);
                VBlankIntrWait(); VBlankIntrWait();
                AddTextPrinterParameterized(0, FONT_NORMAL, sWaitingMessage, 0, 1, 0, NULL);
                sSendRecvMgr.repeatedStepCount++;
            }
            else if (sSendRecvMgr.repeatedStepCount <= 40)
            {
                VBlankIntrWait(); VBlankIntrWait();
                if (sSendRecvMgr.repeatedStepCount % 10 == 0)
                {
                    AddTextPrinterParameterized(0, FONT_NORMAL, sDot, ((sSendRecvMgr.repeatedStepCount - 1) * 8) / 10, 16, 0, NULL);
                }
                sSendRecvMgr.repeatedStepCount++;
            }
            else
            {
                sSendRecvMgr.nextProcessStep = DOWNLOAD_MART_RECEIVE_DATA;
                sSendRecvMgr.repeatedStepCount = 0;
            }
            break;

        case DOWNLOAD_MART_RECEIVE_DATA:
            sSendRecvMgr.disableChecks = FALSE;
            if (sSendRecvMgr.repeatedStepCount == 0)
            {
                DebugPrintfLevel(MGBA_LOG_DEBUG, "--- DOWNLOAD_MART_RECEIVE_DATA");
                sSendRecvMgr.retryPoint = DOWNLOAD_MART_RECEIVE_DATA;
            }
            configureSendRecvMgrChunked(NET_CONN_RCHF0_REQ, (vu32 *) &gStringVar3[0], 16, NET_CONN_STATE_RECEIVE, DOWNLOAD_MART_FINISH, MINIMUM_CHUNK_SIZE);
            break;

        case DOWNLOAD_MART_FINISH:
        default:
        {
            u8 i;
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- DOWNLOAD_MART_FINISH");
            gSpecialVar_0x8003 = 1;
            for (i = 0; i < DOWNLOAD_MART_SIZE; i++)
            {
                if ((u16) (gStringVar3[i*2] | gStringVar3[(i*2) + 1] << 8) > ITEMS_COUNT)
                    gSpecialVar_0x8003 = 0;
            }
            sSendRecvMgr.state = NET_CONN_STATE_DONE;
            break;
        }

    }

    gTasks[taskId].func = Task_NetworkTaskLoop;
}

static void Task_EndOnlineMartConnection(u8 taskId) 
{
    HideFieldMessageBox();
    NetConnDisableSerial();
    ScriptContext_Enable();

    if (gSpecialVar_0x8003 == 1)
    {
        gStringVar3[12] = 0;
        gStringVar3[13] = 0; 
        CreatePokemartMenu((u16 *) &gStringVar3[0]);
        ScriptContext_Stop();
    }

    DestroyTask(taskId);
}

/**
*   =====================================================================
*   === NET_CONN_EGG_FUNC                                             ===
*   =====================================================================
*/
enum {
    DOWNLOAD_GIFT_EGG_SEND_REQUEST = 0,
    DOWNLOAD_GIFT_EGG_TRANSMIT_REQUEST,
    DOWNLOAD_GIFT_EGG_WAIT_FOR_SERVER,
    DOWNLOAD_GIFT_EGG_RECEIVE_DATA,
    DOWNLOAD_GIFT_EGG_FINISH
};

static void SetupForGiftEggTask();
static void Task_GiftEggCancel(u8 taskId);
static void Task_GiftEggProcess(u8 taskId);
static void Task_EndGiftEggConnection(u8 taskId);


static void SetupForGiftEggTask()
{
    sSendRecvMgr.allowCancel       = TRUE;
    sSendRecvMgr.retriesLeft       = MAX_CONNECTION_RETRIES;
    sSendRecvMgr.onFinish          = Task_EndGiftEggConnection;
    sSendRecvMgr.onCancel          = Task_GiftEggCancel;
    sSendRecvMgr.onProcess         = Task_GiftEggProcess;
    sSendRecvMgr.nextProcessStep   = DOWNLOAD_GIFT_EGG_SEND_REQUEST;
    sSendRecvMgr.disableChecks     = FALSE;
    sSendRecvMgr.repeatedStepCount = 0;
    gSpecialVar_0x8003 = 0;
}

static void Task_GiftEggCancel(u8 taskId)
{
    Task_EndLinkupConnection(taskId);
}

static void Task_GiftEggProcess(u8 taskId)
{
    switch (sSendRecvMgr.nextProcessStep)
    {
        case DOWNLOAD_GIFT_EGG_SEND_REQUEST:
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- DOWNLOAD_GIFT_EGG_SEND_REQUEST");
            gStringVar3[0] = 'G'; gStringVar3[1] = 'E'; gStringVar3[2] = '_'; gStringVar3[3] = '1';
            configureSendRecvMgr(NET_CONN_SCH2_REQ, (vu32 *) &gStringVar3[0], 4, NET_CONN_STATE_SEND, DOWNLOAD_GIFT_EGG_TRANSMIT_REQUEST);
            break;

        case DOWNLOAD_GIFT_EGG_TRANSMIT_REQUEST:
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- DOWNLOAD_GIFT_EGG_TRANSMIT_REQUEST");
            sSendRecvMgr.disableChecks = TRUE; 
            sSendRecvMgr.retryPoint = DOWNLOAD_GIFT_EGG_TRANSMIT_REQUEST;
            CpuFill32(0, &gStringVar3, sizeof(gStringVar3)); 
            configureSendRecvMgr(NET_CONN_TCH2_REQ, 0, 4, NET_CONN_STATE_SEND, DOWNLOAD_GIFT_EGG_WAIT_FOR_SERVER);
            break;

        case DOWNLOAD_GIFT_EGG_WAIT_FOR_SERVER:
            if (sSendRecvMgr.repeatedStepCount == 0)
            {
                DebugPrintfLevel(MGBA_LOG_DEBUG, "--- DOWNLOAD_GIFT_EGG_WAIT_FOR_SERVER");
                LoadMessageBoxAndFrameGfx(0, TRUE);
                VBlankIntrWait(); VBlankIntrWait();
                AddTextPrinterParameterized(0, FONT_NORMAL, sWaitingMessage, 0, 1, 0, NULL);
                sSendRecvMgr.repeatedStepCount++;
            }
            else if (sSendRecvMgr.repeatedStepCount <= 40)
            {
                VBlankIntrWait(); VBlankIntrWait();
                if (sSendRecvMgr.repeatedStepCount % 10 == 0)
                {
                    AddTextPrinterParameterized(0, FONT_NORMAL, sDot, ((sSendRecvMgr.repeatedStepCount - 1) * 8) / 10, 16, 0, NULL);
                }
                sSendRecvMgr.repeatedStepCount++;
            }
            else
            {
                sSendRecvMgr.nextProcessStep = DOWNLOAD_GIFT_EGG_RECEIVE_DATA;
                sSendRecvMgr.repeatedStepCount = 0;
            }
            break;

        case DOWNLOAD_GIFT_EGG_RECEIVE_DATA:
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- DOWNLOAD_GIFT_EGG_RECEIVE_DATA");
            sSendRecvMgr.disableChecks = FALSE;
            if (sSendRecvMgr.repeatedStepCount == 0)
            {
                DebugPrintfLevel(MGBA_LOG_DEBUG, "--- DOWNLOAD_MART_RECEIVE_DATA");
                sSendRecvMgr.retryPoint = DOWNLOAD_MART_RECEIVE_DATA;
            }
            configureSendRecvMgrChunked(NET_CONN_RCHF0_REQ, (vu32 *) &gStringVar3[0], 4, NET_CONN_STATE_RECEIVE, DOWNLOAD_GIFT_EGG_FINISH, MINIMUM_CHUNK_SIZE);
            break;

        case DOWNLOAD_GIFT_EGG_FINISH:
        default:
            DebugPrintfLevel(MGBA_LOG_DEBUG, "--- DOWNLOAD_GIFT_EGG_FINISH");
            sSendRecvMgr.state = NET_CONN_STATE_DONE;
            gSpecialVar_0x8003 = (u16) (gStringVar3[0] | gStringVar3[1] << 8);
            gSpecialVar_0x8005 = (u16) (gStringVar3[2] | gStringVar3[3] << 8);
            
            if (gSpecialVar_0x8003 > SPECIES_EGG) 
            {
                gSpecialVar_0x8003 = 0;
            }
            if (gSpecialVar_0x8005 >= MOVES_COUNT)
            {
                gSpecialVar_0x8005 = MOVE_NONE;
            }
            break;
    }

    gTasks[taskId].func = Task_NetworkTaskLoop;
}

static void Task_EndGiftEggConnection(u8 taskId) 
{
    NetConnDisableSerial();
    StopFieldMessage();
    ScriptContext_Enable();
    DestroyTask(taskId);
}