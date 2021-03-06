/*
** Copyright 2008-2010, The Android Open-Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <math.h>

//#define LOG_NDEBUG 0
#define LOG_TAG "AudioHardwareTegra"
#include <utils/Log.h>
#include <utils/String8.h>

#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <fcntl.h>

#include "AudioHardware.h"
#include <audio_effects/effect_aec.h>
#include <audio_effects/effect_ns.h>

namespace android_audio_legacy {
const uint32_t AudioHardware::inputSamplingRates[] = {
    8000, 11025, 12000, 16000, 22050, 32000, 44100, 48000
};

// number of times to attempt init() before giving up
const uint32_t MAX_INIT_TRIES = 10;

// When another thread wants to acquire the Mutex on the input or output stream, a short sleep
// period is forced in the read or write function to release the processor before acquiring the
// Mutex. Otherwise, as the read/write thread sleeps most of the time waiting for DMA buffers with
// the Mutex locked, the other thread could wait quite long before being able to acquire the Mutex.
#define FORCED_SLEEP_TIME_US  10000

// ----------------------------------------------------------------------------

//OK
// always succeeds, must call init() immediately after
AudioHardware::AudioHardware() :
    mInit(false), mMicMute(false), mBluetoothNrec(true), mBluetoothId(0),
    mOutput(0), /*mCurOut/InDevice*/ mCpcapCtlFd(-1), mHwOutRate(0), mHwInRate(0),
    mMasterVol(1.0), mVoiceVol(1.0),
    /*mCpcapGain*/
    mSpkrVolume(-1), mMicVolume(-1), mEcnsEnabled(0), mEcnsRequested(0), mBtScoOn(false)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    ALOGV("AudioHardware constructor");
}

//OK
// designed to be called multiple times for retries
status_t AudioHardware::init() {

    //ALOGE("%s[%u]\n", __func__, __LINE__);
    if (mInit) {
        return NO_ERROR;
    }

    mCpcapCtlFd = ::open("/dev/audio_ctl", O_RDWR);
    if (mCpcapCtlFd < 0) {
        ALOGE("open /dev/audio_ctl failed: %s", strerror(errno));
        goto error;
    }

    if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_OUT_GET_OUTPUT, &mCurOutDevice) < 0) {
        ALOGE("could not get output device: %s", strerror(errno));
        goto error;
    }
    if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_IN_GET_INPUT, &mCurInDevice) < 0) {
        ALOGE("could not get input device: %s", strerror(errno));
        goto error;
    }
    // For bookkeeping only
    if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_OUT_GET_RATE, &mHwOutRate) < 0) {
        ALOGE("could not get output rate: %s", strerror(errno));
        goto error;
    }
    if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_IN_GET_RATE, &mHwInRate) < 0) {
        ALOGE("could not get input rate: %s", strerror(errno));
        goto error;
    }

    readHwGainFile();

    mInit = true;
    return NO_ERROR;

error:
    if (mCpcapCtlFd >= 0) {
        (void) ::close(mCpcapCtlFd);
        mCpcapCtlFd = -1;
    }
    return NO_INIT;
}

AudioHardware::~AudioHardware()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    ALOGV("AudioHardware destructor");
    for (size_t index = 0; index < mInputs.size(); index++) {
        closeInputStream((AudioStreamIn*)mInputs[index]);
    }
    mInputs.clear();
    closeOutputStream((AudioStreamOut*)mOutput);
    if (mCpcapCtlFd >= 0) {
        (void) ::close(mCpcapCtlFd);
        mCpcapCtlFd = -1;
    }
}

//OK
void AudioHardware::readHwGainFile()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    int fd;
    int rc=0;
    int i;
    uint32_t format, version, barker;
    fd = open("/system/etc/cpcap_gain.bin", O_RDONLY);
    if (fd>=0) {
        ::read(fd, &format, sizeof(uint32_t));
        ::read(fd, &version, sizeof(uint32_t));
        ::read(fd, &barker, sizeof(uint32_t));
        rc = ::read(fd, mCpcapGain, sizeof(mCpcapGain));
        ALOGD("Read gain file, format %X version %X", format, version);
        ::close(fd);
    }
    if (rc != sizeof(mCpcapGain) || format != 0x30303032) {
        int gain;
        ALOGE("CPCAP gain file not valid. Using defaults.");
        for (int i=0; i<AUDIO_HW_GAIN_NUM_DIRECTIONS; i++) {
            if (i==AUDIO_HW_GAIN_SPKR_GAIN)
                gain = 11;
            else
                gain = 31;
            for (int j=0; j<AUDIO_HW_GAIN_NUM_USECASES; j++)
                for (int k=0; k<AUDIO_HW_GAIN_NUM_PATHS; k++)
                    mCpcapGain[i][j][k]=gain;
        }
    }
    return;
}

//OK
status_t AudioHardware::initCheck()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    return mInit ? NO_ERROR : NO_INIT;
}

//adev_open_output_stream
AudioStreamOut* AudioHardware::openOutputStream(
        uint32_t devices, int *format, uint32_t *channels, uint32_t *sampleRate, status_t *status)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    { // scope for the lock
        Mutex::Autolock lock(mLock);

        // only one output stream allowed
        if (mOutput) {
            if (status) {
                *status = INVALID_OPERATION;
            }
            return 0;
        }

        // create new output stream
        AudioStreamOutTegra* out = new AudioStreamOutTegra();
        for (unsigned tries = 0; tries < MAX_INIT_TRIES; ++tries) {
            if (NO_ERROR == out->init())
                break;
            ALOGW("AudioStreamOutTegra::init failed soft, retrying");
            sleep(1);
        }
        status_t lStatus;
        lStatus = out->initCheck();
        if (NO_ERROR != lStatus) {
            ALOGE("AudioStreamOutTegra::init failed hard");
        } else {
            lStatus = out->set(this, devices, format, channels, sampleRate);
        }
        if (status) {
            *status = lStatus;
        }
        if (lStatus == NO_ERROR) {
            mOutput = out;
        } else {
            mLock.unlock();
            delete out;
            out = NULL;
            mLock.lock();
        }
    }
    return mOutput;
}

//OK
//adev_close_output_stream
void AudioHardware::closeOutputStream(AudioStreamOut* out) {
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    Mutex::Autolock lock(mLock);
    if (mOutput == 0 || mOutput != out) {
        ALOGW("Attempt to close invalid output stream");
    }
    else {
        // AudioStreamOutTegra destructor calls standby which locks
        mOutput = 0;
        mLock.unlock();
        delete out;
        mLock.lock();
    }
}

//adev_open_input_stream
AudioStreamIn* AudioHardware::openInputStream(
        uint32_t devices, int *format, uint32_t *channels, uint32_t *sampleRate, status_t *status,
        AudioSystem::audio_in_acoustics acoustic_flags)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    // check for valid input source
    if (!AudioSystem::isInputDevice((AudioSystem::audio_devices)devices)) {
        return 0;
    }

    Mutex::Autolock lock(mLock);

    AudioStreamInTegra* in = new AudioStreamInTegra();
    // this serves a similar purpose as init()
    status_t lStatus = in->set(this, devices, format, channels, sampleRate, acoustic_flags);
    if (status) {
        *status = lStatus;
    }
    if (lStatus != NO_ERROR) {
        mLock.unlock();
        delete in;
        mLock.lock();
        return 0;
    }

    mInputs.add(in);

    return in;
}

//adev_close_input_stream
void AudioHardware::closeInputStream(AudioStreamIn* in)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    Mutex::Autolock lock(mLock);

    ssize_t index = mInputs.indexOf((AudioStreamInTegra *)in);
    if (index < 0) {
        ALOGW("Attempt to close invalid input stream");
    } else {
        mInputs.removeAt(index);
        mLock.unlock();
        delete in;
        mLock.lock();
    }
}

