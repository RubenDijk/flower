#include "AF.h"
#include "MT_SYS.h"
#include "OSAL.h"
#include "ZComDef.h"
#include "ZDApp.h"
#include "ZDObject.h"

#include "nwk_util.h"

#include "zcl.h"
#include "zcl_diagnostic.h"
#include "zcl_general.h"
#include "zcl_genericapp.h"

#include "bdb.h"
#include "bdb_interface.h"
#include "gp_interface.h"

#include "Debug.h"

#include "onboard.h"

/* HAL */
#include "hal_drivers.h"
#include "hal_key.h"
#include "hal_led.h"

/*********************************************************************
 * MACROS
 */

/*********************************************************************
 * CONSTANTS
 */

/*********************************************************************
 * TYPEDEFS
 */

/*********************************************************************
 * GLOBAL VARIABLES
 */
byte zclGenericApp_TaskID;

/*********************************************************************
 * GLOBAL FUNCTIONS
 */

/*********************************************************************
 * LOCAL VARIABLES
 */

uint8 gPermitDuration = 0; // permit joining default to disabled
uint8 SeqNum = 0;
devStates_t zclGenericApp_NwkState = DEV_INIT;
static uint8 halKeySavedKeys;

// Endpoint to allow SYS_APP_MSGs
static endPointDesc_t sampleSw_TestEp = {
    1, // endpoint
    0, &zclGenericApp_TaskID,
    (SimpleDescriptionFormat_t *)NULL, // No Simple description for this test endpoint
    (afNetworkLatencyReq_t)0           // No Network Latency req
};

/*********************************************************************
 * LOCAL FUNCTIONS
 */
static void zclGenericApp_HandleKeys(byte shift, byte keys);
static void zclGenericApp_BasicResetCB(void);
static void zclGenericApp_BindNotification(bdbBindNotificationData_t *data);
void halProcessKeyInterrupt(void);

void GenericApp_HalKeyInit(void);
void zclGenericApp_ReportOnOff(uint8 endPoint, bool state);
void zclGenericApp_LeaveNetwork(void);

static void zclGenericApp_ProcessCommissioningStatus(bdbCommissioningModeMsg_t *bdbCommissioningModeMsg);

// Functions to process ZCL Foundation incoming Command/Response messages
static void zclGenericApp_ProcessIncomingMsg(zclIncomingMsg_t *msg);
#ifdef ZCL_READ
static uint8 zclGenericApp_ProcessInReadRspCmd(zclIncomingMsg_t *pInMsg);
#endif
#ifdef ZCL_WRITE
static uint8 zclGenericApp_ProcessInWriteRspCmd(zclIncomingMsg_t *pInMsg);
#endif
static uint8 zclGenericApp_ProcessInDefaultRspCmd(zclIncomingMsg_t *pInMsg);
#ifdef ZCL_DISCOVER
static uint8 zclGenericApp_ProcessInDiscCmdsRspCmd(zclIncomingMsg_t *pInMsg);
static uint8 zclGenericApp_ProcessInDiscAttrsRspCmd(zclIncomingMsg_t *pInMsg);
static uint8 zclGenericApp_ProcessInDiscAttrsExtRspCmd(zclIncomingMsg_t *pInMsg);
#endif

static void zclSampleApp_BatteryWarningCB(uint8 voltLevel);

/*********************************************************************
 * STATUS STRINGS
 */

/*********************************************************************
 * ZCL General Profile Callback table
 */
