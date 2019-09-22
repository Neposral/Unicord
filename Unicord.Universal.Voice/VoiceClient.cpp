﻿#include "pch.h"
#include "Rtp.h"
#include "VoiceOutputStream.h"
#include "VoiceClient.h"
#include "VoiceClient.g.cpp"
#include <iomanip>
#include <bitset>

using namespace winrt;
using namespace std::chrono;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Networking;
using namespace winrt::Windows::Networking::Sockets;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Unicord::Universal::Voice::Interop;

namespace winrt::Unicord::Universal::Voice::implementation
{
    hstring VoiceClient::OpusVersion()
    {
        auto strptr = opus_get_version_string();
        return to_hstring(strptr);
    }

    hstring VoiceClient::SodiumVersion()
    {
        auto strptr = sodium_version_string();
        return to_hstring(strptr);
    }

    VoiceClient::VoiceClient(VoiceClientOptions const& options)
    {
        if (sodium_init() == -1) {
            throw hresult_error(E_UNEXPECTED, L"Failed to initialize libsodium!");
        }

        if (options.Token().size() == 0 && options.ChannelId() == 0) {
            throw hresult_invalid_argument();
        }

        this->options = options;

        std::wstring_view raw_endpoint = options.Endpoint();
        std::size_t index = raw_endpoint.find_last_of(':');

        if (index != std::wstring::npos) {
            std::wstring str(raw_endpoint.substr(index + 1));
            ws_endpoint.hostname = raw_endpoint.substr(0, index);
            ws_endpoint.port = (uint16_t)std::stoi(str);
        }
        else {
            ws_endpoint.hostname = raw_endpoint;
            ws_endpoint.port = 443;
        }

        InitialiseSockets();
    }

    void VoiceClient::InitialiseSockets()
    {
        audio_format = AudioFormat();
        udp_socket = DatagramSocket();
        udp_socket.Control().QualityOfService(SocketQualityOfService::LowLatency);
        udp_socket.MessageReceived({ this, &VoiceClient::OnUdpMessage });

        web_socket = MessageWebSocket();
        web_socket.Control().MessageType(SocketMessageType::Utf8);
        web_socket.MessageReceived({ this, &VoiceClient::OnWsMessage });
        web_socket.Closed({ this, &VoiceClient::OnWsClosed });
    }

    uint32_t VoiceClient::WebSocketPing()
    {
        return ws_ping;
    }

    uint32_t VoiceClient::UdpSocketPing()
    {
        return udp_ping;
    }

    winrt::event_token VoiceClient::WebSocketPingUpdated(Windows::Foundation::EventHandler<uint32_t> const & handler)
    {
        return wsPingUpdated.add(handler);
    }

    void VoiceClient::WebSocketPingUpdated(winrt::event_token const & token) noexcept
    {
        wsPingUpdated.remove(token);
    }

    winrt::event_token VoiceClient::UdpSocketPingUpdated(Windows::Foundation::EventHandler<uint32_t> const & handler)
    {
        return udpPingUpdated.add(handler);
    }

    void VoiceClient::UdpSocketPingUpdated(winrt::event_token const & token) noexcept
    {
        udpPingUpdated.remove(token);
    }

    winrt::event_token VoiceClient::Connected(Windows::Foundation::EventHandler<bool> const & handler)
    {
        return connected.add(handler);
    }

    void VoiceClient::Connected(winrt::event_token const & token) noexcept
    {
        connected.remove(token);
    }

    winrt::event_token VoiceClient::Disconnected(Windows::Foundation::EventHandler<bool> const & handler)
    {
        return disconnected.add(handler);
    }

    void VoiceClient::Disconnected(winrt::event_token const & token) noexcept
    {
        disconnected.remove(token);
    }

