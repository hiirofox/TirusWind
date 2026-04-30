#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#else
#include <alsa/asoundlib.h>
#endif

class WaveIO_I2S
{
#if defined(_WIN32)
private:
	static constexpr size_t kBufferCount = 3;

	HWAVEOUT waveOut = nullptr;
	HANDLE bufferEvent = nullptr;
	std::vector<std::vector<int16_t>> buffers;
	std::vector<WAVEHDR> headers;
	uint32_t periodSizeFrames = 0;
	size_t nextBuffer = 0;

public:
	explicit WaveIO_I2S(unsigned int sampleRate)
	{
		periodSizeFrames = 1024;
		buffers.resize(kBufferCount);
		headers.resize(kBufferCount);

		bufferEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
		if(!bufferEvent)
		{
			std::cerr << "Failed to create WinMM audio event.\n";
			return;
		}

		WAVEFORMATEX format{};
		format.wFormatTag = WAVE_FORMAT_PCM;
		format.nChannels = 2;
		format.nSamplesPerSec = sampleRate;
		format.wBitsPerSample = 16;
		format.nBlockAlign = static_cast<WORD>((format.nChannels * format.wBitsPerSample) / 8);
		format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

		const MMRESULT result = waveOutOpen(
			&waveOut,
			WAVE_MAPPER,
			&format,
			reinterpret_cast<DWORD_PTR>(bufferEvent),
			0,
			CALLBACK_EVENT);

		if(result != MMSYSERR_NOERROR)
		{
			std::cerr << "Failed to open WinMM wave output.\n";
			waveOut = nullptr;
			return;
		}

		for(size_t i = 0; i < kBufferCount; ++i)
		{
			buffers[i].resize(static_cast<size_t>(periodSizeFrames) * 2);
			std::memset(&headers[i], 0, sizeof(WAVEHDR));
			headers[i].lpData = reinterpret_cast<LPSTR>(buffers[i].data());
			headers[i].dwBufferLength = static_cast<DWORD>(buffers[i].size() * sizeof(int16_t));
			headers[i].dwFlags = WHDR_DONE;
		}

		std::cout << "[WaveIO] WinMM output started. Period size: "
			<< periodSizeFrames << " frames\n";
	}

	uint32_t GetPeriodSizeInFrames() const
	{
		return periodSizeFrames;
	}

