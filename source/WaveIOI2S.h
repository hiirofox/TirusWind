#pragma once

#include <iostream>
#include <vector>
#include <stdint.h>
#include "alsa/asoundlib.h"

class WaveIO_I2S
{
private:
    snd_pcm_t *pcmHandle;
    snd_pcm_hw_params_t *params;
    std::vector<int16_t> interleaved;
    snd_pcm_uframes_t period_size_frames;

public:
    WaveIO_I2S(unsigned int SampleRate) : period_size_frames(0)
    {
        int err;
        const char *device = "default";
        int dir;
        unsigned int target_period_time;
        unsigned int target_buffer_time;
        snd_pcm_uframes_t buffer_size_frames;

        err = snd_pcm_open(&pcmHandle, device, SND_PCM_STREAM_PLAYBACK, 0);
        if (err < 0)
        {
            std::cerr << "无法打开 ALSA PCM 设备 (" << device << "): "
                      << snd_strerror(err) << std::endl;
            pcmHandle = nullptr;
            return;
        }

        snd_pcm_hw_params_alloca(&params);
        snd_pcm_hw_params_any(pcmHandle, params);

        err = snd_pcm_hw_params_set_access(pcmHandle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
        if (err < 0)
            goto hw_error;

        err = snd_pcm_hw_params_set_format(pcmHandle, params, SND_PCM_FORMAT_S16_LE);
        if (err < 0)
            goto hw_error;

        err = snd_pcm_hw_params_set_channels(pcmHandle, params, 2);
        if (err < 0)
            goto hw_error;

        err = snd_pcm_hw_params_set_rate_near(pcmHandle, params, &SampleRate, 0);
        if (err < 0)
            goto hw_error;

        target_period_time = 10000;
        target_buffer_time = 32000;

        err = snd_pcm_hw_params_set_period_time_near(pcmHandle, params, &target_period_time, &dir);
        if (err < 0)
            goto hw_error;

        err = snd_pcm_hw_params_set_buffer_time_near(pcmHandle, params, &target_buffer_time, &dir);
        if (err < 0)
            goto hw_error;

        err = snd_pcm_hw_params(pcmHandle, params);
        if (err < 0)
            goto hw_error;

        snd_pcm_hw_params_get_period_size(params, &period_size_frames, &dir);
        snd_pcm_hw_params_get_buffer_size(params, &buffer_size_frames);

        std::cout << "--- ALSA 低延迟配置 ---" << std::endl;
        std::cout << "  实际周期: " << target_period_time << " µs" << std::endl;
        std::cout << "  实际缓冲: " << target_buffer_time << " µs" << std::endl;
        std::cout << "  Period size: " << period_size_frames << " frames" << std::endl;
        std::cout << "  Buffer size: " << buffer_size_frames << " frames" << std::endl;
        std::cout << "------------------------" << std::endl;

        return;

    hw_error:
        std::cerr << "硬件参数设置失败: "
                  << snd_strerror(err) << std::endl;
        snd_pcm_close(pcmHandle);
        pcmHandle = nullptr;
    }

    snd_pcm_uframes_t GetPeriodSizeInFrames()
    {
        return period_size_frames;
    }

    int PlayAudio(const float *outl, const float *outr, int numSamples)
    {
        if (!pcmHandle)
            return -1;
        if (numSamples <= 0)
            return 0;

        if (interleaved.size() < (size_t)(numSamples * 2))
            interleaved.resize(numSamples * 2);

        for (int i = 0; i < numSamples; i++)
        {
            float sampleL = outl[i] * 32767.0f;
            float sampleR = outr[i] * 32767.0f;
            if (sampleL > 32767.0f)
                sampleL = 32767.0f;
            if (sampleL < -32768.0f)
                sampleL = -32768.0f;
            if (sampleR > 32767.0f)
                sampleR = 32767.0f;
            if (sampleR < -32768.0f)
                sampleR = -32768.0f;
            interleaved[i * 2 + 0] = (int16_t)sampleL;
            interleaved[i * 2 + 1] = (int16_t)sampleR;
        }

        int written = snd_pcm_writei(pcmHandle, interleaved.data(), numSamples);

        if (written == -EPIPE)
        {
            snd_pcm_prepare(pcmHandle);
            return 0;
        }
        else if (written < 0)
        {
            std::cerr << "写入 PCM 时发生错误: "
                      << snd_strerror(written) << std::endl;
            return -1;
        }

        return 0;
    }

    void Stop()
    {
        if (pcmHandle)
        {
            snd_pcm_drop(pcmHandle);
            snd_pcm_close(pcmHandle);
            pcmHandle = nullptr;
        }
    }

    ~WaveIO_I2S()
    {
        Stop();
    }
};
