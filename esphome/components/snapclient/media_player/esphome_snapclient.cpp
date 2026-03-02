#include "esphome/core/defines.h"
#ifdef USE_ESP32
#ifndef USE_I2S_LEGACY
#include "esphome_snapclient.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/components/network/util.h"
#include "esphome/components/media_player/media_player.h"
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_mac.h"
#if CONFIG_SNAPSERVER_USE_MDNS
#include "mdns.h"
#endif
#if CONFIG_USE_DSP_PROCESSOR
#include "dsp_processor.h"
#endif

#include "snapclient.h"
#include "player.h"

namespace esphome::snapclient {

SnapClientComponent *global_snapclient = nullptr;

static void player_set_mute(bool mute) { global_snapclient->set_mute_from_isr(mute, false); }

static void set_mute_state(bool mute) { global_snapclient->set_mute_from_isr(mute, true); }

static void audio_set_volume(int volume) { global_snapclient->set_volume_from_isr(volume); }

static void player_state_changed() {
  if (global_snapclient->playerStateChangedMutex != NULL) {
    xSemaphoreGive(global_snapclient->playerStateChangedMutex);
  }
}

void SnapClientComponent::setup() {
  ESP_LOGD(TAG, "SnapClient setup() executing");
  if (!this->lock_()) {
    this->mark_failed();
    return;
  }
  global_snapclient = this;
  ESP_LOGD(TAG, "init player");
  i2s_std_gpio_config_t i2s_pin_config0 = this->parent_->get_pin_config();
  i2s_pin_config0.dout = (gpio_num_t) this->dout_pin_;

  this->audio_dac_semaphore_ = xSemaphoreCreateMutex();
  this->audio_q_hdl_ = xQueueCreate(1, sizeof(audioDACdata_t));
  this->playerStateChangedMutex = xSemaphoreCreateBinary();

#ifdef USE_AUDIO_DAC
  if (this->audio_dac_) {
    this->audio_dac_->set_mute_on();
  }
#endif
  if (this->mute_pin_ != nullptr) {
    this->mute_pin_->setup();
    this->mute_pin_->digital_write(false);
  }

  init_player(i2s_pin_config0, this->parent_->get_port(), player_set_mute);
  add_player_state_cb(player_state_changed);
  init_snapcast(audio_set_volume, set_mute_state);

#if CONFIG_USE_DSP_PROCESSOR
  dsp_processor_init();
#endif
  this->network_initialized_ = false;
  this->waiting_for_network_logged_ = false;
  this->state = media_player::MEDIA_PLAYER_STATE_OFF;
}

void SnapClientComponent::dump_config() {
  this->refresh_snapserver_hostname_from_mdns_();

  char mac_address[18] = {0};
  uint8_t base_mac[6] = {0};
  esp_err_t mac_err = esp_efuse_mac_get_default(base_mac);
  if (mac_err != ESP_OK) {
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
    mac_err = esp_read_mac(base_mac, ESP_MAC_ETH);
#else
    mac_err = esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
#endif
  }
  if (mac_err == ESP_OK) {
    snprintf(mac_address, sizeof(mac_address), "%02X:%02X:%02X:%02X:%02X:%02X",
             base_mac[0], base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);
  }

  ESP_LOGCONFIG(TAG, "Snapclient Media Player:");
  ESP_LOGCONFIG(TAG, "  I2S Port: %u", this->parent_->get_port());
  ESP_LOGCONFIG(TAG, "  I2S Lock Held: %s", YESNO(this->has_lock_));
  ESP_LOGCONFIG(TAG, "  I2S Data Output Pin: %u", this->dout_pin_); //uint8_t
  if (this->mute_pin_) { //GPIOPin
    LOG_PIN("  Mute Pin: ", this->mute_pin_);
  } else {
    ESP_LOGCONFIG(TAG, "  Mute Pin: None");
  }
#ifdef USE_AUDIO_DAC
  ESP_LOGCONFIG(TAG, "  Audio DAC: %s", this->audio_dac_ != nullptr ? "configured" : "None");
#else
  ESP_LOGCONFIG(TAG, "  Audio DAC: None");
#endif
  ESP_LOGCONFIG(TAG, "  Mute State: %s", ONOFF(this->mute_state_));
  ESP_LOGCONFIG(TAG, "  Volume: %.0f%%", this->volume_ * 100.0f);
  ESP_LOGCONFIG(TAG, "  Client Name: %s", CONFIG_SNAPCLIENT_NAME);
  if (mac_err == ESP_OK) {
    ESP_LOGCONFIG(TAG, "  MAC Address: %s", mac_address);
  } else {
    ESP_LOGCONFIG(TAG, "  MAC Address: awaiting detection (%s)",
                  esp_err_to_name(mac_err));
  }
  ESP_LOGCONFIG(TAG, "  Network Initialized: %s", YESNO(this->network_initialized_));
  ESP_LOGCONFIG(TAG, "  Discovery Mode: %s", this->snapserver_use_mdns_ ? "mDNS" : "Static Host");
  if (!this->snapserver_hostname_.empty()) {
    ESP_LOGCONFIG(TAG, "  Snapserver Hostname: %s", this->snapserver_hostname_.c_str());
  } else if (this->snapserver_use_mdns_) {
    ESP_LOGCONFIG(TAG, "  Snapserver Hostname: awaiting mDNS discovery");
  } else {
    ESP_LOGCONFIG(TAG, "  Snapserver Hostname: not configured");
  }
  ESP_LOGCONFIG(TAG, "  Snapserver Port: %u", this->snapserver_port_);

}

void SnapClientComponent::loop() {
  static uint32_t last_hb = 0;
  if (millis() - last_hb > 5000) {
    ESP_LOGV(TAG, "SnapClient heartbeat: network=%d state=%d lock=%d", this->network_initialized_, this->state, this->has_lock_);
    last_hb = millis();
  }

  if (!this->network_initialized_) {
    const bool connected = network::is_connected();
    // Keep original behavior: only start snapclient after ESPHome reports network connectivity.
    if (!connected) {
      if (!this->waiting_for_network_logged_) {
        ESP_LOGI(TAG, "Waiting for network before starting snapclient");
        this->waiting_for_network_logged_ = true;
      }
    } else {
      this->refresh_snapserver_hostname_from_mdns_();
      ESP_LOGI(TAG, "Network connected, starting snapclient");
      start_snapcast();
      this->network_initialized_ = true;
      this->state = this->get_state_from_player_state_(this->player_state);
      this->publish_state();
    }
  }
  if (xQueueReceive(this->audio_q_hdl_, &(this->dac_data_), 0) == pdTRUE) {
    this->dac_control_();
  }

  if (xSemaphoreTake(this->playerStateChangedMutex, 0) == pdTRUE) {
    player_state_e state_new = get_player_state();
    if (state_new != this->player_state) {
      ESP_LOGD(TAG, "Player state changed: %d -> %d", this->player_state, state_new);
      if (state_new == PAUSED) {
        this->unlock_();
      } else if (this->player_state == PAUSED && !this->lock_()) {
        pause_player(true);
      }
#ifdef USE_WIFI
      if (state_new == PLAYING) {
        wifi::global_wifi_component->request_high_performance();
      } else {
        wifi::global_wifi_component->release_high_performance();
      }
#endif
      this->player_state = state_new;
      this->state = this->get_state_from_player_state_(state_new);
      this->publish_state();
    }
  }
}

void SnapClientComponent::refresh_snapserver_hostname_from_mdns_() {
#if CONFIG_SNAPSERVER_USE_MDNS
  if (!this->snapserver_use_mdns_ || !this->snapserver_hostname_.empty() || !network::is_connected()) {
    return;
  }

  esp_err_t mdns_err = mdns_init();
  if (mdns_err != ESP_OK && mdns_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGV(TAG, "mDNS init failed while refreshing hostname: %s", esp_err_to_name(mdns_err));
    return;
  }

  mdns_result_t *results = nullptr;
  mdns_err = mdns_query_ptr("_snapcast", "_tcp", 250, 3, &results);
  if (mdns_err != ESP_OK || results == nullptr) {
    if (results != nullptr) {
      mdns_query_results_free(results);
    }
    return;
  }

  uint32_t result_count = 0;
  for (mdns_result_t *result = results; result != nullptr; result = result->next) {
    result_count++;
    if (result->hostname != nullptr && result->hostname[0] != '\0') {
      this->snapserver_hostname_ = result->hostname;
      break;
    }
  }

  ESP_LOGI(TAG, "mDNS returned %u snapcast service result(s)", (unsigned int) result_count);

  if (this->snapserver_hostname_.empty()) {
    for (mdns_result_t *result = results; result != nullptr; result = result->next) {
      if (result->instance_name != nullptr && result->instance_name[0] != '\0') {
        this->snapserver_hostname_ = result->instance_name;
        break;
      }
    }
  }

  mdns_query_results_free(results);

  if (!this->snapserver_hostname_.empty()) {
    ESP_LOGI(TAG, "Snapserver discovered via mDNS: %s", this->snapserver_hostname_.c_str());
  }
#endif
}

media_player::MediaPlayerState SnapClientComponent::get_state_from_player_state_(player_state_e state) {
  switch (state) {
    case IDLE:
      return media_player::MEDIA_PLAYER_STATE_IDLE;
    case PLAYING:
      return media_player::MEDIA_PLAYER_STATE_PLAYING;
    case PAUSED:
      return media_player::MEDIA_PLAYER_STATE_PAUSED;
    default:
      return media_player::MEDIA_PLAYER_STATE_NONE;
  }
}

void SnapClientComponent::dac_control_() {
  static audioDACdata_t dac_data_old = {
      .playerMute = true,
      .stateMute = true,
      .volume = -1,
  };
  if (this->dac_data_.playerMute != dac_data_old.playerMute || this->dac_data_.stateMute != dac_data_old.stateMute) {
    // if either player or state mute is active, we need to mute the output
    bool mute = this->dac_data_.playerMute || this->dac_data_.stateMute;
    if (mute != this->mute_state_) {
      if (this->mute_pin_ != nullptr) {
        this->mute_pin_->digital_write(!mute);  // for most DACs mute = low
      }
#ifdef USE_AUDIO_DAC
      if (this->audio_dac_) {
        if (mute) {
          this->audio_dac_->set_mute_on();
        } else {
          this->audio_dac_->set_mute_off();
        }
      }
#endif
      this->mute_state_ = mute;
      ESP_LOGD(TAG, "%s", mute ? "Mute" : "Unmute");
    }
  }
  if (this->dac_data_.volume != dac_data_old.volume) {
    this->volume_ = (float) dac_data_.volume / 100;
#ifdef USE_AUDIO_DAC
    if (this->audio_dac_ != nullptr) {
      this->audio_dac_->set_volume(this->volume_);
    }
#endif
  }
  dac_data_old = this->dac_data_;
}

void SnapClientComponent::set_mute_from_isr(bool mute, bool set_state) {
  xSemaphoreTake(this->audio_dac_semaphore_, portMAX_DELAY);
  if (set_state && (mute != this->dac_data_external_.stateMute)) {
    this->dac_data_external_.stateMute = mute;
    xQueueOverwrite(this->audio_q_hdl_, &this->dac_data_external_);
  } else if (!set_state && mute != this->dac_data_external_.playerMute) {
    this->dac_data_external_.playerMute = mute;
    xQueueOverwrite(this->audio_q_hdl_, &this->dac_data_external_);
  }
  xSemaphoreGive(this->audio_dac_semaphore_);
}

void SnapClientComponent::set_volume_from_isr(int volume) {
  xSemaphoreTake(this->audio_dac_semaphore_, portMAX_DELAY);
  if (volume != this->dac_data_external_.volume) {
    this->dac_data_external_.volume = volume;
    xQueueOverwrite(this->audio_q_hdl_, &this->dac_data_external_);
  }
  xSemaphoreGive(this->audio_dac_semaphore_);
}

void SnapClientComponent::set_mute_(bool mute) {
  // send mute to snapserver
}

void SnapClientComponent::set_volume_(float volume, bool publish) {
  // send volume to snapserver
}

void SnapClientComponent::control(const media_player::MediaPlayerCall &call) {
  if (this->state == media_player::MEDIA_PLAYER_STATE_OFF || this->state == media_player::MEDIA_PLAYER_STATE_NONE) {
    ESP_LOGW(TAG, "Player is off. Ignoring control command.");
    return;
  }
  if (call.get_volume().has_value()) {
    this->set_volume_(call.get_volume().value());
  }
  if (call.get_command().has_value()) {
    switch (call.get_command().value()) {
      case media_player::MEDIA_PLAYER_COMMAND_MUTE:
        this->set_mute_(true);
        break;
      case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
        this->set_mute_(false);
        break;
      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_UP: {
        break;
      }
      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_DOWN: {
        break;
      }
      default:
        break;
    }
    switch (call.get_command().value()) {
      case media_player::MEDIA_PLAYER_COMMAND_PLAY:
        if (this->lock_())
          pause_player(false);
        break;
      case media_player::MEDIA_PLAYER_COMMAND_PAUSE:
        pause_player(true);
        break;
      case media_player::MEDIA_PLAYER_COMMAND_STOP:
        break;
      case media_player::MEDIA_PLAYER_COMMAND_TOGGLE:
        if (this->player_state == PAUSED) {
          if (this->lock_())
            pause_player(false);
        } else {
          pause_player(true);
        }
        break;
      default:
        break;
    }
  }
}

media_player::MediaPlayerTraits SnapClientComponent::get_traits() {
  auto traits = media_player::MediaPlayerTraits();
  traits.clear_feature_flags(
      media_player::MediaPlayerEntityFeature::PLAY_MEDIA | media_player::MediaPlayerEntityFeature::BROWSE_MEDIA |
      media_player::MediaPlayerEntityFeature::STOP | media_player::MediaPlayerEntityFeature::VOLUME_SET |
      media_player::MediaPlayerEntityFeature::VOLUME_MUTE | media_player::MediaPlayerEntityFeature::MEDIA_ANNOUNCE);
  traits.add_feature_flags(media_player::MediaPlayerEntityFeature::PLAY |
                           media_player::MediaPlayerEntityFeature::PAUSE);
  return traits;
};

}  // namespace esphome::snapclient

#endif
#endif
