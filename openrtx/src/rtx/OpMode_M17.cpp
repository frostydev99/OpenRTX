/***************************************************************************
 *   Copyright (C) 2021 - 2023 by Federico Amedeo Izzo IU2NUO,             *
 *                                Niccolò Izzo IU2KIN                      *
 *                                Frederik Saraci IU2NRO                   *
 *                                Silvano Seva IU2KWO                      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>   *
 ***************************************************************************/

#include <interfaces/platform.h>
#include <interfaces/delays.h>
#include <interfaces/audio.h>
#include <interfaces/radio.h>
#include <OpMode_M17.hpp>
#include <audio_codec.h>
#include <rtx.h>

#ifdef PLATFORM_MOD17
#include <calibInfo_Mod17.h>
#include <interfaces/platform.h>

extern mod17Calib_t mod17CalData;
#endif

using namespace std;
using namespace M17;

OpMode_M17::OpMode_M17() : startRx(false), startTx(false), locked(false),
                           invertTxPhase(false), invertRxPhase(false)
{

}

OpMode_M17::~OpMode_M17()
{
    disable();
}

void OpMode_M17::enable()
{
    codec_init();
    modulator.init();
    demodulator.init();
    locked  = false;
    startRx = true;
    startTx = false;
}

void OpMode_M17::disable()
{
    startRx = false;
    startTx = false;
    platform_ledOff(GREEN);
    platform_ledOff(RED);
    audioPath_release(rxAudioPath);
    audioPath_release(txAudioPath);
    codec_terminate();
    radio_disableRtx();
    modulator.terminate();
    demodulator.terminate();
}

void OpMode_M17::update(rtxStatus_t *const status, const bool newCfg)
{
    (void) newCfg;

    #if defined(PLATFORM_MD3x0) || defined(PLATFORM_MDUV3x0)
    //
    // Invert TX phase for all MDx models.
    // Invert RX phase for MD-3x0 VHF and MD-UV3x0 radios.
    //
    const hwInfo_t* hwinfo = platform_getHwInfo();
    invertTxPhase = true;
    if(hwinfo->vhf_band == 1)
        invertRxPhase = true;
    else
        invertRxPhase = false;
    #elif defined(PLATFORM_MOD17)
    //
    // Get phase inversion settings from calibration.
    //
    invertTxPhase = (mod17CalData.tx_invert == 1) ? true : false;
    invertRxPhase = (mod17CalData.rx_invert == 1) ? true : false;
    #endif

    // Main FSM logic
    switch(status->opStatus)
    {
        case OFF:
            offState(status);
            break;

        case RX:
            rxState(status);
            break;

        case TX:
            txState(status);
            break;

        default:
            break;
    }

    // Led control logic
    switch(status->opStatus)
    {
        case RX:

            if(locked)
                platform_ledOn(GREEN);
            else
                platform_ledOff(GREEN);

            break;

        case TX:
            platform_ledOff(GREEN);
            platform_ledOn(RED);
            break;

        default:
            platform_ledOff(GREEN);
            platform_ledOff(RED);
            break;
    }
}

void OpMode_M17::offState(rtxStatus_t *const status)
{
    radio_disableRtx();

    audioPath_release(rxAudioPath);
    audioPath_release(txAudioPath);
    codec_stop();

    if(startRx)
    {
        status->opStatus = RX;
    }

    if(platform_getPttStatus() && (status->txDisable == 0))
    {
        startTx = true;
        status->opStatus = TX;
    }
}

void OpMode_M17::rxState(rtxStatus_t *const status)
{
    if(startRx)
    {
        demodulator.startBasebandSampling();
        demodulator.invertPhase(invertRxPhase);

        rxAudioPath = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_RX);
        codec_startDecode(SINK_SPK);

        radio_enableRx();

        startRx = false;
    }

    bool newData = demodulator.update();
    bool lock    = demodulator.isLocked();

    // Reset frame decoder when transitioning from unlocked to locked state
    if((lock == true) && (locked == false))
    {
        decoder.reset();
    }

    locked = lock;

    if(locked && newData)
    {
        auto&   frame  = demodulator.getFrame();
        auto    type   = decoder.decodeFrame(frame);
        bool    lsfOk  = decoder.getLsf().valid();
        uint8_t pthSts = audioPath_getStatus(rxAudioPath);

        if((type == M17FrameType::STREAM) && (lsfOk == true) &&
           (pthSts == PATH_OPEN))
        {
            M17StreamFrame sf = decoder.getStreamFrame();
            codec_pushFrame(sf.payload().data(),     false);
            codec_pushFrame(sf.payload().data() + 8, false);
        }
    }

    if(platform_getPttStatus())
    {
        demodulator.stopBasebandSampling();
        locked = false;
        status->opStatus = OFF;
    }
}

void OpMode_M17::txState(rtxStatus_t *const status)
{
    frame_t m17Frame;

    if(startTx)
    {
        startTx = false;

        std::string src(status->source_address);
        std::string dst(status->destination_address);
        M17LinkSetupFrame lsf;

        lsf.clear();
        lsf.setSource(src);
        if(!dst.empty()) lsf.setDestination(dst);

        streamType_t type;
        type.fields.stream   = 1;             // Stream
        type.fields.dataType = 2;             // Voice data
        type.fields.CAN      = status->can;   // Channel access number

        lsf.setType(type);
        lsf.updateCrc();

        encoder.reset();
        encoder.encodeLsf(lsf, m17Frame);

        txAudioPath = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
        codec_startEncode(SOURCE_MIC);
        radio_enableTx();

        modulator.invertPhase(invertTxPhase);
        modulator.start();
        modulator.send(m17Frame);
    }

    payload_t dataFrame;
    bool      lastFrame = false;

    // Wait until there are 16 bytes of compressed speech, then send them
    codec_popFrame(dataFrame.data(),     true);
    codec_popFrame(dataFrame.data() + 8, true);

    if(platform_getPttStatus() == false)
    {
        lastFrame = true;
        startRx   = true;
        status->opStatus = OFF;
    }

    encoder.encodeStreamFrame(dataFrame, m17Frame, lastFrame);
    modulator.send(m17Frame);

    if(lastFrame)
    {
        encoder.encodeEotFrame(m17Frame);
        modulator.send(m17Frame);
        modulator.stop();
    }
}
