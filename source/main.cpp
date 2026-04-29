#include "synthLib/audioTypes.h"
#include "synthLib/midiTypes.h"
#include "synthLib/device.h"

#include "virusLib/device.h"
#include "virusLib/microcontroller.h"
#include "virusLib/romfile.h"

#include "MidiIO.h"
#include "WaveIO.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
	constexpr uint32_t kSampleRate = 44100;
	constexpr uint32_t kFallbackBlockSize = 256;
	constexpr uint32_t kWarmupFrames = kSampleRate / 2;
	constexpr int kDefaultPresetBank = 0;
	constexpr int kDefaultPresetProgram = 10;

	std::atomic_bool g_shouldRun{true};

	void handleSignal(int)
	{
		g_shouldRun.store(false);
	}

	std::filesystem::path projectRoot()
	{
#ifdef TIRUSWIND_PROJECT_ROOT
		return TIRUSWIND_PROJECT_ROOT;
#else
		return std::filesystem::current_path();
#endif
	}

	std::vector<uint8_t> readBinaryFile(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if(!file)
			throw std::runtime_error("Failed to open " + path.string());

		const auto size = file.tellg();
		if(size <= 0)
			throw std::runtime_error("File is empty: " + path.string());

		std::vector<uint8_t> data(static_cast<size_t>(size));
		file.seekg(0);
		file.read(reinterpret_cast<char*>(data.data()), size);

		if(!file)
			throw std::runtime_error("Failed to read " + path.string());

		return data;
	}

	uint32_t clampMidi7(const int value)
	{
		return static_cast<uint32_t>(std::clamp(value, 0, 127));
	}

	bool midiEventToSynthEvent(const MidiIO_Universal::Event& ev, synthLib::SMidiEvent& out)
	{
		using namespace synthLib;

		out = SMidiEvent(MidiEventSource::Host);
		const auto type = ev.status & 0xf0;
		const auto channel = ev.status & 0x0f;

		switch(type)
		{
		case M_NOTEON:
			out.a = static_cast<uint8_t>((ev.data2 == 0 ? M_NOTEOFF : M_NOTEON) | channel);
			out.b = static_cast<uint8_t>(clampMidi7(ev.data1));
			out.c = static_cast<uint8_t>(clampMidi7(ev.data2));
			return true;

		case M_NOTEOFF:
			out.a = static_cast<uint8_t>(M_NOTEOFF | channel);
			out.b = static_cast<uint8_t>(clampMidi7(ev.data1));
			out.c = static_cast<uint8_t>(clampMidi7(ev.data2));
			return true;

		case M_POLYPRESSURE:
			out.a = static_cast<uint8_t>(M_POLYPRESSURE | channel);
			out.b = static_cast<uint8_t>(clampMidi7(ev.data1));
			out.c = static_cast<uint8_t>(clampMidi7(ev.data2));
			return true;

		case M_CONTROLCHANGE:
			out.a = static_cast<uint8_t>(M_CONTROLCHANGE | channel);
			out.b = static_cast<uint8_t>(clampMidi7(ev.data1));
			out.c = static_cast<uint8_t>(clampMidi7(ev.data2));
			return true;

		case M_PROGRAMCHANGE:
			out.a = static_cast<uint8_t>(M_PROGRAMCHANGE | channel);
			out.b = static_cast<uint8_t>(clampMidi7(ev.data1));
			out.c = 0;
			return true;

		case M_AFTERTOUCH:
			out.a = static_cast<uint8_t>(M_AFTERTOUCH | channel);
			out.b = static_cast<uint8_t>(clampMidi7(ev.data1));
			out.c = 0;
			return true;

		case M_PITCHBEND:
			{
				const int bend = std::clamp(static_cast<int>(ev.data1) | (static_cast<int>(ev.data2) << 7), 0, 16383);
				out.a = static_cast<uint8_t>(M_PITCHBEND | channel);
				out.b = static_cast<uint8_t>(bend & 0x7f);
				out.c = static_cast<uint8_t>((bend >> 7) & 0x7f);
				return true;
			}

		default:
			return false;
		}
	}

	void resizeAudioBuffers(
		std::array<std::vector<float>, 4>& inputs,
		std::array<std::vector<float>, 12>& outputs,
		const uint32_t frames)
	{
		for(auto& input : inputs)
			input.assign(frames, 0.0f);

		for(auto& output : outputs)
			output.assign(frames, 0.0f);
	}

	void clearAudioBuffers(std::array<std::vector<float>, 12>& outputs, const uint32_t frames)
	{
		for(auto& output : outputs)
			std::fill(output.begin(), output.begin() + frames, 0.0f);
	}

	void processBlock(
		virusLib::Device& device,
		std::array<std::vector<float>, 4>& inputs,
		std::array<std::vector<float>, 12>& outputs,
		const uint32_t frames,
		const std::vector<synthLib::SMidiEvent>& midiIn)
	{
		synthLib::TAudioInputs inputPtrs{};
		synthLib::TAudioOutputs outputPtrs{};

		for(size_t i = 0; i < inputPtrs.size(); ++i)
			inputPtrs[i] = inputs[i].data();

		for(size_t i = 0; i < outputPtrs.size(); ++i)
			outputPtrs[i] = outputs[i].data();

		clearAudioBuffers(outputs, frames);

		std::vector<synthLib::SMidiEvent> midiOut;
		device.process(inputPtrs, outputPtrs, frames, midiIn, midiOut);
	}

	void processSilentWarmup(
		virusLib::Device& device,
		std::array<std::vector<float>, 4>& inputs,
		std::array<std::vector<float>, 12>& outputs,
		uint32_t frames,
		std::vector<synthLib::SMidiEvent> firstBlockMidi)
	{
		const auto blockFrames = static_cast<uint32_t>(outputs[0].size());

		while(frames > 0)
		{
			const auto framesNow = std::min(frames, blockFrames);
			processBlock(device, inputs, outputs, framesNow, firstBlockMidi);
			firstBlockMidi.clear();
			frames -= framesNow;
		}
	}
}