static zclGeneral_AppCallbacks_t zclGenericApp_CmdCallbacks = {
    zclGenericApp_BasicResetCB, // Basic Cluster Reset command
    NULL,                       // Identify Trigger Effect command
    NULL,                       // On/Off cluster commands
    NULL,                       // On/Off cluster enhanced command Off with Effect
    NULL,                       // On/Off cluster enhanced command On with Recall Global Scene
    NULL,                       // On/Off cluster enhanced command On with Timed Off
#ifdef ZCL_LEVEL_CTRL
    NULL, // Level Control Move to Level command
    NULL, // Level Control Move command
    NULL, // Level Control Step command
    NULL, // Level Control Stop command
#endif
#ifdef ZCL_GROUPS
    NULL, // Group Response commands
#endif
#ifdef ZCL_SCENES
    NULL, // Scene Store Request command
    NULL, // Scene Recall Request command
    NULL, // Scene Response command
#endif
#ifdef ZCL_ALARMS
    NULL, // Alarm (Response) commands
#endif
#ifdef SE_UK_EXT
    NULL, // Get Event Log command
    NULL, // Publish Event Log command
#endif
    NULL, // RSSI Location command
    NULL  // RSSI Location Response command
};

void zclGenericApp_Init(byte task_id) {
    zclGenericApp_TaskID = task_id;

    for (int i = 0; i < zclGenericApp_SimpleDescsCount; i++) {
        bdb_RegisterSimpleDescriptor(&zclGenericApp_SimpleDescs[i]);
        // Register the ZCL General Cluster Library callback functions
        zclGeneral_RegisterCmdCallbacks(zclGenericApp_SimpleDescs[i].EndPoint, &zclGenericApp_CmdCallbacks);

        // Register the application's attribute list
        zcl_registerAttrList(zclGenericApp_SimpleDescs[i].EndPoint, zclGenericApp_NumAttributes, zclGenericApp_Attrs);

#ifdef ZCL_DISCOVER
        // Register the application's command list
        zcl_registerCmdList(zclGenericApp_SimpleDescs[i].EndPoint, zclCmdsArraySize, zclGenericApp_Cmds);
#endif

        // Register for a test endpoint
        afRegister(&sampleSw_TestEp);

#ifdef ZCL_DIAGNOSTIC
        // Register the application's callback function to read/write attribute
        // data. This is only required when the attribute data format is unknown
        // to ZCL.
        zcl_registerReadWriteCB(zclGenericApp_SimpleDescs[i].EndPoint, zclDiagnostic_ReadWriteAttrCB, NULL);

        if (zclDiagnostic_InitStats() == ZSuccess) {
            // Here the user could start the timer to save Diagnostics to NV
        }
#endif
    }

    // GENERICAPP_TODO: Register other cluster command callbacks here

    // Register the Application to receive the unprocessed Foundation
    // command/response messages
    zcl_registerForMsg(zclGenericApp_TaskID);

    // Register low voltage NV memory protection application callback
    RegisterVoltageWarningCB(zclSampleApp_BatteryWarningCB);

    // Register for all key events - This app will handle all key events
    RegisterForKeys(zclGenericApp_TaskID);

    bdb_RegisterCommissioningStatusCB(zclGenericApp_ProcessCommissioningStatus);

    bdb_RegisterBindNotificationCB(zclGenericApp_BindNotification);

    bdb_StartCommissioning(BDB_COMMISSIONING_MODE_NWK_FORMATION | BDB_COMMISSIONING_MODE_NWK_STEERING |
                           BDB_COMMISSIONING_MODE_FINDING_BINDING);

    DebugInit();
    LREPMaster("Initialized debug module \n");
    LREPMaster("Hello world \n");
}

/*********************************************************************
 * @fn          zclSample_event_loop
 *
 * @brief       Event Loop Processor for zclGeneral.
 *
 * @param       none
 *
 * @return      none
 */