    IAsyncAction VoiceClient::ConnectAsync()
    {
        renderer = new AudioRenderer(this);
        renderer->Initialise(options.PreferredPlaybackDevice(), options.PreferredRecordingDevice());

        auto format = renderer->GetRenderProperties();
        std::cout << "Render: " << format.SampleRate() << " " << format.ChannelCount() << " " << format.BitsPerSample() << "\n";
        format = renderer->GetCaptureProperties();
        std::cout << "Capture: " << format.SampleRate() << " " << format.ChannelCount() << " " << format.BitsPerSample() << "\n";

        audio_format = AudioFormat(format.SampleRate(), format.ChannelCount(), VoiceApplication::low_latency);
        opus = new OpusWrapper(audio_format);

        Windows::Foundation::Uri url{ L"wss://" + ws_endpoint.hostname + L"/?encoding=json&v=4" };
        co_await web_socket.ConnectAsync(url);
    }

    bool VoiceClient::Muted()
    {
        return is_muted;
    }

    void VoiceClient::Muted(bool value)
    {
        if (is_muted != value && renderer != nullptr) {
            if (value) {
                renderer->StopCapture();
            }
            else {
                renderer->BeginCapture();
            }
        }

        is_muted = value;
    }

    bool VoiceClient::Deafened()
    {
        return is_deafened;
    }

    void VoiceClient::Deafened(bool value)
    {
        if (is_muted != value && renderer != nullptr) {
            if (value) {
                renderer->StopCapture();
            }
            else {
                renderer->BeginCapture();
            }
        }

        if (is_deafened != value && renderer != nullptr) {
            if (value) {
                renderer->StopRender();
            }
            else {
                renderer->BeginRender();
            }
        }

        is_muted = value;
        is_deafened = value;
    }

    IAsyncAction VoiceClient::SendIdentifyAsync(bool isResume)
    {
        heartbeat_timer = ThreadPoolTimer::CreatePeriodicTimer({ this, &VoiceClient::OnWsHeartbeat }, milliseconds(heartbeat_interval));

        JsonObject payload;
        if (options.GuildId() != 0) {
            payload.SetNamedValue(L"server_id", JsonValue::CreateStringValue(to_hstring(options.GuildId())));
        }
        else {
            payload.SetNamedValue(L"server_id", JsonValue::CreateNullValue());
        }

        if (!isResume)
            payload.SetNamedValue(L"user_id", JsonValue::CreateStringValue(to_hstring(options.CurrentUserId())));

        payload.SetNamedValue(L"session_id", JsonValue::CreateStringValue(options.SessionId()));
        payload.SetNamedValue(L"token", JsonValue::CreateStringValue(options.Token()));
        payload.SetNamedValue(L"video", JsonValue::CreateBooleanValue(true));

        JsonObject disp;
        disp.SetNamedValue(L"op", JsonValue::CreateNumberValue(isResume ? 7 : 0));
        disp.SetNamedValue(L"d", payload);

        co_await SendJsonPayloadAsync(disp);
    }

    IAsyncAction VoiceClient::Stage1(JsonObject obj)
    {
        if (obj != nullptr) {
            udp_endpoint.hostname = obj.GetNamedString(L"ip");
            udp_endpoint.port = (uint16_t)obj.GetNamedNumber(L"port");
        }

        HostName remoteHost{ udp_endpoint.hostname };
        EndpointPair pair{ nullptr, L"", remoteHost, to_hstring(udp_endpoint.port) };
        co_await udp_socket.ConnectAsync(pair);

        mode = SodiumWrapper::SelectEncryptionMode(obj.GetNamedArray(L"modes"));

        uint8_t buff[70]{ 0 };
        std::copy(&ssrc, &ssrc + sizeof ssrc, buff);

        DataWriter writer{ udp_socket.OutputStream() };
        writer.WriteBytes(buff);
        co_await writer.StoreAsync();
        writer.DetachStream();
    }

