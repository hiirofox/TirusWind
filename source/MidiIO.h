#pragma once

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <mutex>
#include <queue>
#else
#include <alsa/asoundlib.h>
#endif

class MidiIO_Universal
{
public:
	struct Event
	{
		uint8_t status = 0;
		uint8_t data1 = 0;
		uint8_t data2 = 0;
		uint32_t size = 0;
	};

#if defined(_WIN32)
private:
	HMIDIIN midiIn = nullptr;
	HMIDIOUT midiOut = nullptr;
	std::mutex queueMutex;
	std::queue<Event> events;

	static void CALLBACK midiInCallback(
		HMIDIIN,
		UINT message,
		DWORD_PTR instance,
		DWORD_PTR param1,
		DWORD_PTR)
	{
		if(message != MIM_DATA || instance == 0)
			return;

		auto* self = reinterpret_cast<MidiIO_Universal*>(instance);
		const auto packed = static_cast<DWORD>(param1);

		Event ev;
		ev.status = static_cast<uint8_t>(packed & 0xff);
		ev.data1 = static_cast<uint8_t>((packed >> 8) & 0xff);
		ev.data2 = static_cast<uint8_t>((packed >> 16) & 0xff);
		ev.size = 3;

		const auto type = ev.status & 0xf0;
		if(type == 0xc0 || type == 0xd0)
			ev.size = 2;

		if(ev.status >= 0x80 && ev.status < 0xf0)
		{
			std::lock_guard<std::mutex> lock(self->queueMutex);
			self->events.push(ev);
		}
	}

public:
	MidiIO_Universal() = default;
	~MidiIO_Universal() { Stop(); }

	int Start(const char* appName = "TirusWind")
	{
		(void)appName;

		const MMRESULT outResult = midiOutOpen(&midiOut, MIDI_MAPPER, 0, 0, CALLBACK_NULL);
		if(outResult != MMSYSERR_NOERROR)
		{
			midiOut = nullptr;
			std::cerr << "[MidiIO] No Windows MIDI output available; SendMidiMsg will be ignored.\n";
		}

		if(midiInGetNumDevs() == 0)
		{
			std::cerr << "[MidiIO] No Windows MIDI input devices found.\n";
			return 0;
		}

		const MMRESULT inResult = midiInOpen(
			&midiIn,
			0,
			reinterpret_cast<DWORD_PTR>(&midiInCallback),
			reinterpret_cast<DWORD_PTR>(this),
			CALLBACK_FUNCTION);

		if(inResult != MMSYSERR_NOERROR)
		{
			midiIn = nullptr;
			std::cerr << "[MidiIO] Failed to open Windows MIDI input device.\n";
			return -1;
		}

		midiInStart(midiIn);
		std::cout << "[MidiIO] Windows MIDI input started.\n";
		return 0;
	}

	void Stop()
	{
		if(midiIn)
		{
			midiInStop(midiIn);
			midiInReset(midiIn);
			midiInClose(midiIn);
			midiIn = nullptr;
		}

		if(midiOut)
		{
			midiOutReset(midiOut);
			midiOutClose(midiOut);
			midiOut = nullptr;
		}

		std::lock_guard<std::mutex> lock(queueMutex);
		std::queue<Event> empty;
		events.swap(empty);
	}

	void SendMidiMsg(unsigned char status, unsigned char data1, unsigned char data2)
	{
		if(!midiOut)
			return;

		const DWORD packed =
			static_cast<DWORD>(status) |
			(static_cast<DWORD>(data1) << 8) |
			(static_cast<DWORD>(data2) << 16);
		midiOutShortMsg(midiOut, packed);
	}

	bool PopEvent(Event& outEvent)
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		if(events.empty())
			return false;

		outEvent = events.front();
		events.pop();
		return true;
	}

