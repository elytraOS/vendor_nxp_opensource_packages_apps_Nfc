/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/******************************************************************************
 *
 *  The original Work has been changed by NXP Semiconductors.
 *
 *  Copyright (C) 2015 NXP Semiconductors
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/
/*
 *  Communicate with secure elements that are attached to the NFC
 *  controller.
 */
#include <semaphore.h>
#include <errno.h>
#include <ScopedLocalRef.h>
#include "_OverrideLog.h"
#include "SecureElement.h"
#include "config.h"
#include "PowerSwitch.h"
#include "JavaClassConstants.h"
#include "nfc_api.h"
#include "phNxpConfig.h"
#include "PeerToPeer.h"
#include "DataQueue.h"
#include "TransactionController.h"

#if(NXP_EXTNS == TRUE)
#include "RoutingManager.h"

#include <signal.h>
#include <sys/types.h>
extern "C"{
#include "nfa_api.h"
}
#endif
#define ALOGV ALOGD
/*****************************************************************************
**
** public variables
**
*****************************************************************************/
static int gSEId = -1;     // secure element ID to use in connectEE(), -1 means not set
static int gGatePipe = -1; // gate id or static pipe id to use in connectEE(), -1 means not set
static bool gUseStaticPipe = false;    // if true, use gGatePipe as static pipe id.  if false, use as gate id
extern bool gTypeB_listen;
bool gReaderNotificationflag = false;
bool hold_the_transceive = false;
int dual_mode_current_state=0;
nfc_jni_native_data* mthreadnative;
#if (NXP_EXTNS == TRUE)
nfcee_disc_state sNfcee_disc_state;
extern uint8_t nfcee_swp_discovery_status;
extern int32_t gSeDiscoverycount;
static void rfFeildEventTimeoutCallback(union sigval);
extern Rdr_req_ntf_info_t swp_rdr_req_ntf_info ;
pthread_t passiveListenEnable_thread;
void *passiveListenEnableThread(void *arg);
bool ceTransactionPending = false;
static uint8_t passiveListenState = 0x00;
static bool isTransceiveOngoing = false;
static void passiveListenDisablecallBack(union sigval);
#endif
namespace android
{
    extern void startRfDiscovery (bool isStart);
    extern void setUiccIdleTimeout (bool enable);
    extern bool isDiscoveryStarted();
    extern void requestFwDownload();
    extern int getScreenState();
    extern void checkforNfceeConfig(uint8_t type);
#if (NXP_EXTNS == TRUE)
    extern bool nfcManager_checkNfcStateBusy();
#endif
    extern bool isp2pActivated();
    extern SyncEvent sNfaSetConfigEvent;
    extern tNFA_STATUS EmvCo_dosetPoll(jboolean enable);
    extern tNFA_STATUS ResetEseSession();
    extern void start_timer_msec(struct timeval  *start_tv);
    extern long stop_timer_getdifference_msec(struct timeval  *start_tv, struct timeval  *stop_tv);
    extern bool nfcManager_isNfcActive();
    extern bool nfcManager_isNfcDisabling();
#if(NXP_EXTNS == TRUE)
    extern int gMaxEERecoveryTimeout;
    extern uint8_t nfcManager_getNfcState();
#endif
}
#if(NXP_EXTNS == TRUE)
    static    uint32_t        nfccStandbytimeout;        // timeout for secelem standby mode detection
    int       active_ese_reset_control = 0;
    bool      hold_wired_mode = false;
    static    nfcc_standby_operation_t standby_state = STANDBY_MODE_ON;
    SyncEvent mWiredModeHoldEvent;
    static int gWtxCount = 0;
    static void NFCC_StandbyModeTimerCallBack (union sigval);
    /*  hold the transceive flag should be set when the prio session is actrive/about to active*/
    /*  Event used to inform the prio session end and transceive resume*/
        SyncEvent sSPIPrioSessionEndEvent;
    SyncEvent sSPISignalHandlerEvent;
    SyncEvent sSPIForceEnableDWPEvent;
    SyncEvent sSPISVDDSyncOnOffEvent;
    int       spiDwpSyncState = STATE_IDLE;
    void      *spiEventHandlerThread(void *arg);
    Mutex     mSPIEvtMutex;
    DataQueue gSPIEvtQueue;
    volatile  uint16_t  usSPIActEvent = 0;
    pthread_t spiEvtHandler_thread;
    bool createSPIEvtHandlerThread();
    void releaseSPIEvtHandlerThread();
    static void nfaVSC_SVDDSyncOnOff(bool type);
    static void nfaVSC_ForceDwpOnOff(bool type);
#endif
    SyncEvent mDualModeEvent;
    static void setSPIState(bool mState);
//////////////////////////////////////////////
//////////////////////////////////////////////
#if(NXP_EXTNS == TRUE)
#define NFC_NUM_INTERFACE_MAP 3
#define NFC_SWP_RD_NUM_INTERFACE_MAP 1
#define STATIC_PIPE_0x19 0x19 //PN54X Gemalto's proprietary static pipe
#define STATIC_PIPE_0x70 0x70 //Broadcom's proprietary static pipe
uint8_t  SecureElement::mStaticPipeProp;

static const tNCI_DISCOVER_MAPS nfc_interface_mapping_default[NFC_NUM_INTERFACE_MAP] =
{
        /* Protocols that use Frame Interface do not need to be included in the interface mapping */
        {
                NCI_PROTOCOL_ISO_DEP,
                NCI_INTERFACE_MODE_POLL_N_LISTEN,
                NCI_INTERFACE_ISO_DEP
        }
        ,
        {
                NCI_PROTOCOL_NFC_DEP,
                NCI_INTERFACE_MODE_POLL_N_LISTEN,
                NCI_INTERFACE_NFC_DEP
        }
        ,
        {
                NCI_PROTOCOL_MIFARE,
                NCI_INTERFACE_MODE_POLL,
                NCI_INTERFACE_MIFARE
        }
};
static const tNCI_DISCOVER_MAPS nfc_interface_mapping_uicc[NFC_SWP_RD_NUM_INTERFACE_MAP] =
{
        /* Protocols that use Frame Interface do not need to be included in the interface mapping */
        {
                NCI_PROTOCOL_ISO_DEP,
                NCI_INTERFACE_MODE_POLL,
                NCI_INTERFACE_UICC_DIRECT_STAT
        }

};

static const tNCI_DISCOVER_MAPS nfc_interface_mapping_ese[NFC_SWP_RD_NUM_INTERFACE_MAP] =
{
        /* Protocols that use Frame Interface do not need to be included in the interface mapping */
        {
                NCI_PROTOCOL_ISO_DEP,
                NCI_INTERFACE_MODE_POLL,
                NCI_INTERFACE_ESE_DIRECT_STAT
        }

};

/*******************************************************************************
**
** Function:        startStopSwpReaderProc
**
** Description:     handle timeout
**
** Returns:         None
**
*******************************************************************************/
static void startStopSwpReaderProc (union sigval)
{
    if(!nfcFL.nfcNxpEse || !nfcFL.eseFL._ESE_ETSI_READER_ENABLE) {
        ALOGV("%s: nfcNxpEse or ETSI_READER not enabled. Returning", __func__);
        return;
    }
    ALOGV("%s: Timeout!!!", __func__);
    JNIEnv* e = NULL;
    int disc_ntf_timeout = 10;

        ScopedAttach attach(RoutingManager::getInstance().mNativeData->vm, &e);
        if (e == NULL)
        {
            ALOGE("%s: jni env is null", __func__);
            return;
        }
        GetNumValue ( NAME_NFA_DM_DISC_NTF_TIMEOUT, &disc_ntf_timeout, sizeof ( disc_ntf_timeout ) );

        e->CallVoidMethod (RoutingManager::getInstance().mNativeData->manager, android::gCachedNfcManagerNotifyETSIReaderModeSwpTimeout,disc_ntf_timeout);

}
void SecureElement::discovery_map_cb (tNFC_DISCOVER_EVT event, tNFC_DISCOVER *p_data)
{
    (void)event;
    (void)p_data;
    SyncEventGuard guard (sSecElem.mDiscMapEvent);
//    ALOGV("discovery_map_cb; status=%u", eventData->ee_register);
    sSecElem.mDiscMapEvent.notifyOne();
}
#endif


SecureElement SecureElement::sSecElem;
const char* SecureElement::APP_NAME = "nfc_jni";
const uint16_t ACTIVE_SE_USE_ANY = 0xFFFF;

/*******************************************************************************
**
** Function:        SecureElement
**
** Description:     Initialize member variables.
**
** Returns:         None
**
*******************************************************************************/
SecureElement::SecureElement ()
:   mActiveEeHandle (NFA_HANDLE_INVALID),
#if(NXP_EXTNS == TRUE)
    mIsWiredModeOpen(false),
    mIsAllowWiredInDesfireMifareCE(false),
    mRfFieldEventTimeout(0),
    mIsIntfRstEnabled (false),
    mETSI12InitStatus (NFA_STATUS_FAILED),
    mModeSetInfo(NFA_STATUS_FAILED),
    meseETSI12Recovery(false),
    meSESessionIdOk (false),
    mPwrCmdstatus(NFA_STATUS_FAILED),
    mNfccPowerMode(0),
    mPassiveListenCnt(0),
    mPassiveListenTimeout(0),
#endif
    mDestinationGate (4), //loopback gate
    mNfaHciHandle (NFA_HANDLE_INVALID),
    mNativeData (NULL),
    mIsInit (false),
    mActualNumEe (0),
    mNumEePresent(0),
    mbNewEE (true),   // by default we start w/thinking there are new EE
    mNewPipeId (0),
    mNewSourceGate (0),
    mActiveSeOverride(ACTIVE_SE_USE_ANY),
    mCommandStatus (NFA_STATUS_OK),
    mIsPiping (false),
    mCurrentRouteSelection (NoRoute),
    mActualResponseSize(0),
    mAtrInfolen (0),
    mUseOberthurWarmReset (false),
    mActivatedInListenMode (false),
    mOberthurWarmResetCommand (3),
    mGetAtrRspwait (false),
    mRfFieldIsOn(false),
    mTransceiveWaitOk(false),
    mWmMaxWtxCount(0)
{
    memset (&mEeInfo, 0, nfcFL.nfccFL._NFA_EE_MAX_EE_SUPPORTED *sizeof(tNFA_EE_INFO));
    memset (&mUiccInfo, 0, sizeof(mUiccInfo));
    memset (&mHciCfg, 0, sizeof(mHciCfg));
    memset (mAidForEmptySelect, 0, sizeof(mAidForEmptySelect));
    memset (&mLastRfFieldToggle, 0, sizeof(mLastRfFieldToggle));
    memset (mAtrInfo, 0, sizeof( mAtrInfo));
    memset (&mNfceeData_t, 0, sizeof(mNfceeData_t));
}


/*******************************************************************************
**
** Function:        ~SecureElement
**
** Description:     Release all resources.
**
** Returns:         None
**
*******************************************************************************/
SecureElement::~SecureElement ()
{
}


/*******************************************************************************
**
** Function:        getInstance
**
** Description:     Get the SecureElement singleton object.
**
** Returns:         SecureElement object.
**
*******************************************************************************/
SecureElement& SecureElement::getInstance()
{
    return sSecElem;
}


/*******************************************************************************
**
** Function:        setActiveSeOverride
**
** Description:     Specify which secure element to turn on.
**                  activeSeOverride: ID of secure element
**
** Returns:         None
**
*******************************************************************************/
void SecureElement::setActiveSeOverride(uint8_t activeSeOverride)
{
    ALOGV("SecureElement::setActiveSeOverride, seid=0x%X", activeSeOverride);
    mActiveSeOverride = activeSeOverride;
}


/*******************************************************************************
**
** Function:        initialize
**
** Description:     Initialize all member variables.
**                  native: Native data.
**
** Returns:         True if ok.
**
*******************************************************************************/
bool SecureElement::initialize (nfc_jni_native_data* native)
{
    static const char fn [] = "SecureElement::initialize";
    tNFA_STATUS nfaStat;
    unsigned long num = 0;
    unsigned long retValue;

    ALOGV("%s: enter", fn);
    if (GetNumValue("NFA_HCI_DEFAULT_DEST_GATE", &num, sizeof(num)))
        mDestinationGate = num;
    ALOGV("%s: Default destination gate: 0x%X", fn, mDestinationGate);

    mStaticPipeProp = nfcFL.nfccFL._GEMALTO_SE_SUPPORT ? STATIC_PIPE_0x19 : STATIC_PIPE_0x70;
    // active SE, if not set active all SEs, use the first one.
    if (GetNumValue("ACTIVE_SE", &num, sizeof(num)))
    {
        mActiveSeOverride = num;
        ALOGV("%s: Active SE override: 0x%X", fn, mActiveSeOverride);
    }
#if(NXP_EXTNS == TRUE)
    if(nfcFL.nfcNxpEse) {
        if(nfcFL.eseFL._NFCC_ESE_UICC_CONCURRENT_ACCESS_PROTECTION) {
            if (GetNxpNumValue(NAME_NXP_NFCC_PASSIVE_LISTEN_TIMEOUT, &mPassiveListenTimeout, sizeof(mPassiveListenTimeout)) == false)
            {
                mPassiveListenTimeout = 2500;
                ALOGV("%s: NFCC Passive Listen Disable timeout =%ld", fn, mPassiveListenTimeout);
            }
            ALOGV("%s: NFCC Passive Listen Disable timeout =%ld", fn, mPassiveListenTimeout);
        }
        if (GetNxpNumValue(NAME_NXP_NFCC_STANDBY_TIMEOUT, &nfccStandbytimeout, sizeof(nfccStandbytimeout)) == false)
        {
            nfccStandbytimeout = 20000;
        }
        ALOGV("%s: NFCC standby mode timeout =0x%lx", fn, nfccStandbytimeout);
        if(nfccStandbytimeout > 0 && nfccStandbytimeout < 5000 )
        {
            nfccStandbytimeout = 5000;
        }
        else if (nfccStandbytimeout > 20000)
        {
            nfccStandbytimeout = 20000;
        }
        standby_state = STANDBY_MODE_ON;
        if(nfcFL.eseFL._ESE_JCOP_DWNLD_PROTECTION || nfcFL.eseFL._ESE_SVDD_SYNC
                || nfcFL.eseFL._ESE_DWP_SPI_SYNC_ENABLE) {
            spiDwpSyncState = STATE_IDLE;
        }
        if (GetNxpNumValue(NAME_NXP_WM_MAX_WTX_COUNT, &mWmMaxWtxCount, sizeof(mWmMaxWtxCount)) == false || (mWmMaxWtxCount == 0))
            mWmMaxWtxCount = 9000;
        ALOGD ("%s: NFCC Wired Mode Max WTX Count =%ld", fn, mWmMaxWtxCount);
        dual_mode_current_state = SPI_DWPCL_NOT_ACTIVE;
        hold_the_transceive = false;
        active_ese_reset_control = 0;
        hold_wired_mode = false;
        mlistenDisabled = false;
        mIsExclusiveWiredMode = false;

        if (GetNxpNumValue(NAME_NXP_NFCC_RF_FIELD_EVENT_TIMEOUT, &mRfFieldEventTimeout, sizeof(mRfFieldEventTimeout)) == false)
        {
            mRfFieldEventTimeout = 2000;
            ALOGV("%s: RF Field Off event timeout =%ld", fn, mRfFieldEventTimeout);
        }

        if (GetNxpNumValue(NAME_NXP_ALLOW_WIRED_IN_MIFARE_DESFIRE_CLT, &retValue, sizeof(retValue)) == false)
        {
            mIsAllowWiredInDesfireMifareCE = false;
        }
        else
        {
            mIsAllowWiredInDesfireMifareCE = (retValue == 0x00)? false: true;
        }

        if(nfcFL.eseFL._WIRED_MODE_STANDBY) {
            if (GetNxpNumValue (NAME_NXP_ESE_POWER_DH_CONTROL, (void*)&num, sizeof(num)) == false)
            {
                mNfccPowerMode = 0;
            }
            else
            {
                mNfccPowerMode = (uint8_t)num;
            }
        }

        retValue = 0;
        if (GetNxpNumValue(NAME_NXP_DWP_INTF_RESET_ENABLE, &retValue, sizeof(retValue)))
        {
            mIsIntfRstEnabled = (retValue == 0x00)? false: true;
        }
    }
#endif
    /*
     * Since NXP doesn't support OBERTHUR RESET COMMAND, Hence commented
    if (GetNumValue("OBERTHUR_WARM_RESET_COMMAND", &num, sizeof(num)))
    {
        mUseOberthurWarmReset = true;
        mOberthurWarmResetCommand = (uint8_t) num;
    }*/

    mActiveEeHandle = NFA_HANDLE_INVALID;
    mNfaHciHandle = NFA_HANDLE_INVALID;

    mNativeData     = native;
    mthreadnative    = native;
    mActualNumEe    = nfcFL.nfccFL._NFA_EE_MAX_EE_SUPPORTED;;
    mbNewEE         = true;
    mNewPipeId      = 0;
    mNewSourceGate  = 0;
    mRfFieldIsOn    = false;
    mActivatedInListenMode = false;
    mCurrentRouteSelection = NoRoute;
    if(nfcFL.nfcNxpEse && nfcFL.eseFL._NFCC_ESE_UICC_CONCURRENT_ACCESS_PROTECTION) {
        mPassiveListenEnabled = true;
        meseUiccConcurrentAccess = false;
    }

    memset (mEeInfo, 0, sizeof(mEeInfo));
    memset (&mUiccInfo, 0, sizeof(mUiccInfo));
    memset (&mHciCfg, 0, sizeof(mHciCfg));
    mUsedAids.clear ();
    memset(mAidForEmptySelect, 0, sizeof(mAidForEmptySelect));
#if(NXP_EXTNS == TRUE)
    mIsWiredModeBlocked = false;
#endif

    // if no SE is to be used, get out.
    if (mActiveSeOverride == 0)
    {
        ALOGV("%s: No SE; No need to initialize SecureElement", fn);
        return (false);
    }

    // Get Fresh EE info.
    if (! getEeInfo())
        return (false);
    initializeEeHandle();
    // If the controller has an HCI Network, register for that
    //for (size_t xx = 0; xx < mActualNumEe; xx++)
    for (size_t xx = 0; xx < MAX_NUM_EE; xx++)
    {

        if((!nfcFL.nfccFL._GEMALTO_SE_SUPPORT && mEeInfo[xx].ee_handle != EE_HANDLE_0xF4)
           || (nfcFL.nfccFL._GEMALTO_SE_SUPPORT && (((mEeInfo[xx].ee_interface[0] == NCI_NFCEE_INTERFACE_HCI_ACCESS)
                 && (mEeInfo[xx].ee_status == NFC_NFCEE_STATUS_ACTIVE)) || (NFA_GetNCIVersion() == NCI_VERSION_2_0))))
            {
                ALOGV("%s: Found HCI network, try hci register", fn);

                SyncEventGuard guard (mHciRegisterEvent);

                nfaStat = NFA_HciRegister (const_cast<char*>(APP_NAME), nfaHciCallback, true);
                if (nfaStat != NFA_STATUS_OK)
                {
                    ALOGE("%s: fail hci register; error=0x%X", fn, nfaStat);
                    return (false);
                }
                mHciRegisterEvent.wait();
                break;
            }
    }
    GetStrValue(NAME_AID_FOR_EMPTY_SELECT, (char*)&mAidForEmptySelect[0], sizeof(mAidForEmptySelect));
    mIsInit = true;
    ALOGV("%s: exit", fn);
    return (true);
}
#if((NXP_EXTNS == TRUE))
/*******************************************************************************
 **
 ** Function:        updateEEStatus
 **
 ** Description:     updateEEStatus
 **                  Reads EE related information from libnfc
 **                  and updates in JNI
 **
 ** Returns:         True if ok.
 **
*******************************************************************************/
bool SecureElement::updateEEStatus ()
{
    tNFA_STATUS nfaStat;
    mActualNumEe    = nfcFL.nfccFL._NFA_EE_MAX_EE_SUPPORTED;
    ALOGV("%s: Enter", __func__);

    if (! getEeInfo())
        return (false);

    // If the controller has an HCI Network, register for that
    for (size_t xx = 0; xx < mActualNumEe; xx++)
    {
        if((!nfcFL.nfccFL._GEMALTO_SE_SUPPORT && (mEeInfo[xx].ee_handle != EE_HANDLE_0xF4 ))
            || (nfcFL.nfccFL._GEMALTO_SE_SUPPORT && (((mEeInfo[xx].ee_interface[0] == NCI_NFCEE_INTERFACE_HCI_ACCESS))
            || (NFA_GetNCIVersion() == NCI_VERSION_2_0))))
         {
                ALOGV("%s: Found HCI network, try hci register", __func__);

                SyncEventGuard guard (mHciRegisterEvent);

                nfaStat = NFA_HciRegister (const_cast<char*>(APP_NAME), nfaHciCallback, true);
                if (nfaStat != NFA_STATUS_OK)
                {
                    ALOGE("%s: fail hci register; error=0x%X", __func__, nfaStat);
                    return (false);
                }
                mHciRegisterEvent.wait();
                break;
            }
    }

    GetStrValue(NAME_AID_FOR_EMPTY_SELECT, (char*)&mAidForEmptySelect[0], sizeof(mAidForEmptySelect));
    ALOGV("%s: exit", __func__);
    return (true);
}

/*******************************************************************************
 **
 ** Function:        isTeckInfoReceived
 **
 ** Description:     isTeckInfoReceived
 **                  Checks if discovery_req_ntf received
 **                  for a given EE
 **
 ** Returns:         True if discovery_req_ntf is received.
 **
 *******************************************************************************/
bool SecureElement::isTeckInfoReceived (uint16_t eeHandle)
{
    ALOGV("%s: enter", __func__);
    bool stat = false;
    if (! getEeInfo())
    {
        ALOGE("%s: No updated eeInfo available", __func__);
        stat = false;
    }
    else
    {
        for (uint8_t xx = 0; xx < mActualNumEe; xx++)
        {
            if ((mEeInfo[xx].ee_handle == eeHandle) &&
                    ((mEeInfo[xx].la_protocol != 0x00) || (mEeInfo[xx].lb_protocol != 0x00) ||
                            (mEeInfo[xx].lf_protocol != 0x00) || (mEeInfo[xx].lbp_protocol != 0x00)))
            {
                stat = true;
                break;
            }
        }
    }
    ALOGV("%s: stat : 0x%02x", __func__,stat);
    return stat;
}
#endif
/*******************************************************************************
**
** Function:        finalize
**
** Description:     Release all resources.
**
** Returns:         None
**
*******************************************************************************/
void SecureElement::finalize ()
{
    static const char fn [] = "SecureElement::finalize";
    ALOGV("%s: enter", fn);

/*    if (mNfaHciHandle != NFA_HANDLE_INVALID)
        NFA_HciDeregister (const_cast<char*>(APP_NAME));*/
#if(NXP_EXTNS == TRUE)
    if(nfcFL.nfcNxpEse) {
        NfccStandByOperation(STANDBY_TIMER_STOP);
    }
#endif

    mNfaHciHandle = NFA_HANDLE_INVALID;
    mNativeData   = NULL;
    mIsInit       = false;
    mActualNumEe  = 0;
    mNumEePresent = 0;
    mNewPipeId    = 0;
    mNewSourceGate = 0;
    mIsPiping = false;
    memset (mEeInfo, 0, sizeof(mEeInfo));
    memset (&mUiccInfo, 0, sizeof(mUiccInfo));

    ALOGV("%s: exit", fn);
}