    void VoiceClient::Stage2(JsonObject data)
    {
        if (data != nullptr) {
            JsonArray secret_key = data.GetNamedArray(L"secret_key");
            hstring new_mode = data.GetNamedString(L"mode");

            uint8_t key[32];
            for (uint32_t i = 0; i < 32; i++)
            {
                key[i] = (uint8_t)secret_key.GetNumberAt(i);
            }

            sodium = new SodiumWrapper(array_view<const uint8_t>(key), SodiumWrapper::GetEncryptionMode(new_mode));
        }

        keepalive_timer = ThreadPoolTimer::CreatePeriodicTimer({ this, &VoiceClient::OnUdpHeartbeat }, 5000ms);
        voice_thread = std::thread(&VoiceClient::VoiceSendLoop, this);

        auto size = audio_format.CalculateSampleSize(20);
        for (size_t i = 0; i < 3; i++)
        {
            uint8_t* null_pcm = new uint8_t[size]{ 0 };
            voice_queue.push(PCMPacket(gsl::make_span(null_pcm, size), 20));
        }

        if (!is_deafened)
            renderer->BeginRender();

        if (!is_muted && !is_deafened)
            renderer->BeginCapture();

        connected(*this, true);
    }

    void VoiceClient::VoiceSendLoop()
    {
        try
        {
            PCMPacket packet;
            DataWriter writer{ udp_socket.OutputStream() };

            auto start_time = high_resolution_clock::now();
            while (!cancel_voice_send) {
                bool has_packet = voice_queue.try_pop(packet) && packet.duration != 0;

                gsl::span<uint8_t> packet_array;
                if (has_packet) {
                    packet_array = packet.bytes;
                }

                // Provided by Laura#0090 (214796473689178133); this is Python, but adaptable:
                // 
                // delay = max(0, self.delay + ((start_time + self.delay * loops) + - time.time()))

                duration packet_duration = has_packet ? milliseconds(packet.duration) : 20ms;
                duration current_time_offset = high_resolution_clock::now() - start_time;

                if (current_time_offset < packet_duration) {
                    std::this_thread::sleep_for(packet_duration - current_time_offset);
                }

                start_time += packet_duration;

                if (!has_packet || is_deafened)
                {
                    SendSpeakingAsync(false).get();
                    continue;
                }

                SendSpeakingAsync(true).get();

                auto opus_packet = PreparePacket(array_view(packet_array.data(), packet_array.data() + packet_array.size()), packet.is_silence, packet.is_float);
                writer.WriteBytes(array_view<const uint8_t>(opus_packet.bytes.data(), opus_packet.bytes.data() + opus_packet.bytes.size()));
                writer.StoreAsync().get();

                delete[] packet_array.data();

                if (!packet.is_silence && voice_queue.unsafe_size() == 0) {
                    auto size = audio_format.CalculateSampleSize(20);
                    for (size_t i = 0; i < 3; i++)
                    {
                        auto null_pcm = new uint8_t[size]{ 0 };
                        voice_queue.push(PCMPacket(gsl::make_span(null_pcm, size), 20));
                    }
                }
                else if (voice_queue.unsafe_size() == 0) {
                    SendSpeakingAsync(false).get();
                }
            }
        }
        catch (const winrt::hresult_error& ex)
        {
            if (!ws_closed)
                web_socket.Close();
            std::cout << "ERROR: " << to_string(ex.message()) << "\n";
        }
    }