uint16 zclGenericApp_event_loop(uint8 task_id, uint16 events) {
    afIncomingMSGPacket_t *MSGpkt;

    char buffer[11];
    sprintf(buffer, "%x \n", events);
    LREPMaster(buffer);

    (void)task_id; // Intentionally unreferenced parameter

    if (events & SYS_EVENT_MSG) {
        while ((MSGpkt = (afIncomingMSGPacket_t *)osal_msg_receive(zclGenericApp_TaskID))) {
            switch (MSGpkt->hdr.event) {
            case ZCL_INCOMING_MSG:
                // Incoming ZCL Foundation command/response messages
                zclGenericApp_ProcessIncomingMsg((zclIncomingMsg_t *)MSGpkt);
                break;

            case KEY_CHANGE:
                zclGenericApp_HandleKeys(((keyChange_t *)MSGpkt)->state, ((keyChange_t *)MSGpkt)->keys);
                break;

            case ZDO_STATE_CHANGE:
                zclGenericApp_NwkState = (devStates_t)(MSGpkt->hdr.status);
                break;

            default:
                break;
            }

            // Release the memory
            osal_msg_deallocate((uint8 *)MSGpkt);
        }

        // return unprocessed events
        return (events ^ SYS_EVENT_MSG);
    }

#if ZG_BUILD_ENDDEVICE_TYPE
    if (events & GENERICAPP_END_DEVICE_REJOIN_EVT) {
        bdb_ZedAttemptRecoverNwk();
        return (events ^ GENERICAPP_END_DEVICE_REJOIN_EVT);
    }
#endif

    /* GENERICAPP_TODO: handle app events here */

    if (events & HAL_LED_BLINK_EVENT) {
        // toggle LED 2 state, start another timer for 500ms
        HalLedSet(HAL_LED_2, HAL_LED_MODE_TOGGLE);
        osal_start_timerEx(zclGenericApp_TaskID, HAL_LED_BLINK_EVENT, 500);

        return (events ^ HAL_LED_BLINK_EVENT);
    }

    if (events & GENERICAPP_EVT_GO_TO_SLEEP) {
        LREPMaster("Going to sleep....\n");
        halSleep(10000);
        return (events ^ GENERICAPP_EVT_GO_TO_SLEEP);
    }

    if (events & GENERICAPP_SW1_LONG_PRESS) {
        LREPMaster("GENERICAPP_SW1_LONG_PRESS detected \n");
        if (bdbAttributes.bdbNodeIsOnANetwork) {
            // покидаем сеть
            zclGenericApp_LeaveNetwork();
        } else {
            // инициируем вход в сеть
            bdb_StartCommissioning(BDB_COMMISSIONING_MODE_NWK_FORMATION | BDB_COMMISSIONING_MODE_NWK_STEERING |
                                   BDB_COMMISSIONING_MODE_FINDING_BINDING | BDB_COMMISSIONING_MODE_INITIATOR_TL);
            // будем мигать пока не подключимся
            osal_start_timerEx(zclGenericApp_TaskID, HAL_LED_BLINK_EVENT, 500);
        }

        return events ^ GENERICAPP_SW1_LONG_PRESS;
    }

    // событие опроса кнопок
    if (events & HAL_KEY_EVENT) {
        /* Считывание кнопок */
        GenericApp_HalKeyPoll();

        return events ^ HAL_KEY_EVENT;
    }

    // Discard unknown events
    return 0;
}

// Инициализация выхода из сети
void zclGenericApp_LeaveNetwork(void) {
    LREPMaster("Leaving network\n");
    zclGenericApp_ResetAttributesToDefaultValues();

    NLME_LeaveReq_t leaveReq;
    // Set every field to 0
    osal_memset(&leaveReq, 0, sizeof(NLME_LeaveReq_t));

    // This will enable the device to rejoin the network after reset.
    leaveReq.rejoin = FALSE;

    // Set the NV startup option to force a "new" join.
    zgWriteStartupOptions(ZG_STARTUP_SET, ZCD_STARTOPT_DEFAULT_NETWORK_STATE);

    // Leave the network, and reset afterwards
    if (NLME_LeaveReq(&leaveReq) != ZSuccess) {
        // Couldn't send out leave; prepare to reset anyway
        ZDApp_LeaveReset(FALSE);
    }
}