/*******************************************************************************
**
** Function:        getEeInfo
**
** Description:     Get latest information about execution environments from stack.
**
** Returns:         True if at least 1 EE is available.
**
*******************************************************************************/
bool SecureElement::getEeInfo()
{
    static const char fn [] = "SecureElement::getEeInfo";
    ALOGV("%s: enter; mbNewEE=%d, mActualNumEe=%d", fn, mbNewEE, mActualNumEe);
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;

    /*Reading latest eEinfo  incase it is updated*/
#if(NXP_EXTNS == TRUE)
    mbNewEE = true;
    mNumEePresent = 0;
#endif
    // If mbNewEE is true then there is new EE info.
    if (mbNewEE)
    {

#if(NXP_EXTNS == TRUE)
        memset (&mNfceeData_t, 0, sizeof (mNfceeData_t));
#endif

        mActualNumEe = nfcFL.nfccFL._NFA_EE_MAX_EE_SUPPORTED;

        if ((nfaStat = NFA_EeGetInfo (&mActualNumEe, mEeInfo)) != NFA_STATUS_OK)
        {
            ALOGE("%s: fail get info; error=0x%X", fn, nfaStat);
            mActualNumEe = 0;
        }
        else
        {
            mbNewEE = false;

            ALOGV("%s: num EEs discovered: %u", fn, mActualNumEe);
            if (mActualNumEe != 0)
            {
                for (uint8_t xx = 0; xx < mActualNumEe; xx++)
                {
                    if (mEeInfo[xx].ee_interface[0] != NCI_NFCEE_INTERFACE_HCI_ACCESS)
                        mNumEePresent++;

                    ALOGV("%s: EE[%u] Handle: 0x%04x  Status: %s  Num I/f: %u: (0x%02x, 0x%02x)  Num TLVs: %u, Tech : (LA:0x%02x, LB:0x%02x, "
                            "LF:0x%02x, LBP:0x%02x)", fn, xx, mEeInfo[xx].ee_handle, eeStatusToString(mEeInfo[xx].ee_status),
                            mEeInfo[xx].num_interface, mEeInfo[xx].ee_interface[0], mEeInfo[xx].ee_interface[1], mEeInfo[xx].num_tlvs,
                            mEeInfo[xx].la_protocol, mEeInfo[xx].lb_protocol, mEeInfo[xx].lf_protocol, mEeInfo[xx].lbp_protocol);

#if(NXP_EXTNS == TRUE)
                    mNfceeData_t.mNfceeHandle[xx] = mEeInfo[xx].ee_handle;
                    mNfceeData_t.mNfceeStatus[xx] = mEeInfo[xx].ee_status;
#endif
                    for (size_t yy = 0; yy < mEeInfo[xx].num_tlvs; yy++)
                    {
                        ALOGV("%s: EE[%u] TLV[%lu]  Tag: 0x%02x  Len: %u  Values[]: 0x%02x  0x%02x  0x%02x ...",
                                fn, xx, yy, mEeInfo[xx].ee_tlv[yy].tag, mEeInfo[xx].ee_tlv[yy].len, mEeInfo[xx].ee_tlv[yy].info[0],
                                mEeInfo[xx].ee_tlv[yy].info[1], mEeInfo[xx].ee_tlv[yy].info[2]);
                    }
                }
            }
        }
    }
    ALOGV("%s: exit; mActualNumEe=%d, mNumEePresent=%d", fn, mActualNumEe,mNumEePresent);

#if(NXP_EXTNS == TRUE)
    mNfceeData_t.mNfceePresent = mNumEePresent;
#endif

    return (mActualNumEe != 0);
}
/*******************************************************************************
**
** Function:        activateAllNfcee
**
** Description:     Activate all Nfcee present/connected to NFCC.
**
** Returns:         True/False based on  activation status.
**
*******************************************************************************/
bool SecureElement::activateAllNfcee()
{
    static const char fn [] = "SecureElement::activateAllNfcee";
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    int numActivatedEe = 0;
    uint8_t numEe = SecureElement::MAX_NUM_EE;
    tNFA_EE_INFO mEeInfo[numEe];

    if((nfaStat = NFA_AllEeGetInfo(&numEe, mEeInfo)) != NFA_STATUS_OK)
    {
        ALOGV("%s failed to get info, error = 0x%X ",__func__, nfaStat);
        mActualNumEe = 0;
        return false;
    }
    {
        if (numEe != 0)
        {
            for (uint8_t xx = 0; xx < numEe; xx++)
            {
                tNFA_EE_INFO& eeItem = mEeInfo[xx];
                ALOGV("%s: set EE mode activate; h=0x%X status : %d interface : 0x%X", fn, eeItem.ee_handle, eeItem.ee_status, eeItem.ee_interface[0]);
                if (eeItem.ee_status != NFC_NFCEE_STATUS_INACTIVE || eeItem.ee_interface[0] == NCI_NFCEE_INTERFACE_HCI_ACCESS)
                {
                    ALOGV("%s: h=0x%X already activated", fn, eeItem.ee_handle);
                    numActivatedEe++;
                    continue;
                }
                else
                {
                    ALOGV("%s: set EE mode activate; h=0x%X", fn, eeItem.ee_handle);
                    if ((nfaStat = SecElem_EeModeSet (eeItem.ee_handle, NFA_EE_MD_ACTIVATE)) == NFA_STATUS_OK)
                    {
                        if (eeItem.ee_status == NFC_NFCEE_STATUS_ACTIVE)
                            numActivatedEe++;
                    }
                }
             }
         }
     }
    return (nfaStat==NFA_STATUS_OK?true:false);
}
/*******************************************************************************
**
** Function         TimeDiff
**
** Description      Computes time difference in milliseconds.
**
** Returns          Time difference in milliseconds
**
*******************************************************************************/
static uint32_t TimeDiff(timespec start, timespec end)
{
    end.tv_sec -= start.tv_sec;
    end.tv_nsec -= start.tv_nsec;

    if (end.tv_nsec < 0) {
        end.tv_nsec += 10e8;
        end.tv_sec -=1;
    }

    return (end.tv_sec * 1000) + (end.tv_nsec / 10e5);
}

/*******************************************************************************
**
** Function:        isRfFieldOn
**
** Description:     Can be used to determine if the SE is in an RF field
**
** Returns:         True if the SE is activated in an RF field
**
*******************************************************************************/
bool SecureElement::isRfFieldOn() {
    AutoMutex mutex(mMutex);
    if (mRfFieldIsOn) {
        return true;
    }
    struct timespec now;
    int ret = clock_gettime(CLOCK_MONOTONIC, &now);
    if (ret == -1) {
        ALOGE("isRfFieldOn(): clock_gettime failed");
        return false;
    }
    if (TimeDiff(mLastRfFieldToggle, now) < 50) {
        // If it was less than 50ms ago that RF field
        // was turned off, still return ON.
        return true;
    } else {
        return false;
    }
}


/*******************************************************************************
**
** Function:        setEseListenTechMask
**
** Description:     Can be used to force ESE to only listen the specific
**                  Technologies.
**                  NFA_TECHNOLOGY_MASK_A       0x01
**                  NFA_TECHNOLOGY_MASK_B       0x02
**
** Returns:         True if listening is configured.
**
*******************************************************************************/
bool SecureElement::setEseListenTechMask(uint8_t tech_mask ) {

    static const char fn [] = "SecureElement::setEseListenTechMask";
    tNFA_STATUS nfaStat;

    ALOGV("%s: enter", fn);

    if (!mIsInit)
    {
        ALOGE("%s: not init", fn);
        return false;
    }

    {
        SyncEventGuard guard (SecureElement::getInstance().mEseListenEvent);
        nfaStat = NFA_CeConfigureEseListenTech (EE_HANDLE_0xF3, (0x00));
        if(nfaStat == NFA_STATUS_OK)
        {
            SecureElement::getInstance().mEseListenEvent.wait ();
            return true;
        }
        else
            ALOGE("fail to stop ESE listen");
    }

    {
        SyncEventGuard guard (SecureElement::getInstance().mEseListenEvent);
        nfaStat = NFA_CeConfigureEseListenTech (EE_HANDLE_0xF3, (tech_mask));
        if(nfaStat == NFA_STATUS_OK)
        {
            SecureElement::getInstance().mEseListenEvent.wait ();
            return true;
        }
        else
            ALOGE("fail to start ESE listen");
    }

    return false;
}

/*******************************************************************************
**
** Function:        isActivatedInListenMode
**
** Description:     Can be used to determine if the SE is activated in listen mode
**
** Returns:         True if the SE is activated in listen mode
**
*******************************************************************************/
bool SecureElement::isActivatedInListenMode() {
    return mActivatedInListenMode;
}

/*******************************************************************************
**
** Function:        getListOfEeHandles
**
** Description:     Get the list of handles of all execution environments.
**                  e: Java Virtual Machine.
**
** Returns:         List of handles of all execution environments.
**
*******************************************************************************/
jintArray SecureElement::getListOfEeHandles (JNIEnv* e)
{
    static const char fn [] = "SecureElement::getListOfEeHandles";
    ALOGV("%s: enter", fn);
    if (mNumEePresent == 0)
        return NULL;

    if (!mIsInit)
    {
        ALOGE("%s: not init", fn);
        return (NULL);
    }

    // Get Fresh EE info.
    if (! getEeInfo())
        return (NULL);

    jintArray list = e->NewIntArray (mNumEePresent); //allocate array
    jint jj = 0;
    int cnt = 0;
    for (int ii = 0; ii < mActualNumEe && cnt < mNumEePresent; ii++)
    {
        ALOGV("%s: %u = 0x%X", fn, ii, mEeInfo[ii].ee_handle);
        if (mEeInfo[ii].ee_interface[0] == NCI_NFCEE_INTERFACE_HCI_ACCESS)
        {
            continue;
        }

        jj = mEeInfo[ii].ee_handle & ~NFA_HANDLE_GROUP_EE;

        ALOGV("%s: Handle %u = 0x%X", fn, ii, jj);

        jj = getGenericEseId(jj);

        ALOGV("%s: Generic id %u = 0x%X", fn, ii, jj);
        e->SetIntArrayRegion (list, cnt++, 1, &jj);
    }
    ALOGV("%s: exit", fn);
    return list;
}

/*******************************************************************************
**
** Function:        getActiveSecureElementList
**
** Description:     Get the list of Activated Secure elements.
**                  e: Java Virtual Machine.
**
** Returns:         List of Activated Secure elements.
**
*******************************************************************************/
jintArray SecureElement::getActiveSecureElementList (JNIEnv* e)
{
    uint8_t num_of_nfcee_present = 0;
    tNFA_HANDLE nfcee_handle[MAX_NFCEE];
    tNFA_EE_STATUS nfcee_status[MAX_NFCEE];
    jint seId = 0;
    int cnt = 0;
    int i;
    ALOGV("%s: ENTER", __func__);

    if (! getEeInfo())
        return (NULL);

    num_of_nfcee_present = mNfceeData_t.mNfceePresent;
    ALOGV("num_of_nfcee_present = %d",num_of_nfcee_present);

    jintArray list = e->NewIntArray (num_of_nfcee_present); //allocate array

    for(i = 1; i<= num_of_nfcee_present ; i++)
    {
        nfcee_handle[i] = mNfceeData_t.mNfceeHandle[i];
        nfcee_status[i] = mNfceeData_t.mNfceeStatus[i];

        if(nfcee_handle[i] == EE_HANDLE_0xF3 && nfcee_status[i] == NFC_NFCEE_STATUS_ACTIVE)
        {
            seId = getGenericEseId(EE_HANDLE_0xF3 & ~NFA_HANDLE_GROUP_EE);
            ALOGV("eSE  Active");
        }

        if(nfcee_handle[i] == EE_HANDLE_0xF4 && nfcee_status[i] == NFC_NFCEE_STATUS_ACTIVE)
        {
            seId = getGenericEseId(EE_HANDLE_0xF4 & ~NFA_HANDLE_GROUP_EE);
            ALOGV("UICC  Active");
        }

        if(nfcee_handle[i] == EE_HANDLE_0xF8 && nfcee_status[i] == NFC_NFCEE_STATUS_ACTIVE)
        {
            seId = getGenericEseId(EE_HANDLE_0xF8 & ~NFA_HANDLE_GROUP_EE);
            ALOGV("UICC2  Active");
        }

        ALOGV("%s: Generic id %u = 0x%X", __func__, i, seId);
        e->SetIntArrayRegion (list, cnt++, 1, &seId);
    }

    ALOGV("%s: exit", __func__);
    return list;
}


/*******************************************************************************
**
** Function:        activate
**
** Description:     Turn on the secure element.
**                  seID: ID of secure element; 0xF3 or 0xF4.
**
** Returns:         True if ok.
**
*******************************************************************************/
bool SecureElement::activate (jint seID)
{
    static const char fn [] = "SecureElement::activate";
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    int numActivatedEe = 0;

    ALOGV("%s: enter; seID=0x%X", fn, seID);

    tNFA_HANDLE handle = getEseHandleFromGenericId(seID);

    ALOGV("%s: handle=0x%X", fn, handle);

    if (!mIsInit)
    {
        ALOGE("%s: not init", fn);
        return false;
    }

    //if (mActiveEeHandle != NFA_HANDLE_INVALID)
    //{
    //    ALOGV("%s: already active", fn);
    //    return true;
    //}

    // Get Fresh EE info if needed.
    if (! getEeInfo())
    {
        ALOGE("%s: no EE info", fn);
        return false;
    }

    uint16_t overrideEeHandle = 0;
    // If the Active SE is overridden
    if (mActiveSeOverride && (mActiveSeOverride != ACTIVE_SE_USE_ANY))
        overrideEeHandle = NFA_HANDLE_GROUP_EE | mActiveSeOverride;
    else //NXP
        overrideEeHandle = handle;

    ALOGV("%s: override ee h=0x%X", fn, overrideEeHandle );

    if (!nfcFL.nfcNxpEse && mRfFieldIsOn) {
        ALOGE("%s: RF field indication still on, resetting", fn);
        mRfFieldIsOn = false;
    }

    //activate every discovered secure element
    for (int index=0; index < mActualNumEe; index++)
    {
        tNFA_EE_INFO& eeItem = mEeInfo[index];

        if ((eeItem.ee_handle == EE_HANDLE_0xF3) || (eeItem.ee_handle == EE_HANDLE_0xF4) ||
                (nfcFL.nfccFL._NFCC_DYNAMIC_DUAL_UICC && (eeItem.ee_handle == EE_HANDLE_0xF8)))
        {
            if (overrideEeHandle && (overrideEeHandle != eeItem.ee_handle) )
                continue;   // do not enable all SEs; only the override one

            if (eeItem.ee_status != NFC_NFCEE_STATUS_INACTIVE)
            {
                ALOGV("%s: h=0x%X already activated", fn, eeItem.ee_handle);
                numActivatedEe++;
                continue;
            }

            {
                ALOGV("%s: set EE mode activate; h=0x%X", fn, eeItem.ee_handle);
#if(NXP_EXTNS == TRUE)
                if ((nfaStat = SecElem_EeModeSet (eeItem.ee_handle, NFA_EE_MD_ACTIVATE)) == NFA_STATUS_OK)
                {
                    if (eeItem.ee_status == NFC_NFCEE_STATUS_ACTIVE)
                        numActivatedEe++;
                    if(eeItem.ee_handle == EE_HANDLE_0xF3)
                    {
                        SyncEventGuard guard (SecureElement::getInstance().mModeSetNtf);
                        if(SecureElement::getInstance().mModeSetNtf.wait(500) == false)
                        {
                            ALOGE("%s: timeout waiting for setModeNtf", __func__);
                        }
                    }
                }
                else
#endif
                    ALOGE("%s: NFA_EeModeSet failed; error=0x%X", fn, nfaStat);
            }
        }
    } //for
#if (NXP_EXTNS == TRUE)
    mActiveEeHandle = getActiveEeHandle(handle);
#else
    mActiveEeHandle = getDefaultEeHandle();
#endif

    if (mActiveEeHandle == NFA_HANDLE_INVALID)
        ALOGE("%s: ee handle not found", fn);
    ALOGV("%s: exit; active ee h=0x%X", fn, mActiveEeHandle);
    return mActiveEeHandle != NFA_HANDLE_INVALID;
}


/*******************************************************************************
**
** Function:        deactivate
**
** Description:     Turn off the secure element.
**                  seID: ID of secure element; 0xF3 or 0xF4.
**
** Returns:         True if ok.
**
*******************************************************************************/
bool SecureElement::deactivate (jint seID)
{
    static const char fn [] = "SecureElement::deactivate";
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    bool retval = false;

    ALOGV("%s: enter; seID=0x%X, mActiveEeHandle=0x%X", fn, seID, mActiveEeHandle);

    tNFA_HANDLE handle = getEseHandleFromGenericId(seID);

    ALOGV("%s: handle=0x%X", fn, handle);

    if (!mIsInit)
    {
        ALOGE("%s: not init", fn);
        goto TheEnd;
    }

    //if the controller is routing to sec elems or piping,
    //then the secure element cannot be deactivated
    if ((mCurrentRouteSelection == SecElemRoute) || mIsPiping)
    {
        ALOGE("%s: still busy", fn);
        goto TheEnd;
    }

//    if (mActiveEeHandle == NFA_HANDLE_INVALID)
//    {
//        ALOGE("%s: invalid EE handle", fn);
//        goto TheEnd;
//    }

    if (seID == NFA_HANDLE_INVALID)
    {
        ALOGE("%s: invalid EE handle", fn);
        goto TheEnd;
    }

    mActiveEeHandle = NFA_HANDLE_INVALID;

    //NXP
    //deactivate secure element
    for (int index=0; index < mActualNumEe; index++)
    {
        tNFA_EE_INFO& eeItem = mEeInfo[index];

        if (eeItem.ee_handle == handle &&
                ((eeItem.ee_handle == EE_HANDLE_0xF3) || (eeItem.ee_handle == EE_HANDLE_0xF4) ||
                        (nfcFL.nfccFL._NFCC_DYNAMIC_DUAL_UICC && (eeItem.ee_handle == EE_HANDLE_0xF8)))) {
            if (eeItem.ee_status == NFC_NFCEE_STATUS_INACTIVE)
            {
                ALOGV("%s: h=0x%X already deactivated", fn, eeItem.ee_handle);
                break;
            }

            {
                ALOGV("%s: set EE mode activate; h=0x%X", fn, eeItem.ee_handle);
#if(NXP_EXTNS == TRUE)
                if ((nfaStat = SecElem_EeModeSet (eeItem.ee_handle, NFA_EE_MD_DEACTIVATE)) == NFA_STATUS_OK)
                {
                    ALOGV("%s: eeItem.ee_status =0x%X  NFC_NFCEE_STATUS_INACTIVE = %x", fn, eeItem.ee_status, NFC_NFCEE_STATUS_INACTIVE);
                    if (eeItem.ee_status == NFC_NFCEE_STATUS_INACTIVE)
                    {
                        ALOGE("%s: NFA_EeModeSet success; status=0x%X", fn, nfaStat);
                        retval = true;
                    }
                }
                else
#endif
                    ALOGE("%s: NFA_EeModeSet failed; error=0x%X", fn, nfaStat);
            }
        }
    } //for

TheEnd:
    ALOGV("%s: exit; ok=%u", fn, retval);
    return retval;
}


/*******************************************************************************
**
** Function:        notifyTransactionListenersOfAid
**
** Description:     Notify the NFC service about a transaction event from secure element.
**                  aid: Buffer contains application ID.
**                  aidLen: Length of application ID.
**
** Returns:         None
**
*******************************************************************************/
void SecureElement::notifyTransactionListenersOfAid (const uint8_t* aidBuffer, uint8_t aidBufferLen, const uint8_t* dataBuffer, uint32_t dataBufferLen,uint32_t evtSrc)
{
    static const char fn [] = "SecureElement::notifyTransactionListenersOfAid";
    ALOGV("%s: enter; aid len=%u data len=%ld", fn, aidBufferLen, dataBufferLen);

    if (aidBufferLen == 0) {
        return;
    }

    JNIEnv* e = NULL;
    ScopedAttach attach(mNativeData->vm, &e);
    if (e == NULL)
    {
        ALOGE("%s: jni env is null", fn);
        return;
    }

    const uint16_t tlvMaxLen = aidBufferLen + 10;
    uint8_t* tlv = new uint8_t [tlvMaxLen];
    if (tlv == NULL)
    {
        ALOGE("%s: fail allocate tlv", fn);
        return;
    }

    memcpy (tlv, aidBuffer, aidBufferLen);
    uint16_t tlvActualLen = aidBufferLen;

    ScopedLocalRef<jobject> tlvJavaArray(e, e->NewByteArray(tlvActualLen));
    if (tlvJavaArray.get() == NULL)
    {
        ALOGE("%s: fail allocate array", fn);
        goto TheEnd;
    }

    e->SetByteArrayRegion ((jbyteArray)tlvJavaArray.get(), 0, tlvActualLen, (jbyte *)tlv);
    if (e->ExceptionCheck())
    {
        e->ExceptionClear();
        ALOGE("%s: fail fill array", fn);
        goto TheEnd;
    }

    if(dataBufferLen > 0)
    {
        const uint32_t dataTlvMaxLen = dataBufferLen + 10;
        uint8_t* datatlv = new uint8_t [dataTlvMaxLen];
        if (datatlv == NULL)
        {
            ALOGE("%s: fail allocate tlv", fn);
            return;
        }

        memcpy (datatlv, dataBuffer, dataBufferLen);
        uint16_t dataTlvActualLen = dataBufferLen;

        ScopedLocalRef<jobject> dataTlvJavaArray(e, e->NewByteArray(dataTlvActualLen));
        if (dataTlvJavaArray.get() == NULL)
        {
            ALOGE("%s: fail allocate array", fn);
            goto Clean;
        }

        e->SetByteArrayRegion ((jbyteArray)dataTlvJavaArray.get(), 0, dataTlvActualLen, (jbyte *)datatlv);
        if (e->ExceptionCheck())
        {
            e->ExceptionClear();
            ALOGE("%s: fail fill array", fn);
            goto Clean;
        }

        e->CallVoidMethod (mNativeData->manager, android::gCachedNfcManagerNotifyTransactionListeners, tlvJavaArray.get(), dataTlvJavaArray.get(), evtSrc);
        if (e->ExceptionCheck())
        {
            e->ExceptionClear();
            ALOGE("%s: fail notify", fn);
            goto Clean;
        }

     Clean:
        delete [] datatlv;
    }
    else
    {
        e->CallVoidMethod (mNativeData->manager, android::gCachedNfcManagerNotifyTransactionListeners, tlvJavaArray.get(), NULL, evtSrc);
        if (e->ExceptionCheck())
        {
            e->ExceptionClear();
            ALOGE("%s: fail notify", fn);
            goto TheEnd;
        }
    }
TheEnd:
    delete [] tlv;
    ALOGV("%s: exit", fn);
}

/*******************************************************************************
**
** Function:        notifyConnectivityListeners
**
** Description:     Notify the NFC service about a connectivity event from secure element.
**                  evtSrc: source of event UICC/eSE.
**
** Returns:         None
**
*******************************************************************************/
void SecureElement::notifyConnectivityListeners (uint8_t evtSrc)
{
    static const char fn [] = "SecureElement::notifyConnectivityListeners";
    ALOGV("%s: enter; evtSrc =%u", fn, evtSrc);

    JNIEnv* e = NULL;
    ScopedAttach attach(mNativeData->vm, &e);
    if (e == NULL)
    {
        ALOGE("%s: jni env is null", fn);
        return;
    }

    e->CallVoidMethod (mNativeData->manager, android::gCachedNfcManagerNotifyConnectivityListeners,evtSrc);
    if (e->ExceptionCheck())
    {
        e->ExceptionClear();
        ALOGE("%s: fail notify", fn);
        goto TheEnd;
    }

TheEnd:
    ALOGV("%s: exit", fn);
}

/*******************************************************************************
**
** Function:        notifyEmvcoMultiCardDetectedListeners
**
** Description:     Notify the NFC service about a multiple card presented to
**                  Emvco reader.
**
** Returns:         None
**
*******************************************************************************/
void SecureElement::notifyEmvcoMultiCardDetectedListeners ()
{
    static const char fn [] = "SecureElement::notifyEmvcoMultiCardDetectedListeners";
    ALOGV("%s: enter", fn);

    JNIEnv* e = NULL;
    ScopedAttach attach(mNativeData->vm, &e);
    if (e == NULL)
    {
        ALOGE("%s: jni env is null", fn);
        return;
    }

    e->CallVoidMethod (mNativeData->manager, android::gCachedNfcManagerNotifyEmvcoMultiCardDetectedListeners);
    if (e->ExceptionCheck())
    {
        e->ExceptionClear();
        ALOGE("%s: fail notify", fn);
        goto TheEnd;
    }

TheEnd:
    ALOGV("%s: exit", fn);
}