#else
private:
	snd_seq_t* seq = nullptr;
	int myport = -1;
	int myclient = -1;
	snd_midi_event_t* midiParser = nullptr;

	bool isUsbGadget(const char* name)
	{
		if(!name)
			return false;

		const std::string s(name);
		return s.find("f_midi") != std::string::npos ||
			s.find("g_midi") != std::string::npos ||
			s.find("MIDI function") != std::string::npos ||
			s.find("Gadget") != std::string::npos;
	}

	void handlePortConnection(int client, int port, const char* clientName, unsigned int caps)
	{
		if(client == myclient || client == SND_SEQ_CLIENT_SYSTEM)
			return;

		if((caps & SND_SEQ_PORT_CAP_READ) && (caps & SND_SEQ_PORT_CAP_SUBS_READ))
			snd_seq_connect_from(seq, myport, client, port);

		if(isUsbGadget(clientName) &&
			(caps & SND_SEQ_PORT_CAP_WRITE) &&
			(caps & SND_SEQ_PORT_CAP_SUBS_WRITE))
		{
			snd_seq_connect_to(seq, myport, client, port);
			std::cout << "[MidiIO] Connected output to " << clientName
				<< " (client " << client << ")\n";
		}
	}

	void scanAndConnectAll()
	{
		snd_seq_client_info_t* cinfo;
		snd_seq_port_info_t* pinfo;
		snd_seq_client_info_alloca(&cinfo);
		snd_seq_port_info_alloca(&pinfo);

		snd_seq_client_info_set_client(cinfo, -1);
		while(snd_seq_query_next_client(seq, cinfo) >= 0)
		{
			const int clientId = snd_seq_client_info_get_client(cinfo);
			if(clientId == SND_SEQ_CLIENT_SYSTEM || clientId == myclient)
				continue;

			snd_seq_port_info_set_client(pinfo, clientId);
			snd_seq_port_info_set_port(pinfo, -1);
			while(snd_seq_query_next_port(seq, pinfo) >= 0)
			{
				handlePortConnection(
					clientId,
					snd_seq_port_info_get_port(pinfo),
					snd_seq_client_info_get_name(cinfo),
					snd_seq_port_info_get_capability(pinfo));
			}
		}
	}

	void handleSystemEvent(const snd_seq_event_t* ev)
	{
		if(ev->type != SND_SEQ_EVENT_PORT_START)
			return;

		snd_seq_client_info_t* cinfo;
		snd_seq_port_info_t* pinfo;
		snd_seq_client_info_alloca(&cinfo);
		snd_seq_port_info_alloca(&pinfo);

		if(snd_seq_get_any_client_info(seq, ev->data.addr.client, cinfo) >= 0 &&
			snd_seq_get_any_port_info(seq, ev->data.addr.client, ev->data.addr.port, pinfo) >= 0)
		{
			handlePortConnection(
				ev->data.addr.client,
				ev->data.addr.port,
				snd_seq_client_info_get_name(cinfo),
				snd_seq_port_info_get_capability(pinfo));
		}
	}

	static uint8_t clamp7(int value)
	{
		return static_cast<uint8_t>(std::clamp(value, 0, 127));
	}

	static bool convertAlsaEvent(const snd_seq_event_t& ev, Event& outEvent)
	{
		outEvent = {};

		switch(ev.type)
		{
		case SND_SEQ_EVENT_NOTE:
		case SND_SEQ_EVENT_NOTEON:
			outEvent.status = static_cast<uint8_t>((ev.data.note.velocity == 0 ? 0x80 : 0x90) | (ev.data.note.channel & 0x0f));
			outEvent.data1 = clamp7(ev.data.note.note);
			outEvent.data2 = clamp7(ev.data.note.velocity);
			outEvent.size = 3;
			return true;

		case SND_SEQ_EVENT_NOTEOFF:
			outEvent.status = static_cast<uint8_t>(0x80 | (ev.data.note.channel & 0x0f));
			outEvent.data1 = clamp7(ev.data.note.note);
			outEvent.data2 = clamp7(ev.data.note.velocity);
			outEvent.size = 3;
			return true;

		case SND_SEQ_EVENT_KEYPRESS:
			outEvent.status = static_cast<uint8_t>(0xa0 | (ev.data.note.channel & 0x0f));
			outEvent.data1 = clamp7(ev.data.note.note);
			outEvent.data2 = clamp7(ev.data.note.velocity);
			outEvent.size = 3;
			return true;

		case SND_SEQ_EVENT_CONTROLLER:
			outEvent.status = static_cast<uint8_t>(0xb0 | (ev.data.control.channel & 0x0f));
			outEvent.data1 = clamp7(ev.data.control.param);
			outEvent.data2 = clamp7(ev.data.control.value);
			outEvent.size = 3;
			return true;

		case SND_SEQ_EVENT_PGMCHANGE:
			outEvent.status = static_cast<uint8_t>(0xc0 | (ev.data.control.channel & 0x0f));
			outEvent.data1 = clamp7(ev.data.control.value);
			outEvent.size = 2;
			return true;

		case SND_SEQ_EVENT_CHANPRESS:
			outEvent.status = static_cast<uint8_t>(0xd0 | (ev.data.control.channel & 0x0f));
			outEvent.data1 = clamp7(ev.data.control.value);
			outEvent.size = 2;
			return true;

		case SND_SEQ_EVENT_PITCHBEND:
			{
				const int bend = std::clamp(ev.data.control.value + 8192, 0, 16383);
				outEvent.status = static_cast<uint8_t>(0xe0 | (ev.data.control.channel & 0x0f));
				outEvent.data1 = static_cast<uint8_t>(bend & 0x7f);
				outEvent.data2 = static_cast<uint8_t>((bend >> 7) & 0x7f);
				outEvent.size = 3;
				return true;
			}

		default:
			return false;
		}
	}

