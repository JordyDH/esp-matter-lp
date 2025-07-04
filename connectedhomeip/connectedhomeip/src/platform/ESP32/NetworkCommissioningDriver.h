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

#pragma once
#include <esp_wifi.h>
#include <platform/NetworkCommissioning.h>

using chip::BitFlags;
using chip::app::Clusters::NetworkCommissioning::WiFiSecurityBitmap;

namespace chip {
namespace DeviceLayer {
namespace NetworkCommissioning {
namespace {
inline constexpr uint8_t kMaxWiFiNetworks                  = 1;
inline constexpr uint8_t kWiFiScanNetworksTimeOutSeconds   = 10;
inline constexpr uint8_t kWiFiConnectNetworkTimeoutSeconds = 30;
} // namespace

BitFlags<WiFiSecurityBitmap> ConvertSecurityType(wifi_auth_mode_t authMode);

class ESPScanResponseIterator : public Iterator<WiFiScanResponse>
{
public:
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 3)
    ESPScanResponseIterator(const size_t size) : mSize(size) {}
#else
    ESPScanResponseIterator(const size_t size, const wifi_ap_record_t * scanResults) : mSize(size), mpScanResults(scanResults) {}
#endif // ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 3)
    size_t Count() override { return mSize; }

    bool Next(WiFiScanResponse & item) override
    {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 3)
        wifi_ap_record_t ap_record;
        VerifyOrReturnValue(esp_wifi_scan_get_ap_record(&ap_record) == ESP_OK, false);
        SetApData(item, ap_record);
#else
        if (mIternum >= mSize)
        {
            return false;
        }
        SetApData(item, mpScanResults[mIternum]);
        mIternum++;
#endif // ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 3)
        return true;
    }

    void Release() override
    {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 3)
        esp_wifi_clear_ap_list();
#endif // ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 3)
    }

private:
    const size_t mSize;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 1, 3)
    const wifi_ap_record_t * mpScanResults;
    size_t mIternum = 0;
#endif // ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 1, 3)

    void SetApData(WiFiScanResponse & item, wifi_ap_record_t ap_record)
    {
        item.security = ConvertSecurityType(ap_record.authmode);
        static_assert(chip::DeviceLayer::Internal::kMaxWiFiSSIDLength <= UINT8_MAX, "SSID length might not fit in item.ssidLen");
        item.ssidLen = static_cast<uint8_t>(
            strnlen(reinterpret_cast<const char *>(ap_record.ssid), chip::DeviceLayer::Internal::kMaxWiFiSSIDLength));
        item.channel  = ap_record.primary;
        item.wiFiBand = chip::DeviceLayer::NetworkCommissioning::WiFiBand::k2g4;
        item.rssi     = ap_record.rssi;
        memcpy(item.ssid, ap_record.ssid, item.ssidLen);
        memcpy(item.bssid, ap_record.bssid, sizeof(item.bssid));
    }
};

class ESPWiFiDriver final : public WiFiDriver
{
public:
    class WiFiNetworkIterator final : public NetworkIterator
    {
    public:
        WiFiNetworkIterator(ESPWiFiDriver * aDriver) : mDriver(aDriver) {}
        size_t Count() override;
        bool Next(Network & item) override;
        void Release() override { delete this; }
        ~WiFiNetworkIterator() = default;

    private:
        ESPWiFiDriver * mDriver;
        bool mExhausted = false;
    };

    struct WiFiNetwork
    {
        char ssid[DeviceLayer::Internal::kMaxWiFiSSIDLength];
        uint8_t ssidLen = 0;
        char credentials[DeviceLayer::Internal::kMaxWiFiKeyLength];
        uint8_t credentialsLen = 0;
    };

    // BaseDriver
    NetworkIterator * GetNetworks() override { return new WiFiNetworkIterator(this); }
    CHIP_ERROR Init(NetworkStatusChangeCallback * networkStatusChangeCallback) override;
    void Shutdown() override;

    // WirelessDriver
    uint8_t GetMaxNetworks() override { return kMaxWiFiNetworks; }
    uint8_t GetScanNetworkTimeoutSeconds() override { return kWiFiScanNetworksTimeOutSeconds; }
    uint8_t GetConnectNetworkTimeoutSeconds() override { return kWiFiConnectNetworkTimeoutSeconds; }

    CHIP_ERROR CommitConfiguration() override;
    CHIP_ERROR RevertConfiguration() override;

