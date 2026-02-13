#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <esp_websocket_client.h>

class WsTransport {
public:
    using AudioCallback = std::function<void(const uint8_t* data, size_t len)>;
    using JsonCallback = std::function<void(const char* json, size_t len)>;

    WsTransport();
    ~WsTransport();

    void SetAudioCallback(AudioCallback cb) { on_audio_ = cb; }
    void SetJsonCallback(JsonCallback cb) { on_json_ = cb; }

    bool Connect(const char* uri);
    void Disconnect();
    bool IsConnected() const;

    bool SendAudio(const uint8_t* data, size_t len);
    bool SendJson(const char* json, size_t len);

private:
    static void EventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);

    esp_websocket_client_handle_t client_ = nullptr;
    AudioCallback on_audio_;
    JsonCallback on_json_;
    bool connected_ = false;
};
