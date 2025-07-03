/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <lib/support/CodeUtils.h>
#include <lib/support/SafeInt.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ESP32/ESP32Utils.h>
#include <platform/ESP32/NetworkCommissioningDriver.h>

#include "esp_wifi.h"

#include <limits>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace ::chip;
using namespace ::chip::DeviceLayer::Internal;
namespace chip {
namespace DeviceLayer {
namespace NetworkCommissioning {

namespace {
constexpr char kWiFiSSIDKeyName[]        = "wifi-ssid";
constexpr char kWiFiCredentialsKeyName[] = "wifi-pass";
static uint8_t WiFiSSIDStr[DeviceLayer::Internal::kMaxWiFiSSIDLength];
} // namespace

BitFlags<WiFiSecurityBitmap> ConvertSecurityType(wifi_auth_mode_t authMode)
{
    BitFlags<WiFiSecurityBitmap> securityType;
    switch (authMode)
    {
    case WIFI_AUTH_OPEN:
        securityType.Set(WiFiSecurity::kUnencrypted);
        break;
    case WIFI_AUTH_WEP:
        securityType.Set(WiFiSecurity::kWep);
        break;
    case WIFI_AUTH_WPA_PSK:
        securityType.Set(WiFiSecurity::kWpaPersonal);
        break;
    case WIFI_AUTH_WPA2_PSK:
        securityType.Set(WiFiSecurity::kWpa2Personal);
        break;
    case WIFI_AUTH_WPA_WPA2_PSK:
        securityType.Set(WiFiSecurity::kWpa2Personal);
        securityType.Set(WiFiSecurity::kWpaPersonal);
        break;
    case WIFI_AUTH_WPA3_PSK:
        securityType.Set(WiFiSecurity::kWpa3Personal);
        break;
    case WIFI_AUTH_WPA2_WPA3_PSK:
        securityType.Set(WiFiSecurity::kWpa3Personal);
        securityType.Set(WiFiSecurity::kWpa2Personal);
        break;
    default:
        break;
    }
    return securityType;
}

CHIP_ERROR GetConfiguredNetwork(Network & network)
{
    wifi_ap_record_t ap_info;
    esp_err_t err;
    err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK)
    {
        return chip::DeviceLayer::Internal::ESP32Utils::MapError(err);
    }
    static_assert(chip::DeviceLayer::Internal::kMaxWiFiSSIDLength <= UINT8_MAX, "SSID length might not fit in length");
    uint8_t length =
        static_cast<uint8_t>(strnlen(reinterpret_cast<const char *>(ap_info.ssid), DeviceLayer::Internal::kMaxWiFiSSIDLength));
    if (length > sizeof(network.networkID))
    {
        return CHIP_ERROR_INTERNAL;
    }
    memcpy(network.networkID, ap_info.ssid, length);
    network.networkIDLen = length;
    return CHIP_NO_ERROR;
}

CHIP_ERROR ESPWiFiDriver::Init(NetworkStatusChangeCallback * networkStatusChangeCallback)
{
    wifi_config_t stationConfig;
    if (esp_wifi_get_config(WIFI_IF_STA, &stationConfig) == ESP_OK && stationConfig.sta.ssid[0] != 0)
    {
        uint8_t ssidLen = static_cast<uint8_t>(
            strnlen(reinterpret_cast<const char *>(stationConfig.sta.ssid), DeviceLayer::Internal::kMaxWiFiSSIDLength));
        memcpy(mStagingNetwork.ssid, stationConfig.sta.ssid, ssidLen);
        mStagingNetwork.ssidLen = ssidLen;

        uint8_t credentialsLen = static_cast<uint8_t>(
            strnlen(reinterpret_cast<const char *>(stationConfig.sta.password), DeviceLayer::Internal::kMaxWiFiKeyLength));

        memcpy(mStagingNetwork.credentials, stationConfig.sta.password, credentialsLen);
        mStagingNetwork.credentialsLen = credentialsLen;
    }

    mpScanCallback         = nullptr;
    mpConnectCallback      = nullptr;
    mpStatusChangeCallback = networkStatusChangeCallback;

    // If the network configuration backup exists, it means that the device has been rebooted with
    // the fail-safe armed. Since ESP-WiFi persists all wifi credentials changes, the backup must
    // be restored on the boot. If there's no backup, the below function is a no-op.
    RevertConfiguration();

    return CHIP_NO_ERROR;
}

void ESPWiFiDriver::Shutdown()
{
    mpStatusChangeCallback = nullptr;
}

CHIP_ERROR ESPWiFiDriver::CommitConfiguration()
{
    PersistedStorage::KeyValueStoreMgr().Delete(kWiFiSSIDKeyName);
    PersistedStorage::KeyValueStoreMgr().Delete(kWiFiCredentialsKeyName);

    return CHIP_NO_ERROR;
}

CHIP_ERROR ESPWiFiDriver::RevertConfiguration()
{
    WiFiNetwork network;
    Network configuredNetwork;
    size_t ssidLen        = 0;
    size_t credentialsLen = 0;

    CHIP_ERROR error = PersistedStorage::KeyValueStoreMgr().Get(kWiFiSSIDKeyName, network.ssid, sizeof(network.ssid), &ssidLen);
    VerifyOrReturnError(error != CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND, CHIP_NO_ERROR);
    VerifyOrExit(CanCastTo<uint8_t>(ssidLen), error = CHIP_ERROR_INTERNAL);
    VerifyOrExit(PersistedStorage::KeyValueStoreMgr().Get(kWiFiCredentialsKeyName, network.credentials, sizeof(network.credentials),
                                                          &credentialsLen) == CHIP_NO_ERROR,
                 error = CHIP_ERROR_INTERNAL);
    VerifyOrExit(CanCastTo<uint8_t>(credentialsLen), error = CHIP_ERROR_INTERNAL);

    network.ssidLen        = static_cast<uint8_t>(ssidLen);
    network.credentialsLen = static_cast<uint8_t>(credentialsLen);
    mStagingNetwork        = network;

    if (GetConfiguredNetwork(configuredNetwork) == CHIP_NO_ERROR)
    {
        VerifyOrExit(!NetworkMatch(mStagingNetwork, ByteSpan(configuredNetwork.networkID, configuredNetwork.networkIDLen)),
                     error = CHIP_NO_ERROR);
    }

    if (error == CHIP_NO_ERROR)
    {
        // ConnectWiFiNetwork can work with empty mStagingNetwork (ssidLen = 0).
        error = ConnectWiFiNetwork(reinterpret_cast<const char *>(mStagingNetwork.ssid), mStagingNetwork.ssidLen,
                                   reinterpret_cast<const char *>(mStagingNetwork.credentials), mStagingNetwork.credentialsLen);
    }

exit:

    // Remove the backup.
    PersistedStorage::KeyValueStoreMgr().Delete(kWiFiSSIDKeyName);
    PersistedStorage::KeyValueStoreMgr().Delete(kWiFiCredentialsKeyName);

    return error;
}

bool ESPWiFiDriver::NetworkMatch(const WiFiNetwork & network, ByteSpan networkId)
{
    return networkId.size() == network.ssidLen && memcmp(networkId.data(), network.ssid, network.ssidLen) == 0;
}

Status ESPWiFiDriver::AddOrUpdateNetwork(ByteSpan ssid, ByteSpan credentials, MutableCharSpan & outDebugText,
                                         uint8_t & outNetworkIndex)
{
    ESP_LOGI(TAG, "ESPWiFiDriver - AddOrUpdateNetwork");
    outDebugText.reduce_size(0);
    outNetworkIndex = 0;
    VerifyOrReturnError(mStagingNetwork.ssidLen == 0 || NetworkMatch(mStagingNetwork, ssid), Status::kBoundsExceeded);
    VerifyOrReturnError(credentials.size() <= sizeof(mStagingNetwork.credentials), Status::kOutOfRange);
    VerifyOrReturnError(ssid.size() <= sizeof(mStagingNetwork.ssid), Status::kOutOfRange);
    VerifyOrReturnError(BackupConfiguration() == CHIP_NO_ERROR, Status::kUnknownError);

    memcpy(mStagingNetwork.credentials, credentials.data(), credentials.size());
    mStagingNetwork.credentialsLen = static_cast<decltype(mStagingNetwork.credentialsLen)>(credentials.size());

    memcpy(mStagingNetwork.ssid, ssid.data(), ssid.size());
    mStagingNetwork.ssidLen = static_cast<decltype(mStagingNetwork.ssidLen)>(ssid.size());

    return Status::kSuccess;
}

Status ESPWiFiDriver::RemoveNetwork(ByteSpan networkId, MutableCharSpan & outDebugText, uint8_t & outNetworkIndex)
{
    outDebugText.reduce_size(0);
    outNetworkIndex = 0;
    VerifyOrReturnError(NetworkMatch(mStagingNetwork, networkId), Status::kNetworkIDNotFound);
    VerifyOrReturnError(BackupConfiguration() == CHIP_NO_ERROR, Status::kUnknownError);

    // Use empty ssid for representing invalid network
    mStagingNetwork.ssidLen = 0;
    return Status::kSuccess;
}

Status ESPWiFiDriver::ReorderNetwork(ByteSpan networkId, uint8_t index, MutableCharSpan & outDebugText)
{
    outDebugText.reduce_size(0);

    // Only one network is supported now
    VerifyOrReturnError(index == 0, Status::kOutOfRange);
    VerifyOrReturnError(NetworkMatch(mStagingNetwork, networkId), Status::kNetworkIDNotFound);
    return Status::kSuccess;
}

CHIP_ERROR ESPWiFiDriver::ConnectWiFiNetwork(const char * ssid, uint8_t ssidLen, const char * key, uint8_t keyLen, uint8_t channel)
{
    // If device is already connected to WiFi, then disconnect the WiFi,
    // clear the WiFi configurations and add the newly provided WiFi configurations.
    if (chip::DeviceLayer::Internal::ESP32Utils::IsStationProvisioned())
    {
        ChipLogProgress(DeviceLayer, "Disconnecting WiFi station interface");
        esp_err_t err = esp_wifi_disconnect();
        if (err != ESP_OK)
        {
            ChipLogError(DeviceLayer, "esp_wifi_disconnect() failed: %s", esp_err_to_name(err));
            return chip::DeviceLayer::Internal::ESP32Utils::MapError(err);
        }
        CHIP_ERROR error = chip::DeviceLayer::Internal::ESP32Utils::ClearWiFiStationProvision();
        if (error != CHIP_NO_ERROR)
        {
            ChipLogError(DeviceLayer, "ClearWiFiStationProvision failed: %s", chip::ErrorStr(error));
            return chip::DeviceLayer::Internal::ESP32Utils::MapError(err);
        }
    }

    ReturnErrorOnFailure(ConnectivityMgr().SetWiFiStationMode(ConnectivityManager::kWiFiStationMode_Disabled));

    wifi_config_t wifiConfig;

    // Set the wifi configuration
    memset(&wifiConfig, 0, sizeof(wifiConfig));
    memcpy(wifiConfig.sta.ssid, ssid, std::min(ssidLen, static_cast<uint8_t>(sizeof(wifiConfig.sta.ssid))));
    memcpy(wifiConfig.sta.password, key, std::min(keyLen, static_cast<uint8_t>(sizeof(wifiConfig.sta.password))));
    wifiConfig.sta.listen_interval = 10;
    
    // If we know the channel, set it in the configuration
    if (channel > 0) {
        wifiConfig.sta.channel = channel;
        ChipLogProgress(DeviceLayer, "Setting WiFi connection to use channel %d", channel);
    }

    // Configure the ESP WiFi interface.
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifiConfig);
    if (err != ESP_OK)
    {
        ChipLogError(DeviceLayer, "esp_wifi_set_config() failed: %s", esp_err_to_name(err));
        return chip::DeviceLayer::Internal::ESP32Utils::MapError(err);
    }

