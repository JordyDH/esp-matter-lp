/*
 *
 *    Copyright (c) 2023 Project CHIP Authors
 *    All rights reserved.
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

/**
 *    @file
 *      This file defines a ConnectionManager class used to coordinate state between transport layers
 */

#pragma once

namespace chip {
namespace Transport {

/**
 * @brief Singleton class to manage cross-transport connection state
 */
class ConnectionManager 
{
public:
    /**
     * @brief Get the singleton instance
     * 
     * @return ConnectionManager& The singleton instance
     */
    static ConnectionManager & GetInstance()
    {
        static ConnectionManager instance;
        return instance;
    }
    
    /**
     * @brief Set the BLE connection active state
     * 
     * @param active True if BLE connection is active, false otherwise
     */
    void SetBLEConnectionActive(bool active)
    {
        mBLEConnectionActive = active;
    }
    
    /**
     * @brief Check if a BLE connection is currently active
     * 
     * @return bool True if BLE connection is active, false otherwise
     */
    bool IsBLEConnectionActive() const
    {
        return mBLEConnectionActive;
    }
    
private:
    ConnectionManager() : mBLEConnectionActive(false) {}
    bool mBLEConnectionActive;
};

} // namespace Transport
} // namespace chip 