public:
	MidiIO_Universal() = default;
	~MidiIO_Universal() { Stop(); }

	int Start(const char* appName = "Midi Universal Host")
	{
		if(snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0)
		{
			std::cerr << "Error: Failed to open ALSA Sequencer.\n";
			return -1;
		}

		snd_seq_set_client_name(seq, appName);
		myclient = snd_seq_client_id(seq);

		if(snd_midi_event_new(256, &midiParser) < 0)
		{
			Stop();
			return -1;
		}

		myport = snd_seq_create_simple_port(
			seq,
			"Main Port",
			SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE |
				SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
			SND_SEQ_PORT_TYPE_APPLICATION);

		if(myport < 0)
		{
			Stop();
			return -1;
		}

		snd_seq_connect_from(seq, myport, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);
		scanAndConnectAll();

		std::cout << "[MidiIO] ALSA sequencer started. Client ID: " << myclient << '\n';
		return 0;
	}

	void Stop()
	{
		if(midiParser)
		{
			snd_midi_event_free(midiParser);
			midiParser = nullptr;
		}

		if(seq)
		{
			snd_seq_close(seq);
			seq = nullptr;
		}
	}

	void SendMidiMsg(unsigned char status, unsigned char data1, unsigned char data2)
	{
		if(!seq || !midiParser)
			return;

		snd_seq_event_t ev;
		unsigned char bytes[3] = {status, data1, data2};
		snd_midi_event_reset_encode(midiParser);
		if(snd_midi_event_encode(midiParser, bytes, 3, &ev) > 0)
		{
			snd_seq_ev_set_source(&ev, myport);
			snd_seq_ev_set_subs(&ev);
			snd_seq_ev_set_direct(&ev);
			snd_seq_event_output(seq, &ev);
			snd_seq_drain_output(seq);
		}
	}

	bool PopEvent(Event& outEvent)
	{
		if(!seq)
			return false;

		snd_seq_event_t* ev = nullptr;
		if(snd_seq_event_input(seq, &ev) < 0 || !ev)
			return false;

		bool valid = false;
		if(ev->source.client == SND_SEQ_CLIENT_SYSTEM)
		{
			handleSystemEvent(ev);
		}
		else
		{
			valid = convertAlsaEvent(*ev, outEvent);
		}

		snd_seq_free_event(ev);
		return valid;
	}
#endif
};

using MidiIO = MidiIO_Universal;