int main()
{
	try
	{
		std::signal(SIGINT, handleSignal);
		std::signal(SIGTERM, handleSignal);

		const auto firmwarePath = projectRoot() / "firmware" / "firmware.bin";
		auto firmwareData = readBinaryFile(firmwarePath);

		virusLib::ROMFile rom(firmwareData, firmwarePath.string(), virusLib::DeviceModel::Snow);
		if(!rom.isValid())
			throw std::runtime_error("Firmware is not a valid Virus TI2 firmware: " + firmwarePath.string());

		virusLib::ROMFile::TPreset preset{};
		if(!rom.getSingle(kDefaultPresetBank, kDefaultPresetProgram, preset))
			throw std::runtime_error("Failed to read default preset from firmware");

		std::cout << "Loaded firmware: " << firmwarePath << '\n';
		std::cout << "Selected preset: " << virusLib::ROMFile::getSingleName(preset) << '\n';

		synthLib::DeviceCreateParams params;
		params.romName = firmwarePath.string();
		params.romData = std::move(firmwareData);
		params.customData = static_cast<uint32_t>(virusLib::DeviceModel::Snow);
		params.hostSamplerate = static_cast<float>(kSampleRate);
		params.preferredSamplerate = static_cast<float>(kSampleRate);

		virusLib::Device device(params, false);
		device.setDspClockPercent(40);
		if(!device.isValid())
			throw std::runtime_error("Failed to create Virus TI2 device");

		MidiIO_Universal midi;
		if(midi.Start("TirusWind") != 0)
			throw std::runtime_error("Failed to start MIDI IO");

		WaveIO_I2S wave(kSampleRate);
		auto blockSize = static_cast<uint32_t>(wave.GetPeriodSizeInFrames());
		if(blockSize == 0)
			blockSize = kFallbackBlockSize;

		std::array<std::vector<float>, 4> inputs;
		std::array<std::vector<float>, 12> outputs;
		resizeAudioBuffers(inputs, outputs, blockSize);

		std::vector<synthLib::SMidiEvent> presetMidi;
		auto& presetEvent = presetMidi.emplace_back(synthLib::MidiEventSource::Host);
		presetEvent.sysex = virusLib::Microcontroller::createSingleDump(
			rom,
			virusLib::BankNumber::EditBuffer,
			virusLib::SINGLE,
			preset);

		processSilentWarmup(device, inputs, outputs, kWarmupFrames, presetMidi);

		std::cout << "Running realtime engine at " << kSampleRate
		          << " Hz, block size " << blockSize << " frames\n";
		std::cout << "Press Ctrl+C to stop.\n";

		while(g_shouldRun.load())
		{
			std::vector<synthLib::SMidiEvent> midiIn;
			MidiIO_Universal::Event midiEvent{};

			while(midi.PopEvent(midiEvent))
			{
				synthLib::SMidiEvent synthEvent;
				if(midiEventToSynthEvent(midiEvent, synthEvent))
					midiIn.emplace_back(std::move(synthEvent));
			}

			processBlock(device, inputs, outputs, blockSize, midiIn);

			if(wave.PlayAudio(outputs[0].data(), outputs[1].data(), static_cast<int>(blockSize)) < 0)
				throw std::runtime_error("Audio playback failed");
		}

		std::cout << "Stopped.\n";
		return 0;
	}
	catch(const std::exception& e)
	{
		std::cerr << "Error: " << e.what() << '\n';
		return 1;
	}
}
