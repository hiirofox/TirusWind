#pragma once

#include <iostream>
#include <vector>
#include <string.h>
#include <alsa/asoundlib.h>

class MidiIO_Host
{
private:
    snd_seq_t *seq;
    int myport;
    int myclient;

    // 用于 Raw MIDI 字节 -> ALSA Event 的转换解析器
    snd_midi_event_t *midi_parser;

    // 尝试连接指定的客户端:端口
    void ConnectFrom(int client, int port)
    {
        snd_seq_connect_from(seq, myport, client, port);
        // 如果希望双向通讯（你的程序发送的数据也能传给设备），可以把下面这行解注
        // snd_seq_connect_to(seq, myport, client, port);
    }
    // 检查端口能力并决定是否连接 (双向逻辑)
    void CheckAndConnect(const snd_seq_addr_t &addr)
    {
        if (addr.client == myclient)
            return; // 别连自己

        snd_seq_client_info_t *cinfo;
        snd_seq_port_info_t *pinfo;
        snd_seq_client_info_alloca(&cinfo);
        snd_seq_port_info_alloca(&pinfo);

        if (snd_seq_get_any_client_info(seq, addr.client, cinfo) < 0)
            return;
        if (snd_seq_get_any_port_info(seq, addr.client, addr.port, pinfo) < 0)
            return;

        // 排除系统端口
        if (addr.client == SND_SEQ_CLIENT_SYSTEM)
            return;

        unsigned int caps = snd_seq_port_info_get_capability(pinfo);

        // ---------------------------------------------------------
        // 方向 1: 输入 (Input) - 让我们可以接收设备的 MIDI
        // 条件: 设备端口可读 (READ) 且允许被订阅 (SUBS_READ)
        // ---------------------------------------------------------
        if ((caps & SND_SEQ_PORT_CAP_READ) && (caps & SND_SEQ_PORT_CAP_SUBS_READ))
        {
            int err = snd_seq_connect_from(seq, myport, addr.client, addr.port);
            if (err >= 0)
            {
                printf("[AutoConnect-IN] 已连接输入源: %s (Client %d: Port %d)\n",
                       snd_seq_client_info_get_name(cinfo), addr.client, addr.port);
            }
        }

        // ---------------------------------------------------------
        // 方向 2: 输出 (Output) - 让 SendMidiMsg 可以发给设备
        // 条件: 设备端口可写 (WRITE) 且允许被写入 (SUBS_WRITE)
        // ---------------------------------------------------------
        if ((caps & SND_SEQ_PORT_CAP_WRITE) && (caps & SND_SEQ_PORT_CAP_SUBS_WRITE))
        {
            int err = snd_seq_connect_to(seq, myport, addr.client, addr.port);
            if (err >= 0)
            {
                printf("[AutoConnect-OUT] 已连接输出目标: %s (Client %d: Port %d)\n",
                       snd_seq_client_info_get_name(cinfo), addr.client, addr.port);
            }
        }
    }

    // 处理热插拔系统事件
    void HandleSystemEvent(const snd_seq_event_t *ev)
    {
        if (ev->type == SND_SEQ_EVENT_PORT_START)
        {
            // 当有新端口启动时，检查是否需要连接
            CheckAndConnect(ev->data.addr);
        }
        // 你也可以处理 SND_SEQ_EVENT_PORT_EXIT 来打印设备断开日志
        else if (ev->type == SND_SEQ_EVENT_PORT_EXIT)
        {
            printf("[AutoConnect] 设备可能已断开 (Client %d: Port %d)\n",
                   ev->data.addr.client, ev->data.addr.port);
        }
    }

public:
    MidiIO_Host() : seq(nullptr), myport(-1), myclient(-1), midi_parser(nullptr) {}

    ~MidiIO_Host()
    {
        Stop();
    }

    int StartAuto()
    {
        // 1. 打开 Sequencer
        if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0)
        {
            std::cerr << "Error: 无法打开 ALSA Sequencer." << std::endl;
            return -1;
        }

        snd_seq_set_client_name(seq, "My MIDI Host");
        myclient = snd_seq_client_id(seq);