    ReturnErrorOnFailure(ConnectivityMgr().SetWiFiStationMode(ConnectivityManager::kWiFiStationMode_Disabled));
    return ConnectivityMgr().SetWiFiStationMode(ConnectivityManager::kWiFiStationMode_Enabled);
}

#if CHIP_DEVICE_CONFIG_SUPPORTS_CONCURRENT_CONNECTION
CHIP_ERROR ESPWiFiDriver::DisconnectFromNetwork()
{
    if (chip::DeviceLayer::Internal::ESP32Utils::IsStationProvisioned())
    {
        // Attaching to an empty network will disconnect the network.
        ReturnErrorOnFailure(ConnectWiFiNetwork(nullptr, 0, nullptr, 0));
    }
    return CHIP_NO_ERROR;
}
#endif


#define WIFI_CONNECTED_DELAY 1000
#define WIFI_FAILED_DELAY 5000

void ESPWiFiDriver::OnConnectWiFiNetwork()
{
    // Create a mutex or semaphore to ensure this function completes before Matter continues
    static bool onConnectInProgress = false;
    
    // Only one connection callback should be processed at a time
    if (onConnectInProgress) {
        ESP_LOGI(TAG, "Another connect callback is already in progress, ignoring");
        return;
    }
    
    if (mpConnectCallback)
    {
        onConnectInProgress = true;
        
        // Cancel the connection failure timer since we've connected successfully
        DeviceLayer::SystemLayer().CancelTimer(OnConnectWiFiNetworkFailed, NULL);
        
        // Ensure a full delay to allow the system to stabilize after connection
        ESP_LOGI(TAG, "ONEDGE - WIFI CONNECTED - delay %d ms - START", WIFI_CONNECTED_DELAY);
        
        // Use a blocking delay to ensure the system is fully ready
        vTaskDelay(pdMS_TO_TICKS(WIFI_CONNECTED_DELAY));
        
        ESP_LOGI(TAG, "ONEDGE - WIFI CONNECTED - delay %d ms - END", WIFI_CONNECTED_DELAY);
        
        // Callback AFTER the delay completes
        if (mpConnectCallback) {
            mpConnectCallback->OnResult(Status::kSuccess, CharSpan(), 0);
            mpConnectCallback = nullptr;
        }
        
        onConnectInProgress = false;
    }
}

void ESPWiFiDriver::OnConnectWiFiNetworkFailed()
{
    if (mpConnectCallback)
    {
        ESP_LOGI(TAG, "ONEDGE - WIFI FAILED - Disabling WiFi radio to save power");
        
        // Temporarily disable WiFi radio to save power
        esp_wifi_stop();
        
        // Add a significant delay to allow capacitors to recharge
        ESP_LOGI(TAG, "ONEDGE - WIFI FAILED - delay %d ms - START", WIFI_FAILED_DELAY);
        vTaskDelay(pdMS_TO_TICKS(WIFI_FAILED_DELAY));
        ESP_LOGI(TAG, "ONEDGE - WIFI FAILED - delay %d ms - END", WIFI_FAILED_DELAY);
        
        // Re-enable WiFi radio
        esp_wifi_start();
        ESP_LOGI(TAG, "ONEDGE - WIFI FAILED - Re-enabling WiFi radio");

        mpConnectCallback->OnResult(Status::kNetworkNotFound, CharSpan(), 0);
        mpConnectCallback = nullptr;
    }
}

void ESPWiFiDriver::OnConnectWiFiNetworkFailed(chip::System::Layer * aLayer, void * aAppState)
{
    CHIP_ERROR error = chip::DeviceLayer::Internal::ESP32Utils::ClearWiFiStationProvision();
    if (error != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "ClearWiFiStationProvision failed: %s", chip::ErrorStr(error));
    }
    ESPWiFiDriver::GetInstance().OnConnectWiFiNetworkFailed();
}

