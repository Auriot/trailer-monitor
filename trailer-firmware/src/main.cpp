/*
 * Copyright (c) 2023 Particle Industries, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Changed for the Auriot trailer monitor (2026): starts the trailer add-on (trailer.h),
 * uses the trailer program's own version number (trailer_version.h), and keeps the
 * Bluetooth trace lines off.
 */

#include "Particle.h"
#include "edge.h"
#include "trailer.h"
#include "trailer_version.h"

// SYSTEM_THREAD(ENABLED) is default on 6.2.0+ and will be fully deprecated in
// future, only specify on older versions to resolve deprecation warning
#if SYSTEM_VERSION < SYSTEM_VERSION_DEFAULT(6, 2, 0)
    SYSTEM_THREAD(ENABLED);
#endif
SYSTEM_MODE(SEMI_AUTOMATIC);

#if EDGE_PRODUCT_NEEDED
PRODUCT_ID(EDGE_PRODUCT_ID);
#endif // EDGE_PRODUCT_NEEDED
PRODUCT_VERSION(TRAILER_PROGRAM_VERSION); // was EDGE_PRODUCT_VERSION (3), the same as Monitor Edge v3

STARTUP(
    Edge::startup();
);

/*
 * Workaround for a Device OS 6.5.0 USB CDC panic (SOS 10, "panic, assert_failed").
 * Revert to pre-6.5.0 behavior.
 */
class ThrottledSerialLogHandler: public SerialLogHandler {
public:
    explicit ThrottledSerialLogHandler(LogLevel level = LOG_LEVEL_INFO, LogCategoryFilters filters = {}) :
            SerialLogHandler(level, filters) {
    }

    explicit ThrottledSerialLogHandler(int baud, LogLevel level = LOG_LEVEL_INFO, LogCategoryFilters filters = {}) :
            SerialLogHandler(baud, level, filters) {
    }

    virtual void write(const char *data, size_t size) override {
        for (size_t i = 0; i < size; i++) {
            Serial.write((uint8_t)data[i]);
        }
    }
};

ThrottledSerialLogHandler logHandler(115200, LOG_LEVEL_TRACE, {
    { "app.gps.nmea", LOG_LEVEL_INFO },
    { "app.gps.ubx",  LOG_LEVEL_INFO },
    { "ncp.at", LOG_LEVEL_INFO },
    { "net.ppp.client", LOG_LEVEL_INFO },
    // The heater connect runs on a thread above the cellular and Bluetooth threads (trailer.cpp).
    // Its trace lines could stall that thread on a blocked USB port, so keep them off.
    { "wiring.ble", LOG_LEVEL_INFO },
});

void setup()
{
    trailerSetup();
    Edge::instance().init();
}

void loop()
{
    Edge::instance().loop();
    trailerLoop();
}