//NOT NEEDED ???
status_t AudioHardware::setMode(int mode)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    AutoMutex lock(mLock);
    bool wasInCall = isInCall();
    ALOGV("setMode() : new %d, old %d", mode, mMode);
    status_t status = AudioHardwareBase::setMode(mode);
    if (status == NO_ERROR) {
        if (wasInCall ^ isInCall()) {
            doRouting_l();
            if (wasInCall) {
                setMicMute_l(false);
            }
        }
    }

    return status;
}

//OK
// Must be called with mLock held
status_t AudioHardware::doStandby(int stop_fd, bool output, bool enable)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    status_t status = NO_ERROR;
    struct cpcap_audio_stream standby;

    ALOGV("AudioHardware::doStandby() putting %s in %s mode",
            output ? "output" : "input",
            enable ? "standby" : "online" );

// Debug code
    if (!mLock.tryLock()) {
        ALOGE("doStandby called without mLock held.");
        mLock.unlock();
    }
// end Debug code

    if (output) {
        standby.id = CPCAP_AUDIO_OUT_STANDBY;
        standby.on = enable;

        if (enable) {
            /* Flush the queued playback data.  Putting the output in standby
             * will cause CPCAP to not drive the i2s interface, and write()
             * will block until playback is resumed.
             */
            if (mOutput)
                mOutput->flush();
        }

        if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_OUT_SET_OUTPUT, &standby) < 0) {
            ALOGE("could not turn off current output device: %s",
                 strerror(errno));
            status = errno;
        }

        if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_OUT_GET_OUTPUT, &mCurOutDevice) < 0) {
            ALOGE("could not get current output device after standby: %s",
                 strerror(errno));
        }
        ALOGV("%s: after standby %s, output device %d is %s", __FUNCTION__,
             enable ? "enable" : "disable", mCurOutDevice.id,
             mCurOutDevice.on ? "on" : "off");
    } else {
        standby.id = CPCAP_AUDIO_IN_STANDBY;
        standby.on = enable;

        if (enable && stop_fd >= 0) {
            /* Stop recording, if ongoing.  Muting the microphone will cause
             * CPCAP to not send data through the i2s interface, and read()
             * will block until recording is resumed.
             */
            ALOGV("%s: stop recording", __FUNCTION__);
            if (::ioctl(stop_fd, TEGRA_AUDIO_IN_STOP) < 0) {
                ALOGE("could not stop recording: %s",
                     strerror(errno));
            }
        }

        if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_IN_SET_INPUT, &standby) < 0) {
            ALOGE("could not turn off current input device: %s",
                 strerror(errno));
            status = errno;
        }
        ::ioctl(mCpcapCtlFd, CPCAP_AUDIO_IN_GET_INPUT, &mCurInDevice);
        ALOGV("%s: after standby %s, input device %d is %s", __FUNCTION__,
             enable ? "enable" : "disable", mCurInDevice.id,
             mCurInDevice.on ? "on" : "off");
    }

    return status;
}

//ok
status_t AudioHardware::setMicMute(bool state)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    Mutex::Autolock lock(mLock); //needed ???
    return setMicMute_l(state);
}

//not needed 
status_t AudioHardware::setMicMute_l(bool state)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    if (mMicMute != state) {
        mMicMute = state;
        ALOGV("setMicMute() %s", (state)?"ON":"OFF");
    }
    return NO_ERROR;
}

//ok
status_t AudioHardware::getMicMute(bool* state)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    *state = mMicMute;
    return NO_ERROR;
}

//adev_set_parameters
status_t AudioHardware::setParameters(const String8& keyValuePairs)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 value;
    String8 key;
    const char BT_NREC_KEY[] = "bt_headset_nrec";
    const char BT_NAME_KEY[] = "bt_headset_name";
    const char BT_NREC_VALUE_ON[] = "on";


    ALOGV("setParameters() %s", keyValuePairs.string());

    if (keyValuePairs.length() == 0) return BAD_VALUE;

    key = String8(BT_NREC_KEY);
    if (param.get(key, value) == NO_ERROR) {
        if (value == BT_NREC_VALUE_ON) {
            mBluetoothNrec = true;
            ALOGD("Turn on bluetooth NREC");
        } else {
            mBluetoothNrec = false;
            ALOGD("Turning noise reduction and echo cancellation off for BT "
                 "headset");
        }
        doRouting();
    }
    key = String8(BT_NAME_KEY);
    if (param.get(key, value) == NO_ERROR) {
        mBluetoothId = 0;
#if 0
        for (int i = 0; i < mNumSndEndpoints; i++) {
            if (!strcasecmp(value.string(), mSndEndpoints[i].name)) {
                mBluetoothId = mSndEndpoints[i].id;
                ALOGD("Using custom acoustic parameters for %s", value.string());
                break;
            }
        }
#endif
        if (mBluetoothId == 0) {
            ALOGD("Using default acoustic parameters "
                 "(%s not in acoustic database)", value.string());
            doRouting();
        }
    }
    return NO_ERROR;
}

//adev_get_parameters
String8 AudioHardware::getParameters(const String8& keys)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    AudioParameter request = AudioParameter(keys);
    AudioParameter reply = AudioParameter();
    String8 value;
    String8 key;

    ALOGV("getParameters() %s", keys.string());



    return reply.toString();
}

    //ok
//adev_get_input_buffer_size
size_t AudioHardware::getInputBufferSize(uint32_t sampleRate, int format, int channelCount)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    size_t bufsize;

    if (format != AudioSystem::PCM_16_BIT) {
        ALOGW("getInputBufferSize bad format: %d", format);
        return 0;
    }
    #
    if (channelCount < 1 || channelCount > 2) {
        ALOGW("getInputBufferSize bad channel count: %d", channelCount);
        return 0;
    }

    // Return 20 msec input buffer size.
    bufsize = sampleRate * sizeof(int16_t) * channelCount / 50;
    if (bufsize & 0x7) {
       // Not divisible by 8.
       bufsize +=8;
       bufsize &= ~0x7;
    }
    ALOGV("%s: returns %d for rate %d", __FUNCTION__, bufsize, sampleRate);
    return bufsize;
}

//TARGET_LEGACY_UNSUPPORTED_LIBAUDIO
status_t AudioHardware::setMasterMute(bool muted) {
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    return NO_ERROR;
}

#if 1
int AudioHardware::createAudioPatch(unsigned int num_sources,
                               const struct audio_port_config *sources,
                               unsigned int num_sinks,
                               const struct audio_port_config *sinks,
                               audio_patch_handle_t *handle) {
    ALOGE("%s[%u]\n", __func__, __LINE__);
    return NO_ERROR;
}

int AudioHardware::releaseAudioPatch(audio_patch_handle_t handle) {
    ALOGE("%s[%u]\n", __func__, __LINE__);
    return NO_ERROR;
}

int AudioHardware::getAudioPort(struct audio_port *port) {
    ALOGE("%s[%u]\n", __func__, __LINE__);
    return NO_ERROR;
}

int AudioHardware::setAudioPortConfig(const struct audio_port_config *config) {
    ALOGE("%s[%u]\n", __func__, __LINE__);
    return NO_ERROR;
}
#endif

//ok
//setVoiceVolume is only useful for setting sidetone gains with a baseband
//controlling volume.  Don't adjust hardware volume with this API.
//
//(On Stingray, don't use mVoiceVol for anything.)
status_t AudioHardware::setVoiceVolume(float v)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    if (v < 0.0)
        v = 0.0;
    else if (v > 1.0)
        v = 1.0;

    ALOGV("Setting unused in-call vol to %f",v);
    mVoiceVol = v;

    return NO_ERROR;
}

//ok
//adev_set_master_volume
status_t AudioHardware::setMasterVolume(float v)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    if (v < 0.0)
        v = 0.0;
    else if (v > 1.0)
        v = 1.0;

    ALOGV("Set master vol to %f.", v);
    mMasterVol = v;
    Mutex::Autolock lock(mLock);
    int useCase = AUDIO_HW_GAIN_USECASE_MM;
    AudioStreamInTegra *input = getActiveInput_l();
    if (input) {
        if (isInCall() && mOutput && !mOutput->getStandby() &&
                input->source() == AUDIO_SOURCE_VOICE_COMMUNICATION) {
            useCase = AUDIO_HW_GAIN_USECASE_VOICE;
        } else if (input->source() == AUDIO_SOURCE_VOICE_RECOGNITION) {
            useCase = AUDIO_HW_GAIN_USECASE_VOICE_REC;
        }
    }
    setVolume_l(v, useCase);
    return NO_ERROR;
}