	int PlayAudio(const float* outl, const float* outr, int numSamples)
	{
		if(!waveOut)
			return -1;
		if(numSamples <= 0)
			return 0;

		auto& header = headers[nextBuffer];
		while((header.dwFlags & WHDR_DONE) == 0)
			WaitForSingleObject(bufferEvent, INFINITE);

		if(header.dwFlags & WHDR_PREPARED)
			waveOutUnprepareHeader(waveOut, &header, sizeof(WAVEHDR));

		auto& buffer = buffers[nextBuffer];
		const size_t neededSamples = static_cast<size_t>(numSamples) * 2;
		if(buffer.size() < neededSamples)
			buffer.resize(neededSamples);

		for(int i = 0; i < numSamples; ++i)
		{
			const float sampleL = std::clamp(outl[i] * 32767.0f, -32768.0f, 32767.0f);
			const float sampleR = std::clamp(outr[i] * 32767.0f, -32768.0f, 32767.0f);
			buffer[static_cast<size_t>(i) * 2 + 0] = static_cast<int16_t>(sampleL);
			buffer[static_cast<size_t>(i) * 2 + 1] = static_cast<int16_t>(sampleR);
		}

		header.lpData = reinterpret_cast<LPSTR>(buffer.data());
		header.dwBufferLength = static_cast<DWORD>(neededSamples * sizeof(int16_t));
		header.dwFlags = 0;
		header.dwLoops = 0;

		if(waveOutPrepareHeader(waveOut, &header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
			return -1;

		if(waveOutWrite(waveOut, &header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
			return -1;

		nextBuffer = (nextBuffer + 1) % kBufferCount;
		return 0;
	}

	void Stop()
	{
		if(waveOut)
		{
			waveOutReset(waveOut);
			for(auto& header : headers)
			{
				if(header.dwFlags & WHDR_PREPARED)
					waveOutUnprepareHeader(waveOut, &header, sizeof(WAVEHDR));
			}
			waveOutClose(waveOut);
			waveOut = nullptr;
		}

		if(bufferEvent)
		{
			CloseHandle(bufferEvent);
			bufferEvent = nullptr;
		}
	}

	~WaveIO_I2S()
	{
		Stop();
	}

#else
private:
	snd_pcm_t* pcmHandle = nullptr;
	snd_pcm_hw_params_t* params = nullptr;
	std::vector<int16_t> interleaved;
	snd_pcm_uframes_t periodSizeFrames = 0;

public:
	explicit WaveIO_I2S(unsigned int sampleRate)
	{
		int err;
		const char* device = "default";
		int dir = 0;
		unsigned int targetPeriodTime = 10000;
		unsigned int targetBufferTime = 32000;
		snd_pcm_uframes_t bufferSizeFrames = 0;

		err = snd_pcm_open(&pcmHandle, device, SND_PCM_STREAM_PLAYBACK, 0);
		if(err < 0)
		{
			std::cerr << "Failed to open ALSA PCM device (" << device << "): "
				<< snd_strerror(err) << '\n';
			pcmHandle = nullptr;
			return;
		}

		snd_pcm_hw_params_alloca(&params);
		snd_pcm_hw_params_any(pcmHandle, params);

		err = snd_pcm_hw_params_set_access(pcmHandle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
		if(err < 0)
			goto hw_error;

		err = snd_pcm_hw_params_set_format(pcmHandle, params, SND_PCM_FORMAT_S16_LE);
		if(err < 0)
			goto hw_error;

		err = snd_pcm_hw_params_set_channels(pcmHandle, params, 2);
		if(err < 0)
			goto hw_error;

		err = snd_pcm_hw_params_set_rate_near(pcmHandle, params, &sampleRate, 0);
		if(err < 0)
			goto hw_error;

		err = snd_pcm_hw_params_set_period_time_near(pcmHandle, params, &targetPeriodTime, &dir);
		if(err < 0)
			goto hw_error;

		err = snd_pcm_hw_params_set_buffer_time_near(pcmHandle, params, &targetBufferTime, &dir);
		if(err < 0)
			goto hw_error;

		err = snd_pcm_hw_params(pcmHandle, params);
		if(err < 0)
			goto hw_error;

		snd_pcm_hw_params_get_period_size(params, &periodSizeFrames, &dir);
		snd_pcm_hw_params_get_buffer_size(params, &bufferSizeFrames);

		std::cout << "--- ALSA low-latency config ---\n";
		std::cout << "  Period time: " << targetPeriodTime << " us\n";
		std::cout << "  Buffer time: " << targetBufferTime << " us\n";
		std::cout << "  Period size: " << periodSizeFrames << " frames\n";
		std::cout << "  Buffer size: " << bufferSizeFrames << " frames\n";
		std::cout << "-------------------------------\n";
		return;

	hw_error:
		std::cerr << "Failed to configure ALSA hardware params: "
			<< snd_strerror(err) << '\n';
		snd_pcm_close(pcmHandle);
		pcmHandle = nullptr;
	}

	snd_pcm_uframes_t GetPeriodSizeInFrames() const
	{
		return periodSizeFrames;
	}

	int PlayAudio(const float* outl, const float* outr, int numSamples)
	{
		if(!pcmHandle)
			return -1;
		if(numSamples <= 0)
			return 0;

		if(interleaved.size() < static_cast<size_t>(numSamples * 2))
			interleaved.resize(static_cast<size_t>(numSamples * 2));

		for(int i = 0; i < numSamples; ++i)
		{
			const float sampleL = std::clamp(outl[i] * 32767.0f, -32768.0f, 32767.0f);
			const float sampleR = std::clamp(outr[i] * 32767.0f, -32768.0f, 32767.0f);
			interleaved[static_cast<size_t>(i) * 2 + 0] = static_cast<int16_t>(sampleL);
			interleaved[static_cast<size_t>(i) * 2 + 1] = static_cast<int16_t>(sampleR);
		}

		const int written = snd_pcm_writei(pcmHandle, interleaved.data(), numSamples);

		if(written == -EPIPE)
		{
			snd_pcm_prepare(pcmHandle);
			return 0;
		}

		if(written < 0)
		{
			std::cerr << "Failed to write ALSA PCM: " << snd_strerror(written) << '\n';
			return -1;
		}

		return 0;
	}

	void Stop()
	{
		if(pcmHandle)
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
#endif
};

using WaveIO = WaveIO_I2S;