/*******************************************************************************
**
** Function:        connectEE
**
** Description:     Connect to the execution environment.
**
** Returns:         True if ok.
**
*******************************************************************************/
bool SecureElement::connectEE ()
{
    static const char fn [] = "SecureElement::connectEE";
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    bool        retVal = false;
    uint8_t     destHost = 0;
    unsigned long num = 0;
    char pipeConfName[40];
    tNFA_HANDLE  eeHandle = mActiveEeHandle;

    ALOGV("%s: enter, mActiveEeHandle: 0x%04x, SEID: 0x%x, pipe_gate_num=%d, use pipe=%d",
        fn, mActiveEeHandle, gSEId, gGatePipe, gUseStaticPipe);

    if (!mIsInit)
    {
        ALOGE("%s: not init", fn);
        return (false);
    }

    if (gSEId != -1)
    {
        eeHandle = gSEId | NFA_HANDLE_GROUP_EE;
        ALOGV("%s: Using SEID: 0x%x", fn, eeHandle );
    }

    if (eeHandle == NFA_HANDLE_INVALID)
    {
        ALOGE("%s: invalid handle 0x%X", fn, eeHandle);
        return (false);
    }

    tNFA_EE_INFO *pEE = findEeByHandle (eeHandle);

    if (pEE == NULL)
    {
        ALOGE("%s: Handle 0x%04x  NOT FOUND !!", fn, eeHandle);
        return (false);
    }

#if (NXP_EXTNS == TRUE)
    if(nfcFL.nfcNxpEse) {
        if(nfcFL.eseFL._ESE_WIRED_MODE_DISABLE_DISCOVERY) {
            // Disable RF discovery completely while the DH is connected
            android::startRfDiscovery(false);
        }
    }
    else {
        android::startRfDiscovery(false);
    }
#else
    android::startRfDiscovery(false);
#endif

    // Disable UICC idle timeout while the DH is connected
    //android::setUiccIdleTimeout (false);

    mNewSourceGate = 0;

    if (gGatePipe == -1)
    {
        // pipe/gate num was not specifed by app, get from config file
        mNewPipeId     = 0;

        // Construct the PIPE name based on the EE handle (e.g. NFA_HCI_STATIC_PIPE_ID_F3 for UICC0).
        snprintf (pipeConfName, sizeof(pipeConfName), "NFA_HCI_STATIC_PIPE_ID_%02X", eeHandle & NFA_HANDLE_MASK);

        if (GetNumValue(pipeConfName, &num, sizeof(num)) && (num != 0))
        {
            mNewPipeId = num;
            ALOGV("%s: Using static pipe id: 0x%X", __func__, mNewPipeId);
        }
        else
        {
            ALOGV("%s: Did not find value '%s' defined in the .conf", __func__, pipeConfName);
        }
    }
    else
    {
        if (gUseStaticPipe)
        {
            mNewPipeId     = gGatePipe;
        }
        else
        {
            mNewPipeId      = 0;
            mDestinationGate= gGatePipe;
        }
    }

    // If the .conf file had a static pipe to use, just use it.
    if (mNewPipeId != 0)
    {
#if(NXP_EXTNS == TRUE)
        uint8_t host;
        if(mActiveEeHandle == EE_HANDLE_0xF3)
        {
            host = (mNewPipeId == mStaticPipeProp) ? 0xC0 : 0x03;
        }
        else
        {
            host = (mNewPipeId == STATIC_PIPE_UICC) ? 0x02 : 0x03;
        }
#else
        uint8_t host = (mNewPipeId == mStaticPipeProp) ? 0x02 : 0x03;
#endif
        //TODO according ETSI12 APDU Gate
#if(NXP_EXTNS == TRUE)
        uint8_t gate;
        if(mActiveEeHandle == EE_HANDLE_0xF3)
        {
            gate = (mNewPipeId == mStaticPipeProp) ? 0xF0 : 0xF1;
        }
        else
        {
            gate = (mNewPipeId == STATIC_PIPE_UICC) ? 0x30 : 0x31;
        }
#else
        uint8_t gate = (mNewPipeId == mStaticPipeProp) ? 0xF0 : 0xF1;
#endif
#if(NXP_EXTNS == TRUE)
        ALOGV("%s: Using host id : 0x%X,gate id : 0x%X,pipe id : 0x%X", __func__,host,gate, mNewPipeId);
#endif
        if(nfcFL.eseFL._LEGACY_APDU_GATE && (getApduGateInfo() != ETSI_12_APDU_GATE))
        {
            nfaStat = NFA_HciAddStaticPipe(mNfaHciHandle, host, gate, mNewPipeId);
            if (nfaStat != NFA_STATUS_OK)
            {
                ALOGE("%s: fail create static pipe; error=0x%X", fn, nfaStat);
                retVal = false;
                goto TheEnd;
            }
        }
    }
    else
    {
        if ( (pEE->num_tlvs >= 1) && (pEE->ee_tlv[0].tag == NFA_EE_TAG_HCI_HOST_ID) )
            destHost = pEE->ee_tlv[0].info[0];
        else
#if(NXP_EXTNS == TRUE)
            destHost = (EE_HANDLE_0xF3 & ~NFA_HANDLE_GROUP_EE);
#else
            destHost = 2;
#endif

        // Get a list of existing gates and pipes
        {
            ALOGV("%s: get gate, pipe list", fn);
            SyncEventGuard guard (mPipeListEvent);
            nfaStat = NFA_HciGetGateAndPipeList (mNfaHciHandle);
            if (nfaStat == NFA_STATUS_OK)
            {
                mPipeListEvent.wait();
                if (mHciCfg.status == NFA_STATUS_OK)
                {
                    for (uint8_t xx = 0; xx < mHciCfg.num_pipes; xx++)
                    {
                        if ( (mHciCfg.pipe[xx].dest_host == destHost)
                         &&  (mHciCfg.pipe[xx].dest_gate == mDestinationGate) )
                        {
                            mNewSourceGate = mHciCfg.pipe[xx].local_gate;
                            mNewPipeId     = mHciCfg.pipe[xx].pipe_id;

                            ALOGV("%s: found configured gate: 0x%02x  pipe: 0x%02x", fn, mNewSourceGate, mNewPipeId);
                            break;
                        }
                    }
                }
            }
        }

        if (mNewSourceGate == 0)
        {
            ALOGV("%s: allocate gate", fn);
            //allocate a source gate and store in mNewSourceGate
            SyncEventGuard guard (mAllocateGateEvent);
            if ((nfaStat = NFA_HciAllocGate (mNfaHciHandle, mDestinationGate)) != NFA_STATUS_OK)
            {
                ALOGE("%s: fail allocate source gate; error=0x%X", fn, nfaStat);
                goto TheEnd;
            }
            mAllocateGateEvent.wait ();
            if (mCommandStatus != NFA_STATUS_OK)
               goto TheEnd;
        }

        if (mNewPipeId == 0)
        {
            ALOGV("%s: create pipe", fn);
            SyncEventGuard guard (mCreatePipeEvent);
            nfaStat = NFA_HciCreatePipe (mNfaHciHandle, mNewSourceGate, destHost, mDestinationGate);
            if (nfaStat != NFA_STATUS_OK)
            {
                ALOGE("%s: fail create pipe; error=0x%X", fn, nfaStat);
                goto TheEnd;
            }
            mCreatePipeEvent.wait ();
            if (mCommandStatus != NFA_STATUS_OK)
               goto TheEnd;
        }

        {
            ALOGV("%s: open pipe", fn);
            SyncEventGuard guard (mPipeOpenedEvent);
            nfaStat = NFA_HciOpenPipe (mNfaHciHandle, mNewPipeId);
            if (nfaStat != NFA_STATUS_OK)
            {
                ALOGE("%s: fail open pipe; error=0x%X", fn, nfaStat);
                goto TheEnd;
            }
            mPipeOpenedEvent.wait ();
            if (mCommandStatus != NFA_STATUS_OK)
               goto TheEnd;
        }
    }

    retVal = true;

TheEnd:
    mIsPiping = retVal;
    if (!retVal)
    {
        // if open failed we need to de-allocate the gate
        disconnectEE(0);
    }

    ALOGV("%s: exit; ok=%u", fn, retVal);
    return retVal;
}


/*******************************************************************************
**
** Function:        disconnectEE
**
** Description:     Disconnect from the execution environment.
**                  seID: ID of secure element.
**
** Returns:         True if ok.
**
*******************************************************************************/
bool SecureElement::disconnectEE (jint seID)
{
    static const char fn [] = "SecureElement::disconnectEE";
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    tNFA_HANDLE eeHandle = seID;

    ALOGV("%s: seID=0x%X; handle=0x%04x", fn, seID, eeHandle);

    if (mUseOberthurWarmReset)
    {
        //send warm-reset command to Oberthur secure element which deselects the applet;
        //this is an Oberthur-specific command;
        ALOGV("%s: try warm-reset on pipe id 0x%X; cmd=0x%X", fn, mNewPipeId, mOberthurWarmResetCommand);
        SyncEventGuard guard (mRegistryEvent);
        nfaStat = NFA_HciSetRegistry (mNfaHciHandle, mNewPipeId,
                1, 1, &mOberthurWarmResetCommand);
        if (nfaStat == NFA_STATUS_OK)
        {
            mRegistryEvent.wait ();
            ALOGV("%s: completed warm-reset on pipe 0x%X", fn, mNewPipeId);
        }
    }

    if (mNewSourceGate && (mNewSourceGate != NFA_HCI_ETSI12_APDU_GATE))
    {
        SyncEventGuard guard (mDeallocateGateEvent);
        if ((nfaStat = NFA_HciDeallocGate (mNfaHciHandle, mNewSourceGate)) == NFA_STATUS_OK)
            mDeallocateGateEvent.wait ();
        else
            ALOGE("%s: fail dealloc gate; error=0x%X", fn, nfaStat);
    }

    mIsPiping = false;

    if(nfcFL.eseFL._ESE_DWP_SPI_SYNC_ENABLE && nfcFL.eseFL._ESE_SVDD_SYNC &&
            (dual_mode_current_state & SPI_ON)) {
        /*The state of the SPI should not be cleared based on DWP state close. dual_mode_current_state updated in spi_prio_signal_handler function.*/
        /*clear the SPI transaction flag*/
        dual_mode_current_state ^= SPI_ON;
}

    hold_the_transceive = false;
#if(NXP_EXTNS == TRUE)
    if(nfcFL.nfcNxpEse) {
        hold_wired_mode = false;
    }
#endif

    // Re-enable UICC low-power mode
    // Re-enable RF discovery
    // Note that it only effactuates the current configuration,
    // so if polling/listening were configured OFF (forex because
    // the screen was off), they will stay OFF with this call.
    /*Blocked as part  done in connectEE, to allow wired mode during reader mode.*/
    if(nfcFL.nfcNxpEse && nfcFL.chipType != pn547C2) {
        // Do Nothing
    }
    else {
        android::setUiccIdleTimeout (true);
        android::startRfDiscovery(true);
    }

    return true;
}


/*******************************************************************************
**
** Function:        transceive
**
** Description:     Send data to the secure element; read it's response.
**                  xmitBuffer: Data to transmit.
**                  xmitBufferSize: Length of data.
**                  recvBuffer: Buffer to receive response.
**                  recvBufferMaxSize: Maximum size of buffer.
**                  recvBufferActualSize: Actual length of response.
**                  timeoutMillisec: timeout in millisecond.
**
** Returns:         True if ok.
**
*******************************************************************************/
#if(NXP_EXTNS == TRUE)
eTransceiveStatus SecureElement::transceive (uint8_t* xmitBuffer, int32_t xmitBufferSize, uint8_t* recvBuffer,
        int32_t recvBufferMaxSize, int32_t& recvBufferActualSize, int32_t timeoutMillisec)
#else
bool SecureElement::transceive (uint8_t* xmitBuffer, int32_t xmitBufferSize, uint8_t* recvBuffer,
        int32_t recvBufferMaxSize, int32_t& recvBufferActualSize, int32_t timeoutMillisec)
#endif
{


    static const char fn [] = "SecureElement::transceive";
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    bool isSuccess = false;
    mTransceiveWaitOk = false;
    uint8_t newSelectCmd[NCI_MAX_AID_LEN + 10];
#if(NXP_EXTNS == TRUE)
    eTransceiveStatus tranStatus    = TRANSCEIVE_STATUS_FAILED;
#else
    bool isSuccess                  = false;
#endif
#if(NXP_EXTNS == TRUE)
    bool isEseAccessSuccess = false;
#endif

    ALOGV("%s: enter; xmitBufferSize=%ld; recvBufferMaxSize=%ld; timeout=%ld", fn, xmitBufferSize, recvBufferMaxSize, timeoutMillisec);

    // Check if we need to replace an "empty" SELECT command.
    // 1. Has there been a AID configured, and
    // 2. Is that AID a valid length (i.e 16 bytes max), and
    // 3. Is the APDU at least 4 bytes (for header), and
    // 4. Is INS == 0xA4 (SELECT command), and
    // 5. Is P1 == 0x04 (SELECT by AID), and
    // 6. Is the APDU len 4 or 5 bytes.
    //
    // Note, the length of the configured AID is in the first
    //   byte, and AID starts from the 2nd byte.
    if (mAidForEmptySelect[0]                           // 1
                           && (mAidForEmptySelect[0] <= NCI_MAX_AID_LEN)   // 2
                           && (xmitBufferSize >= 4)                        // 3
                           && (xmitBuffer[1] == 0xA4)                      // 4
                           && (xmitBuffer[2] == 0x04)                      // 5
                           && (xmitBufferSize <= 5))                       // 6
    {
        uint8_t idx = 0;

        // Copy APDU command header from the input buffer.
        memcpy(&newSelectCmd[0], &xmitBuffer[0], 4);
        idx = 4;

        // Set the Lc value to length of the new AID
        newSelectCmd[idx++] = mAidForEmptySelect[0];

        // Copy the AID
        memcpy(&newSelectCmd[idx], &mAidForEmptySelect[1], mAidForEmptySelect[0]);
        idx += mAidForEmptySelect[0];

        // If there is an Le (5th byte of APDU), add it to the end.
        if (xmitBufferSize == 5)
            newSelectCmd[idx++] = xmitBuffer[4];

        // Point to the new APDU
        xmitBuffer = &newSelectCmd[0];
        xmitBufferSize = idx;

        ALOGV("%s: Empty AID SELECT cmd detected, substituting AID from config file, new length=%d", fn, idx);
    }

#if(NXP_EXTNS == TRUE)
        NfccStandByOperation(STANDBY_MODE_OFF);
#endif
    {
        SyncEventGuard guard (mTransceiveEvent);
        mActualResponseSize = 0;
        memset (recvBuffer, 0, recvBufferMaxSize);
#if(NXP_EXTNS == TRUE)
        if(nfcFL.nfcNxpEse) {
            struct timeval start_timer, end_timer;
            int32_t time_elapsed = 0;
            while(hold_the_transceive == true)
            {
                android::start_timer_msec(&start_timer);
                ALOGV("%s: holding the transceive for %ld ms.\n", fn, (timeoutMillisec - time_elapsed));
                SyncEventGuard guard(sSPIPrioSessionEndEvent);
                if(sSPIPrioSessionEndEvent.wait(timeoutMillisec - time_elapsed)== false)
                {
                    ALOGE("%s: wait response timeout \n", fn);
                    time_elapsed = android::stop_timer_getdifference_msec(&start_timer, &end_timer);
                    time_elapsed = 0;
                    goto TheEnd;
                }
                time_elapsed += android::stop_timer_getdifference_msec(&start_timer, &end_timer);
                if((timeoutMillisec - time_elapsed) <= 0)
                {
                    ALOGE("%s: wait response timeout - time_elapsed \n", fn);
                    time_elapsed = 0;
                    goto TheEnd;
                }
            }
        }
#endif
        if ((mNewPipeId == mStaticPipeProp) || (mNewPipeId == STATIC_PIPE_0x71))
#if(NXP_EXTNS == TRUE)
        {
            if (nfcFL.nfccFL._NFCEE_REMOVED_NTF_RECOVERY) {
                if((RoutingManager::getInstance().is_ee_recovery_ongoing()))
                {
                    SyncEventGuard guard (mEEdatapacketEvent);
                    if(mEEdatapacketEvent.wait(timeoutMillisec) == false)
                        goto TheEnd;
                }
            }
        if(nfcFL.nfcNxpEse) {
            if(nfcFL.eseFL._ESE_DUAL_MODE_PRIO_SCHEME == nfcFL.eseFL._ESE_WIRED_MODE_RESUME ||
                    nfcFL.eseFL._ESE_DUAL_MODE_PRIO_SCHEME == nfcFL.eseFL._ESE_WIRED_MODE_RESUME ) {
                if(!checkForWiredModeAccess())
                {
                    ALOGV("%s, Dont allow wired mode in this RF state", fn);
                    goto TheEnd;
                }
            }
        }

        if(((nfcFL.eseFL._NXP_ESE_VER == JCOP_VER_3_2) || (nfcFL.eseFL._NXP_ESE_VER == JCOP_VER_3_3)) &&nfcFL.eseFL._TRIPLE_MODE_PROTECTION){
            if((dual_mode_current_state == SPI_DWPCL_BOTH_ACTIVE))
            {
                ALOGV("%s, Dont allow wired mode...Dual Mode..", fn);
                SyncEventGuard guard (mDualModeEvent);
                mDualModeEvent.wait();
            }
        }

        if(nfcFL.nfcNxpEse) {
            active_ese_reset_control |= TRANS_WIRED_ONGOING;
            if ((((nfcFL.eseFL._NXP_ESE_VER == JCOP_VER_3_1) || (nfcFL.eseFL._NXP_ESE_VER == JCOP_VER_3_2))) && (NFC_GetEseAccess((void *)&timeoutMillisec) != 0))
            {
                ALOGE("%s: NFC_ReqWiredAccess timeout", fn);
                goto TheEnd;
            }
            isEseAccessSuccess = true;
        }
#endif
        if(nfcFL.nfcNxpEse && nfcFL.eseFL._NFCC_ESE_UICC_CONCURRENT_ACCESS_PROTECTION) {
            isTransceiveOngoing = true;
        }
            nfaStat = NFA_HciSendEvent (mNfaHciHandle, mNewPipeId, EVT_SEND_DATA, xmitBufferSize, xmitBuffer, recvBufferMaxSize, recvBuffer, timeoutMillisec);
#if(NXP_EXTNS == TRUE)
        }
        else if (mNewPipeId == STATIC_PIPE_UICC)
        {
            ALOGV("%s, Starting UICC wired mode!!!!!!.....", fn);
            nfaStat = NFA_HciSendEvent (mNfaHciHandle, mNewPipeId, EVT_SEND_DATA, xmitBufferSize, xmitBuffer, recvBufferMaxSize, recvBuffer, timeoutMillisec);
        }
#endif
        else
#if(NXP_EXTNS == TRUE)
        {
            if(nfcFL.nfcNxpEse) {
                active_ese_reset_control |= TRANS_WIRED_ONGOING;
                if (((nfcFL.eseFL._NXP_ESE_VER == JCOP_VER_3_1) || (nfcFL.eseFL._NXP_ESE_VER == JCOP_VER_3_2)) &&
                        (NFC_GetEseAccess((void *)&timeoutMillisec) != 0))
                {
                    ALOGE("%s: NFC_ReqWiredAccess timeout", fn);
                    goto TheEnd;
                }
                isEseAccessSuccess = true;
            }
#endif
            if(nfcFL.nfcNxpEse && nfcFL.eseFL._NFCC_ESE_UICC_CONCURRENT_ACCESS_PROTECTION) {
                isTransceiveOngoing = true;
            }
            nfaStat = NFA_HciSendEvent (mNfaHciHandle, mNewPipeId, NFA_HCI_EVT_POST_DATA, xmitBufferSize, xmitBuffer, recvBufferMaxSize, recvBuffer, timeoutMillisec);
#if(NXP_EXTNS == TRUE)
        }
#endif
        if (nfaStat == NFA_STATUS_OK)
        {
            //          waitOk = mTransceiveEvent.wait (timeoutMillisec);
            mTransceiveEvent.wait ();
#if(NXP_EXTNS == TRUE)
            if(nfcFL.nfcNxpEse && (gWtxCount > mWmMaxWtxCount))
            {
                tranStatus = TRANSCEIVE_STATUS_MAX_WTX_REACHED;
                gWtxCount = 0;
                goto TheEnd;
            }
#endif
            if(nfcFL.nfcNxpEse && nfcFL.eseFL._NFCC_ESE_UICC_CONCURRENT_ACCESS_PROTECTION) {
                isTransceiveOngoing = false;
            }

#if(NXP_EXTNS == TRUE)
            if(nfcFL.nfcNxpEse) {
                if(nfcFL.eseFL._JCOP_WA_ENABLE &&
                        (active_ese_reset_control & TRANS_WIRED_ONGOING))
                {
                    active_ese_reset_control ^= TRANS_WIRED_ONGOING;

                    /*If only reset event is pending*/
                    if((active_ese_reset_control&RESET_BLOCKED))
                    {
                        SyncEventGuard guard (mResetOngoingEvent);
                        mResetOngoingEvent.wait();
                    }
                    if(!(active_ese_reset_control&TRANS_CL_ONGOING) &&
                            (active_ese_reset_control&RESET_BLOCKED))
                    {
                        active_ese_reset_control ^= RESET_BLOCKED;
                    }
                }
                if(mTransceiveWaitOk == false)
                {
                    ALOGE("%s: transceive timed out", fn);
                    goto TheEnd;
                }
            }
#endif
        }
        else
        {
            ALOGE("%s: fail send data; error=0x%X", fn, nfaStat);
            goto TheEnd;
        }
    }
        if (mActualResponseSize > recvBufferMaxSize)
            recvBufferActualSize = recvBufferMaxSize;
        else
            recvBufferActualSize = mActualResponseSize;

#if(NXP_EXTNS == TRUE)
    tranStatus = TRANSCEIVE_STATUS_OK;
#else
     isSuccess = true;
#endif
        TheEnd:
#if(NXP_EXTNS == TRUE)
        if(nfcFL.nfcNxpEse) {
            if ((((nfcFL.eseFL._NXP_ESE_VER == JCOP_VER_3_1) || (nfcFL.eseFL._NXP_ESE_VER == JCOP_VER_3_2))) && isEseAccessSuccess == true)
            {
                if (NFC_RelEseAccess((void *)&nfaStat) != 0)
                {
                    ALOGE("%s: NFC_RelEseAccess failed", fn);
                }
            }
            if((nfcFL.eseFL._JCOP_WA_ENABLE) && (active_ese_reset_control&TRANS_WIRED_ONGOING))
                active_ese_reset_control ^= TRANS_WIRED_ONGOING;
        }
#endif

#if(NXP_EXTNS == TRUE)
    ALOGV ("%s: exit; tranStatus: %d; recvBufferActualSize: %ld", fn, tranStatus, recvBufferActualSize);
    return (tranStatus);
#else
     ALOGV ("%s: exit; isSuccess: %d; recvBufferActualSize: %ld", fn, isSuccess, recvBufferActualSize);
     return (isSuccess);
#endif
}
/*******************************************************************************
 **
 ** Function:       setCLState
 **
 ** Description:    Update current DWP CL state based on CL activation status
 **
 ** Returns:        None .
 **
 *******************************************************************************/
void SecureElement::setCLState(bool mState)
{
    ALOGV("%s: Entry setCLState \n", __func__);
    /*Check if the state is already dual mode*/
    bool inDualModeAlready = (dual_mode_current_state == SPI_DWPCL_BOTH_ACTIVE);
    if(mState)
    {
        dual_mode_current_state |= CL_ACTIVE;
#if(NXP_EXTNS == TRUE)
        if(nfcFL.nfcNxpEse && nfcFL.eseFL._JCOP_WA_ENABLE) {
            active_ese_reset_control |= TRANS_CL_ONGOING;
        }
#endif
    }
    else
    {
       if(dual_mode_current_state & CL_ACTIVE)
       {
           dual_mode_current_state ^= CL_ACTIVE;
#if(NXP_EXTNS == TRUE)
           if(nfcFL.nfcNxpEse) {
               if(nfcFL.eseFL._JCOP_WA_ENABLE && (active_ese_reset_control&TRANS_CL_ONGOING))
               {
                   active_ese_reset_control ^= TRANS_CL_ONGOING;

                   /*If there is no pending wired rapdu or CL session*/
                   if(((active_ese_reset_control&RESET_BLOCKED))&&
                           (!(active_ese_reset_control &(TRANS_WIRED_ONGOING))))
                   {
                       /*unblock pending reset event*/
                       SyncEventGuard guard (sSecElem.mResetEvent);
                       sSecElem.mResetEvent.notifyOne();
                       active_ese_reset_control ^= RESET_BLOCKED;
                   }
               }
           }
#endif
           if(inDualModeAlready)
           {
               SyncEventGuard guard (mDualModeEvent);
               mDualModeEvent.notifyOne();
           }
       }
    }
    ALOGV("%s: Exit setCLState = %d\n", __func__, dual_mode_current_state);
}

void SecureElement::notifyModeSet (tNFA_HANDLE eeHandle, bool success, tNFA_EE_STATUS eeStatus)
{
    static const char* fn = "SecureElement::notifyModeSet";
    if (success)
    {
        tNFA_EE_INFO *pEE = sSecElem.findEeByHandle (eeHandle);
        if (pEE)
        {
            pEE->ee_status = eeStatus;
            ALOGV("%s: NFA_EE_MODE_SET_EVT; pEE->ee_status: %s (0x%04x)", fn, SecureElement::eeStatusToString(pEE->ee_status), pEE->ee_status);
        }
        else
            ALOGE("%s: NFA_EE_MODE_SET_EVT; EE: 0x%04x not found.  mActiveEeHandle: 0x%04x", fn, eeHandle, sSecElem.mActiveEeHandle);
    }
    SyncEventGuard guard (sSecElem.mEeSetModeEvent);
    sSecElem.mEeSetModeEvent.notifyOne();
}