//ok
// Call with mLock held.
status_t AudioHardware::setVolume_l(float v, int usecase)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    int spkr = getGain(AUDIO_HW_GAIN_SPKR_GAIN, usecase);
    int mic = getGain(AUDIO_HW_GAIN_MIC_GAIN, usecase);

    if (spkr==0) {
       // no device to set volume on.  Ignore request.
       return -1;
    }

    spkr = ceil(v * spkr);
    if (mSpkrVolume != spkr) {
        ALOGV("Set tx volume to %d", spkr);
        int ret = ::ioctl(mCpcapCtlFd, CPCAP_AUDIO_OUT_SET_VOLUME, spkr);
        if (ret < 0) {
            ALOGE("could not set spkr volume: %s", strerror(errno));
            return ret;
        }
        mSpkrVolume = spkr;
    }
    if (mMicVolume != mic) {
        ALOGV("Set rx volume to %d", mic);
        int ret = ::ioctl(mCpcapCtlFd, CPCAP_AUDIO_IN_SET_VOLUME, mic);
        if (ret < 0) {
            ALOGE("could not set mic volume: %s", strerror(errno));
            return ret;
        }
        mMicVolume = mic;
    }

    return NO_ERROR;
}

uint8_t AudioHardware::getGain(int direction, int usecase)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    int path;
    AudioStreamInTegra *input = getActiveInput_l();
    uint32_t inDev = (input == NULL) ? 0 : input->devices();
    if (!mOutput) {
       ALOGE("No output device.");
       return 0;
    }
    uint32_t outDev = mOutput->devices();

// In case of an actual phone, with an actual earpiece, uncomment.
//    if (outDev & AudioSystem::DEVICE_OUT_EARPIECE)
//        path = AUDIO_HW_GAIN_EARPIECE;
//    else
    if (outDev & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE)
        path = AUDIO_HW_GAIN_HEADSET_NO_MIC;
    else if (outDev & AudioSystem::DEVICE_OUT_WIRED_HEADSET)
        path = AUDIO_HW_GAIN_HEADSET_W_MIC;
    else if (outDev & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)
        path = AUDIO_HW_GAIN_EMU_DEVICE;
    else
       path = AUDIO_HW_GAIN_SPEAKERPHONE;

    ALOGV("Picked gain[%d][%d][%d] which is %d.",direction, usecase, path,
          mCpcapGain[direction][usecase][path]);

    return mCpcapGain[direction][usecase][path];
}

int AudioHardware::getActiveInputRate()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    AudioStreamInTegra *input = getActiveInput_l();
    return (input != NULL) ? input->sampleRate() : 0;
}

status_t AudioHardware::doRouting()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    Mutex::Autolock lock(mLock);
    return doRouting_l();
}

// Call this with mLock held.
status_t AudioHardware::doRouting_l()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    if (!mOutput) {
        return NO_ERROR;
    }
    uint32_t outputDevices = mOutput->devices();
    AudioStreamInTegra *input = getActiveInput_l();
    uint32_t inputDevice = (input == NULL) ? 0 : input->devices();
    uint32_t btScoOutDevices = outputDevices & (
                           AudioSystem::DEVICE_OUT_BLUETOOTH_SCO |
                           AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET |
                           AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_CARKIT );
    uint32_t spdifOutDevices = outputDevices & (
                           AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET |
                           AudioSystem::DEVICE_OUT_AUX_DIGITAL );
    uint32_t speakerOutDevices = outputDevices ^ btScoOutDevices ^ spdifOutDevices;
    uint32_t btScoInDevice = inputDevice & AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET;
    uint32_t micInDevice   = inputDevice ^ btScoInDevice;
    int sndOutDevice = -1;
    int sndInDevice = -1;
    bool btScoOn = btScoOutDevices||btScoInDevice;

    ALOGV("%s: inputDevice %x, outputDevices %x", __FUNCTION__,
         inputDevice, outputDevices);

    switch (inputDevice) {
    case AudioSystem::DEVICE_IN_DEFAULT:
    case AudioSystem::DEVICE_IN_BUILTIN_MIC:
        sndInDevice = CPCAP_AUDIO_IN_MIC1;
        break;
    case AudioSystem::DEVICE_IN_WIRED_HEADSET:
        sndInDevice = CPCAP_AUDIO_IN_MIC2;
        break;
    default:
        break;
    }

    switch (speakerOutDevices) {
    case AudioSystem::DEVICE_OUT_EARPIECE:
    case AudioSystem::DEVICE_OUT_DEFAULT:
    case AudioSystem::DEVICE_OUT_SPEAKER:
        sndOutDevice = CPCAP_AUDIO_OUT_SPEAKER;
        break;
    case AudioSystem::DEVICE_OUT_WIRED_HEADSET:
    case AudioSystem::DEVICE_OUT_WIRED_HEADPHONE:
        sndOutDevice = CPCAP_AUDIO_OUT_HEADSET;
        break;
    case AudioSystem::DEVICE_OUT_SPEAKER | AudioSystem::DEVICE_OUT_WIRED_HEADSET:
    case AudioSystem::DEVICE_OUT_SPEAKER | AudioSystem::DEVICE_OUT_WIRED_HEADPHONE:
        sndOutDevice = CPCAP_AUDIO_OUT_HEADSET_AND_SPEAKER;
        break;
    case AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET:
        sndOutDevice = CPCAP_AUDIO_OUT_ANLG_DOCK_HEADSET;
        break;
    case AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET:
      // To be implemented
        break;
    default:
        break;
    }

    if (sndInDevice != (int)mCurInDevice.id) {
        if (sndInDevice == -1) {
            ALOGV("input device set %x not supported, defaulting to on-board mic",
                 inputDevice);
            mCurInDevice.id = CPCAP_AUDIO_IN_MIC1;
        }
        else
            mCurInDevice.id = sndInDevice;

        if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_IN_SET_INPUT,
                  &mCurInDevice) < 0)
            ALOGE("could not set input (%d, on %d): %s",
                 mCurInDevice.id, mCurInDevice.on, strerror(errno));

        ALOGV("current input %d, %s",
             mCurInDevice.id,
             mCurInDevice.on ? "on" : "off");
    }

    if (sndOutDevice != (int)mCurOutDevice.id) {
        if (sndOutDevice == -1) {
            ALOGW("output device set %x not supported, defaulting to speaker",
                 outputDevices);
            mCurOutDevice.id = CPCAP_AUDIO_OUT_SPEAKER;
        }
        else
            mCurOutDevice.id = sndOutDevice;

        if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_OUT_SET_OUTPUT,
                  &mCurOutDevice) < 0)
            ALOGE("could not set output (%d, on %d): %s",
                 mCurOutDevice.id, mCurOutDevice.on,
                 strerror(errno));

        ALOGV("current output %d, %s",
             mCurOutDevice.id,
             mCurOutDevice.on ? "on" : "off");
    }

    // enable EC if:
    // - mEcnsRequested AND
    // - the output stream is active
    mEcnsEnabled = mEcnsRequested;
    if (mOutput->getStandby()) {
        mEcnsEnabled &= ~PREPROC_AEC;
    }

    {
        if (input) {
            mHwInRate = getActiveInputRate();
        }
        mHwOutRate = AUDIO_HW_OUT_SAMPLERATE;
        ALOGV("No EC/NS, set input rate %d, output %d.", mHwInRate, mHwOutRate);
    }
    if (btScoOn) {
        mHwOutRate = 8000;
        mHwInRate = 8000;
        ALOGD("Bluetooth SCO active, rate forced to 8K");
    }

    if (input) {
        // acquire mutex if not already locked by read()
        if (!input->isLocked()) {
            input->lock();
        }
    }
    // acquire mutex if not already locked by write()
    if (!mOutput->isLocked()) {
        mOutput->lock();
    }

    mOutput->setDriver_l(speakerOutDevices?true:false,
                       btScoOn,
                       spdifOutDevices?true:false, mHwOutRate);

    if (input) {
        input->setDriver_l(micInDevice?true:false,
                btScoOn, mHwInRate);
    }

    // Changing I2S to port connection when bluetooth starts or stopS must be done simultaneously
    // for input and output while both DMAs are stopped
    if (btScoOn != mBtScoOn) {
        if (input) {
            input->lockFd();
            input->stop_l();
        }
        mOutput->lockFd();
        mOutput->flush_l();

        int bit_format = TEGRA_AUDIO_BIT_FORMAT_DEFAULT;
        bool is_bt_bypass = false;
        if (btScoOn) {
            bit_format = TEGRA_AUDIO_BIT_FORMAT_DSP;
            is_bt_bypass = true;
        }
        ALOGV("%s: bluetooth state changed. is_bt_bypass %d bit_format %d",
             __FUNCTION__, is_bt_bypass, bit_format);
        // Setup the I2S2-> DAP2/4 capture/playback path.
        if (::ioctl(mOutput->mBtFdIoCtl, TEGRA_AUDIO_SET_BIT_FORMAT, &bit_format) < 0) {
            ALOGE("could not set bit format %s", strerror(errno));
        }
        if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_SET_BLUETOOTH_BYPASS, is_bt_bypass) < 0) {
            ALOGE("could not set bluetooth bypass %s", strerror(errno));
        }

        mBtScoOn = btScoOn;
        mOutput->unlockFd();
        if (input) {
            input->unlockFd();
        }
    }

    if (!mOutput->isLocked()) {
        mOutput->unlock();
    }
    if (input && !input->isLocked()) {
        input->unlock();
    }

    // Since HW path may have changed, set the hardware gains.
    int useCase = AUDIO_HW_GAIN_USECASE_MM;
    if (mEcnsEnabled) {
        useCase = AUDIO_HW_GAIN_USECASE_VOICE;
    } else if (input && input->source() == AUDIO_SOURCE_VOICE_RECOGNITION) {
        useCase = AUDIO_HW_GAIN_USECASE_VOICE_REC;
    }
    setVolume_l(mMasterVol, useCase);

    return NO_ERROR;
}