void ESPWiFiDriver::ConnectNetwork(ByteSpan networkId, ConnectCallback * callback)
{
    CHIP_ERROR err          = CHIP_NO_ERROR;
    Status networkingStatus = Status::kSuccess;
    Network configuredNetwork;
    const uint32_t secToMiliSec = 1000;
    
    // Declare all variables at the beginning of the function
    // to avoid jumping over initializations
    bool target_found = false;
    uint8_t target_channel = 0;
    int8_t target_rssi = -128;

    if (!NetworkMatch(mStagingNetwork, networkId)) {
        networkingStatus = Status::kNetworkIDNotFound;
        goto exit;
    }
    if (BackupConfiguration() != CHIP_NO_ERROR) {
        networkingStatus = Status::kUnknownError;
        goto exit;
    }
    if (mpConnectCallback != nullptr) {
        networkingStatus = Status::kUnknownError;
        goto exit;
    }
    ChipLogProgress(NetworkProvisioning, "ESP NetworkCommissioningDelegate: SSID: %.*s", static_cast<int>(networkId.size()),
                    networkId.data());
    if (CHIP_NO_ERROR == GetConfiguredNetwork(configuredNetwork))
    {
        if (NetworkMatch(mStagingNetwork, ByteSpan(configuredNetwork.networkID, configuredNetwork.networkIDLen)))
        {
            if (callback)
            {
                callback->OnResult(Status::kSuccess, CharSpan(), 0);
            }
            return;
        }
    }

    // Store the callback for later use
    mpConnectCallback = callback;

    // First, check if we already have the network in our scan results buffer
    
    // Look for the target network in our existing scan results first
    if (mScanResultCount > 0) {
        ChipLogProgress(NetworkProvisioning, "Checking existing scan results for target network...");
        
        for (uint16_t i = 0; i < mScanResultCount; i++) {
            // Check if this is our target network
            if (memcmp(mStagingNetwork.ssid, mScanResultsBuffer[i].ssid, mStagingNetwork.ssidLen) == 0) {
                target_found = true;
                target_channel = mScanResultsBuffer[i].primary;
                target_rssi = mScanResultsBuffer[i].rssi;
                
                // Log the found network details
                ChipLogProgress(NetworkProvisioning, "Target network found in existing scan results: Channel %d, RSSI %d, Auth %d",
                               mScanResultsBuffer[i].primary, mScanResultsBuffer[i].rssi, mScanResultsBuffer[i].authmode);
                break;
            }
        }
    }
    
    // If not found in existing results, perform a segmented scan like we do in StartScanWiFiNetworks
    if (!target_found) {
        ChipLogProgress(NetworkProvisioning, "Target network not in existing scan results, performing segmented scan...");
        
        // Use StartScanWiFiNetworks with the specific SSID to find our target network
        // This is a blocking function that will search for the network across all channels
        err = StartScanWiFiNetworks(ByteSpan(reinterpret_cast<const uint8_t*>(mStagingNetwork.ssid), mStagingNetwork.ssidLen));
        if (err != CHIP_NO_ERROR) {
            ChipLogError(NetworkProvisioning, "Failed to start scan for target network: %s", chip::ErrorStr(err));
            goto error_exit;
        }
        
        // Now check our updated scan results for the target network
        if (mScanResultCount > 0) {
            for (uint16_t i = 0; i < mScanResultCount; i++) {
                // Check if this is our target network
                if (memcmp(mStagingNetwork.ssid, mScanResultsBuffer[i].ssid, mStagingNetwork.ssidLen) == 0) {
                    target_found = true;
                    target_channel = mScanResultsBuffer[i].primary;
                    target_rssi = mScanResultsBuffer[i].rssi;
                    
                    // Log the found network details
                    ChipLogProgress(NetworkProvisioning, "Target network found in segmented scan: Channel %d, RSSI %d, Auth %d",
                                   mScanResultsBuffer[i].primary, mScanResultsBuffer[i].rssi, mScanResultsBuffer[i].authmode);
                    break;
                }
            }
        }
        
        if (!target_found) {
            ChipLogError(NetworkProvisioning, "Target network not found in segmented scan");
            err = CHIP_ERROR_NOT_FOUND;
            goto error_exit;
        }
    }
    
    // Now connect to the network
    ChipLogProgress(NetworkProvisioning, "Network found, connecting to WiFi network on channel %d...", target_channel);
    err = ConnectWiFiNetwork(reinterpret_cast<const char *>(mStagingNetwork.ssid), mStagingNetwork.ssidLen,
                            reinterpret_cast<const char *>(mStagingNetwork.credentials), mStagingNetwork.credentialsLen,
                            target_channel);

    if (err == CHIP_NO_ERROR) {
        // Start a timer to detect connection failure
        err = DeviceLayer::SystemLayer().StartTimer(
            static_cast<System::Clock::Timeout>(kWiFiConnectNetworkTimeoutSeconds * secToMiliSec), OnConnectWiFiNetworkFailed, NULL);
    }

exit:
    if (err != CHIP_NO_ERROR)
    {
        networkingStatus = Status::kUnknownError;
    }
    if (networkingStatus != Status::kSuccess)
    {
        ChipLogError(NetworkProvisioning, "Failed to connect to WiFi network:%s", chip::ErrorStr(err));
        mpConnectCallback = nullptr;
        callback->OnResult(networkingStatus, CharSpan(), 0);
    }
    return;

error_exit:
    ChipLogError(NetworkProvisioning, "Failed to connect to WiFi network:%s", chip::ErrorStr(err));
    mpConnectCallback = nullptr;
    if (callback) {
        callback->OnResult(Status::kNetworkNotFound, CharSpan(), 0);
    }
    return;
}