    VoicePacket VoiceClient::PreparePacket(array_view<uint8_t> pcm, bool silence, bool is_float)
    {
        if (is_disposed)
            return VoicePacket();

        if (pcm.size() == 0)
            return VoicePacket();

        RtpHeader header;
        header.ssrc = ssrc;
        header.seq = seq;
        header.timestamp = timestamp;
        header.type = Rtp::RTP_TYPE_OPUS; // opus audio

        auto packet_size = audio_format.GetMaxBufferSize() * 2;
        auto packet = std::vector<uint8_t>(packet_size);
        auto packet_span = gsl::make_span(packet);

        auto opus_length = is_float ? opus->EncodeFloat(pcm, packet_span) : opus->Encode(pcm, packet_span);
        auto encrypted_size = sodium->CalculateTargetSize(opus_length);
        auto new_packet_size = Rtp::CalculatePacketSize((uint32_t)encrypted_size, header, mode.second);
        auto new_packet = std::vector<uint8_t>(new_packet_size);
        auto new_packet_span = gsl::make_span(new_packet);

        Rtp::EncodeHeader(header, new_packet_span);

        const size_t size = crypto_secretbox_xsalsa20poly1305_NONCEBYTES;
        uint8_t packet_nonce[size] = { 0 };
        switch (mode.second)
        {
        case EncryptionMode::XSalsa20_Poly1305:
            sodium->GenerateNonce(new_packet_span.subspan(0, header.size()), packet_nonce);
            break;
        case EncryptionMode::XSalsa20_Poly1305_Suffix:
            sodium->GenerateNonce(packet_nonce);
            break;
        case EncryptionMode::XSalsa20_Poly1305_Lite:
            sodium->GenerateNonce(this->nonce++, packet_nonce);
            break;
        }

        sodium->Encrypt(packet_span.subspan(0, opus_length), packet_nonce, new_packet_span.subspan(header.size(), encrypted_size));
        sodium->AppendNonce(packet_nonce, new_packet_span, mode.second);

        auto duration = is_float ? audio_format.CalculateSampleDurationF(pcm.size()) : audio_format.CalculateSampleDuration(pcm.size());
        auto time = audio_format.CalculateFrameSize(duration);
        this->seq++;
        this->timestamp += (uint32_t)time;

        return VoicePacket{ new_packet, duration, silence };
    }

    void VoiceClient::EnqueuePacket(PCMPacket packet)
    {
        if (packet.duration != 0)
            voice_queue.push(packet);
    }

    void VoiceClient::ProcessRawPacket(array_view<uint8_t> data)
    {
        AudioSource* source;
        std::vector<std::vector<uint8_t>> pcm_packets;

        if (ProcessIncomingPacket(array_view<const uint8_t>(data.begin(), data.end()), pcm_packets, &source)) {
            for each (auto raw_packet in pcm_packets)
            {
                renderer->ProcessIncomingPacket(raw_packet, source);
            }
        }

        pcm_packets.clear();
    }

    bool VoiceClient::ProcessIncomingPacket(array_view<const uint8_t> data, std::vector<std::vector<uint8_t>> &pcm, AudioSource** source)
    {
        if (!Rtp::IsRtpHeader(data))
            return false;

        if (is_deafened)
            return false;

        if (is_disposed)
            return false;

        // decode RTP header
        RtpHeader header;
        Rtp::DecodeHeader(data, header);

        //std::cout << "Packet Type: " << (uint32_t)header.type << " SSRC: " << header.ssrc << " Contributing: ";
        //for each (uint32_t ssrc in header.contributing_ssrcs)
        //    std::cout << ssrc << "; ";
        //std::cout << std::endl;

        // get the nonce
        uint8_t packet_nonce[crypto_secretbox_xsalsa20poly1305_NONCEBYTES]{ 0 };
        array_view<uint8_t> nonce_view(packet_nonce);
        sodium->GetNonce(data, nonce_view, header, mode.second);

        // get the data
        array_view<const uint8_t> encrypted_data;
        Rtp::GetDataFromPacket(data, encrypted_data, header, mode.second);

        if (header.type == Rtp::RTP_TYPE_OPUS) {
            return DecodeOpusPacket(header, source, encrypted_data, nonce_view, pcm);
        }

        return false;
    }