status_t AudioHardware::dumpInternals(int fd, const Vector<String16>& args)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    result.append("AudioHardware::dumpInternals\n");
    snprintf(buffer, SIZE, "\tmInit: %s\n", mInit? "true": "false");
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmMicMute: %s\n", mMicMute? "true": "false");
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmBluetoothNrec: %s\n", mBluetoothNrec? "true": "false");
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmBluetoothId: %d\n", mBluetoothId);
    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return NO_ERROR;
}

//adev_dump
status_t AudioHardware::dump(int fd, const Vector<String16>& args)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    dumpInternals(fd, args);
    for (size_t index = 0; index < mInputs.size(); index++) {
        mInputs[index]->dump(fd, args);
    }

    if (mOutput) {
        mOutput->dump(fd, args);
    }
    return NO_ERROR;
}

uint32_t AudioHardware::getInputSampleRate(uint32_t sampleRate)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    uint32_t i;
    uint32_t prevDelta;
    uint32_t delta;

    for (i = 0, prevDelta = 0xFFFFFFFF; i < sizeof(inputSamplingRates)/sizeof(uint32_t); i++, prevDelta = delta) {
        delta = abs(sampleRate - inputSamplingRates[i]);
        if (delta > prevDelta) break;
    }
    // i is always > 0 here
    return inputSamplingRates[i-1];
}

// getActiveInput_l() must be called with mLock held
AudioHardware::AudioStreamInTegra *AudioHardware::getActiveInput_l()
{
    for (size_t i = 0; i < mInputs.size(); i++) {
        // return first input found not being in standby mode
        // as only one input can be in this state
        if (!mInputs[i]->getStandby()) {
            return mInputs[i];
        }
    }

    return NULL;
}

void AudioHardware::setEcnsRequested_l(int ecns, bool enabled)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    if (enabled) {
        mEcnsRequested |= ecns;
    } else {
        mEcnsRequested &= ~ecns;
    }
}

// ----------------------------------------------------------------------------
// Sample Rate Converter wrapper
//
#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
AudioHardware::AudioStreamSrc::AudioStreamSrc() :
        mSrcBuffer(NULL), mSrcInitted(false)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
}
AudioHardware::AudioStreamSrc::~AudioStreamSrc()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    if (mSrcBuffer != NULL) {
        delete[] mSrcBuffer;
    }
}

void AudioHardware::AudioStreamSrc::init(int inRate, int outRate)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    if (mSrcBuffer == NULL) {
        mSrcBuffer = new char[src_memory_required_stereo(MAX_FRAME_LEN, MAX_CONVERT_RATIO)];
    }
    if (mSrcBuffer == NULL) {
        ALOGE("Failed to allocate memory for sample rate converter.");
        return;
    }
    mSrcInit.memory = (SRC16*)(mSrcBuffer);
    mSrcInit.input_rate = inRate;
    mSrcInit.output_rate = outRate;
    mSrcInit.frame_length = MAX_FRAME_LEN;
    mSrcInit.stereo_flag = SRC_OFF;
    mSrcInit.input_interleaved = SRC_OFF;
    mSrcInit.output_interleaved = SRC_OFF;
    rate_convert_init(&mSrcInit, &mSrcObj);

    mSrcInitted = true;
    mSrcInRate = inRate;
    mSrcOutRate = outRate;
}
#endif

// ----------------------------------------------------------------------------

//OK
// always succeeds, must call init() immediately after
AudioHardware::AudioStreamOutTegra::AudioStreamOutTegra() :
    mBtFdIoCtl(-1), mHardware(0), mFd(-1), mFdCtl(-1),
    mBtFd(-1), mBtFdCtl(-1),
    mSpdifFd(-1), mSpdifFdCtl(-1),
    mStartCount(0), mRetryCount(0), mDevices(0),
    mIsSpkrEnabled(false), mIsBtEnabled(false), mIsSpdifEnabled(false),
    mIsSpkrEnabledReq(false), mIsBtEnabledReq(false), mIsSpdifEnabledReq(false),
    mSpareSample(0), mHaveSpareSample(false),
    mState(AUDIO_STREAM_IDLE), /*mSrc*/ mLocked(false), mDriverRate(AUDIO_HW_OUT_SAMPLERATE),
    mInit(false)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    ALOGV("AudioStreamOutTegra constructor");
}

//PK
// designed to be called multiple times for retries
status_t AudioHardware::AudioStreamOutTegra::init()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    if (mInit) {
        return NO_ERROR;
    }

#define OPEN_FD(fd, dev)    fd = ::open(dev, O_RDWR);                              \
                            if (fd < 0) {                                          \
                                ALOGE("open " dev " failed: %s", strerror(errno));   \
                                goto error;                                         \
                            }
    OPEN_FD(mFd, "/dev/audio0_out")
    OPEN_FD(mFdCtl, "/dev/audio0_out_ctl")
    OPEN_FD(mBtFd, "/dev/audio1_out")
    OPEN_FD(mBtFdCtl, "/dev/audio1_out_ctl")
    OPEN_FD(mBtFdIoCtl, "/dev/audio1_ctl")
    // may need to be changed to warnings
    OPEN_FD(mSpdifFd, "/dev/spdif_out")
    OPEN_FD(mSpdifFdCtl, "/dev/spdif_out_ctl")
#undef OPEN_FD

    setNumBufs(AUDIO_HW_NUM_OUT_BUF_LONG);

    mInit = true;
    return NO_ERROR;

error:
#define CLOSE_FD(fd)    if (fd >= 0) {          \
                            (void) ::close(fd); \
                            fd = -1;            \
                        }
    CLOSE_FD(mFd)
    CLOSE_FD(mFdCtl)
    CLOSE_FD(mBtFd)
    CLOSE_FD(mBtFdCtl)
    CLOSE_FD(mBtFdIoCtl)
    CLOSE_FD(mSpdifFd)
    CLOSE_FD(mSpdifFdCtl)