#if(NXP_EXTNS == TRUE)
static void NFCC_StandbyModeTimerCallBack (union sigval)
{
    if(!nfcFL.nfcNxpEse) {
        ALOGV("%s tnfcNxpEse not available. Returning", __func__);
        return;
    }
    ALOGV("%s timer timedout , sending standby mode cmd", __func__);
    SecureElement::getInstance().NfccStandByOperation(STANDBY_TIMER_TIMEOUT);
}
#endif
/*******************************************************************************
**
** Function:        notifyListenModeState
**
** Description:     Notify the NFC service about whether the SE was activated
**                  in listen mode.
**                  isActive: Whether the secure element is activated.
**
** Returns:         None
**
*******************************************************************************/
void SecureElement::notifyListenModeState (bool isActivated) {
    static const char fn [] = "SecureElement::notifyListenMode";

    ALOGV("%s: enter; listen mode active=%u", fn, isActivated);

    JNIEnv* e = NULL;
    if (mNativeData == NULL)
    {
        ALOGE("%s: mNativeData is null", fn);
        return;
    }

    ScopedAttach attach(mNativeData->vm, &e);
    if (e == NULL)
    {
        ALOGE("%s: jni env is null", fn);
        return;
    }

    mActivatedInListenMode = isActivated;
#if (NXP_EXTNS == TRUE)
    if(nfcFL.nfcNxpEse) {
        if(!isActivated)
        {
            setCLState(false);
            if(nfcFL.eseFL._ESE_DUAL_MODE_PRIO_SCHEME != nfcFL.eseFL._ESE_WIRED_MODE_RESUME) {
                setDwpTranseiveState(false, NFCC_DEACTIVATED_NTF);
            }
        }
        else
        {
            /* activated in listen mode */
            if(nfcFL.eseFL._ESE_DUAL_MODE_PRIO_SCHEME != nfcFL.eseFL._ESE_WIRED_MODE_RESUME) {
                setDwpTranseiveState(true, NFCC_ACTIVATED_NTF);
            }
        }
    }
#endif
    if (mNativeData != NULL) {
        if (isActivated) {
            e->CallVoidMethod (mNativeData->manager, android::gCachedNfcManagerNotifySeListenActivated);
        }
        else {
            e->CallVoidMethod (mNativeData->manager, android::gCachedNfcManagerNotifySeListenDeactivated);
        }
    }

    if (e->ExceptionCheck())
    {
        e->ExceptionClear();
        ALOGE("%s: fail notify", fn);
    }

    ALOGV("%s: exit", fn);
}

/*******************************************************************************
**
** Function:        notifyRfFieldEvent
**
** Description:     Notify the NFC service about RF field events from the stack.
**                  isActive: Whether any secure element is activated.
**
** Returns:         None
**
*******************************************************************************/
void SecureElement::notifyRfFieldEvent (bool isActive)
{
    static const char fn [] = "SecureElement::notifyRfFieldEvent";
    ALOGV("%s: enter; is active=%u", fn, isActive);

    JNIEnv* e = NULL;
    ScopedAttach attach(mNativeData->vm, &e);
    if (e == NULL)
    {
        ALOGE("%s: jni env is null", fn);
        return;
    }

    mMutex.lock();
    int ret = clock_gettime (CLOCK_MONOTONIC, &mLastRfFieldToggle);
    if (ret == -1) {
        ALOGE("%s: clock_gettime failed", fn);
        // There is no good choice here...
    }

    if (isActive)
    {
        mRfFieldIsOn = true;
#if (NXP_EXTNS == TRUE)
        if(nfcFL.nfcNxpEse) {
            if(nfcFL.eseFL._NFCC_ESE_UICC_CONCURRENT_ACCESS_PROTECTION &&
                    (isTransceiveOngoing == false && meseUiccConcurrentAccess == true &&
                            mPassiveListenEnabled == false))
            {
                startThread(0x01);
            }
            if(nfcFL.eseFL._ESE_DUAL_MODE_PRIO_SCHEME != nfcFL.eseFL._ESE_WIRED_MODE_RESUME) {
                setDwpTranseiveState(true, NFCC_RF_FIELD_EVT);
            }
        }
#endif
        e->CallVoidMethod (mNativeData->manager, android::gCachedNfcManagerNotifySeFieldActivated);
    }
    else
    {
        mRfFieldIsOn = false;
        setCLState(false);
#if (NXP_EXTNS == TRUE)
        if(nfcFL.nfcNxpEse &&
                (nfcFL.eseFL._ESE_DUAL_MODE_PRIO_SCHEME !=
                        nfcFL.eseFL._ESE_WIRED_MODE_RESUME)) {
            setDwpTranseiveState(false, NFCC_RF_FIELD_EVT);
        }
#endif
        e->CallVoidMethod (mNativeData->manager, android::gCachedNfcManagerNotifySeFieldDeactivated);
    }
    mMutex.unlock();

    if (e->ExceptionCheck())
    {
        e->ExceptionClear();
        ALOGE("%s: fail notify", fn);
    }
    ALOGV("%s: exit", fn);
}

#if(NXP_EXTNS == TRUE)
/*Reader over SWP*/
void SecureElement::notifyEEReaderEvent (int evt, int data)
{
    if(!nfcFL.nfcNxpEse || !nfcFL.eseFL._ESE_ETSI_READER_ENABLE) {
        ALOGV("%s: nfcNxpEse or ETSI_READER not available. Returning", __func__);
        return;
    }
    static const char fn [] = "SecureElement::notifyEEReaderEvent";
    ALOGV("%s: enter; event=%x", fn, evt);



    mMutex.lock();
    int ret = clock_gettime (CLOCK_MONOTONIC, &mLastRfFieldToggle);
    if (ret == -1) {
        ALOGE("%s: clock_gettime failed", fn);
        // There is no good choice here...
    }
    switch (evt) {
        case NFA_RD_SWP_READER_REQUESTED:
            ALOGV("%s: NFA_RD_SWP_READER_REQUESTED for tech %x", fn, data);
            {
                jboolean istypeA = false;
                jboolean istypeB = false;

                if(data & NFA_TECHNOLOGY_MASK_A)
                    istypeA = true;
                if(data & NFA_TECHNOLOGY_MASK_B)
                    istypeB = true;

                /*
                 * Start the protection time.This is to give user a specific time window to wait for the TAG,
                 * and prevents MW from infinite waiting to switch back to normal NFC-Fouram polling mode.
                 * */
                unsigned long timeout = 0;
                GetNxpNumValue(NAME_NXP_SWP_RD_START_TIMEOUT, (void *)&timeout, sizeof(timeout));
                ALOGV("SWP_RD_START_TIMEOUT : %ld", timeout);
                if (timeout > 0)
                    sSwpReaderTimer.set(1000*timeout,startStopSwpReaderProc);
            }
            break;
        case NFA_RD_SWP_READER_START:
            ALOGV("%s: NFA_RD_SWP_READER_START", fn);
            {
                JNIEnv* e = NULL;
                    ScopedAttach attach(mNativeData->vm, &e);
                    if (e == NULL)
                    {
                        ALOGE("%s: jni env is null", fn);
                        break;
                    }
                    sSwpReaderTimer.kill();
                /*
                 * Start the protection time.This is to give user a specific time window to wait for the
                 * SWP Reader to finish with card, and prevents MW from infinite waiting to switch back to
                 * normal NFC-Forum polling mode.
                 *
                 *  configuring timeout.
                 * */
                unsigned long timeout = 0;
                GetNxpNumValue(NAME_NXP_SWP_RD_TAG_OP_TIMEOUT, (void *)&timeout, sizeof(timeout));
                ALOGV("SWP_RD_TAG_OP_TIMEOUT : %ld", timeout);
                if (timeout > 0)
                    sSwpReaderTimer.set(2000*timeout,startStopSwpReaderProc);

                e->CallVoidMethod (mNativeData->manager, android::gCachedNfcManagerNotifySWPReaderActivated);
            }
            break;
        case NFA_RD_SWP_READER_STOP:
            ALOGV("%s: NFA_RD_SWP_READER_STOP", fn);
            break;

            //TODO: Check this later. Need to update libnfc-nci for this symbol.
//        case NFA_RD_SWP_READER_START_FAIL:
//            ALOGV("%s: NFA_RD_SWP_READER_STOP", fn);
//            //sStopSwpReaderTimer.kill();
//            e->CallVoidMethod (mNativeData->manager, android::gCachedNfcManagerNotifySWPReaderRequestedFail);
//            break;

        default:
            ALOGV("%s: UNKNOWN EVENT ??", fn);
            break;
    }

    mMutex.unlock();

    ALOGV("%s: exit", fn);
}
#endif
/*******************************************************************************
**
** Function:        resetRfFieldStatus
**
** Description:     Resets the field status.
**                  isActive: Whether any secure element is activated.
**
** Returns:         None
**
*******************************************************************************/
void SecureElement::resetRfFieldStatus ()
{
    static const char fn [] = "SecureElement::resetRfFieldStatus`";
    ALOGV("%s: enter;", fn);

    mMutex.lock();
    mRfFieldIsOn = false;
    int ret = clock_gettime (CLOCK_MONOTONIC, &mLastRfFieldToggle);
    if (ret == -1) {
        ALOGE("%s: clock_gettime failed", fn);
        // There is no good choice here...
    }
    mMutex.unlock();

    ALOGV("%s: exit", fn);
}


/*******************************************************************************
**
** Function:        storeUiccInfo
**
** Description:     Store a copy of the execution environment information from the stack.
**                  info: execution environment information.
**
** Returns:         None
**
*******************************************************************************/
void SecureElement::storeUiccInfo (tNFA_EE_DISCOVER_REQ& info)
{
    static const char fn [] = "SecureElement::storeUiccInfo";
    ALOGV("%s:  Status: %u   Num EE: %u", fn, info.status, info.num_ee);

    SyncEventGuard guard (mUiccInfoEvent);
    memcpy (&mUiccInfo, &info, sizeof(mUiccInfo));
    for (uint8_t xx = 0; xx < info.num_ee; xx++)
    {
        //for each technology (A, B, F, B'), print the bit field that shows
        //what protocol(s) is support by that technology
        ALOGV("%s   EE[%u] Handle: 0x%04x  techA: 0x%02x  techB: 0x%02x  techF: 0x%02x  techBprime: 0x%02x",
                fn, xx, info.ee_disc_info[xx].ee_handle,
                info.ee_disc_info[xx].la_protocol,
                info.ee_disc_info[xx].lb_protocol,
                info.ee_disc_info[xx].lf_protocol,
                info.ee_disc_info[xx].lbp_protocol);
    }
    mUiccInfoEvent.notifyOne ();
}

/*******************************************************************************
**
** Function         getSeVerInfo
**
** Description      Gets version information and id for a secure element.  The
**                  seIndex parmeter is the zero based index of the secure
**                  element to get verion info for.  The version infommation
**                  is returned as a string int the verInfo parameter.
**
** Returns          ture on success, false on failure
**
*******************************************************************************/
bool SecureElement::getSeVerInfo(int seIndex, char * verInfo, int verInfoSz, uint8_t * seid)
{
    ALOGV("%s: enter, seIndex=%d", __func__, seIndex);

    if (seIndex > (mActualNumEe-1))
    {
        ALOGE("%s: invalid se index: %d, only %d SEs in system", __func__, seIndex, mActualNumEe);
        return false;
    }

    *seid = mEeInfo[seIndex].ee_handle;

    if ((mEeInfo[seIndex].num_interface == 0) || (mEeInfo[seIndex].ee_interface[0] == NCI_NFCEE_INTERFACE_HCI_ACCESS) )
    {
        return false;
    }

    strncpy(verInfo, "Version info not available", verInfoSz-1);
    verInfo[verInfoSz-1] = '\0';

    uint8_t pipe = (mEeInfo[seIndex].ee_handle == EE_HANDLE_0xF3) ? 0x70 : 0x71;
    uint8_t host = (pipe == mStaticPipeProp) ? 0x02 : 0x03;
    uint8_t gate = (pipe == mStaticPipeProp) ? 0xF0 : 0xF1;

    tNFA_STATUS nfaStat = NFA_HciAddStaticPipe(mNfaHciHandle, host, gate, pipe);
    if (nfaStat != NFA_STATUS_OK)
    {
        ALOGE("%s: NFA_HciAddStaticPipe() failed, pipe = 0x%x, error=0x%X", __func__, pipe, nfaStat);
        return true;
    }

    SyncEventGuard guard (mVerInfoEvent);
    if (NFA_STATUS_OK == (nfaStat = NFA_HciGetRegistry (mNfaHciHandle, pipe, 0x02)))
    {
        if (false == mVerInfoEvent.wait(200))
        {
            ALOGE("%s: wait response timeout", __func__);
        }
        else
        {
            snprintf(verInfo, verInfoSz-1, "Oberthur OS S/N: 0x%02x%02x%02x", mVerInfo[0], mVerInfo[1], mVerInfo[2]);
            verInfo[verInfoSz-1] = '\0';
        }
    }
    else
    {
        ALOGE("%s: NFA_HciGetRegistry () failed: 0x%X", __func__, nfaStat);
    }
    return true;
}

/*******************************************************************************
**
** Function         getActualNumEe
**
** Description      Returns number of secure elements we know about.
**
** Returns          Number of secure elements we know about.
**
*******************************************************************************/
uint8_t SecureElement::getActualNumEe()
{
    if(NFA_GetNCIVersion() == NCI_VERSION_2_0)
        return mActualNumEe + 1;
    else
        return mActualNumEe;
}

/*******************************************************************************
**
** Function:        handleClearAllPipe
**
** Description:     To handle clear all pipe event received from HCI based on the
**                  deleted host
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void SecureElement::handleClearAllPipe (tNFA_HCI_EVT_DATA* eventData)
{
    static const char fn [] = "SecureElement::handleClearAllPipe";
    ALOGV("%s: Deleted host=0x%X", fn, eventData->deleted.host);
    if(eventData->deleted.host == (SecureElement::getInstance().EE_HANDLE_0xF4 & ~NFA_HANDLE_GROUP_EE))
     { /* To stop mode-set being called when a clear all pipe is being issued */
        ALOGV("%s: NFA_HCI_DELETE_PIPE_EVT for UICC1 start", fn);
        sSecElem.eSE_ClearAllPipe_handler(SecureElement::getInstance().EE_HANDLE_0xF4 & ~NFA_HANDLE_GROUP_EE);
        ALOGV("%s: NFA_HCI_DELETE_PIPE_EVT for UICC1 poll end", fn);
     }
     else if(eventData->deleted.host == (EE_HANDLE_0xF8 & ~NFA_HANDLE_GROUP_EE))
     {
        /* To stop mode-set being called when a clear all pipe is being issued */
        ALOGV("%s: NFA_HCI_DELETE_PIPE_EVT for UICC2 start", fn);
        sSecElem.eSE_ClearAllPipe_handler(EE_HANDLE_0xF8 & ~NFA_HANDLE_GROUP_EE);
        ALOGV("%s: NFA_HCI_DELETE_PIPE_EVT for UICC2 poll end", fn);
     }
}

/*******************************************************************************
**
** Function:        nfaHciCallback
**
** Description:     Receive Host Controller Interface-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void SecureElement::nfaHciCallback (tNFA_HCI_EVT event, tNFA_HCI_EVT_DATA* eventData)
{
    static const char fn [] = "SecureElement::nfaHciCallback";
    ALOGV("%s: event=0x%X", fn, event);
    int evtSrc = 0xFF;

    switch (event)
    {
    case NFA_HCI_REGISTER_EVT:
        {
            ALOGV("%s: NFA_HCI_REGISTER_EVT; status=0x%X; handle=0x%X", fn,
                    eventData->hci_register.status, eventData->hci_register.hci_handle);
            SyncEventGuard guard (sSecElem.mHciRegisterEvent);
            sSecElem.mNfaHciHandle = eventData->hci_register.hci_handle;
            sSecElem.mHciRegisterEvent.notifyOne();
        }
        break;

    case NFA_HCI_ALLOCATE_GATE_EVT:
        {
            ALOGV("%s: NFA_HCI_ALLOCATE_GATE_EVT; status=0x%X; gate=0x%X", fn, eventData->status, eventData->allocated.gate);
            SyncEventGuard guard (sSecElem.mAllocateGateEvent);
            sSecElem.mCommandStatus = eventData->status;
            sSecElem.mNewSourceGate = (eventData->allocated.status == NFA_STATUS_OK) ? eventData->allocated.gate : 0;
            sSecElem.mAllocateGateEvent.notifyOne();
        }
        break;

    case NFA_HCI_DEALLOCATE_GATE_EVT:
        {
            tNFA_HCI_DEALLOCATE_GATE& deallocated = eventData->deallocated;
            ALOGV("%s: NFA_HCI_DEALLOCATE_GATE_EVT; status=0x%X; gate=0x%X", fn, deallocated.status, deallocated.gate);
            SyncEventGuard guard (sSecElem.mDeallocateGateEvent);
            sSecElem.mDeallocateGateEvent.notifyOne();
        }
        break;

    case NFA_HCI_GET_GATE_PIPE_LIST_EVT:
        {
            ALOGV("%s: NFA_HCI_GET_GATE_PIPE_LIST_EVT; status=0x%X; num_pipes: %u  num_gates: %u", fn,
                    eventData->gates_pipes.status, eventData->gates_pipes.num_pipes, eventData->gates_pipes.num_gates);
            SyncEventGuard guard (sSecElem.mPipeListEvent);
            sSecElem.mCommandStatus = eventData->gates_pipes.status;
            sSecElem.mHciCfg = eventData->gates_pipes;
            sSecElem.mPipeListEvent.notifyOne();
        }
        break;

    case NFA_HCI_CREATE_PIPE_EVT:
        {
            ALOGV("%s: NFA_HCI_CREATE_PIPE_EVT; status=0x%X; pipe=0x%X; src gate=0x%X; dest host=0x%X; dest gate=0x%X", fn,
                    eventData->created.status, eventData->created.pipe, eventData->created.source_gate, eventData->created.dest_host, eventData->created.dest_gate);
            SyncEventGuard guard (sSecElem.mCreatePipeEvent);
            sSecElem.mCommandStatus = eventData->created.status;
            if(eventData->created.dest_gate == 0xF0)
            {
                ALOGE("Pipe=0x%x is created and updated for se transcieve", eventData->created.pipe);
                sSecElem.mNewPipeId = eventData->created.pipe;
            }
#if (NXP_EXTNS == TRUE)
            sSecElem.mCreatedPipe = eventData->created.pipe;
            ALOGV("%s: NFA_HCI_CREATE_PIPE_EVT; pipe=0x%X", fn, eventData->created.pipe);
#endif
            sSecElem.mCreatePipeEvent.notifyOne();
        }
        break;
#if (NXP_EXTNS == TRUE)
    case NFA_HCI_DELETE_PIPE_EVT:
        {
             ALOGV("%s: NFA_HCI_DELETE_PIPE_EVT; status=0x%X; host=0x%X", fn, eventData->deleted.status, eventData->deleted.host);
              sSecElem.handleClearAllPipe(eventData);
        }
        break;
#endif
    case NFA_HCI_OPEN_PIPE_EVT:
        {
            ALOGV("%s: NFA_HCI_OPEN_PIPE_EVT; status=0x%X; pipe=0x%X", fn, eventData->opened.status, eventData->opened.pipe);
            SyncEventGuard guard (sSecElem.mPipeOpenedEvent);
            sSecElem.mCommandStatus = eventData->opened.status;
            sSecElem.mPipeOpenedEvent.notifyOne();
        }
        break;

    case NFA_HCI_EVENT_SENT_EVT:
        ALOGV("%s: NFA_HCI_EVENT_SENT_EVT; status=0x%X", fn, eventData->evt_sent.status);
        break;

    case NFA_HCI_RSP_RCVD_EVT: //response received from secure element
        {
            tNFA_HCI_RSP_RCVD& rsp_rcvd = eventData->rsp_rcvd;
            ALOGV("%s: NFA_HCI_RSP_RCVD_EVT; status: 0x%X; code: 0x%X; pipe: 0x%X; len: %u", fn,
                    rsp_rcvd.status, rsp_rcvd.rsp_code, rsp_rcvd.pipe, rsp_rcvd.rsp_len);
            if((rsp_rcvd.rsp_code == NFA_HCI_ANY_E_PIPE_NOT_OPENED) && (sSecElem.IsCmdsentOnOpenDwpSession))
            {
                SyncEventGuard guard (sSecElem.mPipeStatusCheckEvent);
                sSecElem.mCommandStatus = eventData->closed.status;
                sSecElem.pipeStatus = NFA_HCI_ANY_E_PIPE_NOT_OPENED;
                sSecElem.mPipeStatusCheckEvent.notifyOne();
                sSecElem.IsCmdsentOnOpenDwpSession = false;
            }
        }
        break;

    case NFA_HCI_GET_REG_RSP_EVT :
        ALOGV("%s: NFA_HCI_GET_REG_RSP_EVT; status: 0x%X; pipe: 0x%X, len: %d", fn,
                eventData->registry.status, eventData->registry.pipe, eventData->registry.data_len);
        if(sSecElem.mGetAtrRspwait == true)
        {
            /*GetAtr response*/
            sSecElem.mGetAtrRspwait = false;
            SyncEventGuard guard (sSecElem.mGetRegisterEvent);
            memcpy(sSecElem.mAtrInfo, eventData->registry.reg_data, eventData->registry.data_len);
            sSecElem.mAtrInfolen = eventData->registry.data_len;
            sSecElem.mAtrStatus = eventData->registry.status;
            sSecElem.mGetRegisterEvent.notifyOne();
        }
        else if (eventData->registry.data_len >= 19 && ((eventData->registry.pipe == mStaticPipeProp) || (eventData->registry.pipe == STATIC_PIPE_0x71)))
        {
            SyncEventGuard guard (sSecElem.mVerInfoEvent);
            // Oberthur OS version is in bytes 16,17, and 18
            sSecElem.mVerInfo[0] = eventData->registry.reg_data[16];
            sSecElem.mVerInfo[1] = eventData->registry.reg_data[17];
            sSecElem.mVerInfo[2] = eventData->registry.reg_data[18];
            sSecElem.mVerInfoEvent.notifyOne ();
        }
        break;

    case NFA_HCI_EVENT_RCVD_EVT:
        ALOGV("%s: NFA_HCI_EVENT_RCVD_EVT; code: 0x%X; pipe: 0x%X; data len: %u", fn,
                eventData->rcvd_evt.evt_code, eventData->rcvd_evt.pipe, eventData->rcvd_evt.evt_len);
        if(eventData->rcvd_evt.pipe == 0x0A) //UICC
        {
            ALOGV("%s: NFA_HCI_EVENT_RCVD_EVT; source UICC",fn);
            evtSrc = SecureElement::getInstance().getGenericEseId(SecureElement::getInstance().EE_HANDLE_0xF4 & ~NFA_HANDLE_GROUP_EE); //UICC
        }
        else if(eventData->rcvd_evt.pipe == 0x16) //ESE
        {
            ALOGV("%s: NFA_HCI_EVENT_RCVD_EVT; source ESE",fn);
            evtSrc = SecureElement::getInstance().getGenericEseId(EE_HANDLE_0xF3 & ~NFA_HANDLE_GROUP_EE); //ESE
        }
        else if(nfcFL.nfccFL._NFC_NXP_STAT_DUAL_UICC_WO_EXT_SWITCH && (eventData->rcvd_evt.pipe == 0x23)) /*UICC2*/
        {
            ALOGV("%s: NFA_HCI_EVENT_RCVD_EVT; source UICC2",fn);
            evtSrc = SecureElement::getInstance().getGenericEseId(EE_HANDLE_0xF8 & ~NFA_HANDLE_GROUP_EE); /*UICC2*/
        }
        ALOGV("%s: NFA_HCI_EVENT_RCVD_EVT; ################################### ", fn);

        if(eventData->rcvd_evt.evt_code == NFA_HCI_EVT_WTX)
        {
#if(NXP_EXTNS == TRUE)
            gWtxCount++;
            if(sSecElem.mWmMaxWtxCount >= gWtxCount)
            {
                ALOGV ("%s: NFA_HCI_EVENT_RCVD_EVT: NFA_HCI_EVT_WTX gWtxCount:%d", fn, gWtxCount);
            }
            else
            {
                ALOGV ("%s: NFA_HCI_EVENT_RCVD_EVT: NFA_HCI_EVT_WTX gWtxCount:%d", fn, gWtxCount);
                sSecElem.mTransceiveEvent.notifyOne ();
                break;
            }
#endif
        }
