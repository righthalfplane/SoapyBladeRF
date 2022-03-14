/*
 * This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (C) 2015-2018 Josh Blum
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "bladeRF_SoapySDR.hpp"
#include <SoapySDR/Logger.hpp>
#include <stdexcept>
#include <iostream>
#include <stdio.h>

//cross platform usleep()
#ifdef _MSC_VER
#include <windows.h>
#define usleep(t) Sleep((t)/1000)
#else
#include <unistd.h>
#endif

#define DEF_NUM_BUFFS 32
#define DEF_BUFF_LEN 4096

std::vector<std::string> bladeRF_SoapySDR::getStreamFormats(const int, const size_t) const
{
    std::vector<std::string> formats;
    formats.push_back("CS16");
    formats.push_back("CF32");
    return formats;
}

std::string bladeRF_SoapySDR::getNativeStreamFormat(const int, const size_t, double &fullScale) const
{
    fullScale = 2048;
    return "CS16";
}

SoapySDR::ArgInfoList bladeRF_SoapySDR::getStreamArgsInfo(const int, const size_t) const
{
    SoapySDR::ArgInfoList streamArgs;

    SoapySDR::ArgInfo buffersArg;
    buffersArg.key = "buffers";
    buffersArg.value = std::to_string(DEF_NUM_BUFFS);
    buffersArg.name = "Buffer Count";
    buffersArg.description = "Number of async USB buffers.";
    buffersArg.units = "buffers";
    buffersArg.type = SoapySDR::ArgInfo::INT;
    streamArgs.push_back(buffersArg);

    SoapySDR::ArgInfo lengthArg;
    lengthArg.key = "buflen";
    lengthArg.value = std::to_string(DEF_BUFF_LEN);
    lengthArg.name = "Buffer Length";
    lengthArg.description = "Number of bytes per USB buffer, the number must be a multiple of 1024.";
    lengthArg.units = "bytes";
    lengthArg.type = SoapySDR::ArgInfo::INT;
    streamArgs.push_back(lengthArg);

    SoapySDR::ArgInfo xfersArg;
    xfersArg.key = "transfers";
    xfersArg.value = "0";
    xfersArg.name = "Num Transfers";
    xfersArg.description = "Number of async USB transfers. Use 0 for automatic";
    xfersArg.units = "bytes";
    xfersArg.type = SoapySDR::ArgInfo::INT;
    xfersArg.range = SoapySDR::Range(0, 32);
    streamArgs.push_back(xfersArg);

    return streamArgs;
}

SoapySDR::Stream *bladeRF_SoapySDR::setupStream(
    const int direction,
    const std::string &format,
    const std::vector<size_t> &channels_,
    const SoapySDR::Kwargs &args)
{
    auto channels = channels_;
    if (channels.empty()) channels.push_back(0);
    
    printf("Simple setupStream\n");

    //check the channel configuration
    bladerf_channel_layout layout;
    if (channels.size() == 1 and channels.at(0) == 0)
    {
        layout = (direction == SOAPY_SDR_RX)?BLADERF_RX_X1:BLADERF_TX_X1;
    }
    else if (channels.size() == 2 and channels.at(0) == 0 and channels.at(1) == 1)
    {
        layout = (direction == SOAPY_SDR_RX)?BLADERF_RX_X2:BLADERF_TX_X2;
    }
    else
    {
        throw std::runtime_error("setupStream invalid channel selection");
    }

    //check the format
    if (format == "CF32") {}
    else if (format == "CS16") {}
    else throw std::runtime_error("setupStream invalid format " + format);

    //determine the number of buffers to allocate
    int numBuffs = (args.count("buffers") == 0)? 0 : atoi(args.at("buffers").c_str());
    if (numBuffs == 0) numBuffs = DEF_NUM_BUFFS;
    if (numBuffs == 1) numBuffs++;

    //determine the size of each buffer in samples
    int bufSize = (args.count("buflen") == 0)? 0 : atoi(args.at("buflen").c_str());
    if (bufSize == 0) bufSize = DEF_BUFF_LEN;
    if ((bufSize % 1024) != 0) bufSize = ((bufSize/1024) + 1) * 1024;

    //determine the number of active transfers
    int numXfers = (args.count("transfers") == 0)? 0 : atoi(args.at("transfers").c_str());
    if (numXfers == 0) numXfers = numBuffs/2;
    if (numXfers > numBuffs) numXfers = numBuffs; //cant have more than available buffers
    if (numXfers > 32) numXfers = 32; //libusb limit

    //setup the stream for sync tx/rx calls
    int ret = bladerf_sync_config(
        _dev,
        layout,
        BLADERF_FORMAT_SC16_Q11,
        numBuffs,
        bufSize,
        numXfers,
        1000); //1 second timeout
    if (ret != 0)
    {
        SoapySDR::logf(SOAPY_SDR_ERROR, "bladerf_sync_config() returned %d", ret);
        throw std::runtime_error("setupStream() " + _err2str(ret));
    }

    //enable channels used in streaming
    for (const auto ch : channels)
    {
        ret = bladerf_enable_module(_dev, _toch(direction, ch), true);
        if (ret != 0)
        {
            SoapySDR::logf(SOAPY_SDR_ERROR, "bladerf_enable_module(true) returned %d", ret);
            throw std::runtime_error("setupStream() " + _err2str(ret));
        }
    }

    if (direction == SOAPY_SDR_RX)
    {
        _rxOverflow = false;
        _rxChans = channels;
        _rxFloats = (format == "CF32");
        _rxConvBuff = new int16_t[bufSize*2*_rxChans.size()];
        _rxBuffSize = bufSize;
        this->updateRxMinTimeoutMs();
    }

    if (direction == SOAPY_SDR_TX)
    {
        _txFloats = (format == "CF32");
        _txChans = channels;
        _txConvBuff = new int16_t[bufSize*2*_txChans.size()];
        _txBuffSize = bufSize;
    }

    return (SoapySDR::Stream *)(new int(direction));
}

void bladeRF_SoapySDR::closeStream(SoapySDR::Stream *stream)
{
    const int direction = *reinterpret_cast<int *>(stream);
    auto &chans = (direction == SOAPY_SDR_RX)?_rxChans:_txChans;

    //deactivate the stream here -- only call once
    for (const auto ch : chans)
    {
        const int ret = bladerf_enable_module(_dev, _toch(direction, ch), false);
        if (ret != 0)
        {
            SoapySDR::logf(SOAPY_SDR_ERROR, "bladerf_enable_module(false) returned %s", _err2str(ret).c_str());
            throw std::runtime_error("closeStream() " + _err2str(ret));
        }
    }
    chans.clear();

    //cleanup stream convert buffers
    if (direction == SOAPY_SDR_RX)
    {
        delete [] _rxConvBuff;
    }

    if (direction == SOAPY_SDR_TX)
    {
        delete [] _txConvBuff;
    }

    delete reinterpret_cast<int *>(stream);
}

size_t bladeRF_SoapySDR::getStreamMTU(SoapySDR::Stream *stream) const
{
    const int direction = *reinterpret_cast<int *>(stream);
    return (direction == SOAPY_SDR_RX)?_rxBuffSize:_txBuffSize;
}

int bladeRF_SoapySDR::activateStream(
    SoapySDR::Stream *stream,
    const int flags,
    const long long timeNs,
    const size_t numElems)
{
    const int direction = *reinterpret_cast<int *>(stream);

    if (direction == SOAPY_SDR_RX)
    {
    }

    if (direction == SOAPY_SDR_TX)
    {
        if (flags != 0) return SOAPY_SDR_NOT_SUPPORTED;
    }

    return 0;
}

int bladeRF_SoapySDR::deactivateStream(
    SoapySDR::Stream *stream,
    const int flags,
    const long long)
{
    const int direction = *reinterpret_cast<int *>(stream);
    if (flags != 0) return SOAPY_SDR_NOT_SUPPORTED;

    if (direction == SOAPY_SDR_RX)
    {
        //clear all commands when deactivating
    }

    if (direction == SOAPY_SDR_TX)
    {
    }

    return 0;
}

int bladeRF_SoapySDR::readStream(
    SoapySDR::Stream *,
    void * const *buffs,
    size_t numElems,
    int &flags,
    long long &timeNs,
    const long timeoutUs)
{
    //clip to the available conversion buffer size
    numElems = std::min(numElems, _rxBuffSize);

    //prepare buffers
    void *samples = (void *)buffs[0];
    if (_rxFloats or _rxChans.size() == 2) samples = _rxConvBuff;

    //recv the rx samples
    const long timeoutMs = std::max(_rxMinTimeoutMs, timeoutUs/1000);
    int ret = bladerf_sync_rx(_dev, samples, numElems*_rxChans.size(), NULL, timeoutMs);
    if (ret == BLADERF_ERR_TIMEOUT) return SOAPY_SDR_TIMEOUT;
    if (ret == BLADERF_ERR_TIME_PAST) return SOAPY_SDR_TIME_ERROR;
    if (ret != 0)
    {
        SoapySDR::logf(SOAPY_SDR_ERROR, "bladerf_sync_rx() returned %s", _err2str(ret).c_str());
        return SOAPY_SDR_STREAM_ERROR;
    }
    //perform the int16 to float conversion
    if (_rxFloats and _rxChans.size() == 1)
    {
        float *output = (float *)buffs[0];
        for (size_t i = 0; i < 2 * numElems; i++)
        {
            output[i] = float(_rxConvBuff[i])/2048;
        }
    }
    else if (not _rxFloats and _rxChans.size() == 2)
    {
        int16_t *output0 = (int16_t *)buffs[0];
        int16_t *output1 = (int16_t *)buffs[1];
        for (size_t i = 0; i < 4 * numElems;)
        {
            *(output0++) = _rxConvBuff[i++];
            *(output0++) = _rxConvBuff[i++];
            *(output1++) = _rxConvBuff[i++];
            *(output1++) = _rxConvBuff[i++];
        }
    }
    else if (_rxFloats and _rxChans.size() == 2)
    {
        float *output0 = (float *)buffs[0];
        float *output1 = (float *)buffs[1];
        for (size_t i = 0; i < 4 * numElems;)
        {
            *(output0++) = float(_rxConvBuff[i++])/2048;
            *(output0++) = float(_rxConvBuff[i++])/2048;
            *(output1++) = float(_rxConvBuff[i++])/2048;
            *(output1++) = float(_rxConvBuff[i++])/2048;
        }
    }

    return numElems;
}

int bladeRF_SoapySDR::writeStream(
    SoapySDR::Stream *,
    const void * const *buffs,
    size_t numElems,
    int &flags,
    const long long timeNs,
    const long timeoutUs)
{
    //clip to the available conversion buffer size
    numElems = std::min(numElems, _txBuffSize);

    //prepare buffers
    void *samples = (void *)buffs[0];
    if (_txFloats or _txChans.size() == 2) samples = _txConvBuff;

    //perform the float to int16 conversion
    if (_txFloats and _txChans.size() == 1)
    {
        float *input = (float *)buffs[0];
        for (size_t i = 0; i < 2 * numElems; i++)
        {
            _txConvBuff[i] = int16_t(input[i]*2048);
        }
    }
    else if (not _txFloats and _txChans.size() == 2)
    {
        int16_t *input0 = (int16_t *)buffs[0];
        int16_t *input1 = (int16_t *)buffs[1];
        for (size_t i = 0; i < 4 * numElems;)
        {
            _txConvBuff[i++] = *(input0++);
            _txConvBuff[i++] = *(input0++);
            _txConvBuff[i++] = *(input1++);
            _txConvBuff[i++] = *(input1++);
        }
    }
    else if (_txFloats and _txChans.size() == 2)
    {
        float *input0 = (float *)buffs[0];
        float *input1 = (float *)buffs[1];
        for (size_t i = 0; i < 4 * numElems;)
        {
            _txConvBuff[i++] = int16_t(*(input0++)*2048);
            _txConvBuff[i++] = int16_t(*(input0++)*2048);
            _txConvBuff[i++] = int16_t(*(input1++)*2048);
            _txConvBuff[i++] = int16_t(*(input1++)*2048);
        }
    }

    //send the tx samples
    int ret = bladerf_sync_tx(_dev, samples, numElems*_txChans.size(), NULL, timeoutUs/1000);
    if (ret == BLADERF_ERR_TIMEOUT) return SOAPY_SDR_TIMEOUT;
    if (ret == BLADERF_ERR_TIME_PAST) return SOAPY_SDR_TIME_ERROR;
    if (ret != 0)
    {
        SoapySDR::logf(SOAPY_SDR_ERROR, "bladerf_sync_tx() returned %s", _err2str(ret).c_str());
        return SOAPY_SDR_STREAM_ERROR;
    }
    
   return numElems;
}

int bladeRF_SoapySDR::readStreamStatus(
    SoapySDR::Stream *stream,
    size_t &,
    int &flags,
    long long &timeNs,
    const long timeoutUs
)
{
    const int direction = *reinterpret_cast<int *>(stream);
    if (direction == SOAPY_SDR_RX) return SOAPY_SDR_NOT_SUPPORTED;

    //wait for an event to be ready considering the timeout and time
    //this is an emulation by polling and waiting on the hardware time
    long long timeNowNs = this->getHardwareTime();
    const long long exitTimeNs = timeNowNs + (timeoutUs*1000);
    while (true)
    {
        //no status to report, sleep for a bit
        if (_txResps.empty()) goto pollSleep;

        //no time on the current status, done waiting...
        if ((_txResps.front().flags & SOAPY_SDR_HAS_TIME) == 0) break;

        //current status time expired, done waiting...
        if (_txResps.front().timeNs < timeNowNs) break;

        //sleep a bit, never more than time remaining
        pollSleep:
        usleep(std::min<long>(1000, (exitTimeNs-timeNowNs)/1000));

        //check for timeout expired
        timeNowNs = this->getHardwareTime();
        if (exitTimeNs < timeNowNs) return SOAPY_SDR_TIMEOUT;
    }

    //extract the most recent status event
    if (_txResps.empty()) return SOAPY_SDR_TIMEOUT;
    StreamMetadata resp = _txResps.front();
    _txResps.pop();

    //load the output from the response
    flags = resp.flags;
    timeNs = resp.timeNs;
    return resp.code;
}