static void zclGenericApp_HandleKeys(byte shift, byte keys) {
    LREP("zclGenericApp_HandleKeys" BYTE_TO_BINARY_PATTERN "", BYTE_TO_BINARY(keys));
    LREPMaster("\n");

    if (!keys) {
        LREPMaster("Button released\n");

        osal_stop_timerEx(zclGenericApp_TaskID, GENERICAPP_SW1_LONG_PRESS);
        osal_clear_event(zclGenericApp_TaskID, GENERICAPP_SW1_LONG_PRESS);

        osal_stop_timerEx(zclGenericApp_TaskID, HAL_KEY_EVENT);
        osal_clear_event(zclGenericApp_TaskID, HAL_KEY_EVENT);

        halKeySavedKeys = 0;
        // osal_start_timerEx(zclGenericApp_TaskID, GENERICAPP_EVT_GO_TO_SLEEP,
        // 10000);
    }

    if (keys & HAL_KEY_SW_1) {
        LREPMaster("Pressed button 1\n");
        zclGenericApp_ReportOnOff(zclGenericApp_SimpleDescs[1].EndPoint, TRUE);
        static bool LED_OnOff = FALSE;

        if (LED_OnOff) {
            osal_stop_timerEx(zclGenericApp_TaskID, HAL_LED_BLINK_EVENT);
            HalLedSet(HAL_LED_2, HAL_LED_MODE_OFF);
            LED_OnOff = FALSE;
        } else {
            osal_start_timerEx(zclGenericApp_TaskID, HAL_LED_BLINK_EVENT, 500);
            HalLedSet(HAL_LED_2, HAL_LED_MODE_ON);
            LED_OnOff = TRUE;
        }
    }
    if (keys & HAL_KEY_SW_2) {
        LREPMaster("Pressed button2\n");
    }
}

void GenericApp_HalKeyInit(void) {
    /* Сбрасываем сохраняемое состояние кнопок в 0 */
    halKeySavedKeys = 0;

    PUSH1_SEL &= ~(PUSH1_BV);
    PUSH1_DIR &= ~(PUSH1_BV);
    PUSH1_ICTL |= PUSH1_ICTLBIT;
    PUSH1_IEN |= PUSH1_IENBIT;
    PUSH1_PXIFG = ~(PUSH1_BIT);

    PICTL &= ~(PUSH1_EDGEBIT); /* Clear the edge bit */
    /* For falling edge, the bit must be set. */
#if (PUSH1_6_EDGE == HAL_KEY_FALLING_EDGE)
    PICTL |= PUSH1_EDGEBIT;
#endif

    P2SEL &= ~BV(0); // Port 2 Function Select =>Peripheral function
    // P2SEL |= BV(0);
    P2DIR &= ~BV(0); // Port 2 bit 0 Direction => Input
    PICTL &= ~BV(3); // Rising edge on input(P2.0) gives interrupt.
    P2IEN |= BV(0);  // Interrupt enabled on input (P2.0)
    IEN2 |= BV(1);   // Port 2 interrupt enabled
    P2IFG &= ~BV(0); // Port 2, input(P2.0) interrupt status flag. When the input port pin has an interrupt request
                     // pending, the corresponding flag bit is set.
    IEN0 |= BV(7); // Each interrupt source is individually enabled or disabled by setting its corresponding enable bit.

    // PUSH2_SEL &= ~(PUSH2_BV);
    // PUSH2_DIR &= ~(PUSH2_BV);
    // PUSH2_ICTL |= PUSH2_ICTLBIT;
    // PUSH2_IEN |= PUSH2_IENBIT;
    // PUSH2_PXIFG = ~(PUSH2_BIT);

    //  HAL_KEY_JOY_MOVE_ICTL |= HAL_KEY_JOY_MOVE_ICTLBIT;
    // HAL_KEY_JOY_MOVE_IEN |= HAL_KEY_JOY_MOVE_IENBIT;
    // HAL_KEY_JOY_MOVE_PXIFG = ~(HAL_KEY_JOY_MOVE_BIT);

    // PUSH2_SEL &= ~(PUSH2_BV); /* Set pin function to GPIO */
    // PUSH2_DIR &= ~(PUSH2_BV); /* Set pin direction to Input */

    // PUSH2_ICTL &= ~(PUSH2_ICTLBIT); /* don't generate interrupt */
    // PUSH2_IEN &= ~(PUSH2_IENBIT);   /* Clear interrupt enable bit */
}
#define HAL_KEY_DEBOUNCE_VALUE 25