#if(NXP_EXTNS == TRUE)
        else if (sSecElem.IsCmdsentOnOpenDwpSession)
        {
            SyncEventGuard guard (sSecElem.mPipeStatusCheckEvent);
            sSecElem.mCommandStatus = eventData->closed.status;
            sSecElem.mPipeStatusCheckEvent.notifyOne();
            sSecElem.IsCmdsentOnOpenDwpSession = false;

        }
        else if (((eventData->rcvd_evt.evt_code == NFA_HCI_ABORT) || (eventData->rcvd_evt.last_SentEvtType == EVT_ABORT))
                &&(eventData->rcvd_evt.pipe == mStaticPipeProp))
        {
            ALOGV("%s: NFA_HCI_EVENT_RCVD_EVT: NFA_HCI_ABORT; status:0x%X, pipe:0x%X, len:%d", fn,\
                eventData->rcvd_evt.status, eventData->rcvd_evt.pipe, eventData->rcvd_evt.evt_len);
            if(eventData->rcvd_evt.evt_len > 0)
            {
                sSecElem.mAbortEventWaitOk = true;
                SyncEventGuard guard(sSecElem.mAbortEvent);
                memcpy(sSecElem.mAtrInfo, eventData->rcvd_evt.p_evt_buf, eventData->rcvd_evt.evt_len);
                sSecElem.mAtrInfolen = eventData->rcvd_evt.evt_len;
                sSecElem.mAtrStatus = eventData->rcvd_evt.status;
                sSecElem.mAbortEvent.notifyOne();
            }
            else
            {
                sSecElem.mAbortEventWaitOk = false;
                SyncEventGuard guard(sSecElem.mAbortEvent);
                sSecElem.mAbortEvent.notifyOne();
            }
        }
#endif
        else if ((eventData->rcvd_evt.pipe == mStaticPipeProp) || (eventData->rcvd_evt.pipe == STATIC_PIPE_0x71))
        {
            ALOGV("%s: NFA_HCI_EVENT_RCVD_EVT; data from static pipe", fn);
#if (NXP_EXTNS == TRUE)
            if(nfcFL.eseFL._ESE_DWP_SPI_SYNC_ENABLE &&
                    ((spiDwpSyncState & STATE_WK_WAIT_RSP) && (eventData->rcvd_evt.evt_len == 2)))
            {
                spiDwpSyncState ^= STATE_WK_WAIT_RSP;
                SyncEventGuard guard (sSPIForceEnableDWPEvent);
                sSPIForceEnableDWPEvent.notifyOne();
                break;
            }
#endif
            SyncEventGuard guard (sSecElem.mTransceiveEvent);
            sSecElem.mActualResponseSize = (eventData->rcvd_evt.evt_len > MAX_RESPONSE_SIZE) ? MAX_RESPONSE_SIZE : eventData->rcvd_evt.evt_len;
#if(NXP_EXTNS == TRUE)
            if(nfcFL.nfcNxpEse) {
                if(eventData->rcvd_evt.evt_len > 0)
                {
                    sSecElem.mTransceiveWaitOk = true;
                    sSecElem.NfccStandByOperation(STANDBY_TIMER_START);
                }
                /*If there is pending reset event to process*/
                if((nfcFL.eseFL._JCOP_WA_ENABLE) &&(active_ese_reset_control&RESET_BLOCKED)&&
                        (!(active_ese_reset_control &(TRANS_CL_ONGOING))))
                {
                    SyncEventGuard guard (sSecElem.mResetEvent);
                    sSecElem.mResetEvent.notifyOne();
                }
                gWtxCount = 0;
            } else {
                if(eventData->rcvd_evt.evt_len > 0)
                {
                    sSecElem.mTransceiveWaitOk = true;
                }
            }
#endif
            sSecElem.mTransceiveEvent.notifyOne ();
        }
        else if (eventData->rcvd_evt.evt_code == NFA_HCI_EVT_POST_DATA)
        {
            ALOGV("%s: NFA_HCI_EVENT_RCVD_EVT; NFA_HCI_EVT_POST_DATA", fn);
            SyncEventGuard guard (sSecElem.mTransceiveEvent);
            sSecElem.mActualResponseSize = (eventData->rcvd_evt.evt_len > MAX_RESPONSE_SIZE) ? MAX_RESPONSE_SIZE : eventData->rcvd_evt.evt_len;
            sSecElem.mTransceiveEvent.notifyOne ();
        }
        else if (eventData->rcvd_evt.evt_code == NFA_HCI_EVT_TRANSACTION)
        {
            ALOGV("%s: NFA_HCI_EVENT_RCVD_EVT; NFA_HCI_EVT_TRANSACTION", fn);
            // If we got an AID, notify any listeners
            if ((eventData->rcvd_evt.evt_len > 3) && (eventData->rcvd_evt.p_evt_buf[0] == 0x81) )
            {
                int aidlen = eventData->rcvd_evt.p_evt_buf[1];
                uint8_t* data = NULL;
                int32_t datalen = 0;
                uint8_t dataStartPosition = 0;
                if((eventData->rcvd_evt.evt_len > 2+aidlen) && (eventData->rcvd_evt.p_evt_buf[2+aidlen] == 0x82))
                {
                    //BERTLV decoding here, to support extended data length for params.
                    datalen = SecureElement::decodeBerTlvLength((uint8_t *)eventData->rcvd_evt.p_evt_buf, 2+aidlen+1, eventData->rcvd_evt.evt_len);
                }
                if(datalen >= 0)
                {
                    /* Over 128 bytes data of transaction can not receive on PN547, Ref. BER-TLV length fields in ISO/IEC 7816 */
                    if ( datalen < 0x80)
                    {
                        dataStartPosition = 2+aidlen+2;
                    }
                    else if ( datalen < 0x100)
                    {
                        dataStartPosition = 2+aidlen+3;
                    }
                    else if ( datalen < 0x10000)
                    {
                        dataStartPosition = 2+aidlen+4;
                    }
                    else if ( datalen < 0x1000000)
                    {
                        dataStartPosition = 2+aidlen+5;
                    }
                    data  = &eventData->rcvd_evt.p_evt_buf[dataStartPosition];
                    sSecElem.notifyTransactionListenersOfAid (&eventData->rcvd_evt.p_evt_buf[2],aidlen,data,datalen,evtSrc);
                }
                else
                {
                    ALOGE("Event data TLV length encoding Unsupported!");
                }
            }
        }
        else if (eventData->rcvd_evt.evt_code == NFA_HCI_EVT_CONNECTIVITY)
        {
            ALOGV("%s: NFA_HCI_EVENT_RCVD_EVT; NFA_HCI_EVT_CONNECTIVITY", fn);

//            int pipe = (eventData->rcvd_evt.pipe);                            /*commented to eliminate unused variable warning*/
                sSecElem.notifyConnectivityListeners (evtSrc);
        }
        else
        {
            ALOGV("%s: NFA_HCI_EVENT_RCVD_EVT; ################################### eventData->rcvd_evt.evt_code = 0x%x , NFA_HCI_EVT_CONNECTIVITY = 0x%x", fn, eventData->rcvd_evt.evt_code, NFA_HCI_EVT_CONNECTIVITY);

            ALOGV("%s: NFA_HCI_EVENT_RCVD_EVT; ################################### ", fn);

        }
        break;

    case NFA_HCI_SET_REG_RSP_EVT: //received response to write registry command
        {
            tNFA_HCI_REGISTRY& registry = eventData->registry;
            ALOGV("%s: NFA_HCI_SET_REG_RSP_EVT; status=0x%X; pipe=0x%X", fn, registry.status, registry.pipe);
            SyncEventGuard guard (sSecElem.mRegistryEvent);
            sSecElem.mRegistryEvent.notifyOne ();
            break;
        }
#if(NXP_EXTNS == TRUE)
    case NFA_HCI_CONFIG_DONE_EVT:
        {
            ALOGV("%s: NFA_HCI_CONFIG_DONE_EVT; status=0x%X", fn, eventData->admin_rsp_rcvd.status);
            sSecElem.mETSI12InitStatus = eventData->admin_rsp_rcvd.status;
            SyncEventGuard guard(sSecElem.mNfceeInitCbEvent);
            sSecElem.mNfceeInitCbEvent.notifyOne();
            break;
        }
    case NFA_HCI_EE_RECOVERY_EVT:
    {
        tNFA_HCI_EE_RECOVERY_EVT &ee_recovery = eventData->ee_recovery;
        ALOGV("%s: NFA_HCI_EE_RECOVERY_EVT; status=0x%X", fn, ee_recovery.status);
        if(ee_recovery.status == NFA_HCI_EE_RECOVERY_STARTED)
            RoutingManager::getInstance().setEERecovery(true);
        else if(ee_recovery.status == NFA_HCI_EE_RECOVERY_COMPLETED)
        {
            ALOGV("%s: NFA_HCI_EE_RECOVERY_EVT; recovery completed status=0x%X", fn, ee_recovery.status);
            RoutingManager::getInstance().setEERecovery(false);
            if(active_ese_reset_control & TRANS_WIRED_ONGOING)
            {
                SyncEventGuard guard(sSecElem.mTransceiveEvent);
                sSecElem.mTransceiveEvent.notifyOne();
            }
        }
        break;
    }
    case NFA_HCI_ADD_STATIC_PIPE_EVT:
    {
        ALOGV("%s: NFA_HCI_ADD_STATIC_PIPE_EVT; status=0x%X", fn, eventData->admin_rsp_rcvd.status);
        SyncEventGuard guard(sSecElem.mHciAddStaticPipe);
        sSecElem.mHciAddStaticPipe.notifyOne();
        break;
    }
#endif
    default:
        ALOGE("%s: unknown event code=0x%X ????", fn, event);
        break;
    }
}


/*******************************************************************************
**
** Function:        findEeByHandle
**
** Description:     Find information about an execution environment.
**                  eeHandle: Handle to execution environment.
**
** Returns:         Information about an execution environment.
**
*******************************************************************************/
tNFA_EE_INFO *SecureElement::findEeByHandle (tNFA_HANDLE eeHandle)
{
    for (uint8_t xx = 0; xx < mActualNumEe; xx++)
    {
        if (mEeInfo[xx].ee_handle == eeHandle)
            return (&mEeInfo[xx]);
    }
    return (NULL);
}

/*******************************************************************************
**
** Function:        getSETechnology
**
** Description:     return the technologies suported by se.
**                  eeHandle: Handle to execution environment.
**
** Returns:         Information about an execution environment.
**
*******************************************************************************/
jint SecureElement::getSETechnology(tNFA_HANDLE eeHandle)
{
    int tech_mask = 0x00;
    static const char fn [] = "SecureElement::getSETechnology";
    // Get Fresh EE info.
    if (! getEeInfo())
    {
        ALOGE("%s: No updated eeInfo available", fn);
    }

    tNFA_EE_INFO* eeinfo = findEeByHandle(eeHandle);

    if(eeinfo!=NULL){
        if(eeinfo->la_protocol != 0x00)
        {
            tech_mask |= 0x01;
        }

        if(eeinfo->lb_protocol != 0x00)
        {
            tech_mask |= 0x02;
        }

        if(eeinfo->lf_protocol != 0x00)
        {
            tech_mask |= 0x04;
        }
    }

    return tech_mask;
}
/*******************************************************************************
**
** Function:        getDefaultEeHandle
**
** Description:     Get the handle to the execution environment.
**
** Returns:         Handle to the execution environment.
**
*******************************************************************************/
tNFA_HANDLE SecureElement::getDefaultEeHandle ()
{
    static const char fn [] = "SecureElement::activate";

    ALOGV("%s: - Enter", fn);
    ALOGV("%s: - mActualNumEe = %x mActiveSeOverride = 0x%02X", fn,mActualNumEe, mActiveSeOverride);

    uint16_t overrideEeHandle = NFA_HANDLE_GROUP_EE | mActiveSeOverride;
    // Find the first EE that is not the HCI Access i/f.
    for (uint8_t xx = 0; xx < mActualNumEe; xx++)
    {
        if ( (mActiveSeOverride != ACTIVE_SE_USE_ANY) && (overrideEeHandle != mEeInfo[xx].ee_handle))
            continue; //skip all the EE's that are ignored
        ALOGV("%s: - mEeInfo[xx].ee_handle = 0x%02x, mEeInfo[xx].ee_status = 0x%02x", fn,mEeInfo[xx].ee_handle, mEeInfo[xx].ee_status);

              if((nfcFL.nfccFL._GEMALTO_SE_SUPPORT && (mEeInfo[xx].ee_interface[0] != NCI_NFCEE_INTERFACE_HCI_ACCESS)))
              {
                  return (mEeInfo[xx].ee_handle);
              }
              else if((!nfcFL.nfccFL._GEMALTO_SE_SUPPORT && ((mEeInfo[xx].ee_handle == EE_HANDLE_0xF3
                  || mEeInfo[xx].ee_handle == SecureElement::getInstance().EE_HANDLE_0xF4
                  || (mEeInfo[xx].ee_handle == EE_HANDLE_0xF8 && nfcFL.nfccFL._NFCC_DYNAMIC_DUAL_UICC))
                  && (mEeInfo[xx].ee_status != NFC_NFCEE_STATUS_INACTIVE))))
              {
                  return (mEeInfo[xx].ee_handle);
              }

    }
    return NFA_HANDLE_INVALID;
}
#if(NXP_EXTNS == TRUE)
/*******************************************************************************
**
** Function:        getActiveEeHandle
**
** Description:     Get the handle to the execution environment.
**
** Returns:         Handle to the execution environment.
**
*******************************************************************************/
tNFA_HANDLE SecureElement::getActiveEeHandle (tNFA_HANDLE handle)
{
    static const char fn [] = "SecureElement::getActiveEeHandle";

    ALOGE("%s: - Enter", fn);
    ALOGE("%s: - mActualNumEe = %x mActiveSeOverride = 0x%02X", fn,mActualNumEe, mActiveSeOverride);

    uint16_t overrideEeHandle = NFA_HANDLE_GROUP_EE | mActiveSeOverride;
    ALOGE("%s: - mActualNumEe = %x overrideEeHandle = 0x%02X", fn,mActualNumEe, overrideEeHandle);

    for (uint8_t xx = 0; xx < mActualNumEe; xx++)
    {
        if ( (mActiveSeOverride != ACTIVE_SE_USE_ANY) && (overrideEeHandle != mEeInfo[xx].ee_handle))
             ALOGE("%s: - mEeInfo[xx].ee_handle = 0x%02x, mEeInfo[xx].ee_status = 0x%02x", fn,mEeInfo[xx].ee_handle, mEeInfo[xx].ee_status);

        if(nfcFL.nfccFL._GEMALTO_SE_SUPPORT && (mEeInfo[xx].ee_interface[0] != NCI_NFCEE_INTERFACE_HCI_ACCESS)
             && (mEeInfo[xx].ee_status != NFC_NFCEE_STATUS_INACTIVE) && (mEeInfo[xx].ee_handle == handle))
         {
             return (mEeInfo[xx].ee_handle);
         }
         else if (!nfcFL.nfccFL._GEMALTO_SE_SUPPORT && (mEeInfo[xx].ee_handle == EE_HANDLE_0xF3 || mEeInfo[xx].ee_handle == SecureElement::getInstance().EE_HANDLE_0xF4
              || (mEeInfo[xx].ee_handle == EE_HANDLE_0xF8 && nfcFL.nfccFL._NFCC_DYNAMIC_DUAL_UICC))
              && (mEeInfo[xx].ee_status != NFC_NFCEE_STATUS_INACTIVE) && (mEeInfo[xx].ee_handle == handle))
         {
             return (mEeInfo[xx].ee_handle);
         }

    }
    return NFA_HANDLE_INVALID;
}
#endif
/*******************************************************************************
**
** Function:        findUiccByHandle
**
** Description:     Find information about an execution environment.
**                  eeHandle: Handle of the execution environment.
**
** Returns:         Information about the execution environment.
**
*******************************************************************************/
tNFA_EE_DISCOVER_INFO *SecureElement::findUiccByHandle (tNFA_HANDLE eeHandle)
{
    for (uint8_t index = 0; index < mUiccInfo.num_ee; index++)
    {
        if (mUiccInfo.ee_disc_info[index].ee_handle == eeHandle)
        {
            return (&mUiccInfo.ee_disc_info[index]);
        }
    }
    ALOGE("SecureElement::findUiccByHandle:  ee h=0x%4x not found", eeHandle);
    return NULL;
}


/*******************************************************************************
**
** Function:        eeStatusToString
**
** Description:     Convert status code to status text.
**                  status: Status code
**
** Returns:         None
**
*******************************************************************************/
const char* SecureElement::eeStatusToString (uint8_t status)
{
    switch (status)
    {
    case NFC_NFCEE_STATUS_ACTIVE:
        return("Connected/Active");
    case NFC_NFCEE_STATUS_INACTIVE:
        return("Connected/Inactive");
    case NFC_NFCEE_STATUS_REMOVED:
        return("Removed");
    }
    return("?? Unknown ??");
}


/*******************************************************************************
**
** Function:        connectionEventHandler
**
** Description:     Receive card-emulation related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void SecureElement::connectionEventHandler (uint8_t event, tNFA_CONN_EVT_DATA* /*eventData*/)
{
    switch (event)
    {
    case NFA_CE_UICC_LISTEN_CONFIGURED_EVT:
        {
            SyncEventGuard guard (mUiccListenEvent);
            mUiccListenEvent.notifyOne ();
        }
        break;

    case NFA_CE_ESE_LISTEN_CONFIGURED_EVT:
        {
            SyncEventGuard guard (mEseListenEvent);
            mEseListenEvent.notifyOne ();
        }
        break;
    }

}
/*******************************************************************************
**
** Function:        getAtr
**
** Description:     GetAtr response from the connected eSE
**
** Returns:         Returns True if success
**
*******************************************************************************/
bool SecureElement::getAtr(jint seID, uint8_t* recvBuffer, int32_t *recvBufferSize)
{
    static const char fn[] = "SecureElement::getAtr";
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    uint8_t reg_index = 0x01;
    int timeoutMillisec = 10000;
    ALOGV("%s: enter; seID=0x%X", fn, seID);
#if (NXP_EXTNS == TRUE)
    if(nfcFL.nfcNxpEse) {
        se_apdu_gate_info gateInfo = NO_APDU_GATE;

        if(nfcFL.nfccFL._NFCEE_REMOVED_NTF_RECOVERY) {
            if((RoutingManager::getInstance().is_ee_recovery_ongoing()))
            {
                SyncEventGuard guard (mEEdatapacketEvent);
                if(mEEdatapacketEvent.wait(android::gMaxEERecoveryTimeout) == false)
                {
                    return false;
                }
            }
        }
        if(!checkForWiredModeAccess())
        {
            ALOGV("Denying /atr in SE listen mode active");
            return false;
        }
        if ((((nfcFL.eseFL._NXP_ESE_VER == JCOP_VER_3_1) || (nfcFL.eseFL._NXP_ESE_VER == JCOP_VER_3_2))) &&NFC_GetEseAccess((void *)&timeoutMillisec) != 0)
        {
            ALOGE("%s: NFC_ReqWiredAccess timeout", fn);
            return false;
        }
        NfccStandByOperation(STANDBY_MODE_OFF);

        gateInfo = getApduGateInfo();
        if(gateInfo == PROPREITARY_APDU_GATE)
        {
            SyncEventGuard guard (mGetRegisterEvent);
            nfaStat = NFA_HciGetRegistry (mNfaHciHandle, mNewPipeId, reg_index);
            if(nfaStat == NFA_STATUS_OK)
            {
                mGetAtrRspwait = true;
                mGetRegisterEvent.wait();
                ALOGV("%s: Received ATR response on pipe 0x%x ", fn, mNewPipeId);
            }
            *recvBufferSize = mAtrInfolen;
            memcpy(recvBuffer, mAtrInfo, mAtrInfolen);
        }
        else if(gateInfo == ETSI_12_APDU_GATE)
        {
            mAbortEventWaitOk = false;
            uint8_t mAtrInfo1[EVT_ABORT_MAX_RSP_LEN]={0};
            uint8_t atr_len = EVT_ABORT_MAX_RSP_LEN;
            SyncEventGuard guard (mAbortEvent);
            nfaStat = NFA_HciSendEvent(mNfaHciHandle, mNewPipeId, EVT_ABORT, 0, NULL, atr_len, mAtrInfo1, timeoutMillisec);
            if(nfaStat == NFA_STATUS_OK)
            {
                mAbortEvent.wait();
            }
            if(mAbortEventWaitOk == false)
            {
                ALOGE("%s (EVT_ABORT)Wait reposne timeout", fn);
                nfaStat = NFA_STATUS_FAILED;
            }
            else
            {
                *recvBufferSize = mAtrInfolen;
                memcpy(recvBuffer, mAtrInfo, mAtrInfolen);
            }
        }

        if((nfcFL.eseFL._JCOP_WA_ENABLE) && (mAtrStatus == NFA_HCI_ANY_E_NOK))
            reconfigureEseHciInit();

        if (((nfcFL.eseFL._NXP_ESE_VER == JCOP_VER_3_1) || (nfcFL.eseFL._NXP_ESE_VER == JCOP_VER_3_2)) &&NFC_RelEseAccess((void *)&nfaStat) != 0)
        {
            ALOGE("%s: NFC_ReqWiredAccess timeout", fn);
        }
#endif
    }
    return (nfaStat == NFA_STATUS_OK)?true:false;
}

/*******************************************************************************
**
** Function:        routeToSecureElement
**
** Description:     Adjust controller's listen-mode routing table so transactions
**                  are routed to the secure elements.
**
** Returns:         True if ok.
**
*******************************************************************************/
bool SecureElement::routeToSecureElement ()
{
    static const char fn [] = "SecureElement::routeToSecureElement";
    ALOGV("%s: enter", fn);

//    tNFA_TECHNOLOGY_MASK tech_mask = NFA_TECHNOLOGY_MASK_A | NFA_TECHNOLOGY_MASK_B;   /*commented to eliminate unused variable warning*/
    bool retval = false;

    if (! mIsInit)
    {
        ALOGE("%s: not init", fn);
        return false;
    }

    if (mCurrentRouteSelection == SecElemRoute)
    {
        ALOGE("%s: already sec elem route", fn);
        return true;
    }

    if (mActiveEeHandle == NFA_HANDLE_INVALID)
    {
        ALOGE("%s: invalid EE handle", fn);
        return false;
    }

/*    tNFA_EE_INFO* eeinfo = findEeByHandle(mActiveEeHandle);
    if(eeinfo!=NULL){
        if(eeinfo->la_protocol == 0x00 && eeinfo->lb_protocol != 0x00 )
        {
            gTypeB_listen = true;
        }
    }*/

    ALOGV("%s: exit; ok=%u", fn, retval);
    return retval;
}


/*******************************************************************************
**
** Function:        isBusy
**
** Description:     Whether controller is routing listen-mode events to
**                  secure elements or a pipe is connected.
**
** Returns:         True if either case is true.
**
*******************************************************************************/
bool SecureElement::isBusy ()
{
    bool retval = mIsPiping ;
    ALOGV("SecureElement::isBusy: %u", retval);
    return retval;
}

/*******************************************************************************
**
** Function:        getGenericEseId
**
** Description:     Whether controller is routing listen-mode events to
**                  secure elements or a pipe is connected.
**
** Returns:         Return the generic SE id ex:- 00,01,02,04
**
*******************************************************************************/
jint SecureElement::getGenericEseId(tNFA_HANDLE handle)
{
    jint ret = 0xFF;
    static const char fn [] = "SecureElement::getGenericEseId";
    ALOGV("%s: enter; ESE-Handle = 0x%X", fn, handle);
    //Map the actual handle to generic id
    if(handle == (EE_HANDLE_0xF3 & ~NFA_HANDLE_GROUP_EE) ) //ESE - 0xC0
    {
        ret = ESE_ID;
    }
    else if(handle ==  (SecureElement::getInstance().EE_HANDLE_0xF4 & ~NFA_HANDLE_GROUP_EE) ) //UICC - 0x02
    {
        ret = UICC_ID;
    }
    else if((nfcFL.nfccFL._NFCC_DYNAMIC_DUAL_UICC ||
            nfcFL.nfccFL._NFC_NXP_STAT_DUAL_UICC_WO_EXT_SWITCH) &&
            (handle ==  (EE_HANDLE_0xF8 & ~NFA_HANDLE_GROUP_EE))) //UICC2 - 0x04
    {
        ret = UICC2_ID;
    }
    ALOGV("%s: exit; ESE-Generic-ID = 0x%02X", fn, ret);
    return ret;
}

