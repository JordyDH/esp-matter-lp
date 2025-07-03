#!/bin/bash

echo "Enabling FreeRTOS runtime stats in sdkconfig..."

# Enable runtime stats generation
echo "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y" >> sdkconfig
echo "CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y" >> sdkconfig

# Also enable trace facility which is required
echo "CONFIG_FREERTOS_USE_TRACE_FACILITY=y" >> sdkconfig
echo "CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS=y" >> sdkconfig

# Enable CPU usage tracking
echo "CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID=y" >> sdkconfig

echo "Runtime stats configuration added to sdkconfig."
echo "Please run 'idf.py fullclean' and then rebuild your project."
echo ""
echo "After rebuilding, the runtime stats will be printed before and after the WiFi delay."
echo "This will help identify which tasks are preventing the CPU from going to sleep." 