void halProcessKeyInterrupt(void) {
    bool valid = true;

    // if (PUSH1_PXIFG & PUSH1_BIT) /* Interrupt Flag has been set */
    // {
    //     PUSH1_PXIFG = ~(PUSH1_BIT); /* Clear Interrupt Flag */
    //     valid = TRUE;
    // }

    // if (PUSH2_PXIFG & PUSH2_BIT) /* Interrupt Flag has been set */
    // {
    //     PUSH2_PXIFG = ~(PUSH2_BIT); /* Clear Interrupt Flag */
    //     valid = TRUE;
    // }

    if (valid) {
        osal_start_reload_timer(zclGenericApp_TaskID, HAL_KEY_EVENT, 100);
        osal_start_timerEx(zclGenericApp_TaskID, GENERICAPP_SW1_LONG_PRESS, 5000);
    }
}

HAL_ISR_FUNCTION(halKeyPort2Isr, P2INT_VECTOR) {
    HAL_ENTER_ISR();
    halProcessKeyInterrupt();
    HAL_KEY_CPU_PORT_2_IF = 0;
    PUSH2_PXIFG = 0;
    CLEAR_SLEEP_MODE();
    HAL_EXIT_ISR();

}

HAL_ISR_FUNCTION(halKeyPort0Isr, P0INT_VECTOR) {
    HAL_ENTER_ISR();
    halProcessKeyInterrupt();
    HAL_KEY_CPU_PORT_0_IF = 0;
    PUSH1_PXIFG = 0;
    CLEAR_SLEEP_MODE();
    HAL_EXIT_ISR();
}

// Считывание кнопок
void GenericApp_HalKeyPoll(void) {
    uint8 keys = 0;

    // нажата кнопка 1 ?
    if (HAL_PUSH_BUTTON1()) {
        keys |= HAL_KEY_SW_1;
    }

    // нажата кнопка 2 ?
    if (HAL_PUSH_BUTTON2()) {
        keys |= HAL_KEY_SW_2;
    }

    if (keys == halKeySavedKeys) {
        // Выход - нет изменений
        return;
    }
    // Сохраним текущее состояние кнопок для сравнения в след раз
    halKeySavedKeys = keys;

    // Вызовем генерацию события изменений кнопок
    OnBoard_SendKeys(keys, HAL_KEY_STATE_NORMAL);
}
static void zclGenericApp_ProcessCommissioningStatus(bdbCommissioningModeMsg_t *bdbCommissioningModeMsg) {
    switch (bdbCommissioningModeMsg->bdbCommissioningMode) {
    case BDB_COMMISSIONING_FORMATION:
        if (bdbCommissioningModeMsg->bdbCommissioningStatus == BDB_COMMISSIONING_SUCCESS) {
            // After formation, perform nwk steering again plus the remaining
            // commissioning modes that has not been process yet
            bdb_StartCommissioning(BDB_COMMISSIONING_MODE_NWK_STEERING |
                                   bdbCommissioningModeMsg->bdbRemainingCommissioningModes);
        } else {
            // Want to try other channels?
            // try with bdb_setChannelAttribute
        }
        break;
    case BDB_COMMISSIONING_NWK_STEERING:
        if (bdbCommissioningModeMsg->bdbCommissioningStatus == BDB_COMMISSIONING_SUCCESS) {
            // YOUR JOB:
            // We are on the nwk, what now?
        } else {
            // See the possible errors for nwk steering procedure
            // No suitable networks found
            // Want to try other channels?
            // try with bdb_setChannelAttribute
        }
        break;
    case BDB_COMMISSIONING_FINDING_BINDING:
        if (bdbCommissioningModeMsg->bdbCommissioningStatus == BDB_COMMISSIONING_SUCCESS) {
            // YOUR JOB:
        } else {
            // YOUR JOB:
            // retry?, wait for user interaction?
        }
        break;
    case BDB_COMMISSIONING_INITIALIZATION:
        // Initialization notification can only be successful. Failure on
        // initialization only happens for ZED and is notified as
        // BDB_COMMISSIONING_PARENT_LOST notification

        // YOUR JOB:
        // We are on a network, what now?

        break;
#if ZG_BUILD_ENDDEVICE_TYPE
    case BDB_COMMISSIONING_PARENT_LOST:
        if (bdbCommissioningModeMsg->bdbCommissioningStatus == BDB_COMMISSIONING_NETWORK_RESTORED) {
            // We did recover from losing parent
        } else {
            // Parent not found, attempt to rejoin again after a fixed delay
            osal_start_timerEx(zclGenericApp_TaskID, GENERICAPP_END_DEVICE_REJOIN_EVT,
                               GENERICAPP_END_DEVICE_REJOIN_DELAY);
        }
        break;
#endif
    }
}

