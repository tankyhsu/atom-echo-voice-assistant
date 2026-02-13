#include "ws_transport.h"
#include <esp_log.h>
#include <cstring>

#define TAG "WsTransport"

WsTransport::WsTransport() {}

WsTransport::~WsTransport() {
    Disconnect();
}

bool WsTransport::Connect(const char* uri) {
    esp_websocket_client_config_t cfg = {};
    cfg.uri = uri;
    cfg.buffer_size = 8192;
    cfg.task_stack = 8192;

    client_ = esp_websocket_client_init(&cfg);
    if (!client_) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        return false;
    }

    esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, EventHandler, this);
    esp_err_t err = esp_websocket_client_start(client_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Connecting to %s", uri);
    return true;
}

void WsTransport::Disconnect() {
    if (client_) {
        esp_websocket_client_close(client_, pdMS_TO_TICKS(2000));
        esp_websocket_client_destroy(client_);
        client_ = nullptr;
        connected_ = false;
    }
}

bool WsTransport::IsConnected() const {
    return connected_ && client_ && esp_websocket_client_is_connected(client_);
}

bool WsTransport::SendAudio(const uint8_t* data, size_t len) {
    if (!IsConnected()) return false;
    int sent = esp_websocket_client_send_bin(client_, (const char*)data, len, pdMS_TO_TICKS(1000));
    return sent >= 0;
}

bool WsTransport::SendJson(const char* json, size_t len) {
    if (!IsConnected()) return false;
    int sent = esp_websocket_client_send_text(client_, json, len, pdMS_TO_TICKS(1000));
    return sent >= 0;
}

void WsTransport::EventHandler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    auto* self = (WsTransport*)arg;
    auto* event = (esp_websocket_event_data_t*)data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected");
        self->connected_ = true;
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected");
        self->connected_ = false;
        break;

    case WEBSOCKET_EVENT_DATA:
        if (event->op_code == 0x02 && event->data_len > 0) {
            // Binary = Opus audio
            if (self->on_audio_) {
                self->on_audio_((const uint8_t*)event->data_ptr, event->data_len);
            }
        } else if (event->op_code == 0x01 && event->data_len > 0) {
            // Text = JSON control
            if (self->on_json_) {
                self->on_json_(event->data_ptr, event->data_len);
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;

    default:
        break;
    }
}