        // 2. 初始化 MIDI 解析器 (用于发送 Raw Midi)
        // 这里的 buffer size 设为 256 字节足够处理大多数 MIDI 消息
        if (snd_midi_event_new(256, &midi_parser) < 0)
        {
            std::cerr << "Error: 无法分配 MIDI 解析器." << std::endl;
            Stop();
            return -1;
        }

        // 3. 创建端口
        // 关键修改：增加了 CAP_READ | CAP_SUBS_READ，这样我们不仅能收，也能对外发送数据
        myport = snd_seq_create_simple_port(seq, "MIDI IO",
                                            SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE | SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                            SND_SEQ_PORT_TYPE_APPLICATION);

        if (myport < 0)
        {
            Stop();
            return -1;
        }

        // 4. 订阅系统公告端口 (System Announce)
        // 这样我们就能收到 SND_SEQ_EVENT_PORT_START 等事件
        snd_seq_connect_from(seq, myport, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);

        // 5. 初始扫描：连接现有设备
        snd_seq_client_info_t *cinfo;
        snd_seq_port_info_t *pinfo;
        snd_seq_client_info_alloca(&cinfo);
        snd_seq_port_info_alloca(&pinfo);

        snd_seq_client_info_set_client(cinfo, -1);
        while (snd_seq_query_next_client(seq, cinfo) >= 0)
        {
            int client_id = snd_seq_client_info_get_client(cinfo);
            if (client_id == SND_SEQ_CLIENT_SYSTEM || client_id == myclient)
                continue;

            snd_seq_port_info_set_client(pinfo, client_id);
            snd_seq_port_info_set_port(pinfo, -1);
            while (snd_seq_query_next_port(seq, pinfo) >= 0)
            {
                snd_seq_addr_t addr;
                addr.client = client_id;
                addr.port = snd_seq_port_info_get_port(pinfo);
                CheckAndConnect(addr);
            }
        }

        return 0;
    }

    // 发送 MIDI 消息接口 (支持 status, data1, data2)
    // 例如: SendMidiMsg(0x90, 60, 100); // Note On, C4, Velocity 100
    void SendMidiMsg(unsigned char status, unsigned char data1, unsigned char data2)
    {
        if (!seq || !midi_parser)
            return;

        snd_seq_event_t ev;
        unsigned char bytes[3] = {status, data1, data2};

        // 使用解析器将 raw bytes 编码为 ALSA event
        // 这比手动 switch case 设置 ev.data.note.note 兼容性更好
        snd_midi_event_reset_encode(midi_parser);
        long len = 0;

        // 根据 status 判断是 1字节, 2字节 还是 3字节指令
        // 简单的处理方式：让 snd_midi_event_encode 自己判断 buffer 需要多长
        // 这里我们假设用户传入的是标准的 3字节短消息（除了 ProgramChange/ChannelPressure 是2字节）
        // 为了通用，我们可以传满3字节，解析器会自己截断多余的

        // 编码
        len = snd_midi_event_encode(midi_parser, bytes, 3, &ev);

        if (len > 0)
        {
            snd_seq_ev_set_source(&ev, myport);
            // 发送给所有订阅了我们端口的人 (Subscribers)
            snd_seq_ev_set_subs(&ev);
            snd_seq_ev_set_direct(&ev); // 立即发送，不放入队列等待

            snd_seq_event_output(seq, &ev);
            snd_seq_drain_output(seq); // 刷新缓冲区确保发出去
        }
    }

    // 替代原有的 midimsg 和 GetMidiMsg
    // 返回 true 表示获取到了有效的 MIDI 消息，存入 out_ev
    // 返回 false 表示没有消息，或者处理的是内部系统消息
    bool PopEvent(snd_seq_event_t &out_ev)
    {
        if (!seq)
            return false;

        snd_seq_event_t *ev = nullptr;
        // 非阻塞读取
        int err = snd_seq_event_input(seq, &ev);

        if (err < 0 || !ev)
            return false;

        bool is_valid_user_msg = false;

        // 判断消息来源
        if (ev->source.client == SND_SEQ_CLIENT_SYSTEM)
        {
            // 如果是系统广播（热插拔），内部处理，不返回给用户
            HandleSystemEvent(ev);
            is_valid_user_msg = false;
        }
        else if (ev->type >= SND_SEQ_EVENT_SYSTEM && ev->type <= SND_SEQ_EVENT_KEYSIGN)
        {
            // 这是一个标准的 MIDI 范围事件 (Note, CC, PitchBend, etc.)
            is_valid_user_msg = true;
        }
        // 如果你需要处理其他特定事件，可以在这里添加逻辑

        if (is_valid_user_msg)
        {
            // 拷贝数据给用户
            out_ev = *ev; // struct copy
        }

        // 释放 ALSA 分配的事件内存
        snd_seq_free_event(ev);

        // 如果刚才处理的是系统事件，我们返回 false，让上层继续轮询
        // 否则返回 true
        return is_valid_user_msg;
    }

    void Stop()
    {
        if (midi_parser)
        {
            snd_midi_event_free(midi_parser);
            midi_parser = nullptr;
        }
        if (seq)
        {
            snd_seq_close(seq);
            seq = nullptr;
        }
    }
};