    Status RemoveNetwork(ByteSpan networkId, MutableCharSpan & outDebugText, uint8_t & outNetworkIndex) override;
    Status ReorderNetwork(ByteSpan networkId, uint8_t index, MutableCharSpan & outDebugText) override;
    void ConnectNetwork(ByteSpan networkId, ConnectCallback * callback) override;
#if CHIP_DEVICE_CONFIG_SUPPORTS_CONCURRENT_CONNECTION
    CHIP_ERROR DisconnectFromNetwork() override;
#endif

    // WiFiDriver
    Status AddOrUpdateNetwork(ByteSpan ssid, ByteSpan credentials, MutableCharSpan & outDebugText,
                              uint8_t & outNetworkIndex) override;
    void ScanNetworks(ByteSpan ssid, ScanCallback * callback) override;
    uint32_t GetSupportedWiFiBandsMask() const override;

    CHIP_ERROR ConnectWiFiNetwork(const char * ssid, uint8_t ssidLen, const char * key, uint8_t keyLen, uint8_t channel = 0);
    void OnConnectWiFiNetwork();
    void OnConnectWiFiNetworkFailed();
    static void OnConnectWiFiNetworkFailed(chip::System::Layer * aLayer, void * aAppState);
    void OnScanWiFiNetworkDone();
    void OnNetworkStatusChange();

    // Chunked WiFi scanning functions
    esp_err_t ScheduleChunkedScanTask(wifi_scan_config_t* scan_config);
    void PerformChunkedScan(void* pvParameters);
    static void ChunkedScanTask(void* pvParameters);

    CHIP_ERROR SetLastDisconnectReason(const ChipDeviceEvent * event);
    uint16_t GetLastDisconnectReason();
    
    // Moved from private to allow direct WiFi scanning
    CHIP_ERROR StartScanWiFiNetworks(ByteSpan ssid);

    static ESPWiFiDriver & GetInstance()
    {
        static ESPWiFiDriver instance;
        return instance;
    }

private:
    bool NetworkMatch(const WiFiNetwork & network, ByteSpan networkId);
    CHIP_ERROR BackupConfiguration();

    WiFiNetwork mStagingNetwork;
    ScanCallback * mpScanCallback;
    ConnectCallback * mpConnectCallback;
    NetworkStatusChangeCallback * mpStatusChangeCallback = nullptr;
    uint16_t mLastDisconnectedReason;
    
    // Fixed-size buffer for WiFi scan results
    static constexpr uint16_t kMaxWiFiScanResults = 20; // Maximum unique APs to track
    wifi_ap_record_t mScanResultsBuffer[kMaxWiFiScanResults];
    uint16_t mScanResultCount = 0;
    
    // Chunked scan tracking
    bool mChunkedScanInProgress = false;
    uint8_t mChunksCompleted = 0;
    uint8_t mTotalChunks = 3; // Matches kChunksCount in implementation
};

class ESPEthernetDriver : public EthernetDriver
{
public:
    class EthernetNetworkIterator final : public NetworkIterator
    {
    public:
        EthernetNetworkIterator(ESPEthernetDriver * aDriver) : mDriver(aDriver) {}
        size_t Count() { return 1; }
        bool Next(Network & item) override
        {
            if (exhausted)
            {
                return false;
            }
            exhausted = true;
            memcpy(item.networkID, interfaceName, interfaceNameLen);
            item.networkIDLen = interfaceNameLen;
            item.connected    = true;
            return true;
        }
        void Release() override { delete this; }
        ~EthernetNetworkIterator() = default;

        uint8_t interfaceName[kMaxNetworkIDLen];
        uint8_t interfaceNameLen = 0;
        bool exhausted           = false;

    private:
        ESPEthernetDriver * mDriver;
    };

    // BaseDriver
    NetworkIterator * GetNetworks() override { return new EthernetNetworkIterator(this); }
    uint8_t GetMaxNetworks() { return 1; }
    CHIP_ERROR Init(NetworkStatusChangeCallback * networkStatusChangeCallback) override;
    void Shutdown()
    {
        // TODO: This method can be implemented if Ethernet is used along with Wifi/Thread.
    }

    static ESPEthernetDriver & GetInstance()
    {
        static ESPEthernetDriver instance;
        return instance;
    }
};

} // namespace NetworkCommissioning
} // namespace DeviceLayer
} // namespace chip