#undef CLOSE_FD
    return NO_INIT;
}

//NOT NEEDED
status_t AudioHardware::AudioStreamOutTegra::initCheck()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    return mInit ? NO_ERROR : NO_INIT;
}

//OK
// Called with mHardware->mLock and mLock held.
void AudioHardware::AudioStreamOutTegra::setDriver_l(
        bool speaker, bool bluetooth, bool spdif, int sampleRate)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    ALOGV("Out setDriver_l() Analog speaker? %s. Bluetooth? %s. S/PDIF? %s. sampleRate %d",
        speaker?"yes":"no", bluetooth?"yes":"no", spdif?"yes":"no", sampleRate);

    // force some reconfiguration at next write()
    if (mState == AUDIO_STREAM_CONFIGURED) {
        if (mIsSpkrEnabled != speaker || mIsBtEnabled != bluetooth || mIsSpdifEnabled != spdif) {
            mState = AUDIO_STREAM_CONFIG_REQ;
        } else if (sampleRate != mDriverRate) {
            mState = AUDIO_STREAM_NEW_RATE_REQ;
        }
    }

    mIsSpkrEnabledReq = speaker;
    mIsBtEnabledReq = bluetooth;
    mIsSpdifEnabledReq = spdif;

}

status_t AudioHardware::AudioStreamOutTegra::set(
        AudioHardware* hw, uint32_t devices, int *pFormat, uint32_t *pChannels, uint32_t *pRate)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    int lFormat = pFormat ? *pFormat : 0;
    uint32_t lChannels = pChannels ? *pChannels : 0;
    uint32_t lRate = pRate ? *pRate : 0;

    mHardware = hw;

    // fix up defaults
    if (lFormat == 0) lFormat = format();
    if (lChannels == 0) lChannels = channels();
    if (lRate == 0) lRate = sampleRate();

    // check values
    if ((lFormat != format()) ||
        (lChannels != channels()) ||
        (lRate != sampleRate())) {
        if (pFormat) *pFormat = format();
        if (pChannels) *pChannels = channels();
        if (pRate) *pRate = sampleRate();
        return BAD_VALUE;
    }

    if (pFormat) *pFormat = lFormat;
    if (pChannels) *pChannels = lChannels;
    if (pRate) *pRate = lRate;

    mDevices = devices;
    if (mFd >= 0 && mFdCtl >= 0 &&
                mBtFd >= 0 &&
                mBtFdCtl >= 0 &&
                mBtFdIoCtl >= 0) {
        if (mSpdifFd < 0 || mSpdifFdCtl < 0)
            ALOGW("s/pdif driver not present");
        return NO_ERROR;
    } else {
        ALOGE("Problem opening device files - Is your kernel compatible?");
        return NO_INIT;
    }
}

//OK
AudioHardware::AudioStreamOutTegra::~AudioStreamOutTegra()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    standby();
    // Prevent someone from flushing the fd during a close.
    Mutex::Autolock lock(mFdLock);
    if (mFd >= 0)         { ::close(mFd);         mFd = -1;         }
    if (mFdCtl >= 0)      { ::close(mFdCtl);      mFdCtl = -1;      }
    if (mBtFd >= 0)       { ::close(mBtFd);       mBtFd = -1;       }
    if (mBtFdCtl >= 0)    { ::close(mBtFdCtl);    mBtFdCtl = -1;    }
    if (mBtFdIoCtl >= 0)  { ::close(mBtFdIoCtl);  mBtFdIoCtl = -1;  }
    if (mSpdifFd >= 0)    { ::close(mSpdifFd);    mSpdifFd = -1;    }
    if (mSpdifFdCtl >= 0) { ::close(mSpdifFdCtl); mSpdifFdCtl = -1; }
}

//OK
ssize_t AudioHardware::AudioStreamOutTegra::write(const void* buffer, size_t bytes)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    status_t status;
    if (!mHardware) {
        ALOGE("%s: mHardware is null", __FUNCTION__);
        return NO_INIT;
    }
    // ALOGD("AudioStreamOutTegra::write(%p, %u) TID %d", buffer, bytes, gettid());
    // Protect output state during the write process.

    if (mSleepReq) {
        // sleep a few milliseconds so that the processor can be given to the thread attempting to
        // lock mLock before we sleep with mLock held while writing below
        usleep(FORCED_SLEEP_TIME_US);
    }

    bool needsOnline = false;
    if (mState < AUDIO_STREAM_CONFIGURED) {
        mHardware->mLock.lock();
        if (mState < AUDIO_STREAM_CONFIGURED) {
            needsOnline = true;
        } else {
            mHardware->mLock.unlock();
        }
    }

    { // scope for the lock
        Mutex::Autolock lock(mLock);

        ssize_t written = 0;
        const uint8_t* p = static_cast<const uint8_t*>(buffer);
        size_t outsize = bytes;
        int outFd = mFd;
        bool stereo;
        ssize_t writtenToSpdif = 0;

        if (needsOnline) {
            status = online_l();
            mHardware->mLock.unlock();
            if (status < 0) {
                goto error;
            }
        }
        stereo = mIsBtEnabled ? false : (channels() == AudioSystem::CHANNEL_OUT_STEREO);


        if (mIsSpkrEnabled && mIsBtEnabled) {
            // When dual routing to CPCAP and Bluetooth, piggyback CPCAP audio now,
            // and then down convert for the BT.
            // CPCAP is always 44.1 in this case.
            // This also works in the three-way routing case.
            Mutex::Autolock lock2(mFdLock);
            ::write(outFd, buffer, outsize);
        }
        if (mIsSpdifEnabled) {
            // When dual routing to Speaker and HDMI, piggyback HDMI now, since it
            // has no mic we'll leave the rest of the acoustic processing for the
            // CPCAP hardware path.
            // This also works in the three-way routing case, except the acoustic
            // tuning will be done on Bluetooth, since it has the exclusive mic amd
            // it also needs the sample rate conversion
            Mutex::Autolock lock2(mFdLock);
            if (mSpdifFd >= 0) {
                writtenToSpdif = ::write(mSpdifFd, buffer, outsize);
                ALOGV("%s: written %d bytes to SPDIF", __FUNCTION__, (int)writtenToSpdif);
            } else {
                ALOGW("s/pdif enabled but unavailable");
            }
        }
        if (mIsBtEnabled) {
            outFd = mBtFd;
        } else if (mIsSpdifEnabled && !mIsSpkrEnabled) {
            outFd = -1;
        }


        if (written != (ssize_t)outsize) {
            // The sample rate conversion modifies the output size.
            if (outsize&0x3) {
                int16_t* bufp = (int16_t *)buffer;
//                ALOGV("Keep the spare sample away from the driver.");
                mHaveSpareSample = true;
                mSpareSample = bufp[outsize/2 - 1];
            }

            if (outFd >= 0) {
                Mutex::Autolock lock2(mFdLock);
                written = ::write(outFd, buffer, outsize&(~0x3));
                if (written != ((ssize_t)outsize&(~0x3))) {
                    status = written;
                    goto error;
                }
            } else {
                written = writtenToSpdif;
            }
        }
        if (written < 0) {
            ALOGE("Error writing %d bytes to output: %s", outsize, strerror(errno));
            status = written;
            goto error;
        }

        // Sample rate converter may be stashing a couple of bytes here or there,
        // so just report that all bytes were consumed. (it would be a bug not to.)
        ALOGV("write() written %d", bytes);
        return bytes;

    }
error:
    ALOGE("write(): error, return %d", status);
    standby();
    usleep(bytes * 1000 / frameSize() / sampleRate() * 1000);

    return status;
}

//OK
void AudioHardware::AudioStreamOutTegra::flush()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    // Prevent someone from writing the fd while we flush
    Mutex::Autolock lock(mFdLock);
    flush_l();
}