/*******************************************************************************
**
** Function:        getEseHandleFromGenericId
**
** Description:     Whether controller is routing listen-mode events to
**                  secure elements or a pipe is connected.
**
** Returns:         Returns Secure element Handle ex:- 402, 4C0, 481
**
*******************************************************************************/
tNFA_HANDLE SecureElement::getEseHandleFromGenericId(jint eseId)
{
    uint16_t handle = NFA_HANDLE_INVALID;
    static const char fn [] = "SecureElement::getEseHandleFromGenericId";
    ALOGV("%s: enter; ESE-ID = 0x%02X", fn, eseId);

    //Map the generic id to actual handle
    if(eseId == ESE_ID) //ESE
    {
        handle = EE_HANDLE_0xF3; //0x4C0;
    }
    else if(eseId == UICC_ID) //UICC
    {
        handle = SecureElement::getInstance().EE_HANDLE_0xF4; //0x402;
    }
    else if((nfcFL.nfccFL._NFCC_DYNAMIC_DUAL_UICC ||
            nfcFL.nfccFL._NFC_NXP_STAT_DUAL_UICC_WO_EXT_SWITCH) && (eseId == UICC2_ID)) //UICC
    {
        handle = EE_HANDLE_0xF8; //0x481;
    }
    else if(eseId == DH_ID) //Host
    {
        handle = NFA_EE_HANDLE_DH; //0x400;
    }
    else if(eseId == EE_HANDLE_0xF3 || eseId == EE_HANDLE_0xF4)
    {
        handle = eseId;
    }
    ALOGV("%s: enter; ESE-Handle = 0x%03X", fn, handle);
    return handle;
}
bool SecureElement::SecEle_Modeset(uint8_t type)
{
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    bool retval = true;

    ALOGV("set EE mode = 0x%X", type);
#if(NXP_EXTNS == TRUE)
    if ((nfaStat = SecElem_EeModeSet (EE_HANDLE_0xF3, type)) == NFA_STATUS_OK)
    {
#if 0
        if (eeItem.ee_status == NFC_NFCEE_STATUS_INACTIVE)
        {
            ALOGE("NFA_EeModeSet enable or disable success; status=0x%X", nfaStat);
            retval = true;
        }
#endif
    }
    else
#endif
    {
        retval = false;
        ALOGE("NFA_EeModeSet failed; error=0x%X",nfaStat);
    }
    return retval;
}


/*******************************************************************************
**
** Function:        getEeHandleList
**
** Description:     Get default Secure Element handle.
**                  isHCEEnabled: whether host routing is enabled or not.
**
** Returns:         Returns Secure Element list and count.
**
*******************************************************************************/
void SecureElement::getEeHandleList(tNFA_HANDLE *list, uint8_t* count)
{
    tNFA_HANDLE handle;
    int i;
    static const char fn [] = "SecureElement::getEeHandleList";
    *count = 0;
    for ( i = 0; i < mActualNumEe; i++)
    {
        ALOGV("%s: %d = 0x%X", fn, i, mEeInfo[i].ee_handle);
        if ((mEeInfo[i].ee_handle == 0x401) || (mEeInfo[i].ee_interface[0] == NCI_NFCEE_INTERFACE_HCI_ACCESS) ||
            (mEeInfo[i].ee_status == NFC_NFCEE_STATUS_INACTIVE))
        {
            ALOGV("%s: %u = 0x%X", fn, i, mEeInfo[i].ee_handle);
            ALOGV("0x%x: 0x%x 0x%x 0x%x",mEeInfo[i].ee_handle, mEeInfo[i].num_interface, mEeInfo[i].ee_interface[0], mEeInfo[i].ee_status);
            continue;
        }

        handle = mEeInfo[i].ee_handle & ~NFA_HANDLE_GROUP_EE;
        list[*count] = handle;
        *count = *count + 1 ;
        ALOGV("%s: Handle %d = 0x%X", fn, i, handle);
    }
}

bool SecureElement::sendEvent(uint8_t event)
{
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    bool retval = true;

    nfaStat = NFA_HciSendEvent (mNfaHciHandle, mNewPipeId, event, 0x00, NULL, 0x00,NULL, 0);

    if(nfaStat != NFA_STATUS_OK)
        retval = false;

    return retval;
}
#if (NXP_EXTNS == TRUE)
bool SecureElement::configureNfceeETSI12()
{
    static const char fn [] = "SecureElement::configureNfceeETSI12";
    ALOGV("%s: enter", fn);
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    bool retval = true;

    nfaStat = NFA_HciConfigureNfceeETSI12();

    if(nfaStat != NFA_STATUS_OK)
        retval = false;

    return retval;
}

bool SecureElement::checkPipeStatusAndRecreate()
{
    bool pipeCorrectStatus = false;
    bool success = true;
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    se_apdu_gate_info gateInfo = NO_APDU_GATE;
    uint8_t xmitBuffer[] = {0x00, 0x00, 0x00, 0x00};
    uint8_t EVT_SEND_DATA = 0x10;
    ALOGD ("checkPipeStatusAndRecreate: Enter");
    pipeCorrectStatus = NFA_IsPipeStatusNotCorrect();
    if(pipeCorrectStatus)
    {
        gateInfo = getApduGateInfo();
        if(gateInfo == ETSI_12_APDU_GATE)
        {
            pipeStatus = NFA_HCI_ANY_OK;
            IsCmdsentOnOpenDwpSession = true;
            SyncEventGuard guard (mPipeStatusCheckEvent);

            nfaStat = NFA_HciSendEvent(mNfaHciHandle, mNewPipeId, EVT_SEND_DATA, sizeof(xmitBuffer), xmitBuffer, 0x00, NULL, 0);
            if(nfaStat == NFA_STATUS_OK)
            {
                mPipeStatusCheckEvent.wait(500);
            }
            IsCmdsentOnOpenDwpSession = false;
            if(pipeStatus == NFA_HCI_ANY_E_PIPE_NOT_OPENED)
            {
                SyncEventGuard guard (mPipeOpenedEvent);
                nfaStat = NFA_HciOpenPipe (mNfaHciHandle, mNewPipeId);
                if (nfaStat != NFA_STATUS_OK)
                {
                    ALOGE("checkPipeStatusAndRecreate:fail open pipe; error=0x%X", nfaStat);
                }

                if(!mPipeOpenedEvent.wait (500) || (mCommandStatus != NFA_STATUS_OK))
                {
                    success = false;
                }
            }
        }
    }
 ALOGD ("checkPipeStatusAndRecreate: Exit status x%x",success);
 return success;
}
/*******************************************************************************
**
** Function:        getUiccGateAndPipeList
**
** Description:     Get the UICC's gate and pipe list present
**
** Returns:         Returns valid PipeId(success) or zero(Failure).
**
*******************************************************************************/
uint8_t SecureElement::getUiccGateAndPipeList(uint8_t uiccNo)
{
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    uint8_t pipeId = 0;
    static const char fn[] = "SecureElement::getUiccGateAndPipeList";
    ALOGV("%s : get gate, pipe list mNfaHandle = %x ", fn, mNfaHciHandle);
    /*HCI initialised and secure element available*/
    if((mNfaHciHandle != NFA_HANDLE_INVALID))
    {
        SyncEventGuard guard (mPipeListEvent);
        nfaStat = NFA_HciGetGateAndPipeList (mNfaHciHandle);
        if (nfaStat == NFA_STATUS_OK)
        {
            mPipeListEvent.wait();
            if (mHciCfg.status == NFA_STATUS_OK)
            {
                for (uint8_t xx = 0; xx < mHciCfg.num_uicc_created_pipes; xx++)
                {
                    ALOGV("%s : get gate, pipe list host = 0x%x gate = 0x%x",fn, mHciCfg.pipe[xx].dest_host,
                            mHciCfg.uicc_created_pipe[xx].dest_gate);
                    if (mHciCfg.uicc_created_pipe[xx].dest_gate == NFA_HCI_CONNECTIVITY_GATE)
                    {
                        ALOGV("%s: found configured gate: 0x%02x  pipe: 0x%02x",fn, mNewSourceGate, mNewPipeId);
                        if((uiccNo == (EE_HANDLE_0xF4 & ~NFA_HANDLE_GROUP_EE) && mHciCfg.uicc_created_pipe[xx].pipe_id == CONNECTIVITY_PIPE_ID_UICC1) ||
                            (uiccNo == (EE_HANDLE_0xF8 & ~NFA_HANDLE_GROUP_EE) && mHciCfg.uicc_created_pipe[xx].pipe_id == CONNECTIVITY_PIPE_ID_UICC2))
                        {
                            ALOGV("Found the pipeId = %x", mHciCfg.uicc_created_pipe[xx].pipe_id);
                            pipeId = mHciCfg.uicc_created_pipe[xx].pipe_id;
                            break;
                        }
                    }
                    else
                    {
                        ALOGV("%s : No GatePresent", fn);
                    }
                }
            }
        }
    }
    return pipeId;
}
/*******************************************************************************
**
** Function:        getHciHandleInfo
**
** Description:     Get the NFA hci handle registered with JNI
**
** Returns:         Returns valid PipeId(success) or zero(Failure).
**
*******************************************************************************/
tNFA_HANDLE SecureElement::getHciHandleInfo()
{
    return mNfaHciHandle;
}
/*******************************************************************************
**
** Function:        eSE_ClearAllPipe_thread_handler
**
** Description:     Upon receiving the Clear ALL pipes take lock and start polling
**
** Returns:         void.
**
*******************************************************************************/
void *eSE_ClearAllPipe_thread_handler(void *data)
{
    static const char fn [] = "eSE_ClearAllPipe_thread_handler";
    uint8_t *host = NULL, nfcee_type = 0;
    SecureElement &se = SecureElement::getInstance();
    ALOGV("%s: enter:", fn);
    if(NULL == data)
    {
        ALOGE("%s: Invalid argument", fn);
        return NULL;
    }
    /*Nfc initialization not completed*/
    if(sNfcee_disc_state < UICC_SESSION_INTIALIZATION_DONE)
    {
        ALOGV("eSE_ClearAllPipe_thread_handler PENDING return");
        sNfcee_disc_state = UICC_CLEAR_ALL_PIPE_NTF_RECEIVED;
        return NULL;
    }
    ALOGV("eSE_ClearAllPipe_thread_handler state NFCEE_STATE_DISCOVERED");
    /* Copying the host-id from the parent thread and freeing allocated space */
    host = (uint8_t *) data;
    nfcee_type = *host;
    free(data);

    SyncEventGuard guard (se.mNfceeInitCbEvent);
    switch(nfcee_type)
    {
        case 0x02:
        case 0x80:
            /* Poll for UICC1 session */
            android ::checkforNfceeConfig(UICC1|UICC2);
            break;
        case 0x81:
            /* Poll for UICC2 session */
            android ::checkforNfceeConfig(UICC1|UICC2);
            break;
        default:
            break;
    }
    se.mNfceeInitCbEvent.notifyOne();
    return NULL;
}

/*******************************************************************************
**
** Function:        initializeEeHandle
**
** Description:     Set NFCEE handle.
**
** Returns:         True if ok.
**
*******************************************************************************/
bool SecureElement::initializeEeHandle ()
{
    if(NFA_GetNCIVersion() == NCI_VERSION_2_0)
        EE_HANDLE_0xF4 = 0x480;
    else
        EE_HANDLE_0xF4 = 0x402;
    return true;
}
/*******************************************************************************
**
** Function:        eSE_ClearAllPipe_handler
**
** Description:     Handler for Clear ALL pipes ntf recreate
**                  APDU pipe for eSE
**
** Returns:         void.
**
*******************************************************************************/
void SecureElement::eSE_ClearAllPipe_handler(uint8_t host)
{

    static const char fn [] = "SecureElement::eSE_ClearAllPipe_handler";
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    uint8_t *host_id = (uint8_t *) malloc(sizeof(uint8_t));
    *host_id = host;
    ALOGE("%s; Enter", fn);
    if (pthread_create (&thread, &attr,  &eSE_ClearAllPipe_thread_handler, (void*)host_id) < 0)
    {
        ALOGV("Thread creation failed");
    }
    else
    {
        ALOGV("Thread creation success");
    }
    pthread_attr_destroy(&attr);

}

#endif


#if(NXP_EXTNS == TRUE)
/*******************************************************************************
**
** Function         NfccStandByTimerOperation
**
** Description      start/stops the standby timer
**
** Returns          void
**
*******************************************************************************/
void SecureElement::NfccStandByOperation(nfcc_standby_operation_t value)
{
    if(!nfcFL.nfcNxpEse) {
        ALOGV("%s  nfcNxpEse not available. Returning",__func__);
        return;
    }
    static IntervalTimer   mNFCCStandbyModeTimer; // timer to enable standby mode for NFCC
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    bool stat = false;
    mNfccStandbyMutex.lock();
    ALOGV("In SecureElement::NfccStandByOperation value = %d, state = %d", value, standby_state);
    switch(value)
    {
    case STANDBY_TIMER_START:
        standby_state = STANDBY_MODE_TIMER_ON;
        if(nfccStandbytimeout > 0)
        {
            mNFCCStandbyModeTimer.set(nfccStandbytimeout , NFCC_StandbyModeTimerCallBack );
        }
        break;
    case STANDBY_MODE_OFF:
    {
        if(nfcFL.eseFL._WIRED_MODE_STANDBY) {
            if(standby_state == STANDBY_MODE_SUSPEND) {
                if(mNfccPowerMode == 1) {
                    nfaStat = setNfccPwrConfig(POWER_ALWAYS_ON|COMM_LINK_ACTIVE);
                    if(nfaStat != NFA_STATUS_OK) {
                        ALOGV("%s: power link command failed", __func__);
                        break;
                    } else {
                        SecEle_Modeset(0x01);
                    }
                }
            }
        }
    }// this is to handle stop timer also.
    case STANDBY_TIMER_STOP:
        {
            if(nfccStandbytimeout > 0)
                mNFCCStandbyModeTimer.kill();
        }
        standby_state = STANDBY_MODE_OFF;
        if(spiDwpSyncState & STATE_DWP_CLOSE) {
            spiDwpSyncState ^= STATE_DWP_CLOSE;
        }
    break;
    case STANDBY_MODE_ON:
    {
        if(nfcFL.eseFL._WIRED_MODE_STANDBY_PROP) {
            if(standby_state == STANDBY_MODE_ON)
                break;
            else if(nfccStandbytimeout > 0)
                mNFCCStandbyModeTimer.kill();
        }
        if(nfcFL.eseFL._WIRED_MODE_STANDBY)
            if(nfccStandbytimeout > 0)
                mNFCCStandbyModeTimer.kill();

        if (nfcFL.eseFL._WIRED_MODE_STANDBY) {
            if (nfcFL.eseFL._ESE_DWP_SPI_SYNC_ENABLE) {
                /*To maintain dwp standby mode on case when spi is close later*/
                spiDwpSyncState = STATE_IDLE;
            if(dual_mode_current_state & SPI_ON)
            {
                ALOGV("%s: SPI is ON-StandBy not allowed", __func__);
                standby_state = STANDBY_MODE_ON;
                /*To maintain wired session close state during SPI on*/
                spiDwpSyncState = STATE_DWP_CLOSE;
                break;
            }
            nfaStat = setNfccPwrConfig(NFCC_DECIDES);
            stat = SecureElement::getInstance().sendEvent(SecureElement::EVT_END_OF_APDU_TRANSFER);
            if(stat)
            {
                standby_state = STANDBY_MODE_ON;
                ALOGV("%s sending standby mode command EVT_END_OF_APDU_TRANSFER successful", __func__);
            }
        }
    }
}
    if(nfcFL.eseFL._WIRED_MODE_STANDBY == true)
        break;
    case STANDBY_TIMER_TIMEOUT:
    {
        bool stat = false;
        if (nfcFL.eseFL._ESE_DWP_SPI_SYNC_ENABLE) {
            if (nfcFL.eseFL._WIRED_MODE_STANDBY) {
                /*Maintain suspend apdu state to send when spi is closed later*/
                spiDwpSyncState |= STATE_TIME_OUT;
        /*Clear apdu state to activate DWP link once spi stand-alone triggered */
        if(spiDwpSyncState & STATE_WK_ENBLE)
        {
            spiDwpSyncState ^= STATE_WK_ENBLE;
        }
    }
    if(dual_mode_current_state & SPI_ON)
    {
        ALOGV("%s: SPI is ON-StandBy not allowed", __func__);
        standby_state = STANDBY_MODE_ON;
        mNfccStandbyMutex.unlock();
        return;
    }
    }
        if(nfcFL.eseFL._WIRED_MODE_STANDBY == true) {
            if(standby_state != STANDBY_MODE_TIMER_ON) {
                ALOGV("%s the timer must be stopped by next atr/transceive, ignoring timeout", __func__);
                break;
            }
        }
    if(nfcFL.eseFL._WIRED_MODE_STANDBY_PROP)
        /*Send the EVT_END_OF_APDU_TRANSFER  after the transceive timer timed out*/
        stat = SecureElement::getInstance().sendEvent(SecureElement::EVT_END_OF_APDU_TRANSFER);
    if (nfcFL.eseFL._WIRED_MODE_STANDBY) {
        setNfccPwrConfig(POWER_ALWAYS_ON);
        stat = SecureElement::getInstance().sendEvent(SecureElement::EVT_SUSPEND_APDU_TRANSFER);
    }
        if(stat)
        {
            standby_state = STANDBY_MODE_SUSPEND;
            ALOGV("%s sending standby command successful", __func__);
            if (nfcFL.eseFL._ESE_DWP_SPI_SYNC_ENABLE && nfcFL.eseFL._WIRED_MODE_STANDBY_PROP)
                spiDwpSyncState = STATE_IDLE;
        }
    }
    break;
    case STANDBY_GPIO_HIGH:
    {
        jint ret_val = -1;
        NFCSTATUS status = NFCSTATUS_FAILED;

        /* Set the ESE VDD gpio to high to make sure P61 is powered, even if NFCC
         * is in standby
         */
        ret_val = NFC_EnableWired ((void *)&status);
        if (ret_val < 0)
        {
            ALOGV("NFC_EnableWired failed");
        }
        else
        {
             if (status != NFCSTATUS_SUCCESS)
             {
                 ALOGV("SE is being used by SPI");
             }
        }
    }
    break;
    case STANDBY_GPIO_LOW:
    {
        jint ret_val = -1;
        NFCSTATUS status = NFCSTATUS_FAILED;
        /* Set the ESE VDD gpio to low to make sure P61 is reset. */
        ret_val = NFC_DisableWired ((void *)&status);
        if (ret_val < 0)
        {
            ALOGV("NFC_DisableWired failed");
        }
        else
        {
            if (status != NFCSTATUS_SUCCESS)
            {
                ALOGV("SE is not being released by Pn54x driver");
            }
        }
    }
    break;
    default:
        ALOGE("Wrong param");
    break;

    }
    mNfccStandbyMutex.unlock();
}
/*******************************************************************************
**
** Function         eSE_Chip_Reset
**
** Description      Performs Chip Reset on eSE using ISO_RST pin.
**
** Returns          Returns Status SUCCESS or FAILED.
**
*******************************************************************************/
NFCSTATUS SecureElement::eSE_Chip_Reset(void)
{
    jint ret_val = -1;
    NFCSTATUS status = NFCSTATUS_FAILED;
    ALOGV("eSE_Chip_Reset");
    SecureElement::getInstance().SecEle_Modeset(0x00);
    /* Reset P73 using ISO Reset Pin. */
    ret_val = NFC_eSEChipReset ((void *)&status);
    if (ret_val < 0)
    {
        ALOGV("Reset eSE failed");
    }
    else
    {
        if (status != NFCSTATUS_SUCCESS)
        {
            ALOGV("SE is not being released by Pn54x driver");
        }
    }
    SecureElement::getInstance().SecEle_Modeset(0x01);
    return status;
}

/*******************************************************************************
**
** Function:        reconfigureEseHciInit
**
** Description:     Reinitialize the HCI network for SecureElement
**
** Returns:         Returns Status SUCCESS or FAILED.
**
*******************************************************************************/
tNFA_STATUS SecureElement::reconfigureEseHciInit()
{
    tNFA_STATUS status = NFA_STATUS_FAILED;
    if(!nfcFL.eseFL._JCOP_WA_ENABLE) {
        ALOGV("reconfigureEseHciInit JCOP_WA_ENABLE not found. Returning");
        return status;
    }

    if (isActivatedInListenMode()) {
        ALOGV("Denying HCI re-initialization due to SE listen mode active");
        return status;
    }

    if (isRfFieldOn()) {
        ALOGV("Denying HCI re-initialization due to SE in active RF field");
        return status;
    }
    if(android::isDiscoveryStarted() == true)
    {
        android::startRfDiscovery(false);
    }
    status = android::ResetEseSession();
    if(status == NFA_STATUS_OK)
    {
        SecEle_Modeset(0x00);
        usleep(100 * 1000);

        SecEle_Modeset(0x01);
        usleep(300 * 1000);
    }

    android::startRfDiscovery(true);
    return status;
}

se_apdu_gate_info SecureElement::getApduGateInfo()
{
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    se_apdu_gate_info ret = NO_APDU_GATE;

    ALOGV("getApduGateInfo : get gate, pipe list");
    /*HCI initialised and secure element available*/
    if((mNfaHciHandle != NFA_HANDLE_INVALID) && (mActiveEeHandle != NFA_HANDLE_INVALID))
    {
        SyncEventGuard guard (mPipeListEvent);
        nfaStat = NFA_HciGetGateAndPipeList (mNfaHciHandle);
        if (nfaStat == NFA_STATUS_OK)
        {
            mPipeListEvent.wait();
            if (mHciCfg.status == NFA_STATUS_OK)
            {
                for (uint8_t xx = 0; xx < mHciCfg.num_pipes; xx++)
                {
                    ALOGV("getApduGateInfo : get gate, pipe list host = 0x%x gate = 0x%x", mHciCfg.pipe[xx].dest_host,
                            mHciCfg.pipe[xx].dest_gate);
                    if ( (mHciCfg.pipe[xx].dest_host == (EE_HANDLE_0xF3 & ~NFA_HANDLE_GROUP_EE)) && (mHciCfg.pipe[xx].dest_gate == NFA_HCI_ETSI12_APDU_GATE) )
                    {
                        ret = ETSI_12_APDU_GATE;
                        ALOGV("getApduGateInfo: found configured gate: 0x%02x  pipe: 0x%02x", mNewSourceGate, mNewPipeId);
                        break;
                    }
                    else if( (mHciCfg.pipe[xx].dest_host == (EE_HANDLE_0xF3 & ~NFA_HANDLE_GROUP_EE)) && (mHciCfg.pipe[xx].dest_gate == 0xF0) )
                    {
                        ret = PROPREITARY_APDU_GATE;
                        ALOGV("propreitary ApduGatePresent: found configured gate: 0x%02x  pipe: 0x%02x", mNewSourceGate, mNewPipeId);
                        break;
                    }
                    else
                    {
                        ALOGV("No ApduGatePresent");
                    }
                }
            }
        }
    }
    return ret;
}