CHIP_ERROR ESPWiFiDriver::StartScanWiFiNetworks(ByteSpan ssid)
{
    ChipLogProgress(DeviceLayer, "ONEDGE - Delay 1000ms - START");
    vTaskDelay(pdMS_TO_TICKS(1000));
    ChipLogProgress(DeviceLayer, "ONEDGE - Delay 1000ms - STOP");
    esp_err_t err = ESP_OK;
    //ESP_LOGI(TAG, "WIFI SCAN CALLED - PATCH HERE");
    
    // If an SSID is provided, first check if it's already in our known AP list
    if (!ssid.empty() && mScanResultCount > 0) {
        ChipLogProgress(DeviceLayer, "Checking existing scan results for SSID: %.*s", 
                       static_cast<int>(ssid.size()), ssid.data());
        
        for (uint16_t i = 0; i < mScanResultCount; i++) {
            size_t apSsidLen = strnlen(reinterpret_cast<const char*>(mScanResultsBuffer[i].ssid), 
                                      sizeof(mScanResultsBuffer[i].ssid));
            if (apSsidLen == ssid.size() && 
                memcmp(mScanResultsBuffer[i].ssid, ssid.data(), ssid.size()) == 0) {
                
                // Found the target SSID in existing results
                ChipLogProgress(DeviceLayer, "Target SSID found in existing scan results!");
                ChipLogProgress(DeviceLayer, "  SSID=\"%.*s\", Ch=%d, RSSI=%d, Auth=%d", 
                               static_cast<int>(ssid.size()), ssid.data(),
                               mScanResultsBuffer[i].primary, 
                               mScanResultsBuffer[i].rssi,
                               mScanResultsBuffer[i].authmode);
                
                // Since we found it in cache, we need to notify that the "scan" is complete
                // Schedule OnScanWiFiNetworkDone to be called on the Matter event loop
                chip::DeviceLayer::SystemLayer().ScheduleWork([](chip::System::Layer *, void * context) {
                    auto * driver = static_cast<ESPWiFiDriver *>(context);
                    driver->OnScanWiFiNetworkDone();
                }, this);
                
                return CHIP_NO_ERROR;
            }
        }
        
        ChipLogProgress(DeviceLayer, "Target SSID not found in existing results, proceeding with scan");
    }
    
//Bypass the ssid scan and use chunked scan
//    // If an SSID is provided, use standard single scan
//    if (!ssid.empty())
//    {
//        ESP_LOGI(TAG, "%s", ssid.data());
//        wifi_scan_config_t scan_config = { 0 };
//        memset(WiFiSSIDStr, 0, sizeof(WiFiSSIDStr));
//        memcpy(WiFiSSIDStr, ssid.data(), ssid.size());
//        scan_config.ssid = WiFiSSIDStr;
//        err = esp_wifi_scan_start(&scan_config, false);
//    }
//    else
    {
        ESP_LOGI(TAG, "NO SSID PROVIDED - Using chunked scan");
        
        // Define constants for chunked scanning
        constexpr uint8_t kTotalChannels = 13;      // Total 2.4GHz WiFi channels
        constexpr uint8_t kChunksCount = 3;         // Number of chunks to divide scanning into
        constexpr uint8_t kChannelsPerChunk = 5;    // Approximate channels per chunk
        constexpr uint16_t kRechargePauseMs = 1000; // Pause between chunks
        constexpr uint8_t kScansPerChunk = 2;       // Number of times to scan each chunk

        // Reset our results buffer
        mScanResultCount = 0;
        
        // Perform chunked scanning - each chunk scans a subset of channels
        for (uint8_t chunk = 0; chunk < kChunksCount; chunk++) 
        {
            // Calculate the channel for this chunk with explicit bounds checking
            uint8_t channel;
            if (chunk == 0) {
                channel = 1; // First channel
            } else {
                // Ensure we don't overflow uint8_t by using proper bounds checking
                unsigned int calculatedChannel = 1U + (static_cast<unsigned int>(chunk) * static_cast<unsigned int>(kChannelsPerChunk));
                // Ensure the channel is within valid range (1-13)
                channel = static_cast<uint8_t>(calculatedChannel <= 13U ? calculatedChannel : 13U);
            }
            if (channel > kTotalChannels) {
                break; // We've covered all channels
            }
            
            ChipLogProgress(DeviceLayer, "Starting scan chunk %d/%d on channel %d (%d scans per chunk)", 
                            chunk+1, kChunksCount, channel, kScansPerChunk);
            
            // Configure scan for just this channel
            wifi_scan_config_t scan_config = { 0 };
            scan_config.channel = channel;
            
            // Scan settings for better detection
            scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
            wifi_scan_time_t scan_time = { 0 };
            
            // Perform multiple scans per chunk
            for (uint8_t scan_count = 0; scan_count < kScansPerChunk; scan_count++) {
                ChipLogProgress(DeviceLayer, "  Scan %d/%d for chunk %d on channel %d", 
                               scan_count+1, kScansPerChunk, chunk+1, channel);
                
                // Start scan for this channel in blocking mode
                err = esp_wifi_scan_start(&scan_config, true);
                if (err != ESP_OK) {
                    ChipLogError(DeviceLayer, "Failed to start scan %d for chunk %d: %s", 
                               scan_count+1, chunk+1, esp_err_to_name(err));
                    continue; // Try next scan
                }
                
                // Immediately process results from this scan
                uint16_t ap_count = 0;
                err = esp_wifi_scan_get_ap_num(&ap_count);
                if (err != ESP_OK) {
                    ChipLogError(DeviceLayer, "Failed to get AP count for scan %d of chunk %d: %s", 
                               scan_count+1, chunk+1, esp_err_to_name(err));
                    ap_count = 0;
                }
                
                // If this isn't the last scan for this chunk, add a small pause
                if (scan_count < kScansPerChunk - 1) {
                    ChipLogProgress(DeviceLayer, "  Pausing between scans for channel %d", channel);
                    vTaskDelay(pdMS_TO_TICKS(kRechargePauseMs)); // 1000ms pause between scans of same channel
                }
            }
            // After all scans for this chunk are complete, process the results
            uint16_t ap_count = 0;
            err = esp_wifi_scan_get_ap_num(&ap_count);
            if (err != ESP_OK) {
                ChipLogError(DeviceLayer, "Failed to get final AP count for chunk %d: %s", 
                           chunk+1, esp_err_to_name(err));
                ap_count = 0;
            }
            
            if (ap_count > 0) {
                ChipLogProgress(DeviceLayer, "Found %d APs in chunk %d after %d scans", ap_count, chunk+1, kScansPerChunk);
                
                // Allocate temporary buffer for this chunk's results
                wifi_ap_record_t* temp_records = static_cast<wifi_ap_record_t*>(
                    malloc(ap_count * sizeof(wifi_ap_record_t))
                );
                
                if (temp_records != nullptr) {
                    // Get the scan records
                    uint16_t to_copy = ap_count;
                    err = esp_wifi_scan_get_ap_records(&to_copy, temp_records);
                    if (err == ESP_OK) {
                        // Log detailed information about APs found in this chunk
                        ChipLogProgress(DeviceLayer, "===== Chunk %d/%d (Channel %d) Details =====", 
                                       chunk+1, kChunksCount, channel);
                        for (uint16_t i = 0; i < to_copy; i++) {
                            const char* ap_ssid = reinterpret_cast<const char*>(temp_records[i].ssid);
                            char bssid_str[18]; // XX:XX:XX:XX:XX:XX + null terminator
                            snprintf(bssid_str, sizeof(bssid_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                                    temp_records[i].bssid[0], temp_records[i].bssid[1], temp_records[i].bssid[2],
                                    temp_records[i].bssid[3], temp_records[i].bssid[4], temp_records[i].bssid[5]);
                            
                            ChipLogProgress(DeviceLayer, "  AP %d: SSID=\"%s\", BSSID=%s, Ch=%d, RSSI=%d, Auth=%d", 
                                           i+1, ap_ssid, bssid_str, temp_records[i].primary,
                                           temp_records[i].rssi, temp_records[i].authmode);
                        }
                        ChipLogProgress(DeviceLayer, "========================================");
                        
                        // Process and deduplicate the results
                        for (uint16_t i = 0; i < to_copy; i++) {
                            // Check if this AP is already in our results
                            bool is_duplicate = false;
                            
                            for (uint16_t j = 0; j < mScanResultCount; j++) {
                                // Compare BSSIDs (MAC addresses)
                                if (memcmp(temp_records[i].bssid, mScanResultsBuffer[j].bssid, 6) == 0) {
                                    is_duplicate = true;
                                    // If this is a stronger signal, update the existing entry
                                    if (temp_records[i].rssi > mScanResultsBuffer[j].rssi) {
                                        mScanResultsBuffer[j] = temp_records[i];
                                    }
                                    break;
                                }
                            }
                            
                            // If not a duplicate and we have space, add it to our results
                            if (!is_duplicate && mScanResultCount < kMaxWiFiScanResults) {
                                mScanResultsBuffer[mScanResultCount++] = temp_records[i];
                            }
                        }
                    } else {
                        ChipLogError(DeviceLayer, "Failed to get scan records for chunk %d: %s", 
                                   chunk+1, esp_err_to_name(err));
                    }
                    
                    // Clean up temporary buffer
                    free(temp_records);
                }
            }
            
            // Pause after scan chunk to allow power system to recharge

            
            // Check if we should stop early when looking for a specific SSID
            if (!ssid.empty() && mScanResultCount > 0) {
                bool targetFound = false;
                for (uint16_t i = 0; i < mScanResultCount; i++) {
                    // Check if this AP's SSID matches our target
                    size_t apSsidLen = strnlen(reinterpret_cast<const char*>(mScanResultsBuffer[i].ssid), 
                                               sizeof(mScanResultsBuffer[i].ssid));
                    if (apSsidLen == ssid.size() && 
                        memcmp(mScanResultsBuffer[i].ssid, ssid.data(), ssid.size()) == 0) {
                        targetFound = true;
                        ChipLogProgress(DeviceLayer, "Target SSID found in chunk %d, stopping scan early", chunk+1);
                        ChipLogProgress(DeviceLayer, "Found target: SSID=\"%.*s\", Ch=%d, RSSI=%d", 
                                       static_cast<int>(ssid.size()), ssid.data(),
                                       mScanResultsBuffer[i].primary, mScanResultsBuffer[i].rssi);
                        break;
                    }
                }
                
                if (targetFound) {
                    ChipLogProgress(DeviceLayer, "Early stop: Target SSID found, skipping remaining chunks");
                    break; // Exit the chunk loop early
                }
            }
            ChipLogProgress(DeviceLayer, "Pausing after scan chunk %d for power recharge", chunk+1);
            vTaskDelay(pdMS_TO_TICKS(kRechargePauseMs));
        }
        
        // After all chunks are done, log detailed summary but DO NOT call the callback
        // The callback will be called from OnScanWiFiNetworkDone after all chunks are processed
        if (mScanResultCount > 0) {
            ChipLogProgress(DeviceLayer, "===== CHUNKED SCAN COMPLETE =====");
            ChipLogProgress(DeviceLayer, "Chunked scan found %d unique networks", mScanResultCount);
            
            // Log information about all unique networks found
            for (uint16_t i = 0; i < mScanResultCount; i++) {
                const char* result_ssid = reinterpret_cast<const char*>(mScanResultsBuffer[i].ssid);
                char bssid_str[18]; // XX:XX:XX:XX:XX:XX + null terminator
                snprintf(bssid_str, sizeof(bssid_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                        mScanResultsBuffer[i].bssid[0], mScanResultsBuffer[i].bssid[1], 
                        mScanResultsBuffer[i].bssid[2], mScanResultsBuffer[i].bssid[3], 
                        mScanResultsBuffer[i].bssid[4], mScanResultsBuffer[i].bssid[5]);
                
                ChipLogProgress(DeviceLayer, "  Network %d: SSID=\"%s\", BSSID=%s, Ch=%d, RSSI=%d, Auth=%d", 
                               i+1, result_ssid, bssid_str, mScanResultsBuffer[i].primary,
                               mScanResultsBuffer[i].rssi, mScanResultsBuffer[i].authmode);
            }
            ChipLogProgress(DeviceLayer, "==================================");
        } else {
            ChipLogProgress(DeviceLayer, "Chunked scan complete, no networks found");
        }
        
        // Set flags for tracking chunked scan events
        mChunkedScanInProgress = true;
        mChunksCompleted = 0; // Will be incremented in OnScanWiFiNetworkDone
        mTotalChunks = kChunksCount; // Store total chunks for tracking
        
        // Return success since we've already processed the results
        return CHIP_NO_ERROR;
    }
    
    if (err != ESP_OK)
    {
        return chip::DeviceLayer::Internal::ESP32Utils::MapError(err);
    }
    return CHIP_NO_ERROR;
}

void ESPWiFiDriver::OnScanWiFiNetworkDone()
{
    if (!mpScanCallback)
    {
        ChipLogProgress(DeviceLayer, "No scan callback");
        return;
    }
    
    // Check if this is part of a chunked scan
    if (mChunkedScanInProgress) {
        // Increment the chunks completed counter
        mChunksCompleted++;
        ChipLogProgress(DeviceLayer, "Chunked scan event %d of %d received", mChunksCompleted, mTotalChunks);
        
        // If this is not the last chunk, just wait for more events
        if (mChunksCompleted < mTotalChunks) {
            return;
        }
        
        // This is the last chunk, now we can process our accumulated results
        ChipLogProgress(DeviceLayer, "All chunked scan events received, processing results");
        
        if (mScanResultCount > 0) {
            ChipLogProgress(DeviceLayer, "===== FINAL SCAN RESULTS =====");
            ChipLogProgress(DeviceLayer, "Using %d accumulated scan results from chunked scan", mScanResultCount);
            
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 3)
            // ESP-IDF 5.1.3+ only needs size
            ESPScanResponseIterator iter(mScanResultCount);
#else
            // Create an iterator with our accumulated results
            ESPScanResponseIterator iter(mScanResultCount, const_cast<const wifi_ap_record_t*>(&mScanResultsBuffer[0]));
#endif
            mpScanCallback->OnFinished(Status::kSuccess, CharSpan(), &iter);
        } else {
            ChipLogProgress(DeviceLayer, "No networks found in chunked scan");
            mpScanCallback->OnFinished(Status::kSuccess, CharSpan(), nullptr);
        }
        
        // Reset chunked scan state
        mChunkedScanInProgress = false;
        mpScanCallback = nullptr;
        return;
    }
    
    // Standard scan handling for specific SSID scans
    uint16_t ap_number;
    esp_wifi_scan_get_ap_num(&ap_number);
    ESP_LOGI(TAG, "AP's found %d", ap_number);
    if (!ap_number)
    {
        ChipLogProgress(DeviceLayer, "No AP found");
        mpScanCallback->OnFinished(Status::kSuccess, CharSpan(), nullptr);
        mpScanCallback = nullptr;
        return;
    }

    // For standard scans, copy the results to our buffer first to maintain consistency
    ap_number = std::min(ap_number, static_cast<uint16_t>(kMaxWiFiScanResults));
    
    if (esp_wifi_scan_get_ap_records(&ap_number, mScanResultsBuffer) == ESP_OK)
    {
        mScanResultCount = ap_number;
        ChipLogProgress(DeviceLayer, "Standard scan complete, found %d networks", mScanResultCount);
        
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 3)
        // ESP-IDF 5.1.3+ only needs size
        ESPScanResponseIterator iter(mScanResultCount);
#else
        // Create an iterator with our buffer
        ESPScanResponseIterator iter(mScanResultCount, const_cast<const wifi_ap_record_t*>(&mScanResultsBuffer[0]));
#endif
        mpScanCallback->OnFinished(Status::kSuccess, CharSpan(), &iter);
    }
    else
    {
        ChipLogError(DeviceLayer, "Can't get AP records");
        mpScanCallback->OnFinished(Status::kUnknownError, CharSpan(), nullptr);
    }
    
    mpScanCallback = nullptr;
    mScanResultCount = 0; // Reset the counter
}