//OK
void AudioHardware::AudioStreamOutTegra::flush_l()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    ALOGV("AudioStreamOutTegra::flush()");
    if (::ioctl(mFdCtl, TEGRA_AUDIO_OUT_FLUSH) < 0)
       ALOGE("could not flush playback: %s", strerror(errno));
    if (::ioctl(mBtFdCtl, TEGRA_AUDIO_OUT_FLUSH) < 0)
       ALOGE("could not flush bluetooth: %s", strerror(errno));
    if (mSpdifFdCtl >= 0 && ::ioctl(mSpdifFdCtl, TEGRA_AUDIO_OUT_FLUSH) < 0)
       ALOGE("could not flush spdif: %s", strerror(errno));
    ALOGV("AudioStreamOutTegra::flush() returns");
}

//OK
// FIXME: this is a workaround for issue 3387419 with impact on latency
// to be removed when root cause is fixed
void AudioHardware::AudioStreamOutTegra::setNumBufs(int numBufs)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    Mutex::Autolock lock(mFdLock);
    ALOGV("AudioStreamOutTegra::setNumBufs(%d)", numBufs);
    if (::ioctl(mFdCtl, TEGRA_AUDIO_OUT_SET_NUM_BUFS, &numBufs) < 0)
       ALOGE("could not set number of output buffers: %s", strerror(errno));
}

// Called with mLock and mHardware->mLock held
status_t AudioHardware::AudioStreamOutTegra::online_l()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    status_t status = NO_ERROR;

    if (mState < AUDIO_STREAM_NEW_RATE_REQ) {
        if (mState == AUDIO_STREAM_IDLE) {
            ALOGV("output %p going online", this);
            mState = AUDIO_STREAM_CONFIG_REQ;
            // update EC state if necessary
            if (mHardware->getActiveInput_l() && mHardware->isEcRequested()) {
                // doRouting_l() will not try to lock mLock when calling setDriver_l()
                mLocked = true;
                mHardware->doRouting_l();
                mLocked = false;
            }
        }

        // If there's no hardware speaker, leave the HW alone. (i.e. SCO/SPDIF is on)
        if (mIsSpkrEnabledReq) {
            status = mHardware->doStandby(mFdCtl, true, false); // output, online
        } else {
            status = mHardware->doStandby(mFdCtl, true, true); // output, standby
        }
        mIsSpkrEnabled = mIsSpkrEnabledReq;

        mIsBtEnabled = mIsBtEnabledReq;
        mIsSpdifEnabled = mIsSpdifEnabledReq;

    }

    // Flush old data (wrong rate) from I2S driver before changing rate.
    flush();
    if (mHardware->mEcnsEnabled) {
        setNumBufs(AUDIO_HW_NUM_OUT_BUF);
    } else {
        setNumBufs(AUDIO_HW_NUM_OUT_BUF_LONG);
    }
    int speaker_rate = mHardware->mHwOutRate;
    if (mIsBtEnabled) {
        speaker_rate = AUDIO_HW_OUT_SAMPLERATE;
    }
    // Now the DMA is empty, change the rate.
    if (::ioctl(mHardware->mCpcapCtlFd, CPCAP_AUDIO_OUT_SET_RATE,
              speaker_rate) < 0)
        ALOGE("could not set output rate(%d): %s",
              speaker_rate, strerror(errno));

    mDriverRate = mHardware->mHwOutRate;

    // If EC is on, pre load one DMA buffer with 20ms of silence to limit underruns
    if (mHardware->mEcnsEnabled) {
        int fd = -1;
        if (mIsBtEnabled) {
            fd = mBtFd;
        } else if (mIsSpkrEnabled) {
            fd = mFd;
        }
        if (fd >= 0) {
            size_t bufSize = (mDriverRate * 2 /* stereo */ * sizeof(int16_t))/ 50;
            char buf[bufSize];
            memset(buf, 0, bufSize);
            Mutex::Autolock lock2(mFdLock);
            ::write(fd, buf, bufSize);
        }
    }

    mState = AUDIO_STREAM_CONFIGURED;

    return status;
}

status_t AudioHardware::AudioStreamOutTegra::standby()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    if (!mHardware) {
        return NO_INIT;
    }

    status_t status = NO_ERROR;
    Mutex::Autolock lock(mHardware->mLock);
    Mutex::Autolock lock2(mLock);

    if (mState != AUDIO_STREAM_IDLE) {
        ALOGV("output %p going into standby", this);
        mState = AUDIO_STREAM_IDLE;

        // update EC state if necessary
        if (mHardware->getActiveInput_l() && mHardware->isEcRequested()) {
            // doRouting_l will not try to lock mLock when calling setDriver_l()
            mLocked = true;
            mHardware->doRouting_l();
            mLocked = false;
        }

        if (mIsSpkrEnabled) {
            // doStandby() calls flush() which also handles the case where multiple devices
            // including bluetooth or SPDIF are selected
            status = mHardware->doStandby(mFdCtl, true, true); // output, standby
        } else if (mIsBtEnabled || mIsSpdifEnabled) {
            flush();
        }
    }

    return status;
}

status_t AudioHardware::AudioStreamOutTegra::dump(int fd, const Vector<String16>& args)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    result.append("AudioStreamOutTegra::dump\n");
    snprintf(buffer, SIZE, "\tsample rate: %d\n", sampleRate());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tbuffer size: %d\n", bufferSize());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tchannels: %d\n", channels());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tformat: %d\n", format());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmHardware: %p\n", mHardware);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmFd: %d\n", mFd);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmStartCount: %d\n", mStartCount);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmRetryCount: %d\n", mRetryCount);
    result.append(buffer);
    if (mHardware)
        snprintf(buffer, SIZE, "\tmStandby: %s\n",
                 mHardware->mCurOutDevice.on ? "false": "true");
    else
        snprintf(buffer, SIZE, "\tmStandby: unknown\n");

    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return NO_ERROR;
}

//OK
bool AudioHardware::AudioStreamOutTegra::getStandby()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    return mState == AUDIO_STREAM_IDLE;;
}

//OK
status_t AudioHardware::AudioStreamOutTegra::setParameters(const String8& keyValuePairs)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key = String8(AudioParameter::keyRouting);
    status_t status = NO_ERROR;
    int device;
    ALOGV("AudioStreamOutTegra::setParameters() %s", keyValuePairs.string());

    if (param.getInt(key, device) == NO_ERROR) {
        if (device != 0) {
            mDevices = device;
            ALOGV("set output routing %x", mDevices);
            status = mHardware->doRouting();
        }
        param.remove(key);
    }

    if (param.size()) {
        status = BAD_VALUE;
    }
    return status;
}

//NOT NEEDED
String8 AudioHardware::AudioStreamOutTegra::getParameters(const String8& keys)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    AudioParameter param = AudioParameter(keys);
    String8 value;
    String8 key = String8(AudioParameter::keyRouting);

    if (param.get(key, value) == NO_ERROR) {
        ALOGV("get routing %x", mDevices);
        param.addInt(key, (int)mDevices);
    }

    ALOGV("AudioStreamOutTegra::getParameters() %s", param.toString().string());
    return param.toString();
}

//NOT NEEDED
status_t AudioHardware::AudioStreamOutTegra::getRenderPosition(uint32_t *dspFrames)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    //TODO: enable when supported by driver
    return -ENODEV;
}

#if 1
//NOT NEEDED
status_t AudioHardware::AudioStreamOutTegra::getPresentationPosition(uint64_t *frames, struct timespec *timestamp)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    //TODO: enable when supported by driver
    return -ENODEV;
}
#endif

//NOT NEEDED
// default implementation is unsupported
status_t AudioHardware::AudioStreamOutTegra::getNextWriteTimestamp(int64_t *timestamp)
{
    return INVALID_OPERATION;
}

// ----------------------------------------------------------------------------