    bool VoiceClient::DecodeOpusPacket(const RtpHeader &header, AudioSource** source, array_view<const uint8_t> &encrypted_data, const array_view<uint8_t> &nonce_view, std::vector<std::vector<uint8_t>> & pcm)
    {
        AudioSource* audio_source = opus->GetOrCreateDecoder(header.ssrc);
        *source = audio_source;

        if (!audio_source->is_speaking) {
            return false;
        }

        if (header.seq < audio_source->seq) { // out of order
            return false;
        }

        uint16_t gap = audio_source->seq != 0 ? header.seq - 1 - audio_source->seq : 0;

        // calculate the size of the opus data
        size_t opus_size = sodium->CalculateSourceSize(encrypted_data.size());
        uint8_t* opus_data = new uint8_t[opus_size]{ 0 };
        array_view<uint8_t> opus_view(opus_data, opus_data + opus_size);

        // decrypt the opus data
        sodium->Decrypt(encrypted_data, nonce_view, opus_view);

        if (header.extension) {
            // RFC 5285, 4.2 One-Byte header
            // http://www.rfcreader.com/#rfc5285_line186
            if (opus_view[0] == 0xBE && opus_view[1] == 0xDE) {
                uint16_t headerLen = opus_view[2] << 8 | opus_view[3];
                uint8_t i = 4;
                for (; i < headerLen + 4; i++)
                {
                    uint8_t b = opus_view[i];

                    // ID is currently unused since we skip it anyway
                    // var id = (byte)(@byte >> 4);
                    uint8_t length = (uint8_t)(b & 0x0F) + 1;
                    i += length;
                }

                // Strip extension padding too
                while (opus_view[i] == 0)
                    i++;

                opus_view = array_view(opus_view.begin() + i, opus_view.end());
            }
        }

        if (gap != 0) {
            AudioFormat packet_format = audio_source->format;
            size_t fec_pcm_length = packet_format.SampleCountToSampleSize(packet_format.GetMaxBufferSize());

            if (gap == 1) {
                std::vector<uint8_t> fec(fec_pcm_length);
                opus->Decode(audio_source, opus_view, fec, true);

                pcm.push_back(fec);
            }
            else if (gap > 1) {
                for (size_t i = 0; i < gap; i++) {
                    int32_t sample_count = opus->GetLastPacketSampleCount(audio_source->decoder);
                    fec_pcm_length = packet_format.SampleCountToSampleSize(sample_count);
                    std::vector<uint8_t> fec(fec_pcm_length);
                    opus->ProcessPacketLoss(audio_source, sample_count, fec);
                    pcm.push_back(fec);
                }
            }
        }

        size_t max_frame_size = audio_source->format.SampleCountToSampleSize(audio_source->format.GetMaxBufferSize());
        std::vector<uint8_t> raw_pcm(max_frame_size);

        opus->Decode(audio_source, opus_view, raw_pcm, false);
        pcm.push_back(raw_pcm);

        audio_source->seq = header.seq;
        audio_source->packets_lost += gap;

        delete[] opus_data;
        return true;
    }