bool SecureElement::checkForWiredModeAccess()
{
    static const char fn[] = "checkForWiredModeAccess";
    bool status = true;
    ALOGV("%s; enter", fn);

    if(mIsExclusiveWiredMode)
    {
        if(mIsWiredModeOpen)
        {
            return status;
        }
        if(android::isp2pActivated()||isActivatedInListenMode()||isRfFieldOn())
        {
            status = false;
            return status;
        }
    }
    else
    {
        if(mIsWiredModeBlocked)
        {
             hold_wired_mode = true;
             SyncEventGuard guard (mWiredModeHoldEvent);
             mWiredModeHoldEvent.wait();
             status = true;
             return status;
        }
        else
        {
            status = true;
            return status;
        }
    }
    ALOGV("%s; status:%d  ",fn, status);
    return status;
}
#endif
/*******************************************************************************
**
** Function:        etsiInitConfig
**
** Description:     Chnage the ETSI state before start configuration
**
** Returns:         None
**
*******************************************************************************/
void SecureElement::etsiInitConfig()
{
    if(!nfcFL.eseFL._ESE_ETSI_READER_ENABLE) {
        ALOGV("%s: ETSI_READER not available. Returning", __func__);
        return;
    }

    ALOGV("%s: Enter", __func__);
    swp_rdr_req_ntf_info.mMutex.lock();

    if((swp_rdr_req_ntf_info.swp_rd_state == STATE_SE_RDR_MODE_START_CONFIG) &&
      ((swp_rdr_req_ntf_info.swp_rd_req_info.tech_mask & NFA_TECHNOLOGY_MASK_A) ||
      (swp_rdr_req_ntf_info.swp_rd_req_info.tech_mask & NFA_TECHNOLOGY_MASK_B)))
    {
        if((swp_rdr_req_ntf_info.swp_rd_req_info.tech_mask & NFA_TECHNOLOGY_MASK_A))
        {
            swp_rdr_req_ntf_info.swp_rd_req_current_info.tech_mask |= NFA_TECHNOLOGY_MASK_A;
        }

        if((swp_rdr_req_ntf_info.swp_rd_req_info.tech_mask & NFA_TECHNOLOGY_MASK_B))
        {
            swp_rdr_req_ntf_info.swp_rd_req_current_info.tech_mask |= NFA_TECHNOLOGY_MASK_B;
        }

        swp_rdr_req_ntf_info.swp_rd_req_current_info.src = swp_rdr_req_ntf_info.swp_rd_req_info.src;
        swp_rdr_req_ntf_info.swp_rd_state = STATE_SE_RDR_MODE_START_IN_PROGRESS;
        ALOGV("%s: new ETSI state : STATE_SE_RDR_MODE_START_IN_PROGRESS", __func__);
    }
    else if((swp_rdr_req_ntf_info.swp_rd_state == STATE_SE_RDR_MODE_STOP_CONFIG) &&
            (swp_rdr_req_ntf_info.swp_rd_req_current_info.src == swp_rdr_req_ntf_info.swp_rd_req_info.src))
    {
        pTransactionController->transactionEnd(TRANSACTION_REQUESTOR(etsiReader));
        swp_rdr_req_ntf_info.swp_rd_state = STATE_SE_RDR_MODE_STOP_IN_PROGRESS;
        ALOGV("%s: new ETSI state : STATE_SE_RDR_MODE_STOP_IN_PROGRESS", __func__);

    }
    swp_rdr_req_ntf_info.mMutex.unlock();
}

/*******************************************************************************
**
** Function:        etsiReaderConfig
**
** Description:     Configuring to Emvco Profile
**
** Returns:         Status
**
*******************************************************************************/
tNFC_STATUS SecureElement::etsiReaderConfig(int eeHandle)
{
    if(!nfcFL.eseFL._ESE_ETSI_READER_ENABLE) {
        ALOGV("%s: ETSI_READER not available. Returning", __func__);
        return NFA_STATUS_FAILED;
    }
    tNFC_STATUS status;

    ALOGV("%s: Enter", __func__);
    ALOGV("%s: eeHandle : 0x%4x", __func__,eeHandle);
    /* Setting up the emvco poll profile*/
    status = android::EmvCo_dosetPoll(true);
    if (status != NFA_STATUS_OK)
    {
        ALOGE("%s: fail enable polling; error=0x%X", __func__, status);
    }

    ALOGV("%s: NFA_RD_SWP_READER_REQUESTED EE_HANDLE_0xF4 %x", __func__, EE_HANDLE_0xF4);
    ALOGV("%s: NFA_RD_SWP_READER_REQUESTED EE_HANDLE_0xF3 %x", __func__, EE_HANDLE_0xF3);

    if(eeHandle == EE_HANDLE_0xF4) //UICC
    {
        SyncEventGuard guard (mDiscMapEvent);
        ALOGV("%s: mapping intf for UICC", __func__);
        status = NFC_DiscoveryMap (NFC_SWP_RD_NUM_INTERFACE_MAP,(tNCI_DISCOVER_MAPS *)nfc_interface_mapping_uicc
                ,SecureElement::discovery_map_cb);
        if (status != NFA_STATUS_OK)
        {
            ALOGE("%s: fail intf mapping for UICC; error=0x%X", __func__, status);
            return status;
        }
        mDiscMapEvent.wait ();
    }
    else if(eeHandle == EE_HANDLE_0xF3) //ESE
    {
        SyncEventGuard guard (mDiscMapEvent);
        ALOGV("%s: mapping intf for ESE", __func__);
        status = NFC_DiscoveryMap (NFC_SWP_RD_NUM_INTERFACE_MAP,(tNCI_DISCOVER_MAPS *)nfc_interface_mapping_ese
                ,SecureElement::discovery_map_cb);
        if (status != NFA_STATUS_OK)
        {
            ALOGE("%s: fail intf mapping for ESE; error=0x%X", __func__, status);
            return status;
        }
        mDiscMapEvent.wait ();
    }
    else
    {
        ALOGV("%s: UNKNOWN SOURCE!!! ", __func__);
        return NFA_STATUS_FAILED;
    }
    return NFA_STATUS_OK;
}

/*******************************************************************************
**
** Function:        etsiResetReaderConfig
**
** Description:     Configuring from Emvco profile to Nfc forum profile
**
** Returns:         Status
**
*******************************************************************************/
tNFC_STATUS SecureElement::etsiResetReaderConfig()
{
    if(!nfcFL.eseFL._ESE_ETSI_READER_ENABLE) {
        ALOGV("%s: ETSI_READER not available. Returning", __func__);
        return NFA_STATUS_FAILED;
    }
    tNFC_STATUS status;
    ALOGV("%s: Enter", __func__);

    status = android::EmvCo_dosetPoll(false);
    if (status != NFA_STATUS_OK)
    {
        ALOGE("%s: fail enable polling; error=0x%X", __func__, status);
    }
    {
        SyncEventGuard guard (mDiscMapEvent);
        ALOGV("%s: mapping intf for DH", __func__);
        status = NFC_DiscoveryMap (NFC_NUM_INTERFACE_MAP,(tNCI_DISCOVER_MAPS *) nfc_interface_mapping_default
                ,SecureElement::discovery_map_cb);
        if (status != NFA_STATUS_OK)
        {
            ALOGE("%s: fail intf mapping for ESE; error=0x%X", __func__, status);
            return status;
        }
        mDiscMapEvent.wait ();
        return NFA_STATUS_OK;
    }
}

int SecureElement::decodeBerTlvLength(uint8_t* data,int index, int data_length )
{
    int decoded_length = -1;
    int length = 0;
    int temp = data[index] & 0xff;

    ALOGV("decodeBerTlvLength index= %d data[index+0]=0x%x data[index+1]=0x%x len=%d",index, data[index], data[index+1], data_length);

    if (temp < 0x80) {
        decoded_length = temp;
    } else if (temp == 0x81) {
        if( index < data_length ) {
            length = data[index+1] & 0xff;
            if (length < 0x80) {
                ALOGE("Invalid TLV length encoding!");
                goto TheEnd;
            }
            if (data_length < length + index) {
                ALOGE("Not enough data provided!");
                goto TheEnd;
            }
        } else {
            ALOGE("Index %d out of range! [0..[%d",index, data_length);
            goto TheEnd;
        }
        decoded_length = length;
    } else if (temp == 0x82) {
        if( (index + 1)< data_length ) {
            length = ((data[index] & 0xff) << 8)
                    | (data[index + 1] & 0xff);
        } else {
            ALOGE("Index out of range! [0..[%d" , data_length);
            goto TheEnd;
        }
        index += 2;
        if (length < 0x100) {
            ALOGE("Invalid TLV length encoding!");
            goto TheEnd;
        }
        if (data_length < length + index) {
            ALOGE("Not enough data provided!");
            goto TheEnd;
        }
        decoded_length = length;
    } else if (temp == 0x83) {
        if( (index + 2)< data_length ) {
            length = ((data[index] & 0xff) << 16)
                    | ((data[index + 1] & 0xff) << 8)
                    | (data[index + 2] & 0xff);
        } else {
            ALOGE("Index out of range! [0..[%d", data_length);
            goto TheEnd;
        }
        index += 3;
        if (length < 0x10000) {
            ALOGE("Invalid TLV length encoding!");
            goto TheEnd;
        }
        if (data_length < length + index) {
            ALOGE("Not enough data provided!");
            goto TheEnd;
        }
        decoded_length = length;
    } else {
        ALOGE("Unsupported TLV length encoding!");
    }
TheEnd:
    ALOGV("decoded_length = %d", decoded_length);

    return decoded_length;
}
#if(NXP_EXTNS == TRUE)
void cleanupStack(void* p)
{
    return;
}
/*******************************************************************************
**
** Function:       spiEventHandlerThread
**
** Description:    thread to trigger on SPI event
**
** Returns:        None .
**
*******************************************************************************/
void *spiEventHandlerThread(void *arg)
{
    void (*pCleanupRoutine)(void* ptr) = cleanupStack;
    __pthread_cleanup_t  __cleanup;
    SecureElement &se = SecureElement::getInstance();

    if(!nfcFL.nfcNxpEse) {
        ALOGV("%s: nfcNxpEse not available. Returning", __func__);
        return NULL;
    }
    if(!nfcFL.eseFL._ESE_SVDD_SYNC && !nfcFL.eseFL._ESE_JCOP_DWNLD_PROTECTION &&
            !nfcFL.nfccFL._NFCC_SPI_FW_DOWNLOAD_SYNC && !nfcFL.eseFL._ESE_DWP_SPI_SYNC_ENABLE) {
        ALOGV("%s: Not allowed. Returning", __func__);
        return NULL;
    }
    (void)arg;
    uint16_t usEvent = 0, usEvtLen = 0;
    tNFC_STATUS stat;

    NFCSTATUS ese_status = NFA_STATUS_FAILED;

    ALOGV("%s: enter", __func__);
    while(android::nfcManager_isNfcActive()&&
            !android::nfcManager_isNfcDisabling() &&
            (android::nfcManager_getNfcState() != NFC_OFF))
    {
        if(true == gSPIEvtQueue.isEmpty()) /* Wait for the event only if the queue is empty */
        { /* scope of the guard start */
            SyncEventGuard guard(sSPISignalHandlerEvent);
            sSPISignalHandlerEvent.wait();
            ALOGE("%s: Empty evt received", __FUNCTION__);
        } /* scope of the guard end */
        /* Dequeue the received signal */
        gSPIEvtQueue.dequeue((uint8_t*)&usEvent, (uint16_t)SIGNAL_EVENT_SIZE, usEvtLen);
        ALOGV("%s: evt received %x len %x", __FUNCTION__, usEvent, usEvtLen);

        if(usEvent & P61_STATE_SPI_PRIO)
        {
            ALOGV("%s: SPI PRIO request Signal....\n", __func__);
            hold_the_transceive = true;
            setSPIState(true);
         }
         else if(usEvent & P61_STATE_SPI_PRIO_END)
         {
             ALOGV("%s: SPI PRIO End Signal\n", __func__);
             hold_the_transceive = false;
             if(!se.mIsWiredModeOpen)
                spiDwpSyncState = STATE_DWP_CLOSE;
             setSPIState(false);
             SyncEventGuard guard (sSPIPrioSessionEndEvent);
             sSPIPrioSessionEndEvent.notifyOne ();
         }
        else if(usEvent & P61_STATE_SPI)
        {
            ALOGV("%s: SPI OPEN request Signal....\n", __func__);
            setSPIState(true);
        }
        else if(usEvent & P61_STATE_SPI_END)
        {
            ALOGV("%s: SPI End Signal\n", __func__);
            hold_the_transceive = false;
            if(!se.mIsWiredModeOpen)
                spiDwpSyncState = STATE_DWP_CLOSE;
            setSPIState(false);
        }

        if((usEvent & P61_STATE_SPI_SVDD_SYNC_START) ||
                (usEvent & P61_STATE_DWP_SVDD_SYNC_START))
        {
            if(nfcFL.eseFL._ESE_DWP_SPI_SYNC_ENABLE) {
                if((usEvent & P61_STATE_SPI_PRIO) ||
                        (usEvent & P61_STATE_SPI))
                {
                    nfaVSC_ForceDwpOnOff(true);
                    if(nfcFL.eseFL._WIRED_MODE_STANDBY) {
                    if(android::nfcManager_getNfcState() != NFC_OFF)
                        NFC_RelForceDwpOnOffWait((void*)&stat);
                    }
                }
                else if((usEvent & P61_STATE_SPI_PRIO_END) ||
                        (usEvent & P61_STATE_SPI_END))
                {
                    nfaVSC_ForceDwpOnOff(false);
                }
            }

            if(nfcFL.eseFL._ESE_SVDD_SYNC) {
                nfaVSC_SVDDSyncOnOff(true);
            }
        }
        else if(nfcFL.eseFL._ESE_SVDD_SYNC &&
                ((usEvent & P61_STATE_SPI_SVDD_SYNC_END) ||
                        (usEvent & P61_STATE_DWP_SVDD_SYNC_END))) {
            nfaVSC_SVDDSyncOnOff(false);
        }

        else if(nfcFL.eseFL._ESE_DWP_SPI_SYNC_ENABLE &&
                ((usEvent & P61_STATE_SPI_PRIO) ||
                        (usEvent & P61_STATE_SPI)))
        {
            nfaVSC_ForceDwpOnOff(true);
            if(nfcFL.eseFL._WIRED_MODE_STANDBY) {
            if(android::nfcManager_getNfcState() != NFC_OFF)
                NFC_RelForceDwpOnOffWait((void*)&stat);
            }
        }
        else if(nfcFL.eseFL._ESE_DWP_SPI_SYNC_ENABLE &&
                ((usEvent & P61_STATE_SPI_PRIO_END) ||
                        (usEvent & P61_STATE_SPI_END)))
        {
            nfaVSC_ForceDwpOnOff(false);
        }

        if(nfcFL.eseFL._ESE_JCOP_DWNLD_PROTECTION) {
            if(usEvent == JCP_DWNLD_INIT)
            {
                ALOGV("%s: JCOP OS download  init request....=%d\n", __func__, usEvent);
                if(android::nfcManager_checkNfcStateBusy() ==  false)
                {
                    if(android::isDiscoveryStarted() == true)
                    {
                        android::startRfDiscovery(false);
                    }
                    NFC_SetP61Status ((void *)&ese_status, JCP_DWNLD_START);
                }
            }
            else if(usEvent == JCP_DWP_DWNLD_COMPLETE)
            {
                ALOGV("%s: JCOP OS download  end request...=%d\n", __func__, usEvent);
                if(android::isDiscoveryStarted() == false)
                {
                    android::startRfDiscovery(true);
                }
            }
        }
        else if(nfcFL.nfccFL._NFCC_SPI_FW_DOWNLOAD_SYNC &&
                ((usEvent & P61_STATE_SPI_PRIO_END) ||
                        (usEvent & P61_STATE_SPI_END))) {
            if(android::nfcManager_isNfcActive()&&
                !android::nfcManager_isNfcDisabling() &&
                    (android::nfcManager_getNfcState() != NFC_OFF)) {
                android::requestFwDownload();
            }
        }
    }
    ALOGV("%s: exit", __func__);
    /*Explicit cleanup to avoid any issue due to pthread datastructure corruption*/
    __pthread_cleanup_push(&__cleanup, pCleanupRoutine, NULL);
    __pthread_cleanup_pop(&__cleanup, 0);
    pthread_exit(NULL);
    return NULL;
}

bool createSPIEvtHandlerThread()
{
    if(!nfcFL.nfcNxpEse) {
        ALOGV("%s: nfcNxpEse not available. Returning", __func__);
        return false;
    }
    if(!nfcFL.eseFL._ESE_SVDD_SYNC && !nfcFL.eseFL._ESE_JCOP_DWNLD_PROTECTION &&
            !nfcFL.nfccFL._NFCC_SPI_FW_DOWNLOAD_SYNC && !nfcFL.eseFL._ESE_DWP_SPI_SYNC_ENABLE) {
        ALOGV("%s: Not allowed. Returning", __func__);
        return false;
    }

    bool stat = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&spiEvtHandler_thread, &attr, spiEventHandlerThread, NULL) != 0)
    {
        ALOGV("Unable to create the thread");
        stat = false;
    }
    pthread_attr_destroy(&attr);
    return stat;
}

void releaseSPIEvtHandlerThread()
{
    if(!nfcFL.nfcNxpEse) {
        ALOGV("%s: nfcNxpEse not available. Returning", __func__);
        return;
    }
    if(!nfcFL.eseFL._ESE_SVDD_SYNC && !nfcFL.eseFL._ESE_JCOP_DWNLD_PROTECTION &&
            !nfcFL.nfccFL._NFCC_SPI_FW_DOWNLOAD_SYNC && !nfcFL.eseFL._ESE_DWP_SPI_SYNC_ENABLE) {
        ALOGV("%s: Not allowed. Returning", __func__);
        return;
    }

    uint16_t usEvent = 0;
    uint16_t usEvtLen = 0;
    ALOGV("releaseSPIEvtHandlerThread");
    while(!gSPIEvtQueue.isEmpty()) /* Wait for the event only if the queue is empty */
    { /* scope of the guard start */
        /* Dequeue the received signal */
        gSPIEvtQueue.dequeue((uint8_t*)&usEvent, (uint16_t)SIGNAL_EVENT_SIZE, usEvtLen);
        ALOGE("%s: Clearing queue ", __func__);
    } /* scope of the guard end */

    /* Notifying the signal handler thread to exit if it is waiting */
    SyncEventGuard guard(sSPISignalHandlerEvent);
    sSPISignalHandlerEvent.notifyOne ();
    {
        SyncEventGuard guard(SecureElement::getInstance().mModeSetNtf);
        SecureElement::getInstance().mModeSetNtf.notifyOne();
    }
}
/*******************************************************************************
**
** Function:        nfaVSC_SVDDSyncOnOffCallback
**
** Description:     callback to process the svdd protection response from FW
**
**
** Returns:         void.
**
*******************************************************************************/
static void nfaVSC_SVDDSyncOnOffCallback(uint8_t event, uint16_t param_len, uint8_t *p_param)
{
    if(!nfcFL.nfcNxpEse || !nfcFL.eseFL._ESE_SVDD_SYNC) {
        ALOGV("%s: nfcNxpEse or ESE_SVDD_SYNC not available. Returning", __func__);
        return;
    }

    (void)event;
    tNFC_STATUS nfaStat;
    char fn[] = "nfaVSC_SVDDProtectionCallback";
    ALOGV("%s", __func__);
    ALOGV("%s param_len = %d ", __func__, param_len);
    ALOGV("%s status = 0x%X ", __func__, p_param[3]);
    SyncEventGuard guard(sSPISVDDSyncOnOffEvent);
    sSPISVDDSyncOnOffEvent.notifyOne();
    if (NFC_RelSvddWait((void *)&nfaStat) != 0)
    {
        ALOGE("%s: NFC_RelSvddWait failed ret = %d", fn, nfaStat);
    }
}
/*******************************************************************************
**
** Function:        nfaVSC_SVDDSyncOnOff
**
** Description:     starts and stops the svdd protection in FW
**
**
** Returns:         void.
**
*******************************************************************************/
static void nfaVSC_SVDDSyncOnOff(bool type)
{
    if(!nfcFL.nfcNxpEse || !nfcFL.eseFL._ESE_SVDD_SYNC) {
        ALOGV("%s: nfcNxpEse or ESE_SVDD_SYNC not available. Returning", __func__);
        return;
    }
    tNFC_STATUS stat;
    uint8_t param = 0x00;
    if(type == true)
    {
        param = 0x01; //SVDD protection on
    }
    if (!android::nfcManager_isNfcActive()||
            android::nfcManager_isNfcDisabling() ||
                (android::nfcManager_getNfcState() == NFC_OFF))
    {
        ALOGE("%s: NFC is no longer active.", __func__);
        return;
    }
    stat = NFA_SendVsCommand (0x31, 0x01, &param, nfaVSC_SVDDSyncOnOffCallback);
    if(NFA_STATUS_OK == stat)
    {
        SyncEventGuard guard(sSPISVDDSyncOnOffEvent);
        sSPISVDDSyncOnOffEvent.wait(50);
        ALOGV("%s: NFA_SendVsCommand pass stat = %d", __func__,stat);
    }
    else
    {
        ALOGV("%s: NFA_SendVsCommand failed stat = %d", __func__,stat);
    }
}

/*******************************************************************************
 **
 ** Function:        nfaVSC_ForceDwpOnOff
**
 ** Description:     starts and stops the dwp channel
 **
 **
 ** Returns:         void.
 **
*******************************************************************************/
static void nfaVSC_ForceDwpOnOff(bool type)
{
    if(!nfcFL.nfcNxpEse || !nfcFL.eseFL._ESE_DWP_SPI_SYNC_ENABLE) {
        ALOGV("%s: nfcNxpEse or DWP_SPI_SYNC_ENABLE not available."
                " Returning", __func__);
        return;
    }
    tNFC_STATUS stat = NFA_STATUS_FAILED;
    uint8_t xmitBuffer[] = {0x00, 0x00, 0x00, 0x00};
    uint8_t EVT_SEND_DATA = 0x10;
    uint8_t EVT_END_OF_APDU_TRANSFER = 0x21;

    if(standby_state == STANDBY_MODE_OFF)
    {
        ALOGV("%s: DWP wired mode is On", __func__);
        return;
    }

    if(!android::nfcManager_isNfcActive()||
            android::nfcManager_isNfcDisabling() ||
                (android::nfcManager_getNfcState() == NFC_OFF))
    {
        ALOGV ("%s: NFC is not activated", __FUNCTION__);
        return;
    }

    ALOGV("nfaVSC_ForceDwpOnOff: syncstate = %d, type = %d", spiDwpSyncState, type);
    if((type == true) && !(spiDwpSyncState & STATE_WK_ENBLE))
    {
        if(nfcFL.eseFL._WIRED_MODE_STANDBY) {
            SecureElement::getInstance().setNfccPwrConfig(SecureElement::getInstance().POWER_ALWAYS_ON | SecureElement::getInstance().COMM_LINK_ACTIVE);
            SecureElement::getInstance().SecEle_Modeset(0x1);
            spiDwpSyncState |= STATE_WK_ENBLE;
        }
        else  {
            spiDwpSyncState |= STATE_WK_ENBLE;
            memset(xmitBuffer, 0, sizeof(xmitBuffer));
            stat = NFA_HciSendEvent (NFA_HANDLE_GROUP_HCI, 0x19, EVT_SEND_DATA, sizeof(xmitBuffer), xmitBuffer,
                    0, NULL, 0);
            if(stat == NFA_STATUS_OK)
            {
                spiDwpSyncState |= STATE_WK_WAIT_RSP;
                SyncEventGuard guard(sSPIForceEnableDWPEvent);
                sSPIForceEnableDWPEvent.wait(50);
            }
            else
            {
                ALOGV("%s: NFA_HciSendEvent failed stat = %d type = %d", __func__,stat, type);
            }
        }
    }
    else if (type == false)
    {
        if(nfcFL.eseFL._WIRED_MODE_STANDBY) {
            /*Just stand by timer expired*/
            if(spiDwpSyncState & STATE_TIME_OUT)
            {
                /*Clear APDU state to activate DWP link once spi standalone is triggered*/
                if(spiDwpSyncState & STATE_WK_ENBLE)
                {
                    spiDwpSyncState ^= STATE_WK_ENBLE;
                }
                SecureElement::getInstance().setNfccPwrConfig(SecureElement::getInstance().POWER_ALWAYS_ON);
                stat = SecureElement::getInstance().sendEvent(SecureElement::EVT_SUSPEND_APDU_TRANSFER);
                if(stat)
                {
                    ALOGV("%s sending standby mode command successful", __func__);
                }
                standby_state = STANDBY_MODE_SUSPEND;
                spiDwpSyncState = STATE_IDLE;
                return;
            }
            /*If DWP session is closed*/
            (void)SecureElement::getInstance().setNfccPwrConfig(SecureElement::getInstance().NFCC_DECIDES);
        }

        if(spiDwpSyncState & STATE_DWP_CLOSE)
        {
            stat = NFA_HciSendEvent (NFA_HANDLE_GROUP_HCI, 0x19, EVT_END_OF_APDU_TRANSFER,
                    0x00, NULL, 0x00,NULL, 0);
            if(NFA_STATUS_OK != stat)
                ALOGV("%s: NFA_HciSendEvent failed stat = %d, type = %d", __func__,stat, type);
        }
        spiDwpSyncState = STATE_IDLE;
    }

    ALOGV("nfaVSC_ForceDwpOnOff: syncstate = %d, type = %d", spiDwpSyncState, type);
}

bool SecureElement::enableDwp(void)
{
    if(!nfcFL.nfcNxpEse || !nfcFL.eseFL._ESE_DWP_SPI_SYNC_ENABLE) {
        ALOGV("%s: nfcNxpEse or DWP_SPI_SYNC_ENABLE not available."
                " Returning", __func__);
        return false;
    }
    bool stat = false;
    static const char fn [] = "SecureElement::enableDwp";
    ALOGV("enter %s", fn);

    mActiveEeHandle = getDefaultEeHandle();
    if(mActiveEeHandle == EE_HANDLE_0xF3)
    {
        stat = connectEE();
    }
    mIsPiping = false;
    ALOGV("exit %s.. stat = %d", fn, stat);
    return stat;
}