void ESPWiFiDriver::OnNetworkStatusChange()
{
    Network configuredNetwork;
    bool staEnabled = false, staConnected = false;
    VerifyOrReturn(ESP32Utils::IsStationEnabled(staEnabled) == CHIP_NO_ERROR);
    VerifyOrReturn(staEnabled && mpStatusChangeCallback != nullptr);
    CHIP_ERROR err = GetConfiguredNetwork(configuredNetwork);
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "Failed to get configured network when updating network status: %s", err.AsString());
        return;
    }
    VerifyOrReturn(ESP32Utils::IsStationConnected(staConnected) == CHIP_NO_ERROR);
    if (staConnected)
    {
        mpStatusChangeCallback->OnNetworkingStatusChange(
            Status::kSuccess, MakeOptional(ByteSpan(configuredNetwork.networkID, configuredNetwork.networkIDLen)), NullOptional);
        return;
    }

    // The disconnect reason for networking status changes is allowed to have
    // manufacturer-specific values, which is why it's an int32_t, even though
    // we just store a uint16_t value in it.
    int32_t lastDisconnectReason = GetLastDisconnectReason();
    mpStatusChangeCallback->OnNetworkingStatusChange(
        Status::kUnknownError, MakeOptional(ByteSpan(configuredNetwork.networkID, configuredNetwork.networkIDLen)),
        MakeOptional(lastDisconnectReason));
}