class MidiIO_Universal
{
private:
    snd_seq_t *seq;
    int myport;
    int myclient;
    snd_midi_event_t *midi_parser; 

    // 判断是否为 USB Gadget (连接电脑的通道)
    bool IsUsbGadget(const char* name)
    {
        if (!name) return false;
        std::string s(name);
        // 常见 Linux Gadget 驱动名
        return (s.find("f_midi") != std::string::npos || 
                s.find("g_midi") != std::string::npos || 
                s.find("MIDI function") != std::string::npos ||
                s.find("Gadget") != std::string::npos);
    }

    // 核心连接逻辑：扫描到一个端口后，根据它是谁，决定怎么连
    void HandlePortConnection(int client, int port, const char* clientName, unsigned int caps)
    {
        if (client == myclient || client == SND_SEQ_CLIENT_SYSTEM) return;

        bool is_gadget = IsUsbGadget(clientName);

        // ------------------------------------------------------
        // 策略 A: 连接 [输入源] (别人 -> 我)
        // ------------------------------------------------------
        // 只要对方能由我读取(READ)且允许订阅(SUBS_READ)，我就连它。
        // 无论是 键盘 还是 电脑发来的信号，我都要收。
        if ((caps & SND_SEQ_PORT_CAP_READ) && (caps & SND_SEQ_PORT_CAP_SUBS_READ))
        {
            int err = snd_seq_connect_from(seq, myport, client, port);
            if (err >= 0) { // >=0 表示连接成功，或者已经连接过
                 // 只有首次连接成功时才打印，避免日志刷屏(ALSA会自动处理重复连接)
                 // 但为了简单，这里每次都尝试连
            }
        }

        // ------------------------------------------------------
        // 策略 B: 连接 [输出目标] (我 -> 别人)
        // ------------------------------------------------------
        // 只有当对方是 USB Gadget (电脑) 时，我才自动把我的输出连过去。
        // *注意*：通常我们不会自动连所有外部键盘的 Input，因为不需要向键盘发声（除非你需要做音序器）。
        // 这里我们只自动连接 Gadget，保证 SendMidiMsg 能发给电脑。
        if (is_gadget) 
        {
            if ((caps & SND_SEQ_PORT_CAP_WRITE) && (caps & SND_SEQ_PORT_CAP_SUBS_WRITE))
            {
                snd_seq_connect_to(seq, myport, client, port);
                printf("[MidiUniversal] 已建立上行链路 -> %s (Client %d)\n", clientName, client);
            }
        }
    }

    // 全局扫描
    void ScanAndConnectAll()
    {
        snd_seq_client_info_t *cinfo;
        snd_seq_port_info_t *pinfo;
        snd_seq_client_info_alloca(&cinfo);
        snd_seq_port_info_alloca(&pinfo);

        snd_seq_client_info_set_client(cinfo, -1);
        while (snd_seq_query_next_client(seq, cinfo) >= 0)
        {
            int client_id = snd_seq_client_info_get_client(cinfo);
            if (client_id == SND_SEQ_CLIENT_SYSTEM || client_id == myclient) continue;

            snd_seq_port_info_set_client(pinfo, client_id);
            snd_seq_port_info_set_port(pinfo, -1);
            while (snd_seq_query_next_port(seq, pinfo) >= 0)
            {
                HandlePortConnection(
                    client_id, 
                    snd_seq_port_info_get_port(pinfo), 
                    snd_seq_client_info_get_name(cinfo),
                    snd_seq_port_info_get_capability(pinfo)
                );
            }
        }
    }