// always succeeds, must call set() immediately after
AudioHardware::AudioStreamInTegra::AudioStreamInTegra() :
    mHardware(0), mFd(-1), mFdCtl(-1), mState(AUDIO_STREAM_IDLE), mRetryCount(0),
    mFormat(AUDIO_HW_IN_FORMAT), mChannels(AUDIO_HW_IN_CHANNELS),
    mSampleRate(AUDIO_HW_IN_SAMPLERATE), mBufferSize(AUDIO_HW_IN_BUFFERSIZE),
    mAcoustics((AudioSystem::audio_in_acoustics)0), mDevices(0),
    mIsMicEnabled(0), mIsBtEnabled(0),
    mSource(AUDIO_SOURCE_DEFAULT), mLocked(false), mTotalBuffersRead(0),
    mDriverRate(AUDIO_HW_IN_SAMPLERATE), mEcnsRequested(0)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    ALOGV("AudioStreamInTegra constructor");
}

//OK
// serves a similar purpose as init()
status_t AudioHardware::AudioStreamInTegra::set(
        AudioHardware* hw, uint32_t devices, int *pFormat, uint32_t *pChannels, uint32_t *pRate,
        AudioSystem::audio_in_acoustics acoustic_flags)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    Mutex::Autolock lock(mLock);
    status_t status = BAD_VALUE;
    mHardware = hw;
    if (pFormat == 0)
        return status;
    if (*pFormat != AUDIO_HW_IN_FORMAT) {
        ALOGE("wrong in format %d, expecting %lld", *pFormat, AUDIO_HW_IN_FORMAT);
        *pFormat = AUDIO_HW_IN_FORMAT;
        return status;
    }

    if (pRate == 0)
        return status;

    uint32_t rate = hw->getInputSampleRate(*pRate);
    if (rate != *pRate) {
        ALOGE("wrong sample rate %d, expecting %d", *pRate, rate);
        *pRate = rate;
        return status;
    }

    if (pChannels == 0)
        return status;

    if (*pChannels != AudioSystem::CHANNEL_IN_MONO &&
        *pChannels != AudioSystem::CHANNEL_IN_STEREO) {
        ALOGE("wrong number of channels %d", *pChannels);
        *pChannels = AUDIO_HW_IN_CHANNELS;
        return status;
    }

    ALOGV("AudioStreamInTegra::set(%d, %d, %u)", *pFormat, *pChannels, *pRate);

    mDevices = devices;
    mFormat = AUDIO_HW_IN_FORMAT;
    mChannels = *pChannels;
    mSampleRate = *pRate;
    mBufferSize = mHardware->getInputBufferSize(mSampleRate, AudioSystem::PCM_16_BIT,
                                                AudioSystem::popCount(mChannels));
    return NO_ERROR;
}

AudioHardware::AudioStreamInTegra::~AudioStreamInTegra()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    ALOGV("AudioStreamInTegra destructor");

    standby();

}

// Called with mHardware->mLock and mLock held.
void AudioHardware::AudioStreamInTegra::setDriver_l(bool mic, bool bluetooth, int sampleRate)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    ALOGV("In setDriver_l() Analog mic? %s. Bluetooth? %s.", mic?"yes":"no", bluetooth?"yes":"no");

    // force some reconfiguration at next read()
    // Note: mState always == AUDIO_STREAM_CONFIGURED when setDriver_l() is called on an input
    if (mic != mIsMicEnabled || bluetooth != mIsBtEnabled) {
        mState = AUDIO_STREAM_CONFIG_REQ;
    } else if (sampleRate != mDriverRate) {
        mState = AUDIO_STREAM_NEW_RATE_REQ;
    }

    mIsMicEnabled = mic;
    mIsBtEnabled = bluetooth;

}

ssize_t AudioHardware::AudioStreamInTegra::read(void* buffer, ssize_t bytes)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    status_t status;
    if (!mHardware) {
        ALOGE("%s: mHardware is null", __FUNCTION__);
        return NO_INIT;
    }
    //
    ALOGV("AudioStreamInTegra::read(%p, %ld) TID %d", buffer, bytes, gettid());

    if (mSleepReq) {
        // sleep a few milliseconds so that the processor can be given to the thread attempting to
        // lock mLock before we sleep with mLock held while reading below
        usleep(FORCED_SLEEP_TIME_US);
    }

    bool needsOnline = false;
    if (mState < AUDIO_STREAM_CONFIGURED) {
        mHardware->mLock.lock();
        if (mState < AUDIO_STREAM_CONFIGURED) {
            needsOnline = true;
        } else {
            mHardware->mLock.unlock();
        }
    }

    {   // scope for mLock
        Mutex::Autolock lock(mLock);

        ssize_t ret;
        bool srcReqd;
        int  hwReadBytes;
        int16_t * inbuf;

        if (needsOnline) {
            status = online_l();
            mHardware->mLock.unlock();
            if (status != NO_ERROR) {
               ALOGE("%s: Problem switching to online.",__FUNCTION__);
               goto error;
            }
        }

        srcReqd = (mDriverRate != (int)mSampleRate);


        if (srcReqd) {
            ALOGE("%s: sample rate mismatch HAL %d, driver %d",
                 __FUNCTION__, mSampleRate, mDriverRate);
            status = INVALID_OPERATION;
            goto error;
        }
        {
            Mutex::Autolock dfl(mFdLock);
            ret = ::read(mFd, buffer, bytes);
        }

        // It is not optimal to mute after all the above processing but it is necessary to
        // keep the clock sync from input device. It also avoids glitches on output streams due
        // to EC being turned on and off
        bool muted;
        mHardware->getMicMute(&muted);
        if (muted) {
            ALOGV("%s muted",__FUNCTION__);
            memset(buffer, 0, bytes);
        }

        ALOGV("%s returns %d.",__FUNCTION__, (int)ret);
        if (ret < 0) {
            status = ret;
            goto error;
        }

        {
            Mutex::Autolock _fl(mFramesLock);
            mTotalBuffersRead++;
        }
        return ret;
    }

error:
    ALOGE("read(): error, return %d", status);
    standby();
    usleep(bytes * 1000 / frameSize() / sampleRate() * 1000);
    return status;
}

bool AudioHardware::AudioStreamInTegra::getStandby() const
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    return mState == AUDIO_STREAM_IDLE;
}

status_t AudioHardware::AudioStreamInTegra::standby()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    if (!mHardware) {
        return NO_INIT;
    }

    Mutex::Autolock lock(mHardware->mLock);
    Mutex::Autolock lock2(mLock);
    status_t status = NO_ERROR;
    if (mState != AUDIO_STREAM_IDLE) {
        ALOGV("input %p going into standby", this);
        mState = AUDIO_STREAM_IDLE;
        // stopping capture now so that the input stream state (AUDIO_STREAM_IDLE)
        // is consistent with the driver state when doRouting_l() is executed.
        // Not doing so makes that I2S reconfiguration fails  when switching from
        // BT SCO to built-in mic.
        stop_l();
        // reset global pre processing state before disabling the input
        mHardware->setEcnsRequested_l(PREPROC_AEC|PREPROC_NS, false);
        // setDriver_l() will not try to lock mLock when called by doRouting_l()
        mLocked = true;
        mHardware->doRouting_l();
        mLocked = false;
        status = mHardware->doStandby(mFdCtl, false, true); // input, standby
        if (mFd >= 0) {
            ::close(mFd);
            mFd = -1;
        }
        if (mFdCtl >= 0) {
            ::close(mFdCtl);
            mFdCtl = -1;
        }
    }

    return status;
}