void spi_prio_signal_handler (int signum, siginfo_t *info, void *unused)
{
    ALOGV("%s: Inside the Signal Handler %d\n", __func__, SIG_NFC);
    uint16_t usEvent = 0;
    if (signum == SIG_NFC)
    {
        ALOGV("%s: Signal is SIG_NFC\n", __func__);
        if(nfcFL.eseFL._ESE_SVDD_SYNC || nfcFL.eseFL._ESE_JCOP_DWNLD_PROTECTION ||
                nfcFL.nfccFL._NFCC_SPI_FW_DOWNLOAD_SYNC || nfcFL.eseFL._ESE_DWP_SPI_SYNC_ENABLE) {
            usEvent = info->si_int;
            gSPIEvtQueue.enqueue((uint8_t*)&usEvent, (uint16_t)SIGNAL_EVENT_SIZE);
            SyncEventGuard guard(sSPISignalHandlerEvent);
            sSPISignalHandlerEvent.notifyOne ();
        }
    }
}

/*******************************************************************************
**
** Function:        setCPTimeout
**
** Description:     sets the CP timeout for SE -P61 if its present.
**
**
** Returns:         void.
**
*******************************************************************************/
void SecureElement::setCPTimeout()
{
    tNFA_HANDLE handle;
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    uint8_t received_getatr[32];
    uint8_t selectISD[]= {0x00,0xa4,0x04,0x00,0x08,0xA0,0x00,0x00,0x01,0x51,0x00,0x00,0x00,0x00};
    uint8_t received_selectISD[64];
    uint8_t setCPTimeoutcmdbuff[]= {0x80,0xDC,0x00,0x00,0x08,0xEF,0x06,0xA0,0x04,0x84,0x02,0x00,0x00};
    uint8_t CPTimeoutvalue[2]= {0x00,0x00};
    uint8_t received_setCPTimeout[32];
    int i;
    bool found =false;
    long retlen = 0;
    int32_t timeout =12000;
    int32_t recvBufferActualSize = 0;
    static const char fn [] = "SecureElement::setCPTimeout";
    for ( i = 0; i < mActualNumEe; i++)
    {
        if (mEeInfo[i].ee_handle == EE_HANDLE_0xF3)
        {
            nfaStat = NFA_STATUS_OK;
            handle = mEeInfo[i].ee_handle & ~NFA_HANDLE_GROUP_EE;
            ALOGV("%s: %u = 0x%X", fn, i, mEeInfo[i].ee_handle);
            break;
        }
    }
    if(nfaStat == NFA_STATUS_OK)
    {
        if (GetNxpByteArrayValue(NAME_NXP_CP_TIMEOUT,(char *) CPTimeoutvalue,
                sizeof(CPTimeoutvalue), &retlen))
        {
           ALOGV("%s: READ NAME_CP_TIMEOUT Value", __func__);
           memcpy((setCPTimeoutcmdbuff+(sizeof(setCPTimeoutcmdbuff)-2)),CPTimeoutvalue,2);
           found = true;
        }
        else
        {
            ALOGV("%s:CP_TIMEOUT Value not found!!!", __func__);
        }
        if(found)
        {
            bool stat = false;
            stat = SecEle_Modeset(0x01);
            if(stat == true)
            {
                mActiveEeHandle = getDefaultEeHandle();
                if(mActiveEeHandle == EE_HANDLE_0xF3 && (getApduGateInfo() != NO_APDU_GATE))
                {
                    stat = connectEE();
                    if(stat == true)
                    {
                        stat = getAtr(ESE_ID,received_getatr,&recvBufferActualSize);
                        if(stat == true)
                        {
                            /*select card manager*/
                            stat = transceive(selectISD,(int32_t)sizeof(selectISD),received_selectISD,
                                               (int)sizeof(received_selectISD), recvBufferActualSize, timeout);
                            if(stat == true)
                            {
                                /*set timeout value in CP registry*/
                                transceive(setCPTimeoutcmdbuff,(int32_t)sizeof(setCPTimeoutcmdbuff),
                                            received_setCPTimeout, (int)sizeof(received_setCPTimeout), recvBufferActualSize, timeout);
                            }
                        }
                        NfccStandByOperation(STANDBY_MODE_ON);
                        disconnectEE(ESE_ID);
                    }
                }
            }
            sendEvent(SecureElement::EVT_END_OF_APDU_TRANSFER);
            NfccStandByOperation(STANDBY_TIMER_STOP);
            disconnectEE(ESE_ID);
        }
    }

}

/*******************************************************************************
**
** Function:        enablePassiveListen
**
** Description:     Enable or disable  Passive A/B listen
**
** Returns:         True if ok.
**
*******************************************************************************/
uint16_t SecureElement::enablePassiveListen (uint8_t event)
{
    if(!nfcFL.eseFL._NFCC_ESE_UICC_CONCURRENT_ACCESS_PROTECTION) {
        ALOGV("%s NFCC_ESE_UICC_CONCURRENT_ACCESS_PROTECTION not available."
                " Returning",__func__);
        return NFA_STATUS_FAILED;
    }

    tNFA_STATUS status = NFA_STATUS_FAILED;

    mPassiveListenMutex.lock();

    if(event == 0x00 && mPassiveListenEnabled == true)
    {
        if(android::isDiscoveryStarted() == true)
        {
            android::startRfDiscovery(false);
        }
        status = NFA_DisablePassiveListening();
        if(status == NFA_STATUS_OK)
        {
            SyncEventGuard g (mPassiveListenEvt);
            mPassiveListenEvt.wait(100);
        }
        resetRfFieldStatus();
        setDwpTranseiveState(false, NFCC_RF_FIELD_EVT);
        mPassiveListenEnabled = false;

        if(android::isDiscoveryStarted() == false)
        {
            android::startRfDiscovery(true);
        }
    }
    else if (event == 0x01 && mPassiveListenEnabled == false)
    {
        if(android::isDiscoveryStarted() == true)
        {
            android::startRfDiscovery(false);
        }
        status = NFA_EnableListening();
        if(status == NFA_STATUS_OK)
        {
            SyncEventGuard g (mPassiveListenEvt);
            mPassiveListenEvt.wait(100);
        }
        mPassiveListenTimer.set(mPassiveListenTimeout , passiveListenDisablecallBack);
        mPassiveListenEnabled = true;
        if(android::isDiscoveryStarted() == false)
        {
            android::startRfDiscovery(true);
        }
    }
    mPassiveListenMutex.unlock();
    ALOGV(" enablePassiveListen exit");
    return 0x00;
}

/*******************************************************************************
 **
 ** Function:       passiveListenEnableThread
 **
 ** Description:    thread to trigger passive Listen Enable
 **
 ** Returns:        None
 **
 *******************************************************************************/
void *passiveListenEnableThread(void *arg)
{
    if(!nfcFL.eseFL._NFCC_ESE_UICC_CONCURRENT_ACCESS_PROTECTION) {
        ALOGV("%s NFCC_ESE_UICC_CONCURRENT_ACCESS_PROTECTION not available."
                " Returning",__func__);
        return NULL;
    }
    ALOGV(" passiveListenEnableThread  %d",*((uint8_t*)arg));
    if (*((uint8_t*)arg))
    {
        SecureElement::getInstance().enablePassiveListen(0x01);
    }
    else
    {
        SecureElement::getInstance().enablePassiveListen(0x00);
    }
    pthread_exit(NULL);
    return NULL;
}

uint16_t SecureElement::startThread(uint8_t thread_arg)
{
    passiveListenState = thread_arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    mPassiveListenCnt = 0x00;
    if (pthread_create(&passiveListenEnable_thread, &attr, passiveListenEnableThread, (void*) &passiveListenState) != 0)
    {
        ALOGV("Unable to create the thread");
    }
    pthread_attr_destroy(&attr);
    return 0x00;
}

/*******************************************************************************
**
** Function:        passiveListenDisablecallBack
**
** Description:     Enable or disable  Passive A/B listen
**
** Returns:         None
**
*******************************************************************************/
static void passiveListenDisablecallBack(union sigval)
{
    if(!nfcFL.eseFL._NFCC_ESE_UICC_CONCURRENT_ACCESS_PROTECTION) {
        ALOGV("%s NFCC_ESE_UICC_CONCURRENT_ACCESS_PROTECTION not available."
                " Returning",__func__);
        return;
    }
    ALOGV(" passiveListenDisablecallBack enter");

    if(SecureElement::getInstance().isRfFieldOn() == true)
    {
        if(SecureElement::getInstance().isActivatedInListenMode())
        {
            //do nothing ,
            return;
        }
        else if((SecureElement::getInstance().isActivatedInListenMode() == false) && (SecureElement::getInstance().mPassiveListenCnt < 0x02))
        {
            ALOGV(" passiveListenEnableThread timer restart");
            SecureElement::getInstance().mPassiveListenTimer.set(SecureElement::getInstance().mPassiveListenTimeout , passiveListenDisablecallBack);
            SecureElement::getInstance().mPassiveListenCnt++;
            return;
        }
    }
    SecureElement::getInstance().enablePassiveListen (0x00);
}

#endif

#if(NXP_EXTNS == TRUE)
/*******************************************************************************
 **
 ** Function:       setSPIState
 **
 ** Description:    Update current SPI state based on Signals
 **
 ** Returns:        None .
 **
 *******************************************************************************/
static void setSPIState(bool mState)
{
    ALOGV("%s: Enter setSPIState \n", __func__);
    /*Check if the state is already dual mode*/
    bool inDualModeAlready = (dual_mode_current_state == SPI_DWPCL_BOTH_ACTIVE);
    if(mState)
    {
        dual_mode_current_state |= SPI_ON;
    }
    else
    {
        if(dual_mode_current_state & SPI_ON)
        {
            dual_mode_current_state ^= SPI_ON;
            if(inDualModeAlready)
            {
                SyncEventGuard guard (mDualModeEvent);
                mDualModeEvent.notifyOne();
            }
        }
    }
    ALOGV("%s: Exit setSPIState = %d\n", __func__, dual_mode_current_state);
}

/*******************************************************************************
 **
 ** Function:       SecElem_EeModeSet
 **
 ** Description:    Perform SE mode set ON/OFF based on mode type
 **
 ** Returns:        NFA_STATUS_OK/NFA_STATUS_FAILED.
 **
 *******************************************************************************/
tNFA_STATUS SecureElement::SecElem_EeModeSet(uint16_t handle, uint8_t mode)
{
    tNFA_STATUS stat = NFA_STATUS_FAILED;
    ALOGV("%s:Enter mode = %d", __func__, mode);

    if(nfcFL.nfcNxpEse) {
        if(nfcFL.eseFL._JCOP_WA_ENABLE &&
                ((mode == NFA_EE_MD_DEACTIVATE)&&(active_ese_reset_control&(TRANS_WIRED_ONGOING|TRANS_CL_ONGOING))))
        {
            active_ese_reset_control |= RESET_BLOCKED;
            SyncEventGuard guard (sSecElem.mResetEvent);
            sSecElem.mResetEvent.wait();
        }
    }
    if((dual_mode_current_state & SPI_ON) && (handle == EE_HANDLE_0xF3) &&
        (mode == NFA_EE_MD_DEACTIVATE))
        return NFA_STATUS_OK;

    SyncEventGuard guard (sSecElem.mEeSetModeEvent);
    stat =  NFA_EeModeSet(handle, mode);
    if(stat == NFA_STATUS_OK && !android::nfcManager_isNfcDisabling() &&
       (android::nfcManager_getNfcState() != NFC_OFF))
    {
        sSecElem.mEeSetModeEvent.wait ();
    }

    if(nfcFL.nfcNxpEse) {
        if(nfcFL.eseFL._JCOP_WA_ENABLE && (active_ese_reset_control&RESET_BLOCKED))
        {
            SyncEventGuard guard (sSecElem.mResetOngoingEvent);
            sSecElem.mResetOngoingEvent.notifyOne();
        }
    }
    return stat;
}
/**********************************************************************************
 **
 ** Function:        getEeStatus
 **
 ** Description:     get the status of EE
 **
 ** Returns:         EE status
 **
 **********************************************************************************/
uint16_t SecureElement::getEeStatus(uint16_t eehandle)
{
    int i;
    uint16_t ee_status = NFA_EE_STATUS_REMOVED;
    ALOGV("%s  num_nfcee_present = %d",__func__,mNfceeData_t.mNfceePresent);

    for(i = 1; i<= mNfceeData_t.mNfceePresent ; i++)
    {
        if(mNfceeData_t.mNfceeHandle[i] == eehandle)
        {
            ee_status = mNfceeData_t.mNfceeStatus[i];
            ALOGV("%s  EE is detected 0x%02x  status = 0x%02x",__func__,eehandle,ee_status);
            break;
        }
    }
    return ee_status;
}
#if(NXP_EXTNS == TRUE)
/**********************************************************************************
 **
 ** Function:        getUiccStatus
 **
 ** Description:     get the status of EE
 **
 ** Returns:         UICC Status
 **
 **********************************************************************************/
uicc_stat_t SecureElement::getUiccStatus(uint8_t selected_uicc)
{
    uint16_t ee_stat = NFA_EE_STATUS_REMOVED;

    if(nfcFL.nfccFL._NFC_NXP_STAT_DUAL_UICC_WO_EXT_SWITCH)
    {
        if(selected_uicc == 0x01)
            ee_stat = getEeStatus(EE_HANDLE_0xF4);
        else if(selected_uicc == 0x02)
            ee_stat = getEeStatus(EE_HANDLE_0xF8);
    }
    if(nfcFL.nfccFL._NFC_NXP_STAT_DUAL_UICC_EXT_SWITCH)
        ee_stat = getEeStatus(EE_HANDLE_0xF4);

    uicc_stat_t uicc_stat = UICC_STATUS_UNKNOWN;

    if(selected_uicc == 0x01)
    {
        switch(ee_stat)
        {
        case 0x00:
            uicc_stat = UICC_01_SELECTED_ENABLED;
            break;
        case 0x01:
            uicc_stat = UICC_01_SELECTED_DISABLED;
            break;
        case 0x02:
            uicc_stat = UICC_01_REMOVED;
            break;
        }
    }
    else if(selected_uicc == 0x02)
    {
        switch(ee_stat)
        {
        case 0x00:
            uicc_stat = UICC_02_SELECTED_ENABLED;
            break;
        case 0x01:
            uicc_stat = UICC_02_SELECTED_DISABLED;
            break;
        case 0x02:
            uicc_stat = UICC_02_REMOVED;
            break;
        }
    }
    return uicc_stat;
}

/**********************************************************************************
 **
 ** Function:        updateNfceeDiscoverInfo
 **
 ** Description:     get the status of EE
 **
 ** Returns:         Number of new NFCEEs discovered
 **
 **********************************************************************************/
uint8_t SecureElement::updateNfceeDiscoverInfo()
{
    uint8_t numEe = nfcFL.nfccFL._NFA_EE_MAX_EE_SUPPORTED;
    tNFA_EE_INFO mEeInfo[numEe];
    tNFA_STATUS nfaStat;

    if((nfaStat = NFA_AllEeGetInfo(&numEe, mEeInfo)) != NFA_STATUS_OK)
    {
        ALOGV("%s failed to get info, error = 0x%X ",__func__, nfaStat);
        mActualNumEe = 0;

    }
    for(int xx = 0; xx < numEe ; xx++ )
    {
        if(mEeInfo[xx].ee_handle == EE_HANDLE_0xF3)
        {
            if(!(nfcee_swp_discovery_status & SWP2_ESE))
            {
                nfcee_swp_discovery_status |= SWP2_ESE;
                gSeDiscoverycount++;
            }
        }else if(mEeInfo[xx].ee_handle == EE_HANDLE_0xF4)
        {
            if(!(nfcee_swp_discovery_status & SWP1_UICC1))
            {
                nfcee_swp_discovery_status |= SWP1_UICC1;
                gSeDiscoverycount++;
            }
        }
        else if(mEeInfo[xx].ee_handle == EE_HANDLE_0xF8)
        {
            if(!(nfcee_swp_discovery_status & SWP1A_UICC2))
            {
                nfcee_swp_discovery_status |= SWP1A_UICC2;
                gSeDiscoverycount++;
            }
        }else if(mEeInfo[xx].ee_handle == EE_HANDLE_HCI)
        {
            if(!(nfcee_swp_discovery_status & HCI_ACESS))
            {
                nfcee_swp_discovery_status |= HCI_ACESS;
                gSeDiscoverycount++;
            }
        }else if(mEeInfo[xx].ee_handle == EE_HANDLE_NDEFEE)
        {
            if(!(nfcee_swp_discovery_status & T4T_NDEFEE))
            {
                nfcee_swp_discovery_status |= T4T_NDEFEE;
                gSeDiscoverycount++;
            }
        }
        else
        {
            ALOGV("%s INVALID HANDLE ",__func__);
        }
    }
    return gSeDiscoverycount;
}

#endif
#if(NXP_EXTNS == TRUE)
/*******************************************************************************
 **
 ** Function:       SecElem_sendEvt_Abort
 **
 ** Description:    Perform interface level reset by sending EVT_ABORT event
 **
 ** Returns:        NFA_STATUS_OK/NFA_STATUS_FAILED.
 **
 *******************************************************************************/

tNFA_STATUS SecureElement::SecElem_sendEvt_Abort()
{
    if(!nfcFL.nfcNxpEse) {
        ALOGE("%s nfcNxpEse not available. Returning",__func__);
        return NFA_STATUS_FAILED;
    }
    static const char fn[] = "SecureElement::SecElem_sendEvt_Abort";
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    int32_t timeoutMillisec = 10000;
    uint8_t atr_len = 0x10;
    uint8_t recvBuffer[MAX_RESPONSE_SIZE];
    mAbortEventWaitOk = false;

    if(nfcFL.nfccFL._NFCEE_REMOVED_NTF_RECOVERY &&
            (RoutingManager::getInstance().is_ee_recovery_ongoing())) {
        SyncEventGuard guard (mEEdatapacketEvent);
        if(mEEdatapacketEvent.wait(android::gMaxEERecoveryTimeout) == false)
        {
            return nfaStat;
        }
    }

    SyncEventGuard guard (mAbortEvent);
    nfaStat = NFA_HciSendEvent(mNfaHciHandle, mNewPipeId, EVT_ABORT, 0, NULL, atr_len, recvBuffer, timeoutMillisec);
    if(nfaStat == NFA_STATUS_OK)
    {
        mAbortEvent.wait();
    }
    if(mAbortEventWaitOk == false)
    {
        ALOGE("%s (EVT_ABORT)Wait reposne timeout",fn);
        return NFA_STATUS_FAILED;
    }
    return nfaStat;
}

/*******************************************************************************
 **
 ** Function:       setDwpTranseiveState
 **
 ** Description:    Update current DWP CL state based on CL activation status
 **
 ** Returns:        None .
 **
 *******************************************************************************/

void SecureElement::setDwpTranseiveState(bool block, tNFCC_EVTS_NTF action)
{
    if(!nfcFL.nfcNxpEse) {
        ALOGE("%s nfcNxpEse not available. Returning",__func__);
        return;
    }
    ALOGV("%s  block = %d action = %d ",__func__,block,action);

    switch(action)
    {
        case NFCC_RF_FIELD_EVT:
            if(!block)
            {
                SyncEventGuard guard (mRfFieldOffEvent);
                mRfFieldOffEvent.notifyOne();
            }
        case NFCC_DEACTIVATED_NTF:
        case NFCC_CE_DATA_EVT:
            if(block)
            {
                mIsWiredModeBlocked = true;
                if(action == NFCC_RF_FIELD_EVT)
                {
                    if(nfcFL.nfcNxpEse &&
                            nfcFL.eseFL._NFCC_ESE_UICC_CONCURRENT_ACCESS_PROTECTION &&
                            (meseUiccConcurrentAccess == false)) {
                        if(nfcFL.eseFL._ESE_DUAL_MODE_PRIO_SCHEME != nfcFL.eseFL._ESE_WIRED_MODE_RESUME) {
                            SecureElement::getInstance().mRfFieldEventTimer.set(
                                    SecureElement::getInstance().mRfFieldEventTimeout, rfFeildEventTimeoutCallback);
                        }
                    }
                    else if(nfcFL.eseFL._ESE_DUAL_MODE_PRIO_SCHEME == nfcFL.eseFL._ESE_WIRED_MODE_RESUME) {
                        SecureElement::getInstance().mRfFieldEventTimer.set(
                                SecureElement::getInstance().mRfFieldEventTimeout, rfFeildEventTimeoutCallback);

                    }
                }
            }
            else
            {
                mIsWiredModeBlocked = false;

                if(nfcFL.nfcNxpEse &&
                        nfcFL.eseFL._NFCC_ESE_UICC_CONCURRENT_ACCESS_PROTECTION &&
                        (meseUiccConcurrentAccess == false)) {
                    if(nfcFL.eseFL._ESE_DUAL_MODE_PRIO_SCHEME != nfcFL.eseFL._ESE_WIRED_MODE_RESUME) {
                        SecureElement::getInstance().mRfFieldEventTimer.kill();
                    }
                }
                else if(nfcFL.eseFL._ESE_DUAL_MODE_PRIO_SCHEME == nfcFL.eseFL._ESE_WIRED_MODE_RESUME) {
                    SecureElement::getInstance().mRfFieldEventTimer.kill();
                }
                if(hold_wired_mode)
                {
                    SyncEventGuard guard (mWiredModeHoldEvent);
                    mWiredModeHoldEvent.notifyOne();
                    hold_wired_mode = false;
                }
            }
            break;
        case NFCC_ACTIVATED_NTF:
        case NFCC_ACTION_NTF:
        case NFCC_RF_TIMEOUT_EVT:
            if(nfcFL.nfcNxpEse &&
                    nfcFL.eseFL._NFCC_ESE_UICC_CONCURRENT_ACCESS_PROTECTION &&
                    (meseUiccConcurrentAccess == false)) {
                if(nfcFL.eseFL._ESE_DUAL_MODE_PRIO_SCHEME != nfcFL.eseFL._ESE_WIRED_MODE_RESUME) {
                    SecureElement::getInstance().mRfFieldEventTimer.kill();
                }
            }
            else if(nfcFL.eseFL._ESE_DUAL_MODE_PRIO_SCHEME == nfcFL.eseFL._ESE_WIRED_MODE_RESUME) {
                SecureElement::getInstance().mRfFieldEventTimer.kill();
            }
            if(!block)
            {
                mIsWiredModeBlocked = false;
                if(hold_wired_mode)
                {
                    SyncEventGuard guard (mWiredModeHoldEvent);
                    mWiredModeHoldEvent.notifyOne();
                    hold_wired_mode = false;
                }
            }
            else
            {
                mIsWiredModeBlocked = true;
            }
    }
    ALOGV("%s  Exit",__func__);
}

/*******************************************************************************
**
** Function         rfFeildEventTimeoutCallback
**
** Description      Call back funtion to resume wired mode after RF field event timeout
**
** Returns          void
**
*******************************************************************************/
static void rfFeildEventTimeoutCallback(union sigval)
{
    ALOGV("rfFeildEventTimeoutCallback  Enter");
    SecureElement::getInstance().setDwpTranseiveState(false, NFCC_RF_FIELD_EVT);
}
#endif

/*******************************************************************************
**
** Function         setNfccPwrConfig
**
** Description      sends the link cntrl command to eSE with the value passed
**
** Returns          status
**
*******************************************************************************/
tNFA_STATUS SecureElement::setNfccPwrConfig(uint8_t value)
{
    if(!nfcFL.eseFL._WIRED_MODE_STANDBY) {
        ALOGV("WIRED_MODE_STANDBY not enabled. Returning");
        return NFA_STATUS_FAILED;
    }

    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    static uint8_t cur_value = 0xFF;

    /*if(cur_value == value)
    {
        mPwrCmdstatus = NFA_STATUS_OK;
    }
    else*/
    if(!android::nfcManager_isNfcActive()||
            android::nfcManager_isNfcDisabling() ||
                (android::nfcManager_getNfcState() == NFC_OFF))
    {
       ALOGE("%s: NFC is no longer active.", __func__);
       return NFA_STATUS_OK;
    }
    else
    {
        cur_value = value;
        SyncEventGuard guard (mPwrLinkCtrlEvent);
        nfaStat = NFA_SendPowerLinkCommand((uint8_t)EE_HANDLE_0xF3, value);
        if(nfaStat ==  NFA_STATUS_OK && !android::nfcManager_isNfcDisabling())
            mPwrLinkCtrlEvent.wait(NFC_CMD_TIMEOUT);
    }
    return mPwrCmdstatus;
}

#endif