    void HandleSystemEvent(const snd_seq_event_t* ev)
    {
        // 无论是有人插进来了(Start)，还是端口属性变了，都检查一遍连接
        if (ev->type == SND_SEQ_EVENT_PORT_START) {
            // 获取新端口的信息并尝试连接
            snd_seq_client_info_t *cinfo;
            snd_seq_port_info_t *pinfo;
            snd_seq_client_info_alloca(&cinfo);
            snd_seq_port_info_alloca(&pinfo);

            if (snd_seq_get_any_client_info(seq, ev->data.addr.client, cinfo) >= 0 &&
                snd_seq_get_any_port_info(seq, ev->data.addr.client, ev->data.addr.port, pinfo) >= 0) 
            {
                printf("[AutoConnect] 检测到新设备: %s\n", snd_seq_client_info_get_name(cinfo));
                HandlePortConnection(
                    ev->data.addr.client, 
                    ev->data.addr.port, 
                    snd_seq_client_info_get_name(cinfo),
                    snd_seq_port_info_get_capability(pinfo)
                );
            }
        }
    }

public:
    MidiIO_Universal() : seq(nullptr), myport(-1), myclient(-1), midi_parser(nullptr) {}

    ~MidiIO_Universal() { Stop(); }

    int Start(const char* appName = "Midi Universal Host")
    {
        if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
            std::cerr << "Error: 无法打开 ALSA Sequencer." << std::endl;
            return -1;
        }
        
        snd_seq_set_client_name(seq, appName);
        myclient = snd_seq_client_id(seq);

        if (snd_midi_event_new(256, &midi_parser) < 0) {
            Stop(); return -1;
        }

        // 创建全功能端口：既能读也能写
        myport = snd_seq_create_simple_port(seq, "Main Port",
            SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE | SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
            SND_SEQ_PORT_TYPE_APPLICATION);

        if (myport < 0) { Stop(); return -1; }

        // 订阅系统公告，监听热插拔
        snd_seq_connect_from(seq, myport, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);

        // 初始全量扫描
        ScanAndConnectAll();

        printf("[MidiUniversal] 已启动. Client ID: %d\n", myclient);
        return 0;
    }

    void Stop()
    {
        if (midi_parser) { snd_midi_event_free(midi_parser); midi_parser = nullptr; }
        if (seq) { snd_seq_close(seq); seq = nullptr; }
    }

    // 发送 MIDI: 你的程序产生的音符 -> 发送给所有连接的 Output (主要是电脑)
    void SendMidiMsg(unsigned char status, unsigned char data1, unsigned char data2)
    {
        if (!seq || !midi_parser) return;
        snd_seq_event_t ev;
        unsigned char bytes[3] = { status, data1, data2 };
        snd_midi_event_reset_encode(midi_parser);
        if (snd_midi_event_encode(midi_parser, bytes, 3, &ev) > 0) {
            snd_seq_ev_set_source(&ev, myport);
            snd_seq_ev_set_subs(&ev); 
            snd_seq_ev_set_direct(&ev);
            snd_seq_event_output(seq, &ev);
            snd_seq_drain_output(seq);
        }
    }

    // 接收 MIDI: 无论是 键盘弹奏 还是 电脑发来的，都从这里出来
    bool PopEvent(snd_seq_event_t &out_ev)
    {
        if (!seq) return false;
        snd_seq_event_t *ev = nullptr;
        if (snd_seq_event_input(seq, &ev) < 0 || !ev) return false;

        bool valid = false;
        if (ev->source.client == SND_SEQ_CLIENT_SYSTEM) {
            HandleSystemEvent(ev);
            valid = false;
        }
        else if (ev->type >= SND_SEQ_EVENT_SYSTEM && ev->type <= SND_SEQ_EVENT_KEYSIGN) {
            valid = true;
        }

        if (valid) out_ev = *ev;
        snd_seq_free_event(ev);
        return valid;
    }
};