    IAsyncAction VoiceClient::OnWsHeartbeat(ThreadPoolTimer timer)
    {
        uint32_t stamp = (uint32_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        last_heartbeat = stamp;

        JsonObject disp;
        disp.SetNamedValue(L"op", JsonValue::CreateNumberValue(3));
        disp.SetNamedValue(L"d", JsonValue::CreateStringValue(to_hstring(stamp)));

        co_await SendJsonPayloadAsync(disp);
    }

    IAsyncAction VoiceClient::OnWsMessage(IWebSocket socket, MessageWebSocketMessageReceivedEventArgs ev)
    {
        try
        {
            ws_closed = false;
            auto reader = ev.GetDataReader();
            reader.UnicodeEncoding(UnicodeEncoding::Utf8);
            auto json_data = reader.ReadString(reader.UnconsumedBufferLength());
            reader.Close();

            std::cout << "> " << to_string(json_data) << "\n";

            auto json = JsonObject::Parse(json_data);
            auto op = (int)json.GetNamedNumber(L"op");
            auto value = json.GetNamedValue(L"d");

            if (value.ValueType() == JsonValueType::Object) {
                auto data = value.GetObject();
                switch (op) {
                case 8: // hello
                {
                    heartbeat_interval = (int32_t)data.GetNamedNumber(L"heartbeat_interval");
                    co_await SendIdentifyAsync(can_resume);
                    break;
                }
                case 2: // ready
                {
                    ssrc = (uint32_t)data.GetNamedNumber(L"ssrc");
                    co_await Stage1(data);
                    break;
                }
                case 4: // session description
                {
                    Stage2(data);
                    break;
                }
                case 5: // speaking
                {
                    uint32_t speaking_ssrc = (uint32_t)data.GetNamedNumber(L"ssrc");
                    AudioSource* audio_source = opus->GetOrCreateDecoder(speaking_ssrc);
                    audio_source->user_id = std::stoll(to_string(data.GetNamedString(L"user_id")));
                    audio_source->is_speaking = data.GetNamedNumber(L"speaking") != 0;

                    break;
                }
                case 13: // client_disconnected
                {
                    uint64_t user_id = std::stoll(to_string(data.GetNamedString(L"user_id")));
                    AudioSource* source = opus->GetAssociatedAudioSource(user_id, true);

                    if (source != nullptr) {
                        renderer->RemoveAudioSource(source->ssrc);
                        delete source;
                    }

                    break;
                }
                }
            }

            if (value.ValueType() == JsonValueType::String) {
                auto data = value.GetString();
                switch (op) {
                case 6: // heartbeat ack
                    auto now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
                    ws_ping = now - last_heartbeat;
                    wsPingUpdated(*this, (const uint32_t)ws_ping);
                    std::cout << "- WS Ping " << ws_ping << "ms\n";
                    break;
                }
            }

            if (value.ValueType() == JsonValueType::Null) {
                switch (op) {
                case 9:
                {
                    HostName remoteHost{ udp_endpoint.hostname };
                    EndpointPair pair{ nullptr, L"", remoteHost, to_hstring(udp_endpoint.port) };
                    co_await udp_socket.ConnectAsync(pair);

                    Stage2(nullptr);
                    connected(*this, true);
                    break;
                }
                }
            }
        }
        catch (const winrt::hresult_error& ex)
        {
            if (!ws_closed && web_socket != nullptr)
                web_socket.Close();
            std::cout << "ERROR: " << to_string(ex.message()) << "\n";
        }
    }

    IAsyncAction VoiceClient::OnWsClosed(IWebSocket socket, WebSocketClosedEventArgs ev)
    {
        try
        {
            ws_closed = true;
            std::cout << "WebSocket closed with code " << ev.Code() << " and reason " << to_string(ev.Reason()) << "\n";

            if (ev.Code() == 4006 || ev.Code() == 4009) {
                disconnected(*this, false);
                Close();
            }
            else {
                disconnected(*this, true);
                co_await ReconnectLoop();
            }
        }
        catch (const winrt::hresult_error& ex)
        {
            std::cout << "ERROR: " << to_string(ex.message()) << "\n";
        }
    }

    IAsyncAction VoiceClient::ReconnectLoop()
    {
        bool connected = false;
        uint32_t reconnection_count = 0;

        while (!connected) {
            try {
                Reset();

                std::chrono::seconds timeout = std::min<std::chrono::seconds>(5s * reconnection_count, 30s);
                std::cout << "- Reconnecting in " << timeout.count() << "s!" << std::endl;

                co_await timeout;
                cancel_voice_send = false;
                InitialiseSockets();

                ws_closed = false;
                can_resume = true;

                co_await this->ConnectAsync();
                connected = true;
            }
            catch (const winrt::hresult_error& ex) {
                ws_closed = true;
                std::cout << "ERROR: " << to_string(ex.message()) << "\n";
            }

            reconnection_count += 1;
        }
    }

    IAsyncAction VoiceClient::SendSpeakingAsync(bool speaking)
    {
        if (is_speaking == speaking)
            return;

        is_speaking = speaking;

        JsonObject payload;
        payload.SetNamedValue(L"speaking", JsonValue::CreateBooleanValue(speaking));
        payload.SetNamedValue(L"delay", JsonValue::CreateNumberValue(0));

        JsonObject disp;
        disp.SetNamedValue(L"op", JsonValue::CreateNumberValue(5));
        disp.SetNamedValue(L"d", payload);

        co_await SendJsonPayloadAsync(disp);
    }

    IOutputStream VoiceClient::GetOutputStream()
    {
        return make_self<VoiceOutputStream>(*this).as<IOutputStream>();
    }

    void VoiceClient::UpdateAudioDevices()
    {
        renderer->Initialise(options.PreferredPlaybackDevice(), options.PreferredRecordingDevice());
        renderer->BeginCapture();
        renderer->BeginRender();
    }

    IAsyncAction VoiceClient::OnUdpHeartbeat(ThreadPoolTimer timer)
    {
        try
        {
            DataWriter writer{ udp_socket.OutputStream() };

            uint64_t now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
            uint64_t count = keepalive_count;
            keepalive_count = count + 1;
            keepalive_timestamps.insert({ count, now });

            std::cout << "< Sending UDP Heartbeat " << count << "\n";

            writer.WriteUInt64(count);
            co_await writer.StoreAsync();
            writer.DetachStream();
        }
        catch (const winrt::hresult_error& ex)
        {
            std::cout << "ERROR: " << to_string(ex.message()) << "\n";
        }
    }

    void VoiceClient::HandleUdpHeartbeat(uint64_t count)
    {
        uint64_t now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        std::cout << "> Got UDP Heartbeat " << count << "!\n";

        auto itr = keepalive_timestamps.find(count);
        if (itr != keepalive_timestamps.end()) {
            uint64_t then = keepalive_timestamps.at(count);
            udp_ping = now - then;

            udpPingUpdated(*this, (const uint32_t)udp_ping);
            keepalive_timestamps.unsafe_erase(count);

            std::cout << "- UDP Ping " << udp_ping << "ms\n";
        }
    }

    IAsyncAction VoiceClient::OnUdpMessage(DatagramSocket socket, DatagramSocketMessageReceivedEventArgs ev)
    {
        auto reader = ev.GetDataReader();

        if (connection_stage == 0) {
            uint8_t buff[70];
            reader.ReadBytes(buff);

            std::string ip{ &buff[4], &buff[64] };
            ip = ip.substr(0, ip.find_first_of('\0'));

            uint16_t port = *(uint16_t*)&buff[68];

            co_await Stage3(ip, port);
        }
        else {
            auto len = reader.UnconsumedBufferLength();
            if (len == 8) {
                HandleUdpHeartbeat(reader.ReadUInt64());
            }
            else if (len > 13) {
                // prolly voice data
                auto data = new uint8_t[len];
                auto data_view = array_view<uint8_t>(data, data + len);
                reader.ReadBytes(data_view);

                ProcessRawPacket(data_view);

                delete[] data;
            }
        }
    }

    IAsyncAction VoiceClient::Stage3(std::string &ip, const uint16_t &port)
    {
        JsonObject data;
        data.SetNamedValue(L"address", JsonValue::CreateStringValue(to_hstring(ip)));
        data.SetNamedValue(L"port", JsonValue::CreateNumberValue(port));
        data.SetNamedValue(L"mode", JsonValue::CreateStringValue(mode.first));

        JsonObject opus;
        opus.SetNamedValue(L"name", JsonValue::CreateStringValue(L"opus"));
        opus.SetNamedValue(L"type", JsonValue::CreateStringValue(L"audio"));
        opus.SetNamedValue(L"priority", JsonValue::CreateNumberValue(1000));
        opus.SetNamedValue(L"payload_type", JsonValue::CreateNumberValue(120));

        JsonObject h264;
        h264.SetNamedValue(L"name", JsonValue::CreateStringValue(L"H264"));
        h264.SetNamedValue(L"type", JsonValue::CreateStringValue(L"video"));
        h264.SetNamedValue(L"priority", JsonValue::CreateNumberValue(1001));
        h264.SetNamedValue(L"payload_type", JsonValue::CreateNumberValue(101));
        h264.SetNamedValue(L"rtx_payload_type", JsonValue::CreateNumberValue(102));

        JsonArray codecs;
        codecs.Append(h264);
        codecs.Append(opus);

        JsonObject protocol_select;
        protocol_select.SetNamedValue(L"protocol", JsonValue::CreateStringValue(L"udp"));
        protocol_select.SetNamedValue(L"data", data);
        protocol_select.SetNamedValue(L"address", JsonValue::CreateStringValue(to_hstring(ip)));
        protocol_select.SetNamedValue(L"port", JsonValue::CreateNumberValue(port));
        protocol_select.SetNamedValue(L"mode", JsonValue::CreateStringValue(mode.first));
        protocol_select.SetNamedValue(L"codecs", codecs);

        JsonObject dispatch;
        dispatch.SetNamedValue(L"op", JsonValue::CreateNumberValue(1));
        dispatch.SetNamedValue(L"d", protocol_select);

        co_await SendJsonPayloadAsync(dispatch);

        JsonObject ssrc_info;
        ssrc_info.SetNamedValue(L"audio_ssrc", JsonValue::CreateNumberValue(ssrc));
        ssrc_info.SetNamedValue(L"video_ssrc", JsonValue::CreateNumberValue(0));
        ssrc_info.SetNamedValue(L"rx_ssrc", JsonValue::CreateNumberValue(0));

        JsonObject dispatch2;
        dispatch2.SetNamedValue(L"op", JsonValue::CreateNumberValue(12));
        dispatch2.SetNamedValue(L"d", ssrc_info);

        co_await SendJsonPayloadAsync(dispatch2);
        connection_stage = 1;
    }

    IAsyncAction VoiceClient::SendJsonPayloadAsync(JsonObject &payload)
    {
        try
        {
            if (!ws_closed) {
                DataWriter writer{ web_socket.OutputStream() };
                auto str = payload.Stringify();

                std::cout << "< " << to_string(str) << "\n";

                writer.WriteString(str);
                co_await writer.StoreAsync();
                writer.DetachStream();
            }
        }
        catch (const winrt::hresult_error& ex)
        {
            if (!ws_closed && web_socket != nullptr) {
                web_socket.Close();
                ws_closed = true;
            }

            std::cout << "ERROR: " << to_string(ex.message()) << "\n";
        }

    }

    void VoiceClient::Close()
    {
        if (is_disposed)
            return;

        is_disposed = true;

        Reset();

        if (sodium != nullptr) {
            SecureZeroMemory(sodium, sizeof sodium);
            delete sodium;
            sodium = nullptr;
        }
    }

    void VoiceClient::Reset()
    {
        if (heartbeat_timer != nullptr) {
            heartbeat_timer.Cancel();
            heartbeat_timer = nullptr;
        }

        if (keepalive_timer != nullptr) {
            keepalive_timer.Cancel();
            keepalive_timer = nullptr;
        }

        if (renderer != nullptr) {
            renderer->StopCapture();
            renderer->StopRender();
            delete renderer;
            renderer = nullptr;
        }

        cancel_voice_send = true;

        if (voice_thread.joinable())
            voice_thread.join();

        PCMPacket packet;
        while (voice_queue.try_pop(packet))
        {
            delete[] packet.bytes.data();
        }

        if (opus != nullptr) {
            delete opus;
            opus = nullptr;
        }

        keepalive_timestamps.clear();

        try {
            web_socket.Close();
            web_socket = nullptr;
            udp_socket.Close();
            udp_socket = nullptr;
        }
        catch (const winrt::hresult_error&) {}
    }

    VoiceClient::~VoiceClient()
    {
        Close();
    }
}