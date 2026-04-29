#include "synthLib/audioTypes.h"
#include "synthLib/midiTypes.h"
#include "synthLib/device.h"

#include "virusLib/device.h"
#include "virusLib/microcontroller.h"
#include "virusLib/romfile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	constexpr uint32_t kSampleRate = 44100;
	constexpr uint32_t kBlockSize = 64;
	constexpr uint32_t kRenderSeconds = 5;
	constexpr uint32_t kWarmupFrames = kSampleRate / 4;

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

	void writeLE16(std::ofstream& file, const uint16_t value)
	{
		file.put(static_cast<char>(value & 0xff));
		file.put(static_cast<char>((value >> 8) & 0xff));
	}

	void writeLE32(std::ofstream& file, const uint32_t value)
	{
		file.put(static_cast<char>(value & 0xff));
		file.put(static_cast<char>((value >> 8) & 0xff));
		file.put(static_cast<char>((value >> 16) & 0xff));
		file.put(static_cast<char>((value >> 24) & 0xff));
	}

	int16_t floatToPcm16(const float sample)
	{
		const auto clipped = std::clamp(sample, -1.0f, 1.0f);
		return static_cast<int16_t>(std::lrint(clipped * 32767.0f));
	}

	void writeStereoWav(const std::filesystem::path& path, const std::vector<float>& interleavedStereo)
	{
		if((interleavedStereo.size() & 1u) != 0)
			throw std::runtime_error("Stereo buffer has an odd number of samples");

		std::ofstream file(path, std::ios::binary);
		if(!file)
			throw std::runtime_error("Failed to create " + path.string());

		const uint16_t channels = 2;
		const uint16_t bitsPerSample = 16;
		const uint16_t blockAlign = channels * bitsPerSample / 8;
		const uint32_t byteRate = kSampleRate * blockAlign;
		const uint32_t dataBytes = static_cast<uint32_t>(interleavedStereo.size() * sizeof(int16_t));

		file.write("RIFF", 4);
		writeLE32(file, 36 + dataBytes);
		file.write("WAVE", 4);
		file.write("fmt ", 4);
		writeLE32(file, 16);
		writeLE16(file, 1);
		writeLE16(file, channels);
		writeLE32(file, kSampleRate);
		writeLE32(file, byteRate);
		writeLE16(file, blockAlign);
		writeLE16(file, bitsPerSample);
		file.write("data", 4);
		writeLE32(file, dataBytes);

		for(const auto sample : interleavedStereo)
			writeLE16(file, static_cast<uint16_t>(floatToPcm16(sample)));
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
		const std::vector<synthLib::SMidiEvent>& midiIn,
		std::vector<float>* recordedStereo)
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

		if(!recordedStereo)
			return;

		recordedStereo->reserve(recordedStereo->size() + frames * 2);
		for(uint32_t i = 0; i < frames; ++i)
		{
			recordedStereo->push_back(outputs[0][i]);
			recordedStereo->push_back(outputs[1][i]);
		}
	}

	void processFrames(
		virusLib::Device& device,
		std::array<std::vector<float>, 4>& inputs,
		std::array<std::vector<float>, 12>& outputs,
		uint32_t frames,
		std::vector<synthLib::SMidiEvent> firstBlockMidi,
		std::vector<float>* recordedStereo)
	{
		while(frames > 0)
		{
			const auto blockFrames = std::min(frames, kBlockSize);
			processBlock(device, inputs, outputs, blockFrames, firstBlockMidi, recordedStereo);
			firstBlockMidi.clear();
			frames -= blockFrames;
		}
	}
}

int main()
{
	try
	{
		const std::filesystem::path projectRoot = TIRUSWIND_PROJECT_ROOT;
		const auto firmwarePath = projectRoot / "firmware" / "firmware.bin";
		const auto wavPath = projectRoot / "test.wav";

		auto firmwareData = readBinaryFile(firmwarePath);

		virusLib::ROMFile rom(firmwareData, firmwarePath.string(), virusLib::DeviceModel::TI2);
		if(!rom.isValid())
			throw std::runtime_error("Firmware is not a valid Virus TI2 firmware: " + firmwarePath.string());

		virusLib::ROMFile::TPreset preset{};
		if(!rom.getSingle(0, 10, preset))
			throw std::runtime_error("Failed to read preset bank 0, program 0 from firmware");

		std::cout << "Loaded firmware: " << firmwarePath << '\n';
		std::cout << "Selected preset: " << virusLib::ROMFile::getSingleName(preset) << '\n';

		synthLib::DeviceCreateParams params;
		params.romName = firmwarePath.string();
		params.romData = std::move(firmwareData);
		params.customData = static_cast<uint32_t>(virusLib::DeviceModel::TI2);
		params.hostSamplerate = static_cast<float>(kSampleRate);
		params.preferredSamplerate = static_cast<float>(kSampleRate);

		virusLib::Device device(params, false);
		if(!device.isValid())
			throw std::runtime_error("Failed to create Virus TI2 device");

		std::array<std::vector<float>, 4> inputs;
		std::array<std::vector<float>, 12> outputs;
		for(auto& input : inputs)
			input.assign(kBlockSize, 0.0f);
		for(auto& output : outputs)
			output.assign(kBlockSize, 0.0f);

		std::vector<synthLib::SMidiEvent> presetMidi;
		auto& presetEvent = presetMidi.emplace_back(synthLib::MidiEventSource::Host);
		presetEvent.sysex = virusLib::Microcontroller::createSingleDump(
			rom,
			virusLib::BankNumber::EditBuffer,
			virusLib::SINGLE,
			preset);

		processFrames(device, inputs, outputs, kWarmupFrames, presetMidi, nullptr);

		std::vector<synthLib::SMidiEvent> noteOn;
		noteOn.emplace_back(synthLib::MidiEventSource::Host, 0x90, 60, 100, 0);

		std::vector<float> renderedStereo;
		renderedStereo.reserve(kSampleRate * kRenderSeconds * 2);

		processFrames(device, inputs, outputs, kSampleRate * kRenderSeconds, noteOn, &renderedStereo);

		writeStereoWav(wavPath, renderedStereo);

		std::cout << "Wrote " << wavPath << '\n';
		return 0;
	}
	catch(const std::exception& e)
	{
		std::cerr << "Error: " << e.what() << '\n';
		return 1;
	}
}