static void zclGenericApp_BindNotification(bdbBindNotificationData_t *data) {
    // GENERICAPP_TODO: process the new bind information
}

static void zclGenericApp_BasicResetCB(void) { zclGenericApp_ResetAttributesToDefaultValues(); }

void zclSampleApp_BatteryWarningCB(uint8 voltLevel) {
    if (voltLevel == VOLT_LEVEL_CAUTIOUS) {
        // Send warning message to the gateway and blink LED
    } else if (voltLevel == VOLT_LEVEL_BAD) {
        // Shut down the system
    }
}

static void zclGenericApp_ProcessIncomingMsg(zclIncomingMsg_t *pInMsg) {
    switch (pInMsg->zclHdr.commandID) {
#ifdef ZCL_READ
    case ZCL_CMD_READ_RSP:
        zclGenericApp_ProcessInReadRspCmd(pInMsg);
        break;
#endif
#ifdef ZCL_WRITE
    case ZCL_CMD_WRITE_RSP:
        zclGenericApp_ProcessInWriteRspCmd(pInMsg);
        break;
#endif
    case ZCL_CMD_CONFIG_REPORT:
    case ZCL_CMD_CONFIG_REPORT_RSP:
    case ZCL_CMD_READ_REPORT_CFG:
    case ZCL_CMD_READ_REPORT_CFG_RSP:
    case ZCL_CMD_REPORT:
        // bdb_ProcessIncomingReportingMsg( pInMsg );
        break;

    case ZCL_CMD_DEFAULT_RSP:
        zclGenericApp_ProcessInDefaultRspCmd(pInMsg);
        break;
#ifdef ZCL_DISCOVER
    case ZCL_CMD_DISCOVER_CMDS_RECEIVED_RSP:
        zclGenericApp_ProcessInDiscCmdsRspCmd(pInMsg);
        break;

    case ZCL_CMD_DISCOVER_CMDS_GEN_RSP:
        zclGenericApp_ProcessInDiscCmdsRspCmd(pInMsg);
        break;

    case ZCL_CMD_DISCOVER_ATTRS_RSP:
        zclGenericApp_ProcessInDiscAttrsRspCmd(pInMsg);
        break;

    case ZCL_CMD_DISCOVER_ATTRS_EXT_RSP:
        zclGenericApp_ProcessInDiscAttrsExtRspCmd(pInMsg);
        break;
#endif
    default:
        break;
    }

    if (pInMsg->attrCmd)
        osal_mem_free(pInMsg->attrCmd);
}

#ifdef ZCL_READ
static uint8 zclGenericApp_ProcessInReadRspCmd(zclIncomingMsg_t *pInMsg) {
    zclReadRspCmd_t *readRspCmd;
    uint8 i;

    readRspCmd = (zclReadRspCmd_t *)pInMsg->attrCmd;
    for (i = 0; i < readRspCmd->numAttr; i++) {
        // Notify the originator of the results of the original read attributes
        // attempt and, for each successfull request, the value of the requested
        // attribute
    }

    return (TRUE);
}
#endif // ZCL_READ

#ifdef ZCL_WRITE

