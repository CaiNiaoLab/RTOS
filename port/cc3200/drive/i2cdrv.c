/*
 * i2cdrv.c
 *
 * Copyright (c) 2021, ZhaoSQ-CH.CaiNiaoLab
 * Created on: 2021年8月3日
 *     Author: ZhaoSQ-CH.CaiNiaoLab
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <i2cdrv.h>

#include "hw_types.h"
#include "hw_memmap.h"
#include "hw_ints.h"
#include "hw_i2c.h"
#include "i2c.h"
#include "pin.h"
#include "rom.h"
#include "rom_map.h"
#include "prcm.h"

static void I2C_Drvice_Init(I2C_Handle handle);
int_least16_t I2C_Drvice_transfer(I2C_Handle handle,
                                 I2C_Transaction *transaction, uint32_t timeout);
extern const uint_least8_t g_I2CCount;

const I2C_FxnTable I2C_Drvice_fxnTable = {
    I2C_Drvice_Init,
    I2C_Drvice_transfer,
};

static void I2C_Drvice_Init(I2C_Handle handle) {
   I2CMCU_HWAttrs const *hwAttrs = handle->hwAttrs;
   I2CMCU_Object        *object = handle->object;
   uint32_t sys_clk = 80000000;  //< TODO: ����һ��ʱ�Ӻ��������滻
   uint_fast8_t i;

   for (i = 0; i < g_I2CCount; ++i) {
       //< set Peripheral clock
       MAP_PRCMPeripheralClkEnable(hwAttrs[i].peripherals, hwAttrs[i].clockEnable);
       MAP_PinTypeI2C(hwAttrs[i].sclPin, hwAttrs[i].pin_mode);
       MAP_PinTypeI2C(hwAttrs[i].sdaPin, hwAttrs[i].pin_mode);
       MAP_PRCMPeripheralClkEnable(hwAttrs[i].peripherals, hwAttrs[i].clockEnable);
       MAP_PRCMPeripheralReset(hwAttrs[i].peripherals);

       //< enable master
       MAP_I2CMasterEnable(hwAttrs[i].baseAddr);

       //< clear all interrupts.
       MAP_I2CMasterIntClear(hwAttrs[i].baseAddr);

       //< enable interrupts.
       MAP_I2CMasterIntEnableEx(hwAttrs[i].baseAddr,hwAttrs[i].intNum);

       if (object[i].bitRate == kI2C_1000KHz) {
           HWREG(hwAttrs[i].baseAddr + I2C_O_MTPR) =
                       ((sys_clk + (2 * 10 * 1000000) - 1) / (2 * 10 * 1000000)) - 1;
       } else if (object[i].bitRate == kI2C_100KHz) {
           MAP_I2CMasterInitExpClk(hwAttrs[i].baseAddr,sys_clk,false);
       } else if (object[i].bitRate == kI2C_400KHz) {
           MAP_I2CMasterInitExpClk(hwAttrs[i].baseAddr,sys_clk,true);
       } else {
           MAP_I2CMasterInitExpClk(hwAttrs[i].baseAddr,sys_clk,true);
       }
    }
}

static int I2CTransact(unsigned long baseAddr, unsigned long ulCmd) {

    //< Clear all interrupts
    MAP_I2CMasterIntClear(baseAddr);

    //< Set the time-out. Not to be used with breakpoints.
    MAP_I2CMasterTimeoutSet(baseAddr, I2C_TIMEOUT_VAL);

    //< Initiate the transfer.
    MAP_I2CMasterControl(baseAddr, ulCmd);

    //< Wait until the current byte has been transferred.
    //< Poll on the raw interrupt status.
    while((MAP_I2CMasterIntStatusEx(baseAddr, false)
                & I2C_MASTER_INT_DATA) == 0) {
        if ((MAP_I2CMasterIntStatusEx(baseAddr, false) & I2C_MASTER_INT_TIMEOUT) > 0) {
            return false;
        }
    }

    //< Check for any errors in transfer
    if(MAP_I2CMasterErr(baseAddr) != I2C_MASTER_ERR_NONE) {
        switch(ulCmd) {
        case I2C_MASTER_CMD_BURST_SEND_START:
        case I2C_MASTER_CMD_BURST_SEND_CONT:
        case I2C_MASTER_CMD_BURST_SEND_STOP:
            MAP_I2CMasterControl(baseAddr,
                         I2C_MASTER_CMD_BURST_SEND_ERROR_STOP);
            break;
        case I2C_MASTER_CMD_BURST_RECEIVE_START:
        case I2C_MASTER_CMD_BURST_RECEIVE_CONT:
        case I2C_MASTER_CMD_BURST_RECEIVE_FINISH:
            MAP_I2CMasterControl(baseAddr,
                         I2C_MASTER_CMD_BURST_RECEIVE_ERROR_STOP);
            break;
        default:
            break;
        }
        return false;
    }

    return true;
}

int_least16_t I2C_Drvice_transfer(I2C_Handle handle,
                                 I2C_Transaction *transaction, uint32_t timeout) {
    I2CMCU_HWAttrs const  *hwAttrs = handle->hwAttrs;
    I2CMCU_Object         *object = handle->object;
    int_least16_t status = I2C_STATUS_SUCCESS;
    int_least16_t ulCmdID;

    object->currentTransaction = transaction;

    object->writeBuf = transaction->writeBuf;
    object->writeCount = transaction->writeCount;

    object->readBuf = transaction->readBuf;
    object->readCount = transaction->readCount;

    object->burstStarted = true;

    transaction->status = I2C_STATUS_INCOMPLETE;

    if(MAP_I2CMasterBusBusy(hwAttrs->baseAddr)) {
        transaction->status = I2C_STATUS_BUS_BUSY;
        status = I2C_STATUS_BUS_BUSY;
    }

    //<
    MAP_I2CMasterSlaveAddrSet(hwAttrs->baseAddr, transaction->slavAddress, false);

    //< Write the first byte to the controller.
    MAP_I2CMasterDataPut(hwAttrs->baseAddr, *transaction->writeBuf);

    //< Initiate the transfer.
    I2CTransact(hwAttrs->baseAddr,I2C_MASTER_CMD_BURST_SEND_START);

    //< Decrement the count and increment the data pointer
    //< to facilitate the next transfer
    object->writeCount--;
    object->writeBuf++;

    //< Loop until the completion of transfer or error
    while(object->writeCount) {

        //< Write the next byte of data
        MAP_I2CMasterDataPut(hwAttrs->baseAddr, *transaction->writeBuf);

        //< Transact over I2C to send byte
        I2CTransact(hwAttrs->baseAddr, I2C_MASTER_CMD_BURST_SEND_CONT);

        //< Decrement the count and increment the data pointer
        //< to facilitate the next transfer
        object->writeCount--;
        object->writeBuf++;
    }

    if (object->readCount) {
        object->burstStarted = false;
    }

    //< If stop bit is to be sent, send it.
    if(object->burstStarted == true) {
        I2CTransact(hwAttrs->baseAddr, I2C_MASTER_CMD_BURST_SEND_STOP);
        return (status);
    }

    //< Set I2C codec slave address
    MAP_I2CMasterSlaveAddrSet(hwAttrs->baseAddr, transaction->slavAddress, true);

    //< Check if its a single receive or burst receive
    if(object->readCount == 1) {

        //< Configure for a single receive
        ulCmdID = I2C_MASTER_CMD_SINGLE_RECEIVE;
    }else{

        //< Initiate a burst receive sequence

        ulCmdID = I2C_MASTER_CMD_BURST_RECEIVE_START;
    }

    //< Initiate the transfer.

    I2CTransact(hwAttrs->baseAddr, ulCmdID);

    //< Decrement the count and increment the data pointer
    //< to facilitate the next transfer
    object->readCount--;

    //< Loop until the completion of reception or error
    while(object->readCount) {

        //< Receive the byte over I2C
        *object->readBuf = MAP_I2CMasterDataGet(hwAttrs->baseAddr);

        //< Decrement the count and increment the data pointer
        //< to facilitate the next transfer
        object->readCount--;
        object->readBuf++;
        if(object->readCount) {

            //< Continue the reception
            I2CTransact(hwAttrs->baseAddr, I2C_MASTER_CMD_BURST_RECEIVE_CONT);
        } else {

            //< Complete the last reception
            I2CTransact(hwAttrs->baseAddr, I2C_MASTER_CMD_BURST_RECEIVE_FINISH);
        }
    }

    //< Receive the byte over I2C
    *object->readBuf = MAP_I2CMasterDataGet(hwAttrs->baseAddr);
    return (status);
}

#if debug
static int I2CTransact(unsigned long ulCmd) {
    //
    // Clear all interrupts
    //
    MAP_I2CMasterIntClear(I2C_BASE);

    //
    // Set the time-out. Not to be used with breakpoints.
    //
    MAP_I2CMasterTimeoutSet(I2C_BASE, I2C_TIMEOUT_VAL);

    //
    // Initiate the transfer.
    //
    MAP_I2CMasterControl(I2C_BASE, ulCmd);

    //
    // Wait until the current byte has been transferred.
    // Poll on the raw interrupt status.
    //
    while((MAP_I2CMasterIntStatusEx(I2C_BASE, false)
                & I2C_MASTER_INT_DATA) == 0){
        if ((MAP_I2CMasterIntStatusEx(I2C_BASE, false) & I2C_MASTER_INT_TIMEOUT) > 0) {
            return false;
        }
    }

    //
    // Check for any errors in transfer
    //
    if(MAP_I2CMasterErr(I2C_BASE) != I2C_MASTER_ERR_NONE) {
        switch(ulCmd) {
        case I2C_MASTER_CMD_BURST_SEND_START:
        case I2C_MASTER_CMD_BURST_SEND_CONT:
        case I2C_MASTER_CMD_BURST_SEND_STOP:
            MAP_I2CMasterControl(I2C_BASE,
                         I2C_MASTER_CMD_BURST_SEND_ERROR_STOP);
            break;
        case I2C_MASTER_CMD_BURST_RECEIVE_START:
        case I2C_MASTER_CMD_BURST_RECEIVE_CONT:
        case I2C_MASTER_CMD_BURST_RECEIVE_FINISH:
            MAP_I2CMasterControl(I2C_BASE,
                         I2C_MASTER_CMD_BURST_RECEIVE_ERROR_STOP);
            break;
        default:
            break;
        }
        return false;
    }

    return true;
}
int
I2C_IF_Read(unsigned char ucDevAddr,
        unsigned char *pucData,
        unsigned char ucLen)
{
    unsigned long ulCmdID;

    //
    // Set I2C codec slave address
    //
    MAP_I2CMasterSlaveAddrSet(I2C_BASE, ucDevAddr, true);
    //
    // Check if its a single receive or burst receive
    //
    if(ucLen == 1)
    {
        //
        // Configure for a single receive
        //
        ulCmdID = I2C_MASTER_CMD_SINGLE_RECEIVE;
    }
    else
    {
        //
        // Initiate a burst receive sequence
        //
        ulCmdID = I2C_MASTER_CMD_BURST_RECEIVE_START;
    }
    //
    // Initiate the transfer.
    //
    I2CTransact(ulCmdID);
    //
    // Decrement the count and increment the data pointer
    // to facilitate the next transfer
    //
    ucLen--;
    //
    // Loop until the completion of reception or error
    //
    while(ucLen)
    {
        //
        // Receive the byte over I2C
        //
        *pucData = MAP_I2CMasterDataGet(I2C_BASE);
        //
        // Decrement the count and increment the data pointer
        // to facilitate the next transfer
        //
        ucLen--;
        pucData++;
        if(ucLen)
        {
            //
            // Continue the reception
            //
            I2CTransact(I2C_MASTER_CMD_BURST_RECEIVE_CONT);
        }
        else
        {
            //
            // Complete the last reception
            //
            I2CTransact(I2C_MASTER_CMD_BURST_RECEIVE_FINISH);
        }
    }
    //
    // Receive the byte over I2C
    //
    *pucData = MAP_I2CMasterDataGet(I2C_BASE);

    return SUCCESS;
}

int
I2C_IF_Write(unsigned char ucDevAddr,
         unsigned char *pucData,
         unsigned char ucLen,
         unsigned char ucStop)
{
    //pucData == NULL);
    //RETERR_IF_TRUE(ucLen == 0);
    //
    // Set I2C codec slave address
    //
    MAP_I2CMasterSlaveAddrSet(I2C_BASE, ucDevAddr, false);
    //
    // Write the first byte to the controller.
    //
    MAP_I2CMasterDataPut(I2C_BASE, *pucData);
    //
    // Initiate the transfer.
    //
    I2CTransact(I2C_MASTER_CMD_BURST_SEND_START);
    //
    // Decrement the count and increment the data pointer
    // to facilitate the next transfer
    //
    ucLen--;
    pucData++;
    //
    // Loop until the completion of transfer or error
    //
    while(ucLen)
    {
        //
        // Write the next byte of data
        //
        MAP_I2CMasterDataPut(I2C_BASE, *pucData);
        //
        // Transact over I2C to send byte
        //
        I2CTransact(I2C_MASTER_CMD_BURST_SEND_CONT);
        //
        // Decrement the count and increment the data pointer
        // to facilitate the next transfer
        //
        ucLen--;
        pucData++;
    }
    //
    // If stop bit is to be sent, send it.
    //
    if(ucStop == true)
    {
        I2CTransact(I2C_MASTER_CMD_BURST_SEND_STOP);
    }

    return SUCCESS;
}

int
I2C_IF_ReadFrom(unsigned char ucDevAddr,
            unsigned char *pucWrDataBuf,
            unsigned char ucWrLen,
            unsigned char *pucRdDataBuf,
            unsigned char ucRdLen)
{
    //
    // Write the register address to be read from.
    // Stop bit implicitly assumed to be 0.
    //
    I2C_IF_Write(ucDevAddr,pucWrDataBuf,ucWrLen,0);
    //
    // Read the specified length of data
    //
    I2C_IF_Read(ucDevAddr, pucRdDataBuf, ucRdLen);

    return SUCCESS;
}

int
GetRegisterValue(unsigned char ucRegAddr, unsigned short *pusRegValue)
{
    unsigned char ucRegData[2];
    //
    // Invoke the readfrom I2C API to get the required byte
    //
    if(I2C_IF_ReadFrom(0x41, &ucRegAddr, 1,
                   &ucRegData[0], 2) != 0)
    {
        //DBG_PRINT("TMP006 sensor I2C ReadFrom failed\n\r");
        return 0;
    }

    *pusRegValue = (unsigned short)(ucRegData[0] << 8) | ucRegData[1];

    return 1;
}

#endif