// Called with mLock and mHardware->mLock held
status_t AudioHardware::AudioStreamInTegra::online_l()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    status_t status = NO_ERROR;

    reopenReconfigDriver();

    if (mState < AUDIO_STREAM_NEW_RATE_REQ) {

        // Use standby to flush the driver.  mHardware->mLock should already be held

        mHardware->doStandby(mFdCtl, false, true);
        if (mDevices & ~AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
            status = mHardware->doStandby(mFdCtl, false, false);
        }

        if (mState == AUDIO_STREAM_IDLE) {
            mState = AUDIO_STREAM_CONFIG_REQ;
            ALOGV("input %p going online", this);
            // apply pre processing requested for this input
            mHardware->setEcnsRequested_l(mEcnsRequested, true);
            // setDriver_l() will not try to lock mLock when called by doRouting_l()
            mLocked = true;
            mHardware->doRouting_l();
            mLocked = false;
            {
                Mutex::Autolock _fl(mFramesLock);
                mTotalBuffersRead = 0;
                mStartTimeNs = systemTime();
            }
        }

        // configuration
        struct tegra_audio_in_config config;
        status = ::ioctl(mFdCtl, TEGRA_AUDIO_IN_GET_CONFIG, &config);
        if (status < 0) {
            ALOGE("cannot read input config: %s", strerror(errno));
            return status;
        }
        config.stereo = AudioSystem::popCount(mChannels) == 2;
        config.rate = mHardware->mHwInRate;
        status = ::ioctl(mFdCtl, TEGRA_AUDIO_IN_SET_CONFIG, &config);

        if (status < 0) {
            ALOGE("cannot set input config: %s", strerror(errno));
            if (::ioctl(mFdCtl, TEGRA_AUDIO_IN_GET_CONFIG, &config) == 0) {
                if (config.stereo) {
                    mChannels = AudioSystem::CHANNEL_IN_STEREO;
                } else {
                    mChannels = AudioSystem::CHANNEL_IN_MONO;
                }
            }
        }


    }

    mDriverRate = mHardware->mHwInRate;

    if (::ioctl(mHardware->mCpcapCtlFd, CPCAP_AUDIO_IN_SET_RATE,
                mDriverRate) < 0)
        ALOGE("could not set input rate(%d): %s", mDriverRate, strerror(errno));

    mState = AUDIO_STREAM_CONFIGURED;

    return status;
}

//ok
// serves a similar purpose as the init() method of other classes
void AudioHardware::AudioStreamInTegra::reopenReconfigDriver()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    // Need to "restart" the driver when changing the buffer configuration.
    if (mFdCtl >= 0 && ::ioctl(mFdCtl, TEGRA_AUDIO_IN_STOP) < 0) {
        ALOGE("%s: could not stop recording: %s", __FUNCTION__, strerror(errno));
    }
    if (mFd >= 0) {
        ::close(mFd);
        mFd = -1;
    }
    if (mFdCtl >= 0) {
        ::close(mFdCtl);
        mFdCtl = -1;
    }

    // This does not have a retry loop to avoid blocking if another record session already in progress
    mFd = ::open("/dev/audio1_in", O_RDWR);
    if (mFd < 0) {
        ALOGE("open /dev/audio1_in failed: %s", strerror(errno));
    }
    mFdCtl = ::open("/dev/audio1_in_ctl", O_RDWR);
    if (mFdCtl < 0) {
        ALOGE("open /dev/audio1_in_ctl failed: %s", strerror(errno));
        if (mFd >= 0) {
            ::close(mFd);
            mFd = -1;
        }
    } else {
        // here we would set mInit = true;
    }
}


status_t AudioHardware::AudioStreamInTegra::dump(int fd, const Vector<String16>& args)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    result.append("AudioStreamInTegra::dump\n");
    snprintf(buffer, SIZE, "\tsample rate: %d\n", sampleRate());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tbuffer size: %d\n", bufferSize());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tchannels: %d\n", channels());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tformat: %d\n", format());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmHardware: %p\n", mHardware);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmFd count: %d\n", mFd);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmState: %d\n", mState);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmRetryCount: %d\n", mRetryCount);
    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return NO_ERROR;
}

status_t AudioHardware::AudioStreamInTegra::setParameters(const String8& keyValuePairs)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key = String8(AudioParameter::keyRouting);
    status_t status = NO_ERROR;
    int device;
    int source;
    ALOGV("AudioStreamInTegra::setParameters() %s", keyValuePairs.string());

    // read source before device so that it is upto date when doRouting() is called
    if (param.getInt(String8(AudioParameter::keyInputSource), source) == NO_ERROR) {
        mSource = source;
        param.remove(String8(AudioParameter::keyInputSource));
    }

    if (param.getInt(key, device) == NO_ERROR) {
        ALOGV("set input routing %x", device);
        if (device & (device - 1)) {
            status = BAD_VALUE;
        } else {
            mDevices = device;
            if (!getStandby() && device != 0) {
                status = mHardware->doRouting();
            }
        }
        param.remove(key);
    }

    if (param.size()) {
        status = BAD_VALUE;
    }
    return status;
}

String8 AudioHardware::AudioStreamInTegra::getParameters(const String8& keys)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    AudioParameter param = AudioParameter(keys);
    String8 value;
    String8 key = String8(AudioParameter::keyRouting);

    if (param.get(key, value) == NO_ERROR) {
        ALOGV("get routing %x", mDevices);
        param.addInt(key, (int)mDevices);
    }

    ALOGV("AudioStreamInTegra::getParameters() %s", param.toString().string());
    return param.toString();
}

unsigned int  AudioHardware::AudioStreamInTegra::getInputFramesLost() const
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    Mutex::Autolock _l(mFramesLock);
    unsigned int lostFrames = 0;
    if (!getStandby()) {
        unsigned int framesPerBuffer = bufferSize() / frameSize();
        uint64_t expectedFrames = ((systemTime() - mStartTimeNs) * mSampleRate) / 1000000000;
        expectedFrames = (expectedFrames / framesPerBuffer) * framesPerBuffer;
        uint64_t actualFrames = (uint64_t)mTotalBuffersRead * framesPerBuffer;
        if (expectedFrames > actualFrames) {
            lostFrames = (unsigned int)(expectedFrames - actualFrames);
            ALOGW("getInputFramesLost() expected %d actual %d lost %d",
                 (unsigned int)expectedFrames, (unsigned int)actualFrames, lostFrames);
        }
    }

    mTotalBuffersRead = 0;
    mStartTimeNs = systemTime();

    return lostFrames;
}

// must be called with mLock and mFdLock held
void AudioHardware::AudioStreamInTegra::stop_l()
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    ALOGV("AudioStreamInTegra::stop_l() starts");
    if (::ioctl(mFdCtl, TEGRA_AUDIO_IN_STOP) < 0) {
        ALOGE("could not stop recording: %d %s", errno, strerror(errno));
    }
    ALOGV("AudioStreamInTegra::stop_l() returns");
}

//ok
void AudioHardware::AudioStreamInTegra::updateEcnsRequested(effect_handle_t effect, bool enabled)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
    effect_descriptor_t desc;
    status_t status = (*effect)->get_descriptor(effect, &desc);
    if (status == 0) {
        int ecns = 0;
        if (memcmp(&desc.type, FX_IID_AEC, sizeof(effect_uuid_t)) == 0) {
            ecns = PREPROC_AEC;
        } else if (memcmp(&desc.type, FX_IID_NS, sizeof(effect_uuid_t)) == 0) {
            ecns = PREPROC_NS;
        }
        ALOGV("AudioStreamInTegra::updateEcnsRequested() %s effect %s",
             enabled ? "enabling" : "disabling", desc.name);
        if (enabled) {
            mEcnsRequested |= ecns;
        } else {
            mEcnsRequested &= ~ecns;
        }
        standby();
    }
#endif
}

//ok
status_t AudioHardware::AudioStreamInTegra::addAudioEffect(effect_handle_t effect)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    updateEcnsRequested(effect, true);
    return NO_ERROR;
}

//ok
status_t AudioHardware::AudioStreamInTegra::removeAudioEffect(effect_handle_t effect)
{
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    updateEcnsRequested(effect, false);
    return NO_ERROR;
}

// ----------------------------------------------------------------------------

extern "C" AudioHardwareInterface* createAudioHardware(void) {
    //ALOGE("%s[%u]\n", __func__, __LINE__);
    AudioHardware *hw = new AudioHardware();
    for (unsigned tries = 0; tries < MAX_INIT_TRIES; ++tries) {
        if (NO_ERROR == hw->init())
            break;
        ALOGW("AudioHardware::init failed soft, retrying");
        sleep(1);
    }
    if (NO_ERROR != hw->initCheck()) {
        ALOGE("AudioHardware::init failed hard");
        delete hw;
        hw = NULL;
    }
    return hw;
}

}; // namespace android