void ESPWiFiDriver::ScanNetworks(ByteSpan ssid, WiFiDriver::ScanCallback * callback)
{
    ESP_LOGI(TAG, "ONEDGE - ESPWiFiDriver::ScanNetworks called");
    if (callback != nullptr)
    {
        mpScanCallback = callback;
        if (StartScanWiFiNetworks(ssid) != CHIP_NO_ERROR)
        {
            mpScanCallback = nullptr;
            callback->OnFinished(Status::kUnknownError, CharSpan(), nullptr);
        }
    }
}

uint32_t ESPWiFiDriver::GetSupportedWiFiBandsMask() const
{
    uint32_t bands = static_cast<uint32_t>(1UL << chip::to_underlying(WiFiBandEnum::k2g4));
    return bands;
}

CHIP_ERROR ESPWiFiDriver::SetLastDisconnectReason(const ChipDeviceEvent * event)
{
    VerifyOrReturnError(event->Type == DeviceEventType::kESPSystemEvent && event->Platform.ESPSystemEvent.Base == WIFI_EVENT &&
                            event->Platform.ESPSystemEvent.Id == WIFI_EVENT_STA_DISCONNECTED,
                        CHIP_ERROR_INVALID_ARGUMENT);
    mLastDisconnectedReason = event->Platform.ESPSystemEvent.Data.WiFiStaDisconnected.reason;
    return CHIP_NO_ERROR;
}