static uint8 zclGenericApp_ProcessInWriteRspCmd(zclIncomingMsg_t *pInMsg) {
    zclWriteRspCmd_t *writeRspCmd;
    uint8 i;

    writeRspCmd = (zclWriteRspCmd_t *)pInMsg->attrCmd;
    for (i = 0; i < writeRspCmd->numAttr; i++) {
        // Notify the device of the results of the its original write attributes
        // command.
    }

    return (TRUE);
}
#endif // ZCL_WRITE

/*********************************************************************
 * @fn      zclGenericApp_ProcessInDefaultRspCmd
 *
 * @brief   Process the "Profile" Default Response Command
 *
 * @param   pInMsg - incoming message to process
 *
 * @return  none
 */
static uint8 zclGenericApp_ProcessInDefaultRspCmd(zclIncomingMsg_t *pInMsg) {
    // zclDefaultRspCmd_t *defaultRspCmd = (zclDefaultRspCmd_t
    // *)pInMsg->attrCmd;

    // Device is notified of the Default Response command.
    (void)pInMsg;

    return (TRUE);
}
afAddrType_t zclGenericApp_DstAddr;

// Информирование о состоянии реле
void zclGenericApp_ReportOnOff(uint8 endPoint, bool state) {
    const uint8 NUM_ATTRIBUTES = 1;

    zclReportCmd_t *pReportCmd;


    pReportCmd = osal_mem_alloc(sizeof(zclReportCmd_t) + (NUM_ATTRIBUTES * sizeof(zclReport_t)));
    if (pReportCmd != NULL) {
        pReportCmd->numAttr = NUM_ATTRIBUTES;

        pReportCmd->attrList[0].attrID = ATTRID_ON_OFF;
        pReportCmd->attrList[0].dataType = ZCL_DATATYPE_BOOLEAN;
        pReportCmd->attrList[0].attrData = (void *)(&state);

        zclGenericApp_DstAddr.addrMode = (afAddrMode_t)Addr16Bit;
        zclGenericApp_DstAddr.addr.shortAddr = 0;
        zclGenericApp_DstAddr.endPoint = 1;

        zcl_SendReportCmd(zclGenericApp_SimpleDescs[0].EndPoint, &zclGenericApp_DstAddr, ZCL_CLUSTER_ID_GEN_ON_OFF,
                          pReportCmd, ZCL_FRAME_CLIENT_SERVER_DIR, false, SeqNum++);
    }

    osal_mem_free(pReportCmd);
}

#ifdef ZCL_DISCOVER
static uint8 zclGenericApp_ProcessInDiscCmdsRspCmd(zclIncomingMsg_t *pInMsg) {
    zclDiscoverCmdsCmdRsp_t *discoverRspCmd;
    uint8 i;

    discoverRspCmd = (zclDiscoverCmdsCmdRsp_t *)pInMsg->attrCmd;
    for (i = 0; i < discoverRspCmd->numCmd; i++) {
        // Device is notified of the result of its attribute discovery command.
    }

    return (TRUE);
}

static uint8 zclGenericApp_ProcessInDiscAttrsRspCmd(zclIncomingMsg_t *pInMsg) {
    zclDiscoverAttrsRspCmd_t *discoverRspCmd;
    uint8 i;

    discoverRspCmd = (zclDiscoverAttrsRspCmd_t *)pInMsg->attrCmd;
    for (i = 0; i < discoverRspCmd->numAttr; i++) {
        // Device is notified of the result of its attribute discovery command.
    }

    return (TRUE);
}

static uint8 zclGenericApp_ProcessInDiscAttrsExtRspCmd(zclIncomingMsg_t *pInMsg) {
    zclDiscoverAttrsExtRsp_t *discoverRspCmd;
    uint8 i;

    discoverRspCmd = (zclDiscoverAttrsExtRsp_t *)pInMsg->attrCmd;
    for (i = 0; i < discoverRspCmd->numAttr; i++) {
        // Device is notified of the result of its attribute discovery command.
    }

    return (TRUE);
}
#endif // ZCL_DISCOVER

/****************************************************************************
****************************************************************************/