uint16_t ESPWiFiDriver::GetLastDisconnectReason()
{
    return mLastDisconnectedReason;
}

size_t ESPWiFiDriver::WiFiNetworkIterator::Count()
{
    return mDriver->mStagingNetwork.ssidLen == 0 ? 0 : 1;
}

bool ESPWiFiDriver::WiFiNetworkIterator::Next(Network & item)
{
    if (mExhausted || mDriver->mStagingNetwork.ssidLen == 0)
    {
        return false;
    }
    memcpy(item.networkID, mDriver->mStagingNetwork.ssid, mDriver->mStagingNetwork.ssidLen);
    item.networkIDLen = mDriver->mStagingNetwork.ssidLen;
    item.connected    = false;
    mExhausted        = true;

    Network configuredNetwork;
    CHIP_ERROR err = GetConfiguredNetwork(configuredNetwork);
    if (err == CHIP_NO_ERROR)
    {
        bool isConnected = false;
        err              = ESP32Utils::IsStationConnected(isConnected);
        if (err == CHIP_NO_ERROR && isConnected && configuredNetwork.networkIDLen == item.networkIDLen &&
            memcmp(configuredNetwork.networkID, item.networkID, item.networkIDLen) == 0)
        {
            item.connected = true;
        }
    }
    return true;
}

CHIP_ERROR ESPWiFiDriver::BackupConfiguration()
{
    CHIP_ERROR err = PersistedStorage::KeyValueStoreMgr().Get(kWiFiSSIDKeyName, nullptr, 0);
    if (err == CHIP_NO_ERROR || err == CHIP_ERROR_BUFFER_TOO_SMALL)
    {
        return CHIP_NO_ERROR;
    }
    ReturnErrorOnFailure(PersistedStorage::KeyValueStoreMgr().Put(kWiFiCredentialsKeyName, mStagingNetwork.credentials,
                                                                  mStagingNetwork.credentialsLen));
    ReturnErrorOnFailure(PersistedStorage::KeyValueStoreMgr().Put(kWiFiSSIDKeyName, mStagingNetwork.ssid, mStagingNetwork.ssidLen));
    return CHIP_NO_ERROR;
}

} // namespace NetworkCommissioning
} // namespace DeviceLayer
} // namespace chip
