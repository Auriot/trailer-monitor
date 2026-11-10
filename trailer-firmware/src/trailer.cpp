// SPDX-License-Identifier: Apache-2.0
/*
 * Trailer add-on for Monitor Edge: listens to a Victron solar charger, SmartShunt and Smart
 * BatteryProtect, reads heater status over Bluetooth, and sets how often the unit reports.
 *
 * Reports: event "trailer/report". Once an hour when nobody is watching. Every 30 seconds
 * for 5 minutes after the phone page calls appOpen, so losing the phone signal is safe.
 *
 * HEATER CONTROL: the phone calls "heat" with "on,<minutes>", "on,<minutes>,<level>", "lvl,<level>" or
 * "off". The unit sends only the messages heaterrules::buildMessage allows: status, on, off, level mode,
 * and a level 1 to 10. It never changes the temperature, altitude, pump or any other heater setting.
 * A level waits until the heater is steady (running, or off in standby), then is tried up to 3 times.
 * "th,<minutes>,<target in 0.1 C>,<highest level>" starts a thermostat run: see heater_rules.h. While it
 * runs, "lvl,<level>" changes its highest level.
 * "bat,<stop V x100>,<stop %>" sets the heater's battery stop limits (% 0 is off): see heater_rules.h.
 * The unit checks them against heaterrules::validLimits, keeps them in flash, and answers ctl.res 7.
 * Off is never blocked: not by a guard, not by the data budget. It repeats for up to 2 minutes until
 * the heater says it is stopping, and keeps trying after that until the heater reads off.
 * A run the unit starts has a timer and the safety stops in heater_rules.h: inside too hot, inside
 * probe lost, battery low (volts, or charge % from the SmartShunt), heater fault. They run on the unit, so they work with no cell signal.
 * The run is kept in flash, so a restart keeps the timer. A run started at the heater's panel is
 * left alone; "on" while it runs puts it on the phone's timer. The Victron is only listened to.
 *
 * Settings, kept in flash so a restart does not lose them. Nothing personal is in this code.
 * The phone page's Settings screen sends them; the command line works too:
 *   particle call <device> victronKey <32 hex digits>            (solar charger; also solar,<32 hex>)
 *   particle call <device> victronKey shunt,<32 hex digits>      (SmartShunt)
 *   particle call <device> victronKey protect,<32 hex digits>    (Smart BatteryProtect)
 *   particle call <device> victronKey shunt,off                  (removes that key; also solar,off, protect,off)
 *   particle call <device> heaterName <Bluetooth name or address of the heater>
 *   particle call <device> heaterName off                        (removes the heater)
 * A heater name change is refused while the heater runs or the box has a run (ctl.res -13), so
 * the box never forgets a heater it may have to stop. Rules and their test: settings_rules.h.
 *
 * Temperature probes: two Dallas probes on an Adafruit DS2484 board on the QWIIC port, read once
 * a minute. The DS2482-RK library runs in loop(), on the same thread as Monitor Edge's own use of
 * this bus, under the Wire lock because the GPS light writes to the bus from a timer thread.
 * The unit keeps the inside probe's ID in flash. The page's Swap button calls probeSwap.
 *
 * Data budget: this unit's share of the free plan's monthly limit (lib/data-budget).
 * It counts this file's events, phone calls, and location events. It does not count vitals, loads
 * of a program, or connection upkeep; the spare share covers those. At 80 percent the gaps are 10
 * times longer. At 100 percent, one report a day and no history. Calls still run at 100 percent.
 * The budget must never block a heater stop command.
 *
 * Report fields: v 3 (2 before the Settings screen), up seconds since start, f 1 in fast mode, batt percent, src power source,
 * sig cell percent, all broadcasts heard, keyset Victron keys held (1 solar, 2 shunt, 4 protect,
 * added), hn the heater name set (left out when none), near only while the page is open: up to 3
 * names (or addresses) heard in the last scan that offer the heater's Bluetooth service, mem free bytes,
 * low lowest free bytes seen since start, bud percent of the budget used, net cell network in use
 * as country code plus network code (the page turns it into a name; left out if the modem does
 * not answer), pr probes: n found in the last read, r s t library results of the last reset,
 * search, and read (1 done, below 0 a fault, 0 did not run or is running), in inside and out
 * outside in 0.01 C, ia and oa their age in seconds; a value with no reading yet is left out.
 * dev Victron list: age seconds since read, r signal, st state, err error, cV battery in
 * 0.01 V, dA current in 0.1 A, W solar watts, y10 yield today in 10 Wh.
 * sh SmartShunt: age, r, soc state of charge in 0.1 %, cV battery in 0.01 V, mA current in mA
 * (minus = out of the battery), ah Ah used since full in 0.1 Ah, min minutes left, al alarm bits.
 * bp Smart BatteryProtect: age, r, st state (249 active), out output (0 shutdown, 1 on, 4 off),
 * err error, al alarm bits, wn warning bits, inV and outV in 0.01 V, off off-reason bits.
 * sh and bp are left out until that device is heard; a field is left out when not available.
 * The battery volts for the safety stops: the shunt if fresh, else the solar charger, else the heater.
 * htr heater: ok 1 if it has answered, age seconds since read, miss failed reads in a row,
 * r signal, st state, step, err fault, v10 volts x10, cs body C, cab10 room C x10, set, lvl,
 * hex last reply as received.
 *
 * htr.run seconds this heater run has lasted, 0 when it is off.
 * htr.md heater mode, 1 is level mode.
 * ctl heater control: a 1 while a run the unit started is going, sw 1 while an off is not yet
 * confirmed, left seconds to the timer, res result of the last phone command and rag its age in
 * seconds, why reason the last run ended and wag its age, wl a level waiting to be set
 * th 1 for a thermostat run, tg its target in 0.1 C, mx its highest level, idle 1 while paused,
 * ah the inside change it expects over the next 5 minutes in 0.1 C,
 * lim the battery stop limits in use: V x100, charge % (0 is off), bw 1 while a paused thermostat run
 * waits for the battery to come back over them
 * (codes in heater_rules.h).
 * rst why the box last started: System.resetReason() and its data (panic code for a panic).
 * bt Bluetooth: cn heater connects since start, stk lowest free stack in bytes of the worker, the
 * connect thread and the application thread (0 if not known).
 * gd movement guard: m mode (0 waiting to guard, 1 guarding, 2 towing), st seconds on the still
 * clock, d whole meters from the guard spot (guarding, with a GPS fix in the last minute), n the
 * last alert number since start and ag its age in seconds, tdy alerts today (UTC), q alerts waiting
 * to send, fw 1 while following, res the last guard command result. Each is left out when it does
 * not apply. near, then hex, are left out if the report would not fit in one event.
 *
 * History: the unit keeps one record for each hour of the last 7 days and saves them in flash.
 * The phone page calls "hist" with the first hour it needs (hours since 1970, UTC) and the unit
 * sends those hours as "trailer/hist" events of under 1024 bytes each, so an open costs one
 * message once the phone has the older hours. Event fields: hist first hour in this event, end 1
 * on the last event, rows one group per hour split by ";", empty if the hour has no record.
 * Group: lowest battery in 0.01 V, highest battery in 0.01 V, peak solar watts, solar added in
 * 10 Wh, heater seconds, heater starts, heater room C, inside probe in 0.1 C, outside probe in
 * 0.1 C, lowest state of charge in 0.1 %, Ah out of the battery in 0.01 Ah, Ah in in 0.01 Ah,
 * peak current out in 0.1 A. A probe value is its last reading in that hour. The battery volts
 * come from the shunt when it is heard, else the solar charger, else the heater's own reading
 * (about 0.3 V high on the first rig). A value with no reading is left empty; the last four
 * are empty for an hour with no shunt reading.
 *
 * Movement guard (rules and numbers: guard_rules.h). loop() reads the GPS every 5 s. Where the
 * trailer has stood still 30 minutes, the box guards it. 3 GPS fixes in a row 100 m or more away
 * make one "moved" alert; then a position every 2 minutes for 2 hours. On its own battery for 2
 * minutes, it makes one "12 V lost" alert. At most 5 alerts a UTC day. The phone's "I'm towing"
 * tap calls "guard" with "tow": no alerts until it is parked again (30 minutes still after it
 * leaves, or 2 hours if it never leaves). Alerts are event "trailer/alert" {"n":<number>,"s":<text>};
 * a Particle webhook turns it into an email. Not limited by the data budget. The guard spot stays
 * in flash on the box: no coordinates go in a report or an event.
 *
 * On-board Bluetooth antenna: it beat the EXTERNAL setting on all three heater reads.
 */
#include "Particle.h"

#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>

#include "DS2482-RK.h"
#include "connect_test.h"
#include "data_budget.h"
#include "edge.h"
#include "guard_rules.h"
#include "heater_rules.h"
#include "probe_roles.h"
#include "rng_hal.h"
#include "settings_rules.h"
#include "trailer.h"
#include "victron_decode.h"

namespace {

constexpr char SETTINGS_DIR[] = "/trailer";
constexpr char KEY_FILE[] = "/trailer/victron.key";
constexpr char SHUNT_KEY_FILE[] = "/trailer/shunt.key";
constexpr char PROTECT_KEY_FILE[] = "/trailer/protect.key";
constexpr char HEATER_FILE[] = "/trailer/heater.name";
constexpr char HISTORY_FILE[] = "/trailer/history.bin";
constexpr char PROBE_FILE[] = "/trailer/probe.in";
constexpr char BUDGET_FILE[] = "/trailer/budget.bin";
constexpr char RUN_FILE[] = "/trailer/run.bin";
constexpr char LIMITS_FILE[] = "/trailer/limits.bin";
constexpr char GUARD_FILE[] = "/trailer/guard.bin";
constexpr char ALERT_N_FILE[] = "/trailer/alertn.bin";  // Own file: a guard.bin change never resets it.
constexpr system_tick_t GUARD_SAMPLE_MS = 5000;
constexpr system_tick_t ALERT_GAP_MS = 1100;       // The cloud allows about one event a second.
constexpr system_tick_t ALERT_WAIT_MS = 30 * 1000;  // No answer this long: counts as failed.
constexpr system_tick_t ALERT_RETRY_MS = 30 * 1000;

constexpr uint32_t BUDGET_OPS = 40000;
constexpr uint32_t BUDGET_BYTES = 30000000;
constexpr size_t OTHER_EVENT_BYTES = 300;  // Estimate for a phone call or a location event.

constexpr system_tick_t PROBE_PERIOD_MS = 60 * 1000;
constexpr system_tick_t PROBE_FRESH_MS = 5 * 60 * 1000;  // Older than this does not go in history.
constexpr size_t PROBE_LIST_MAX = 4;
constexpr size_t PROBE_ROLES = 2;
// Steps of the probe library for each pass of loop(), while a read is running. The library's search
// must finish one probe within 1 second, and takes hundreds of steps. Monitor Edge's loop() is too
// slow for one step a pass: the search failed with -101. A step is at most one short bus message.
constexpr int PROBE_STEPS_PER_LOOP = 40;

constexpr uint16_t SCAN_TIMEOUT_10MS = 500;  // 5 seconds
constexpr system_tick_t FAST_WINDOW_MS = 5 * 60 * 1000;
constexpr system_tick_t FAST_PERIOD_MS = 30 * 1000;
constexpr system_tick_t SAMPLE_PERIOD_MS = 60 * 1000;
constexpr system_tick_t HEATER_IDLE_PERIOD_MS = 5 * 60 * 1000;
constexpr system_tick_t REPORT_PERIOD_MS = 60 * 60 * 1000;
constexpr system_tick_t REPLY_WAIT_MS = 5000;
constexpr system_tick_t REPLY_SETTLE_MS = 1200;

constexpr uint16_t SERVICE_UUID_16 = 0xFFE0;
constexpr uint16_t CHAR_UUID_16 = 0xFFE1;
// AA 55, password 1234 as 0C 22, command, value low, value high, checksum. Command 1 is status,
// 3 is on (1) or off (0); both proven on a real heater in a bench test.
constexpr uint8_t STATUS_REQUEST[8] = {0xAA, 0x55, 0x0C, 0x22, 0x01, 0x00, 0x00, 0x2F};
constexpr uint8_t POWER_ON[8] = {0xAA, 0x55, 0x0C, 0x22, 0x03, 0x01, 0x00, 0x32};
constexpr uint8_t POWER_OFF[8] = {0xAA, 0x55, 0x0C, 0x22, 0x03, 0x00, 0x00, 0x31};
constexpr bool goodChecksum(const uint8_t* m) { return ((m[2] + m[3] + m[4] + m[5] + m[6]) & 0xFF) == m[7]; }
static_assert(goodChecksum(STATUS_REQUEST) && goodChecksum(POWER_ON) && goodChecksum(POWER_OFF), "Bad checksum");
constexpr system_tick_t OFF_RETRY_MS = 120000;
constexpr int ON_TRIES = 3;
constexpr system_tick_t FRESH_MS = 5 * 60 * 1000;  // Battery reading older than this is not used.
constexpr uint8_t XOR_KEY[8] = {'p', 'a', 's', 's', 'w', 'o', 'r', 'd'};
constexpr size_t FRAME_LEN = 48;
constexpr size_t RX_MAX = 96;

constexpr int MAX_DEVICES = 6;  // Victron Instant Readout senders kept per scan.
constexpr size_t MFG_DATA_LEN = 32;
constexpr size_t NAME_LEN = 24;

constexpr uint32_t HISTORY_HOURS = 7 * 24;
constexpr uint32_t HISTORY_FORMAT = 0x54480003;  // Change this if HourRecord changes.
constexpr uint32_t HISTORY_FORMAT_V2 = 0x54480002;  // 20-byte records, before the shunt; migrated on load.
constexpr uint32_t SHUNT_GAP_LIMIT_S = 300;  // Do not count amps across a longer gap in shunt reads.
constexpr size_t HIST_EVENT_MAX = 1000;
constexpr system_tick_t HIST_EVENT_GAP_MS = 1100;  // The cloud allows about one event a second.
constexpr uint32_t HEATER_GAP_LIMIT_S = 300;       // Do not count a long gap in reads as run time.
constexpr int8_t NO_ROOM = -128;
constexpr int16_t NO_TEMP = -32768;
constexpr uint16_t NO_SOC = 0xFFFF;       // No shunt reading this hour.
constexpr uint16_t SOC_UNKNOWN = 0xFFFE;  // Shunt heard, but it gave no state of charge.

struct VictronEntry {
    BleAddress address;
    int rssi = -127;
    bool instantReadout = false;
    uint8_t data[MFG_DATA_LEN] = {};
    size_t dataLen = 0;
};

struct Scan {
    unsigned all = 0;
    int devices = 0;
    VictronEntry entries[MAX_DEVICES];
    bool heaterFound = false;
    BleAddress heater;
    int heaterRssi = 0;
    settingsrules::NearList near;  // Heaters heard, for the page's Settings screen.
};

struct VictronState {
    bool have = false;
    system_tick_t at = 0;
    int rssi = 0;
    victron::SolarReading reading;
};

struct ShuntState {
    bool have = false;
    system_tick_t at = 0;
    int rssi = 0;
    victron::ShuntReading reading;
};

struct ProtectState {
    bool have = false;
    system_tick_t at = 0;
    int rssi = 0;
    victron::ProtectReading reading;
};

struct HeaterState {
    bool have = false;
    system_tick_t at = 0;
    unsigned misses = 0;
    int rssi = 0;
    uint8_t frame[FRAME_LEN] = {};
    uint8_t raw[FRAME_LEN] = {};
};

// One hour of history. A value of 0 in hour means the record is empty.
struct HourRecord {
    uint32_t hour = 0;  // Hours since 1970, UTC.
    uint16_t lowCv = 0;  // 0 means no battery reading in this hour.
    uint16_t highCv = 0;
    uint16_t peakW = 0;
    uint16_t added10Wh = 0;
    uint16_t heaterSec = 0;
    uint8_t heaterStarts = 0;
    int8_t roomC = NO_ROOM;
    int16_t insideC10 = NO_TEMP;   // Last inside probe reading this hour, in 0.1 C.
    int16_t outsideC10 = NO_TEMP;
    uint16_t lowSoc10 = NO_SOC;    // Lowest shunt state of charge this hour, 0.1 %. NO_SOC: no shunt reading.
    uint16_t outCAh = 0;           // Ah out of the battery this hour, 0.01 Ah.
    uint16_t inCAh = 0;            // Ah into the battery this hour, 0.01 Ah.
    uint16_t peakOutDA = 0;        // Highest current out this hour, 0.1 A.
};
static_assert(sizeof(HourRecord) == 28, "Change HISTORY_FORMAT if this size changes");

// The layout before the shunt (format 2), only to carry old hours into the new file.
struct HourRecordV2 {
    uint32_t hour;
    uint16_t lowCv;
    uint16_t highCv;
    uint16_t peakW;
    uint16_t added10Wh;
    uint16_t heaterSec;
    uint8_t heaterStarts;
    int8_t roomC;
    int16_t insideC10;
    int16_t outsideC10;
};
static_assert(sizeof(HourRecordV2) == 20, "Format 2 records are 20 bytes");

// The history file is this header, then one HourRecord for each slot. Only the hour being
// filled stays in memory, because this unit is short of free memory. A file change takes
// effect when the file closes, so a power cut leaves the old file or the new one.
struct HistoryHeader {
    uint32_t format = HISTORY_FORMAT;
    uint16_t lastYield10 = 0;  // Last charger yield, so a restart does not lose or repeat solar.
    uint16_t haveYield = 0;
};
static_assert(sizeof(HistoryHeader) == 8, "Change HISTORY_FORMAT if this size changes");

Scan scan;
VictronState victronState;
ShuntState shuntState;
ProtectState protectState;
HeaterState heaterState;
char report[1024];
HistoryHeader historyHeader;
HourRecord currentHour;
system_tick_t notedVictronAt = 0;
system_tick_t notedShuntAt = 0;
int32_t lastShuntMA = victron::SHUNT_NO_CURRENT;  // Current at the last shunt reading in history.
int32_t outMAs = 0;  // Milliamp-seconds out not yet counted as 0.01 Ah, carried between readings.
int32_t inMAs = 0;
system_tick_t notedHeaterAt = 0;
bool heaterWasOn = false;
bool heaterSeen = false;
uint32_t heaterRunStart = 0;  // Uptime seconds when this run was first seen. 0 means off.

// A run the unit started, kept in flash. Only the worker thread touches it.
struct RunPlan {
    uint32_t endUnix = 0;
    uint8_t active = 0;
    uint8_t stopWanted = 0;  // An off is not yet confirmed: keep trying.
    uint8_t thermo = 0;      // 1: the inside probe sets the level (thermostat).
    uint8_t idle = 0;        // 1: the thermostat has paused the heater; it starts again when cool.
    int16_t targetC10 = 0;   // Thermostat target in 0.1 C.
    uint8_t maxLevel = 0;    // Thermostat highest level.
    uint8_t spare = 0;
};
// A file of another size is not loaded, so a new layout cannot be read wrong. An 8 byte file is
// the layout before the thermostat: endUnix, active, stopWanted, then zeros.
static_assert(sizeof(RunPlan) == 12, "Change the size checks in worker() if this changes");
heaterrules::ThermoTimes thermoTimes;
heaterrules::Trend insideTrend;  // Worker only: how fast the inside temperature moves.
bool levelByThermo = false;  // The waiting level came from the thermostat, not the phone.
RunPlan plan;
heaterrules::Run runRules;
BleAddress heaterAddress;
bool haveHeaterAddress = false;
system_tick_t checkedHeaterAt = 0;
int lastResult = heaterrules::RES_NONE;
system_tick_t lastResultAt = 0;
bool haveResult = false;
int lastWhy = heaterrules::WHY_NONE;
system_tick_t lastWhyAt = 0;
bool haveWhy = false;

// Phone commands, set by the heat function, taken by the worker. Off wins over the others.
std::atomic<bool> offWanted{false};
std::atomic<uint32_t> onMinutes{0};  // 0 means no start waiting.
std::atomic<int> levelAsked{0};      // 0 means no level waiting.
std::atomic<int> targetAsked{0};     // Thermostat target in 0.1 C with a start; 0 means a level run.
int wantedLevel = 0;                 // Worker only: the level still to set.
int levelTries = 0;
std::atomic<bool> reportNow{false};

// Victron keys, one per kind of device. Only the worker reads keys; a call writes a pending key
// and sets its bit in keysPending.
enum KeyKind { KEY_SOLAR, KEY_SHUNT, KEY_PROTECT, KEY_KINDS };
const char* const KEY_FILES[KEY_KINDS] = {KEY_FILE, SHUNT_KEY_FILE, PROTECT_KEY_FILE};
uint8_t keys[KEY_KINDS][16] = {};
bool haveKeys[KEY_KINDS] = {};
uint8_t pendingKeys[KEY_KINDS][16] = {};
std::atomic<int> keysPending{0};
std::atomic<int> keysRemove{0};  // Bits as in keysPending; the last call for a kind wins.
static_assert((int)KEY_SOLAR == (int)settingsrules::KEY_SOLAR && (int)KEY_SHUNT == (int)settingsrules::KEY_SHUNT &&
              (int)KEY_PROTECT == (int)settingsrules::KEY_PROTECT && (int)KEY_KINDS == (int)settingsrules::KEY_KINDS,
              "Key kinds must match settings_rules.h");
static_assert(NAME_LEN == settingsrules::NAME_LEN, "Name length must match settings_rules.h");

char heaterName[NAME_LEN] = {};
char pendingHeater[NAME_LEN] = {};  // Empty: remove the heater.
std::atomic<bool> heaterPending{false};

// Battery limits for the heater. Only the worker reads limits; the heat call writes pendingLimits.
heaterrules::Limits limits;
heaterrules::Limits pendingLimits;
static_assert(sizeof(heaterrules::Limits) == 4, "limits.bin is 4 bytes; a new layout needs a new file name");
bool batteryWait = false;  // Worker only: a paused thermostat run waits for the battery.
std::atomic<bool> limitsPending{false};

std::atomic<bool> histPending{false};
std::atomic<uint32_t> histFrom{0};

DataBudget budget(BUDGET_OPS, BUDGET_BYTES);
std::atomic<uint32_t> otherEvents{0};  // Counted on other threads, spent by the worker.

// Written by loop(), read by the worker when it reports.
struct ProbeValue {
    std::atomic<int16_t> c100{NO_TEMP};
    std::atomic<system_tick_t> at{0};
};

DS2482 probeBoard(Wire, 0);  // Address 0 means 0x18.
DS2482DeviceListStatic<PROBE_LIST_MAX> probeList;
ProbeValue probeInside;
ProbeValue probeOutside;
std::atomic<int> probesFound{0};
std::atomic<int> probeResetResult{0};  // Library results of the last cycle. 1 is done, below 0 is a fault.
std::atomic<int> probeSearchResult{0};
std::atomic<int> probeReadResult{0};
std::atomic<bool> probeSwapPending{false};
uint8_t insideId[proberoles::ID_LEN] = {};
uint8_t lastIds[PROBE_ROLES][proberoles::ID_LEN] = {};
system_tick_t probeCycleStart = 0;
bool probeFirstCycle = true;

// Movement guard. Only loop() (the application thread) touches guard; the guard function stores
// a command for loop() to apply, and the worker reads the gd values below for the report.
guardrules::Guard guard;
std::atomic<int> guardAsk{guardrules::CMD_BAD};  // CMD_BAD: nothing waiting.
std::atomic<int> guardAskValue{0};
std::atomic<int> gdMode{guardrules::WAITING};
std::atomic<int> gdStill{-1};
std::atomic<int> gdMeters{-1};
std::atomic<uint32_t> gdLastN{0};
std::atomic<uint32_t> gdLastAt{0};
std::atomic<int> gdToday{0};
std::atomic<int> gdQueued{0};
std::atomic<bool> gdFollow{false};
std::atomic<int> gdResult{0};
enum class ProbeStep { IDLE, BUSY, DONE };
std::atomic<ProbeStep> probeStep{ProbeStep::IDLE};

uint32_t lowestFree = 0xFFFFFFFF;

void noteMemory() {
    uint32_t now = System.freeMemory();
    if (now < lowestFree) {
        lowestFree = now;
    }
}

std::atomic<system_tick_t> fastStart{0};
std::atomic<bool> fastSet{false};
std::atomic<bool> wake{false};

uint8_t rxBuffer[RX_MAX];
std::atomic<size_t> rxLength{0};

bool saveFile(const char* path, const void* data, size_t len) {
    mkdir(SETTINGS_DIR, 0775);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd < 0) {
        return false;
    }
    bool ok = write(fd, data, len) == (int)len;
    close(fd);
    return ok;
}

int loadFile(const char* path, void* data, size_t len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    int got = read(fd, data, len);
    close(fd);
    return got;
}

void loadSettings() {
    for (int k = 0; k < KEY_KINDS; k++) {
        haveKeys[k] = loadFile(KEY_FILES[k], keys[k], sizeof(keys[k])) == (int)sizeof(keys[k]);
    }
    int got = loadFile(HEATER_FILE, heaterName, sizeof(heaterName) - 1);
    heaterName[got > 0 ? got : 0] = 0;
    DataBudget::Counts saved;
    if (loadFile(BUDGET_FILE, &saved, sizeof(saved)) == (int)sizeof(saved)) {
        budget.restore(saved);
    }
    // No file, or a bad one: the defaults (volts alone, as before the phone could set them).
    heaterrules::Limits savedLimits;
    if (loadFile(LIMITS_FILE, &savedLimits, sizeof(savedLimits)) == (int)sizeof(savedLimits) &&
        heaterrules::validLimits(savedLimits)) {
        limits = savedLimits;
    }
}

void setResult(int result);
bool heaterRunning();

// Deletes a settings file. No file is fine too.
bool removeFile(const char* path) {
    return unlink(path) == 0 || errno == ENOENT;
}

// Forgets what the box heard from a device whose key was removed, so the report leaves it out.
void forgetDevice(int kind) {
    if (kind == KEY_SOLAR) {
        victronState = VictronState();
    } else if (kind == KEY_SHUNT) {
        shuntState = ShuntState();
        lastShuntMA = victron::SHUNT_NO_CURRENT;
        outMAs = 0;
        inMAs = 0;
    } else if (kind == KEY_PROTECT) {
        protectState = ProtectState();
    }
}

// A new heater name, or none. Refused while the heater runs or the box has a run, so the box never
// forgets a heater it may have to stop. Used only once saved, so flash always matches.
void applyHeaterName(const char* asked) {
    settingsrules::NameChange change = settingsrules::nameChange(heaterName, asked, plan.active != 0, heaterRunning());
    if (change == settingsrules::NAME_REFUSE) {
        setResult(heaterrules::RES_HEATER_BUSY);
        return;
    }
    if (change == settingsrules::NAME_APPLY) {
        bool saved = asked[0] != 0 ? saveFile(HEATER_FILE, asked, strlen(asked)) : removeFile(HEATER_FILE);
        if (!saved) {
            setResult(heaterrules::RES_NOT_SAVED);
            return;
        }
        strlcpy(heaterName, asked, sizeof(heaterName));
        // Forget the old heater: its address, its last reading, and a level still to be set.
        haveHeaterAddress = false;
        heaterState = HeaterState();
        heaterSeen = false;
        heaterWasOn = false;
        heaterRunStart = 0;
        notedHeaterAt = 0;
        wantedLevel = 0;
        levelTries = 0;
        levelByThermo = false;
    }
    setResult(heaterrules::RES_HEATER_SET);
}

void applyPendingSettings() {
    int pending = keysPending.exchange(0);
    for (int k = 0; k < KEY_KINDS; k++) {
        if (pending & (1 << k)) {
            memcpy(keys[k], pendingKeys[k], sizeof(keys[k]));
            haveKeys[k] = true;
            saveFile(KEY_FILES[k], keys[k], sizeof(keys[k]));
        }
    }
    int removing = keysRemove.exchange(0);
    for (int k = 0; k < KEY_KINDS; k++) {
        if (removing & (1 << k)) {
            haveKeys[k] = false;
            memset(keys[k], 0, sizeof(keys[k]));
            removeFile(KEY_FILES[k]);
            forgetDevice(k);
        }
    }
    if (heaterPending.exchange(false)) {
        char asked[NAME_LEN];
        strlcpy(asked, pendingHeater, sizeof(asked));
        applyHeaterName(asked);
    }
    if (limitsPending.exchange(false)) {
        heaterrules::Limits asked = pendingLimits;
        // Used only once saved, so the limits in use always match flash. A failed save keeps the old
        // ones, and the report's lim shows the phone nothing changed.
        if (heaterrules::validLimits(asked) && saveFile(LIMITS_FILE, &asked, sizeof(asked))) {
            limits = asked;
            setResult(heaterrules::RES_LIMITS);
        }
    }
}

// Runs during the scan, so it only copies what it needs.
void onScanResult(const BleScanResult& result, void* context) {
    auto state = static_cast<Scan*>(context);
    state->all++;

    // A sender that offers the heater's Bluetooth service goes on the nearby list, by name, or by
    // address when it sends no name.
    uint8_t services[16];
    size_t serviceLen = result.advertisingData().get(BleAdvertisingDataType::SERVICE_UUID_16BIT_COMPLETE, services, sizeof(services));
    bool heaterLike = settingsrules::hasService16(services, serviceLen, SERVICE_UUID_16);
    if (!heaterLike) {
        serviceLen = result.advertisingData().get(BleAdvertisingDataType::SERVICE_UUID_16BIT_MORE_AVAILABLE, services, sizeof(services));
        heaterLike = settingsrules::hasService16(services, serviceLen, SERVICE_UUID_16);
    }
    if (!heaterLike) {
        serviceLen = result.scanResponse().get(BleAdvertisingDataType::SERVICE_UUID_16BIT_COMPLETE, services, sizeof(services));
        heaterLike = settingsrules::hasService16(services, serviceLen, SERVICE_UUID_16);
    }

    if (heaterLike || (heaterName[0] != 0 && !state->heaterFound)) {
        char name[NAME_LEN] = {};
        if (result.advertisingData().deviceName(name, sizeof(name) - 1) == 0) {
            result.scanResponse().deviceName(name, sizeof(name) - 1);
        }
        // The name is often missing from a scan result, so the address also works.
        char mac[20] = {};
        result.address().toString(mac, sizeof(mac));
        if (heaterLike) {
            state->near.add(name[0] != 0 ? name : mac);
        }
        if (heaterName[0] != 0 && !state->heaterFound && (strcmp(name, heaterName) == 0 || strcasecmp(mac, heaterName) == 0)) {
            state->heaterFound = true;
            state->heater = result.address();
            state->heaterRssi = result.rssi();
        }
    }

    uint8_t mfg[MFG_DATA_LEN];
    size_t len = result.advertisingData().get(BleAdvertisingDataType::MANUFACTURER_SPECIFIC_DATA, mfg, sizeof(mfg));
    if (len < 2) {
        len = result.scanResponse().get(BleAdvertisingDataType::MANUFACTURER_SPECIFIC_DATA, mfg, sizeof(mfg));
    }
    if (len < 3 || mfg[0] != victron::COMPANY_LOW || mfg[1] != victron::COMPANY_HIGH ||
        mfg[2] != victron::RECORD_INSTANT_READOUT) {
        return;
    }
    VictronEntry* entry = nullptr;
    for (int i = 0; i < state->devices; i++) {
        if (state->entries[i].address == result.address()) {
            entry = &state->entries[i];
            break;
        }
    }
    if (entry == nullptr) {
        if (state->devices >= MAX_DEVICES) {
            return;
        }
        entry = &state->entries[state->devices++];
        entry->address = result.address();
    }
    entry->rssi = result.rssi();
    memcpy(entry->data, mfg, len);
    entry->dataLen = len;
}

// Decodes each Instant Readout sender of this scan with the key for its kind.
void readVictron() {
    for (int i = 0; i < scan.devices; i++) {
        const VictronEntry& entry = scan.entries[i];
        victron::Header header;
        if (!victron::parseHeader(entry.data, entry.dataLen, &header)) {
            continue;
        }
        if (header.readoutType == victron::READOUT_SOLAR_CHARGER && haveKeys[KEY_SOLAR]) {
            victron::SolarReading reading;
            if (victron::decodeSolar(keys[KEY_SOLAR], header, &reading) == victron::DecodeResult::OK) {
                victronState.have = true;
                victronState.at = millis();
                victronState.rssi = entry.rssi;
                victronState.reading = reading;
            }
        } else if (header.readoutType == victron::READOUT_BATTERY_MONITOR && haveKeys[KEY_SHUNT]) {
            victron::ShuntReading reading;
            if (victron::decodeShunt(keys[KEY_SHUNT], header, &reading) == victron::DecodeResult::OK) {
                shuntState.have = true;
                shuntState.at = millis();
                shuntState.rssi = entry.rssi;
                shuntState.reading = reading;
            }
        } else if (header.readoutType == victron::READOUT_BATTERY_PROTECT && haveKeys[KEY_PROTECT]) {
            victron::ProtectReading reading;
            if (victron::decodeProtect(keys[KEY_PROTECT], header, &reading) == victron::DecodeResult::OK) {
                protectState.have = true;
                protectState.at = millis();
                protectState.rssi = entry.rssi;
                protectState.reading = reading;
            }
        }
    }
}

void onHeaterData(const uint8_t* data, size_t len, const BlePeerDevice& peer, void* context) {
    size_t have = rxLength.load();
    size_t copy = min(len, RX_MAX - have);
    if (copy > 0) {
        memcpy(rxBuffer + have, data, copy);
        rxLength.store(have + copy);
    }
}

void subscribeToHeater(BlePeerDevice& peer) {
    Vector<BleService> services = peer.discoverAllServices();
    for (int s = 0; s < services.size(); s++) {
        if (!(services[s].UUID() == BleUuid(SERVICE_UUID_16))) {
            continue;
        }
        Vector<BleCharacteristic> chars = peer.discoverCharacteristicsOfService(services[s]);
        for (int c = 0; c < chars.size(); c++) {
            if ((uint8_t)chars[c].properties().value() & 0x30) {
                chars[c].onDataReceived(onHeaterData, nullptr);
                chars[c].subscribe(true);
            }
        }
    }
}

// The connect runs on its own thread. Device OS 6.5.0 (6.6.0 is the same) marks a connect as waiting
// only after it asks the radio to connect (ble_hal.cpp, ConnectionsManager::connect). If the link comes
// up in that gap and the system's Bluetooth thread handles it first, nothing wakes the connect: it waits
// 60 s, asserts, and the box restarts (panic 10, 2026-11-06). That thread runs at
// OS_THREAD_PRIORITY_NETWORK. The connect thread runs one step above it, so it finishes the gap before
// that thread can run. It only waits on the radio, so it takes almost no processor time.
constexpr os_thread_prio_t CONNECT_PRIORITY = OS_THREAD_PRIORITY_NETWORK + 1;
constexpr size_t CONNECT_STACK = 2 * 1024;  // Uses about 760 bytes (33,000 connects, 2026-11-07).
constexpr system_tick_t CONNECT_WAIT_MS = 120 * 1000;  // Longer than the system's own 60 s limit.
os_thread_t workerThread = nullptr;
os_thread_t connectThread = nullptr;
os_thread_t appThread = nullptr;  // loop(): the guard reads the GPS here.
os_semaphore_t connectAsk = nullptr;
os_semaphore_t connectDone = nullptr;
BlePeerDevice connectPeer;  // Written by the connect thread before connectDone; read by the worker after.
std::atomic<uint32_t> connects{0};

void connector(void* param) {
    while (true) {
        if (os_semaphore_take(connectAsk, CONCURRENT_WAIT_FOREVER, false) != 0) {
            continue;
        }
        connectPeer = BLE.connect(heaterAddress, false);
        connects.fetch_add(1);
        os_semaphore_give(connectDone, false);
    }
}

// Connects to the heater on the connect thread. Only the worker calls this.
BlePeerDevice connectHeater() {
#ifdef TRAILER_CONNECT_ON_WORKER
    const bool onWorker = true;
#else
    const bool onWorker = connectThread == nullptr;
#endif
    if (onWorker) {
        connects.fetch_add(1);
        return BLE.connect(heaterAddress, false);  // The thread did not start: connect here.
    }
    os_semaphore_take(connectDone, 0, false);  // Clears a late answer, so it is not taken as this one.
    os_semaphore_give(connectAsk, false);
    if (os_semaphore_take(connectDone, CONNECT_WAIT_MS, false) != 0) {
        return BlePeerDevice();
    }
    return connectPeer;
}

// Lowest free stack of a thread since it started, in bytes. 0 if not known.
unsigned long freeStack(os_thread_t thread) {
    size_t words = 0;
    if (thread != nullptr) {
        // Runs with the scheduler stopped: copies one number only.
        os_thread_dump(
            thread,
            [](os_thread_dump_info_t* info, void* data) -> os_result_t {
                *static_cast<size_t*>(data) = info->stack_high_watermark;
                return 0;
            },
            &words);
    }
    return (unsigned long)words * 4;  // FreeRTOS counts 4-byte words.
}

// Connects and subscribes. Heater recipe from the bench tests. False if the heater is not reached.
bool openLink(BlePeerDevice& peer, BleCharacteristic& link) {
    for (int attempt = 0; attempt < 2 && !peer.connected(); attempt++) {
        peer = connectHeater();
        if (!peer.connected()) {
            delay(1500);
        }
    }
    if (!peer.connected()) {
        return false;
    }
    delay(1000);
    subscribeToHeater(peer);
    if (!peer.connected() || !peer.getCharacteristicByUUID(link, BleUuid(CHAR_UUID_16))) {
        return false;
    }
    delay(1500);
    return true;
}

// Sends one message. The heater answers every message with its status; keeps that status.
bool ask(BleCharacteristic& link, const uint8_t* message) {
    rxLength.store(0);
    link.setValue(message, 8, BleTxRxType::ACK);
    system_tick_t start = millis();
    while (rxLength.load() < FRAME_LEN && millis() - start < REPLY_WAIT_MS) {
        delay(50);
    }
    if (rxLength.load() < FRAME_LEN) {
        return false;
    }
    uint8_t frame[FRAME_LEN];
    for (size_t i = 0; i < FRAME_LEN; i++) {
        frame[i] = rxBuffer[i] ^ XOR_KEY[i % sizeof(XOR_KEY)];
    }
    if (frame[0] != 0xAA || frame[1] != 0x66) {
        return false;
    }
    memcpy(heaterState.frame, frame, FRAME_LEN);
    memcpy(heaterState.raw, rxBuffer, FRAME_LEN);
    heaterState.have = true;
    heaterState.at = millis();
    heaterState.misses = 0;
    return true;
}

void closeLink(BlePeerDevice& peer) {
    if (peer.connected()) {
        peer.disconnect();
    }
}

// Connects, asks status once, disconnects.
void readHeater() {
    BlePeerDevice peer;
    BleCharacteristic link;
    bool good = openLink(peer, link) && ask(link, STATUS_REQUEST);
    closeLink(peer);
    if (!good) {
        heaterState.misses++;
    }
}

bool heaterRunning() {
    return heaterState.have && (heaterState.frame[3] != 0 || heaterState.frame[5] != 0);
}

void savePlan() {
    saveFile(RUN_FILE, &plan, sizeof(plan));
}

void setResult(int result) {
    lastResult = result;
    lastResultAt = millis();
    haveResult = true;
    reportNow.store(true);
}

void setWhy(int why) {
    lastWhy = why;
    lastWhyAt = millis();
    haveWhy = true;
    reportNow.store(true);
}

// Shunt volts in 0.01 V if fresh, else -1. The shunt measures at the battery itself.
int shuntCv() {
    if (shuntState.have && millis() - shuntState.at < FRESH_MS && shuntState.reading.centivolts != 0x7FFF &&
        shuntState.reading.centivolts > 0) {
        return shuntState.reading.centivolts;
    }
    return -1;
}

// Shunt state of charge in 0.1 % if fresh, else -1 (also -1 before the shunt has a charge figure).
int shuntChargeDeci() {
    if (shuntState.have && millis() - shuntState.at < FRESH_MS && shuntState.reading.socDeci <= 1000) {
        return shuntState.reading.socDeci;
    }
    return -1;
}

// Solar charger's battery volts in 0.01 V if fresh, else -1.
int solarCv() {
    if (victronState.have && millis() - victronState.at < FRESH_MS && victronState.reading.centivolts != 0x7FFF &&
        victronState.reading.centivolts > 0) {
        return victronState.reading.centivolts;
    }
    return -1;
}

// The heater's own battery reading in 0.01 V if fresh, else -1.
int heaterCv() {
    if (heaterState.have && millis() - heaterState.at < FRESH_MS) {
        return 10 * (256 * heaterState.frame[11] + heaterState.frame[12]);
    }
    return -1;
}

// Battery in 0.01 V: the shunt if fresh, else the solar charger if fresh, else the heater's own
// reading, else -1.
int batteryCv() {
    int shunt = shuntCv();
    if (shunt > 0) {
        return shunt;
    }
    int solar = solarCv();
    if (solar > 0) {
        return solar;
    }
    return heaterCv();
}

heaterrules::Readings readings() {
    heaterrules::Readings r;
    r.clockValid = Time.isValid();
    r.now = r.clockValid ? (uint32_t)Time.now() : 0;
    r.uptime = System.uptime();
    int c100 = probeInside.c100.load();
    r.insideKnown = c100 != NO_TEMP;
    r.insideC100 = c100;
    r.insideAgeS = r.insideKnown ? (millis() - probeInside.at.load()) / 1000 : 0;
    r.aheadC100 = insideTrend.aheadC100();
    r.batteryCv = batteryCv();
    r.chargeDeci = shuntChargeDeci();
    return r;
}

// Adds the newest inside reading to the trend. Runs each pass of the worker.
void noteTrend() {
    int c100 = probeInside.c100.load();
    if (c100 == NO_TEMP) {
        return;
    }
    uint32_t age = (millis() - probeInside.at.load()) / 1000;
    uint32_t up = System.uptime();
    if (age < up) {
        insideTrend.add(up - age, c100);
    }
}

// Sends off until the heater says it is stopping, for up to 2 minutes. why is WHY_NONE for a retry,
// so a retry does not replace the reason the run ended.
// Sends off until the heater says it is stopping, for up to 2 minutes. True if confirmed.
bool sendOff() {
    bool confirmed = false;
    if (haveHeaterAddress) {
        BlePeerDevice peer;
        BleCharacteristic link;
        system_tick_t start = millis();
        while (!confirmed && millis() - start < OFF_RETRY_MS) {
            if (!peer.connected() && !openLink(peer, link)) {
                delay(500);
                continue;
            }
            if (ask(link, POWER_OFF)) {
                confirmed = heaterrules::offConfirmed(heaterState.frame);
            }
            if (!confirmed) {
                delay(1000);
            }
        }
        closeLink(peer);
    }
    checkedHeaterAt = heaterState.at;
    return confirmed;
}

// Sends on, up to 3 times, on an open link. True if the heater says it is on.
bool sendOn(BlePeerDevice& peer, BleCharacteristic& link) {
    bool on = heaterState.frame[heaterrules::BYTE_STATE] != 0;
    for (int i = 0; !on && i < ON_TRIES; i++) {
        if (!peer.connected() && !openLink(peer, link)) {
            continue;
        }
        if (ask(link, POWER_ON)) {
            on = heaterState.frame[heaterrules::BYTE_STATE] != 0;
        }
        if (!on) {
            delay(2000);
        }
    }
    return on;
}

// Ends the run and stops the heater. why is WHY_NONE for a retry, so a retry does not replace the
// reason the run ended.
void stopHeater(int why) {
    wantedLevel = 0;  // A stop drops a level still waiting.
    batteryWait = false;
    plan.active = 0;
    plan.idle = 0;
    plan.stopWanted = 1;
    savePlan();
    if (why != heaterrules::WHY_NONE) {
        setWhy(why);
    }
    bool confirmed = sendOff();
    if (confirmed) {
        plan.stopWanted = 0;
        savePlan();
    }
    setResult(confirmed ? heaterrules::RES_STOPPED : heaterrules::RES_OFF_NOT_CONFIRMED);
}

// Starts the heater, or puts a run started at the panel on the timer. Refuses if a guard fails.
// targetC10 0 is a level run at level (0 keeps the heater's level). Otherwise a thermostat run with
// level as the highest level; if the trailer is already at the target, it waits instead of starting.
void startHeater(uint32_t minutes, int level, int targetC10) {
    BlePeerDevice peer;
    BleCharacteristic link;
    if (!haveHeaterAddress || !openLink(peer, link) || !ask(link, STATUS_REQUEST)) {
        closeLink(peer);
        setResult(heaterrules::RES_NO_LINK);
        return;
    }
    heaterrules::Readings r = readings();
    int refusal = heaterrules::startRefusal(heaterState.frame, r, limits);
    if (refusal != 0) {
        closeLink(peer);
        checkedHeaterAt = heaterState.at;
        setResult(refusal);
        return;
    }
    bool wasOn = heaterState.frame[heaterrules::BYTE_STATE] != 0;
    bool thermo = targetC10 != 0;
    bool warm = thermo && !wasOn && r.insideC100 >= targetC10 * 10;
    // The plan is saved before the on message, so a run that starts is always protected.
    plan.endUnix = r.now + minutes * 60;
    plan.active = 1;
    plan.stopWanted = 0;
    plan.thermo = thermo ? 1 : 0;
    plan.idle = warm ? 1 : 0;
    plan.targetC10 = (int16_t)targetC10;
    plan.maxLevel = (uint8_t)(thermo ? level : 0);
    savePlan();
    batteryWait = false;
    runRules = heaterrules::Run();
    runRules.endUnix = plan.endUnix;
    runRules.startUptime = r.uptime;
    thermoTimes = heaterrules::ThermoTimes();
    thermoTimes.onSince = r.uptime;
    // Already warm: the thermostat may start it as soon as the trailer cools.
    thermoTimes.offSince = r.uptime > heaterrules::MIN_OFF_S ? r.uptime - heaterrules::MIN_OFF_S : 0;
    if (warm) {
        closeLink(peer);
        checkedHeaterAt = heaterState.at;
        setResult(heaterrules::RES_THERMO);
        return;
    }
    bool on = sendOn(peer, link);
    closeLink(peer);
    checkedHeaterAt = heaterState.at;
    if (!on) {
        // Not confirmed: make sure it is off, so it cannot run with no timer.
        stopHeater(heaterrules::WHY_NO_START);
        setResult(heaterrules::RES_ON_NOT_CONFIRMED);
        return;
    }
    setResult(thermo ? heaterrules::RES_THERMO : wasOn ? heaterrules::RES_TIMER : heaterrules::RES_STARTED);
    if (!thermo && level != 0) {
        // Set once the heater is running; during ignition it waits.
        wantedLevel = level;
        levelTries = 0;
        levelByThermo = false;
    }
}

// Thermostat pause: off, but the run goes on and the heater starts again when the trailer cools.
// If off is not confirmed, the heater is still running and the next pass tries again.
void pauseHeater() {
    wantedLevel = 0;
    if (sendOff()) {
        plan.idle = 1;
        savePlan();
        thermoTimes.offSince = System.uptime();
    }
    reportNow.store(true);
}

// Thermostat restart. Uses the same guards as a start from the phone.
void resumeHeater() {
    BlePeerDevice peer;
    BleCharacteristic link;
    if (!haveHeaterAddress || !openLink(peer, link) || !ask(link, STATUS_REQUEST)) {
        closeLink(peer);
        reportNow.store(true);  // Try again on a later pass. The heater is off, so this is safe.
        return;
    }
    checkedHeaterAt = heaterState.at;
    heaterrules::Readings r = readings();
    int refusal = heaterrules::startRefusal(heaterState.frame, r, limits);
    if (heaterrules::resumeWaits(refusal)) {
        // Still cooling down, or the battery under a stop limit: stay paused and try on a later pass.
        // The run's timer still ends it. Design rule: a low battery must not end the run.
        closeLink(peer);
        bool battery = refusal != heaterrules::RES_BUSY;
        if (battery != batteryWait) {
            batteryWait = battery;
            reportNow.store(true);
        }
        return;
    }
    if (refusal != 0) {
        closeLink(peer);
        int why = refusal == heaterrules::RES_LOW_BATTERY ? heaterrules::WHY_BATTERY
                  : refusal == heaterrules::RES_LOW_CHARGE ? heaterrules::WHY_CHARGE
                  : refusal == heaterrules::RES_FAULT     ? heaterrules::WHY_FAULT
                  : refusal == heaterrules::RES_HOT       ? heaterrules::WHY_HOT
                  : refusal == heaterrules::RES_NO_PROBE  ? heaterrules::WHY_PROBE
                                                          : heaterrules::WHY_CLOCK;
        stopHeater(why);  // The heater is off; this ends the run and confirms off.
        return;
    }
    bool on = sendOn(peer, link);
    closeLink(peer);
    checkedHeaterAt = heaterState.at;
    if (!on) {
        stopHeater(heaterrules::WHY_NO_START);
        return;
    }
    plan.idle = 0;
    savePlan();
    batteryWait = false;
    thermoTimes.onSince = r.uptime;
    thermoTimes.levelAt = 0;
    runRules.startUptime = r.uptime;  // The glow plug start is not a low battery.
    runRules.lowSince = 0;
    reportNow.store(true);
}

// Sends level mode if needed, then the level. The heater must be steady; the link must be open
// and heaterState fresh. Returns a result, or 0 if the heater is not steady yet.
int applyLevel(BleCharacteristic& link, int level) {
    if (!heaterrules::steadyForLevel(heaterState.frame)) {
        return 0;
    }
    uint8_t message[8];
    if (heaterState.frame[heaterrules::BYTE_MODE] != heaterrules::MODE_LEVEL) {
        if (!heaterrules::buildMessage(message, heaterrules::CMD_MODE, heaterrules::MODE_LEVEL) || !ask(link, message) ||
            heaterState.frame[heaterrules::BYTE_MODE] != heaterrules::MODE_LEVEL) {
            return heaterrules::RES_MODE_NOT_SET;
        }
    }
    if (!heaterrules::buildMessage(message, heaterrules::CMD_LEVEL, level)) {
        return heaterrules::RES_LEVEL_NOT_CONFIRMED;
    }
    for (int i = 0; i < 2; i++) {
        if (ask(link, message) && heaterState.frame[heaterrules::BYTE_LEVEL] == level) {
            return heaterrules::RES_LEVEL;
        }
        // The reply may still show the old level: read once more.
        delay(1000);
        if (ask(link, STATUS_REQUEST) && heaterState.frame[heaterrules::BYTE_LEVEL] == level) {
            return heaterrules::RES_LEVEL;
        }
    }
    return heaterrules::RES_LEVEL_NOT_CONFIRMED;
}

// Tries the waiting level once. Called each pass of the worker.
void applyWantedLevel() {
    if (wantedLevel == 0 || !heaterState.have) {
        return;
    }
    // Wait without a connection while the last reading shows the heater is not steady.
    if (!heaterrules::steadyForLevel(heaterState.frame) && millis() - heaterState.at < FRESH_MS) {
        return;
    }
    BlePeerDevice peer;
    BleCharacteristic link;
    int result = heaterrules::RES_NO_LINK;
    if (haveHeaterAddress && openLink(peer, link) && ask(link, STATUS_REQUEST)) {
        result = heaterrules::steadyForLevel(heaterState.frame) && heaterState.frame[heaterrules::BYTE_LEVEL] == wantedLevel
                     ? heaterrules::RES_LEVEL
                     : applyLevel(link, wantedLevel);
    }
    closeLink(peer);
    checkedHeaterAt = heaterState.at;
    if (result == 0) {
        return;  // Not steady yet; try on a later pass.
    }
    if (result == heaterrules::RES_LEVEL) {
        thermoTimes.levelAt = System.uptime();
    }
    if (result == heaterrules::RES_LEVEL || ++levelTries >= heaterrules::LEVEL_TRIES) {
        wantedLevel = 0;
        // A thermostat change does not replace the answer to the last phone command.
        if (levelByThermo) {
            reportNow.store(true);
        } else {
            setResult(result);
        }
    } else {
        reportNow.store(true);
    }
}

// Runs once each pass of the worker, after the heater read.
void checkRun() {
    bool fresh = heaterState.have && heaterState.at != checkedHeaterAt;
    checkedHeaterAt = heaterState.at;
    if (plan.stopWanted && !plan.active) {
        if (fresh && heaterState.frame[heaterrules::BYTE_STATE] == 0) {
            plan.stopWanted = 0;
            savePlan();
            reportNow.store(true);
        } else {
            stopHeater(heaterrules::WHY_NONE);
        }
        return;
    }
    if (!plan.active) {
        return;
    }
    // Paused, but someone started it at the panel: the thermostat takes it over again.
    if (plan.idle && fresh && heaterState.frame[heaterrules::BYTE_STATE] != 0 &&
        heaterState.frame[heaterrules::BYTE_STEP] != heaterrules::STEP_COOLDOWN) {
        plan.idle = 0;
        savePlan();
        thermoTimes.onSince = System.uptime();
    }
    runRules.endUnix = plan.endUnix;
    heaterrules::Readings r = readings();
    heaterrules::Decision d = heaterrules::check(heaterState.frame, fresh, r, &runRules, plan.idle != 0, limits);
    if (d.action == heaterrules::END) {
        plan.active = 0;
        savePlan();
        setWhy(d.why);
        return;
    }
    if (d.action == heaterrules::STOP) {
        stopHeater(d.why);
        return;
    }
    if (!plan.thermo || wantedLevel != 0) {
        return;
    }
    heaterrules::ThermoStep step =
        heaterrules::thermo(heaterState.frame, fresh, plan.idle != 0, r, plan.targetC10 * 10, plan.maxLevel, thermoTimes);
    if (step.kind == heaterrules::T_LEVEL) {
        wantedLevel = step.level;
        levelTries = 0;
        levelByThermo = true;
        applyWantedLevel();
    } else if (step.kind == heaterrules::T_PAUSE) {
        pauseHeater();
    } else if (step.kind == heaterrules::T_RESUME) {
        resumeHeater();
    }
}

off_t slotOffset(uint32_t hour) {
    return sizeof(HistoryHeader) + (hour % HISTORY_HOURS) * sizeof(HourRecord);
}

// Reads the header, or starts an empty file if there is none or its format is old.
// Copies a format 2 history (20-byte hours, before the shunt) into the new layout, hour by hour in
// the same slots, with the shunt fields empty. Writes a new file, then renames it over the old one,
// so a power cut leaves one whole file. True when done.
bool migrateHistory(const HistoryHeader& saved) {
    int oldFd = open(HISTORY_FILE, O_RDONLY);
    if (oldFd < 0) {
        return false;
    }
    char tmp[sizeof(HISTORY_FILE) + 4];
    snprintf(tmp, sizeof(tmp), "%s.new", HISTORY_FILE);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd < 0) {
        close(oldFd);
        return false;
    }
    HistoryHeader header = saved;
    header.format = HISTORY_FORMAT;
    bool ok = write(fd, &header, sizeof(header)) == (int)sizeof(header);
    for (uint32_t i = 0; ok && i < HISTORY_HOURS; i++) {
        HourRecordV2 was;
        ok = lseek(oldFd, sizeof(HistoryHeader) + i * sizeof(HourRecordV2), SEEK_SET) >= 0 &&
             read(oldFd, &was, sizeof(was)) == (int)sizeof(was);
        HourRecord now;
        now.hour = was.hour;
        now.lowCv = was.lowCv;
        now.highCv = was.highCv;
        now.peakW = was.peakW;
        now.added10Wh = was.added10Wh;
        now.heaterSec = was.heaterSec;
        now.heaterStarts = was.heaterStarts;
        now.roomC = was.roomC;
        now.insideC10 = was.insideC10;
        now.outsideC10 = was.outsideC10;
        ok = ok && write(fd, &now, sizeof(now)) == (int)sizeof(now);
    }
    close(fd);
    close(oldFd);
    if (!ok || rename(tmp, HISTORY_FILE) != 0) {
        unlink(tmp);
        return false;
    }
    historyHeader = header;
    return true;
}

void loadHistory() {
    mkdir(SETTINGS_DIR, 0775);
    int fd = open(HISTORY_FILE, O_RDWR | O_CREAT, 0664);
    if (fd < 0) {
        return;
    }
    HistoryHeader saved;
    bool readHeader = read(fd, &saved, sizeof(saved)) == (int)sizeof(saved);
    off_t size = lseek(fd, 0, SEEK_END);
    bool good = readHeader && saved.format == HISTORY_FORMAT &&
                size == slotOffset(0) + (off_t)(HISTORY_HOURS * sizeof(HourRecord));
    bool old = readHeader && saved.format == HISTORY_FORMAT_V2 &&
               size == (off_t)(sizeof(HistoryHeader) + HISTORY_HOURS * sizeof(HourRecordV2));
    if (old) {
        close(fd);  // The copy opens it again; nothing stays open across the rename.
        if (migrateHistory(saved)) {
            return;
        }
        fd = open(HISTORY_FILE, O_RDWR | O_CREAT, 0664);
        if (fd < 0) {
            return;
        }
    }
    if (good) {
        historyHeader = saved;
    } else {
        ftruncate(fd, 0);
        lseek(fd, 0, SEEK_SET);
        write(fd, &historyHeader, sizeof(historyHeader));
        const HourRecord empty;
        for (uint32_t i = 0; i < HISTORY_HOURS; i++) {
            write(fd, &empty, sizeof(empty));
        }
    }
    close(fd);
}

bool readSlot(int fd, uint32_t hour, HourRecord* record) {
    return lseek(fd, slotOffset(hour), SEEK_SET) == slotOffset(hour) &&
           read(fd, record, sizeof(HourRecord)) == (int)sizeof(HourRecord) && record->hour == hour;
}

void saveCurrentHour() {
    if (currentHour.hour == 0) {
        return;
    }
    int fd = open(HISTORY_FILE, O_WRONLY);
    if (fd < 0) {
        return;
    }
    write(fd, &historyHeader, sizeof(historyHeader));
    lseek(fd, slotOffset(currentHour.hour), SEEK_SET);
    write(fd, &currentHour, sizeof(currentHour));
    close(fd);
}

// Saves the finished hour when a new one starts. After a restart, carries on with this
// hour's saved record if it has one.
HourRecord& currentRecord(uint32_t hour) {
    if (currentHour.hour != hour) {
        saveCurrentHour();
        HourRecord saved;
        int fd = open(HISTORY_FILE, O_RDONLY);
        bool found = fd >= 0 && readSlot(fd, hour, &saved);
        if (fd >= 0) {
            close(fd);
        }
        currentHour = found ? saved : HourRecord();
        currentHour.hour = hour;
    }
    return currentHour;
}

void noteProbeHistory(const ProbeValue& value, int16_t* slot) {
    int c100 = value.c100.load();
    if (c100 != NO_TEMP && millis() - value.at.load() < PROBE_FRESH_MS) {
        *slot = (c100 + (c100 >= 0 ? 5 : -5)) / 10;
    }
}

void noteHistory() {
    if (!Time.isValid()) {
        return;
    }
    HourRecord& record = currentRecord(Time.now() / 3600);

    if (victronState.have && victronState.at != notedVictronAt) {
        notedVictronAt = victronState.at;
        const victron::SolarReading& r = victronState.reading;
        // The shunt measures at the battery, so its volts win when it is heard.
        if (r.centivolts != 0x7FFF && r.centivolts > 0 && shuntCv() < 0) {
            uint16_t cv = r.centivolts;
            record.lowCv = record.lowCv == 0 ? cv : min(record.lowCv, cv);
            record.highCv = max(record.highCv, cv);
        }
        if (r.watts != 0xFFFF) {
            record.peakW = max(record.peakW, r.watts);
        }
        if (r.yield10Wh != 0xFFFF) {
            // The charger counts up through its day, then restarts from zero.
            if (historyHeader.haveYield) {
                uint16_t last = historyHeader.lastYield10;
                record.added10Wh += r.yield10Wh >= last ? r.yield10Wh - last : r.yield10Wh;
            }
            historyHeader.lastYield10 = r.yield10Wh;
            historyHeader.haveYield = 1;
        }
    }

    if (shuntState.have && shuntState.at != notedShuntAt) {
        const victron::ShuntReading& r = shuntState.reading;
        uint32_t gap = notedShuntAt != 0 ? (shuntState.at - notedShuntAt) / 1000 : 0;
        notedShuntAt = shuntState.at;
        if (r.centivolts != 0x7FFF && r.centivolts > 0) {
            uint16_t cv = r.centivolts;
            record.lowCv = record.lowCv == 0 ? cv : min(record.lowCv, cv);
            record.highCv = max(record.highCv, cv);
        }
        if (r.socDeci != 0x3FF) {
            record.lowSoc10 = record.lowSoc10 == NO_SOC ? r.socDeci : min<uint16_t>(record.lowSoc10, r.socDeci);
        } else if (record.lowSoc10 == NO_SOC) {
            record.lowSoc10 = SOC_UNKNOWN;
        }
        if (r.milliamps != victron::SHUNT_NO_CURRENT) {
            if (r.milliamps < 0) {
                record.peakOutDA = max<uint16_t>(record.peakOutDA, (uint16_t)min<int32_t>(-r.milliamps / 100, 65535));
            }
            // Amp-seconds since the last reading, from the mean of the two readings.
            if (lastShuntMA != victron::SHUNT_NO_CURRENT && gap > 0 && gap <= SHUNT_GAP_LIMIT_S) {
                int32_t mean = (lastShuntMA + r.milliamps) / 2;
                if (mean < 0) {
                    outMAs += -mean * (int32_t)gap;
                } else {
                    inMAs += mean * (int32_t)gap;
                }
                constexpr int32_t CAH_MAS = 36000;  // 0.01 Ah in milliamp-seconds.
                record.outCAh = (uint16_t)min<uint32_t>(65535, record.outCAh + outMAs / CAH_MAS);
                outMAs %= CAH_MAS;
                record.inCAh = (uint16_t)min<uint32_t>(65535, record.inCAh + inMAs / CAH_MAS);
                inMAs %= CAH_MAS;
            }
        }
        lastShuntMA = r.milliamps;
    }

    if (heaterState.have && heaterState.at != notedHeaterAt) {
        uint32_t gap = heaterSeen ? (heaterState.at - notedHeaterAt) / 1000 : 0;
        notedHeaterAt = heaterState.at;
        bool on = heaterRunning();
        if (on && heaterWasOn) {
            record.heaterSec = min<uint32_t>(3600, record.heaterSec + min(gap, HEATER_GAP_LIMIT_S));
        }
        if (on && !heaterWasOn) {
            heaterRunStart = System.uptime();
            // A heater already on at the first read after a restart is not a new start.
            if (heaterSeen && record.heaterStarts < 255) {
                record.heaterStarts++;
            }
        }
        if (!on) {
            heaterRunStart = 0;
        }
        heaterWasOn = on;
        heaterSeen = true;
        const uint8_t* b = heaterState.frame;
        int roomC10 = (int16_t)(256 * b[32] + b[33]);
        record.roomC = constrain((roomC10 + (roomC10 >= 0 ? 5 : -5)) / 10, -127, 127);
        // With no Victron volts, the heater's own reading is the battery history.
        int cv = heaterCv();
        if (shuntCv() < 0 && solarCv() < 0 && cv > 0 && cv <= 6000) {
            record.lowCv = record.lowCv == 0 ? (uint16_t)cv : min<uint16_t>(record.lowCv, (uint16_t)cv);
            record.highCv = max<uint16_t>(record.highCv, (uint16_t)cv);
        }
    }

    noteProbeHistory(probeInside, &record.insideC10);
    noteProbeHistory(probeOutside, &record.outsideC10);
}

size_t appendf(size_t pos, const char* format, ...) {
    if (pos >= sizeof(report)) {
        return pos;
    }
    va_list args;
    va_start(args, format);
    int wrote = vsnprintf(report + pos, sizeof(report) - pos, format, args);
    va_end(args);
    return wrote > 0 ? pos + wrote : pos;
}

size_t appendValue(size_t pos, const char* name, int value, bool available) {
    return available ? appendf(pos, ",\"%s\":%d", name, value) : pos;
}

size_t appendNetwork(size_t pos) {
    CellularGlobalIdentity cgi = {};
    cgi.size = sizeof(cgi);
    cgi.version = CGI_VERSION_LATEST;
    if (cellular_global_identity(&cgi, nullptr) != 0 || cgi.mobile_country_code == 0) {
        return pos;
    }
    return appendf(pos, ",\"net\":\"%03u%03u\"", cgi.mobile_country_code, cgi.mobile_network_code);
}

size_t appendProbe(size_t pos, const char* name, const char* ageName, const ProbeValue& value, system_tick_t now) {
    int c100 = value.c100.load();
    if (c100 == NO_TEMP) {
        return pos;
    }
    return appendf(pos, ",\"%s\":%d,\"%s\":%lu", name, c100, ageName, (unsigned long)((now - value.at.load()) / 1000));
}

// Sends an event and counts it against this unit's share of the plan limit.
void sendEvent(const char* name, const char* data) {
    Particle.publish(name, data);
    budget.spend(strlen(name) + strlen(data));
}

void keepBudget() {
    for (uint32_t other = otherEvents.exchange(0); other > 0; other--) {
        budget.spend(OTHER_EVENT_BYTES);
    }
    if (Time.isValid()) {
        budget.setMonth(Time.year() * 100 + Time.month());
    }
    if (budget.saveDue()) {
        saveFile(BUDGET_FILE, &budget.counts(), sizeof(DataBudget::Counts));
        budget.markSaved();
    }
}

// The movement guard's part of the report. Only fields that mean something are sent.
size_t appendGuard(size_t pos) {
    pos = appendf(pos, ",\"gd\":{\"m\":%d", gdMode.load());
    int still = gdStill.load();
    if (still >= 0) {
        pos = appendf(pos, ",\"st\":%d", still);
    }
    int meters = gdMeters.load();
    if (meters >= 0) {
        pos = appendf(pos, ",\"d\":%d", meters);
    }
    uint32_t n = gdLastN.load();
    if (n != 0) {
        pos = appendf(pos, ",\"n\":%lu,\"ag\":%lu", (unsigned long)n, (unsigned long)(System.uptime() - gdLastAt.load()));
    }
    int today = gdToday.load();
    if (today > 0) {
        pos = appendf(pos, ",\"tdy\":%d", today);
    }
    int queued = gdQueued.load();
    if (queued > 0) {
        pos = appendf(pos, ",\"q\":%d", queued);
    }
    if (gdFollow.load()) {
        pos = appendf(pos, ",\"fw\":1");
    }
    int result = gdResult.load();
    if (result != 0) {
        pos = appendf(pos, ",\"res\":%d", result);
    }
    return appendf(pos, "}");
}

// Builds the report in report[] and returns its length; a length of sizeof(report) or more means
// it was cut. near and hex can be left out to keep it under the event limit.
size_t buildReport(bool fast, bool withNear, bool withHex) {
    system_tick_t now = millis();
    size_t pos = appendf(0, "{\"v\":3,\"up\":%lu,\"f\":%d,\"batt\":%d,\"src\":%d,\"sig\":%d,\"all\":%u,\"keyset\":%d,\"mem\":%lu,\"low\":%lu,\"bud\":%lu",
                         (unsigned long)System.uptime(), fast ? 1 : 0, (int)System.batteryCharge(), System.powerSource(),
                         (int)Cellular.RSSI().getStrength(), scan.all,
                         (haveKeys[KEY_SOLAR] ? 1 : 0) | (haveKeys[KEY_SHUNT] ? 2 : 0) | (haveKeys[KEY_PROTECT] ? 4 : 0),
                         (unsigned long)System.freeMemory(), (unsigned long)lowestFree, (unsigned long)budget.usedPercent());
    if (heaterName[0] != 0 && settingsrules::validHeaterName(heaterName)) {
        pos = appendf(pos, ",\"hn\":\"%s\"", heaterName);
    }
    if (withNear && fast && scan.near.count > 0) {
        pos = appendf(pos, ",\"near\":[");
        for (size_t i = 0; i < scan.near.count; i++) {
            pos = appendf(pos, "%s\"%s\"", i > 0 ? "," : "", scan.near.names[i]);
        }
        pos = appendf(pos, "]");
    }
    pos = appendNetwork(pos);
    pos = appendf(pos, ",\"pr\":{\"n\":%d,\"r\":%d,\"s\":%d,\"t\":%d", probesFound.load(), probeResetResult.load(),
                  probeSearchResult.load(), probeReadResult.load());
    pos = appendProbe(pos, "in", "ia", probeInside, now);
    pos = appendProbe(pos, "out", "oa", probeOutside, now);
    pos = appendf(pos, "}");
    pos = appendf(pos, ",\"dev\":[");
    if (victronState.have) {
        const victron::SolarReading& r = victronState.reading;
        pos = appendf(pos, "{\"age\":%lu,\"r\":%d", (unsigned long)((now - victronState.at) / 1000), victronState.rssi);
        pos = appendValue(pos, "st", r.state, r.state != 0xFF);
        pos = appendValue(pos, "err", r.error, r.error != 0xFF);
        pos = appendValue(pos, "cV", r.centivolts, r.centivolts != 0x7FFF);
        pos = appendValue(pos, "dA", r.deciamps, r.deciamps != 0x7FFF);
        pos = appendValue(pos, "W", r.watts, r.watts != 0xFFFF);
        pos = appendValue(pos, "y10", r.yield10Wh, r.yield10Wh != 0xFFFF);
        pos = appendf(pos, "}");
    }
    pos = appendf(pos, "]");
    if (shuntState.have) {
        const victron::ShuntReading& r = shuntState.reading;
        pos = appendf(pos, ",\"sh\":{\"age\":%lu,\"r\":%d", (unsigned long)((now - shuntState.at) / 1000), shuntState.rssi);
        pos = appendValue(pos, "soc", r.socDeci, r.socDeci != 0x3FF);
        pos = appendValue(pos, "cV", r.centivolts, r.centivolts != 0x7FFF);
        pos = appendValue(pos, "mA", r.milliamps, r.milliamps != victron::SHUNT_NO_CURRENT);
        pos = appendValue(pos, "ah", (int)r.usedDeciAh, r.usedDeciAh != 0xFFFFF);
        pos = appendValue(pos, "min", r.minutesLeft, r.minutesLeft != 0xFFFF);
        pos = appendValue(pos, "al", r.alarm, true);
        pos = appendf(pos, "}");
    }
    if (protectState.have) {
        const victron::ProtectReading& r = protectState.reading;
        pos = appendf(pos, ",\"bp\":{\"age\":%lu,\"r\":%d", (unsigned long)((now - protectState.at) / 1000),
                      protectState.rssi);
        pos = appendValue(pos, "st", r.state, r.state != 0xFF);
        pos = appendValue(pos, "out", r.output, r.output != 0xFF);
        pos = appendValue(pos, "err", r.error, r.error != 0xFF);
        pos = appendValue(pos, "al", r.alarm, true);
        pos = appendValue(pos, "wn", r.warning, true);
        pos = appendValue(pos, "inV", r.inCentivolts, r.inCentivolts != 0x7FFF);
        pos = appendValue(pos, "outV", r.outCentivolts, r.outCentivolts != 0xFFFF);
        pos = appendValue(pos, "off", (int)r.offReason, true);
        pos = appendf(pos, "}");
    }
    pos = appendf(pos, ",\"htr\":{\"ok\":%d,\"miss\":%u,\"run\":%lu", heaterState.have ? 1 : 0, heaterState.misses,
                  (unsigned long)(heaterRunStart != 0 ? System.uptime() - heaterRunStart : 0));
    if (heaterState.have) {
        const uint8_t* b = heaterState.frame;
        pos = appendf(pos, ",\"age\":%lu,\"r\":%d,\"st\":%u,\"step\":%u,\"err\":%u,\"v10\":%u,\"cs\":%d,\"cab10\":%d,\"set\":%u,\"lvl\":%u,\"md\":%u",
                      (unsigned long)((now - heaterState.at) / 1000), heaterState.rssi, b[3], b[5], b[35],
                      256 * b[11] + b[12], (int16_t)(256 * b[13] + b[14]), (int16_t)(256 * b[32] + b[33]), b[9], b[10], b[8]);
        if (withHex) {
            pos = appendf(pos, ",\"hex\":\"");
            for (size_t i = 0; i < FRAME_LEN; i++) {
                pos = appendf(pos, "%02X", heaterState.raw[i]);
            }
            pos = appendf(pos, "\"");
        }
    }
    pos = appendf(pos, "},\"ctl\":{\"a\":%d,\"sw\":%d,\"res\":%d,\"why\":%d", plan.active, plan.stopWanted, lastResult, lastWhy);
    if (plan.active && Time.isValid()) {
        pos = appendf(pos, ",\"left\":%ld", (long)plan.endUnix - (long)Time.now());
    }
    if (haveResult) {
        pos = appendf(pos, ",\"rag\":%lu", (unsigned long)((now - lastResultAt) / 1000));
    }
    if (haveWhy) {
        pos = appendf(pos, ",\"wag\":%lu", (unsigned long)((now - lastWhyAt) / 1000));
    }
    if (wantedLevel != 0 && !levelByThermo) {
        pos = appendf(pos, ",\"wl\":%d", wantedLevel);
    }
    if (plan.active && plan.thermo) {
        pos = appendf(pos, ",\"th\":1,\"tg\":%d,\"mx\":%u,\"idle\":%u,\"ah\":%d", plan.targetC10, plan.maxLevel,
                      plan.idle, insideTrend.aheadC100() / 10);
        if (plan.idle && batteryWait) {
            pos = appendf(pos, ",\"bw\":1");
        }
    }
    pos = appendf(pos, ",\"lim\":[%d,%d]}", limits.stopCv, limits.stopPct);
    pos = appendGuard(pos);
    pos = appendf(pos, ",\"rst\":[%d,%lu],\"bt\":{\"cn\":%lu,\"stk\":[%lu,%lu,%lu]}", (int)System.resetReason(),
                  (unsigned long)System.resetReasonData(), (unsigned long)connects.load(), freeStack(workerThread),
                  freeStack(connectThread), freeStack(appThread));
    return appendf(pos, "}");
}

void publishReport(bool fast) {
    // An event holds up to 1024 bytes. A full report with the heater, both Victron parts, a
    // thermostat run and three nearby names comes close, so drop near, then hex, if needed.
    constexpr size_t REPORT_SAFE = 1000;
    size_t pos = buildReport(fast, true, true);
    if (pos >= REPORT_SAFE) {
        pos = buildReport(fast, false, true);
    }
    if (pos >= REPORT_SAFE) {
        buildReport(fast, false, false);
    }
    sendEvent("trailer/report", report);
}

size_t appendRow(size_t pos, const HourRecord& r) {
    if (r.lowCv != 0) {
        pos = appendf(pos, "%u,%u", r.lowCv, r.highCv);
    } else {
        pos = appendf(pos, ",");
    }
    pos = appendf(pos, ",%u,%u,%u,%u,", r.peakW, r.added10Wh, r.heaterSec, r.heaterStarts);
    pos = r.roomC != NO_ROOM ? appendf(pos, "%d", r.roomC) : pos;
    pos = appendf(pos, ",");
    pos = r.insideC10 != NO_TEMP ? appendf(pos, "%d", r.insideC10) : pos;
    pos = appendf(pos, ",");
    pos = r.outsideC10 != NO_TEMP ? appendf(pos, "%d", r.outsideC10) : pos;
    if (r.lowSoc10 == NO_SOC) {
        return appendf(pos, ",,,,");
    }
    pos = appendf(pos, ",");
    pos = r.lowSoc10 != SOC_UNKNOWN ? appendf(pos, "%u", r.lowSoc10) : pos;
    return appendf(pos, ",%u,%u,%u", r.outCAh, r.inCAh, r.peakOutDA);
}

// Sends the hours from "from" up to now. The unit does not resend older hours the phone has.
void publishHistory(uint32_t from) {
    if (!Time.isValid() || !Particle.connected()) {
        return;
    }
    saveCurrentHour();
    int fd = open(HISTORY_FILE, O_RDONLY);
    if (fd < 0) {
        return;
    }
    uint32_t now = Time.now() / 3600;
    uint32_t oldest = now - (HISTORY_HOURS - 1);
    uint32_t hour = from < oldest ? oldest : min(from, now);
    while (hour <= now) {
        size_t pos = appendf(0, "{\"hist\":%lu,\"rows\":\"", (unsigned long)hour);
        bool firstRow = true;
        // A row is at most about 75 characters, so stop while the end still fits.
        while (hour <= now && pos < HIST_EVENT_MAX - 80) {
            if (!firstRow) {
                pos = appendf(pos, ";");
            }
            firstRow = false;
            HourRecord r;
            if (readSlot(fd, hour, &r)) {
                pos = appendRow(pos, r);
            }
            hour++;
        }
        appendf(pos, "\",\"end\":%d}", hour > now ? 1 : 0);
        sendEvent("trailer/hist", report);
        noteMemory();
        delay(HIST_EVENT_GAP_MS);
    }
    close(fd);
}

void worker(void* param) {
    workerThread = os_thread_current(nullptr);
    waitFor(Particle.connected, 120000);
    BLE.on();
    BLE.selectAntenna(BleAntennaType::INTERNAL);
    loadSettings();
    loadHistory();
    // The 8 byte file from before the thermostat has the same first fields; the rest stays 0.
    int got = loadFile(RUN_FILE, &plan, sizeof(plan));
    if (got != (int)sizeof(plan) && got != 8) {
        plan = RunPlan();
    }
    runRules.endUnix = plan.endUnix;
    runRules.startUptime = System.uptime();

    bool first = true;
    system_tick_t lastSample = 0;
    system_tick_t lastHeater = 0;
    system_tick_t lastReport = 0;
    while (true) {
        applyPendingSettings();
        if (histPending.exchange(false) && budget.level() != DataBudget::kStop) {
            publishHistory(histFrom.load());
        }
        bool woken = wake.exchange(false);
        system_tick_t now = millis();
        bool fast = fastSet.load() && now - fastStart.load() < FAST_WINDOW_MS;
        if (!first && !woken && now - lastSample < (fast ? FAST_PERIOD_MS : SAMPLE_PERIOD_MS)) {
            noteMemory();
#ifdef TRAILER_STRESS_CONNECT_MS
            // Test build only: extra connects with nothing sent, so a night holds thousands of them.
            static system_tick_t lastStress = 0;
            if (haveHeaterAddress && heaterName[0] != 0 && millis() - lastStress >= TRAILER_STRESS_CONNECT_MS) {
                BlePeerDevice peer = connectHeater();
                closeLink(peer);
                lastStress = millis();
            }
#endif
            delay(200);
            continue;
        }
        lastSample = now;

        scan = Scan();
        BLE.setScanTimeout(SCAN_TIMEOUT_10MS);
        BLE.scan(onScanResult, &scan);
        readVictron();
        noteMemory();
        if (scan.heaterFound) {
            heaterAddress = scan.heater;
            haveHeaterAddress = true;
            heaterState.rssi = scan.heaterRssi;
        }

        // Phone commands. Off wins over the others.
        bool commanded = false;
        if (offWanted.exchange(false)) {
            onMinutes.store(0);
            levelAsked.store(0);
            stopHeater(heaterrules::WHY_APP);
            commanded = true;
        } else {
            uint32_t minutes = onMinutes.exchange(0);
            int level = levelAsked.exchange(0);
            int target = targetAsked.load();
            if (minutes != 0) {
                startHeater(minutes, level, target);
                commanded = true;
            } else if (level != 0 && plan.active && plan.thermo) {
                // Thermostat: the phone's level is the highest level; the thermostat applies it.
                plan.maxLevel = (uint8_t)level;
                savePlan();
                thermoTimes.levelAt = 0;
                setResult(heaterrules::RES_MAX_LEVEL);
                commanded = true;
            } else if (level != 0) {
                wantedLevel = level;
                levelTries = 0;
                levelByThermo = false;
                applyWantedLevel();
                commanded = true;
            }
        }
        if (commanded) {
            lastHeater = millis();
            noteMemory();
        }

        system_tick_t heaterPeriod = fast ? FAST_PERIOD_MS : (heaterRunning() ? SAMPLE_PERIOD_MS : HEATER_IDLE_PERIOD_MS);
        if (heaterName[0] != 0 && (first || woken || millis() - lastHeater >= heaterPeriod)) {
            lastHeater = millis();
            if (scan.heaterFound) {
                readHeater();
                noteMemory();
            } else {
                heaterState.misses++;
            }
        }

        noteHistory();
        noteTrend();
        if (!commanded) {
            applyWantedLevel();
        }
        checkRun();

        keepBudget();
        DataBudget::Level level = budget.level();
        // Normal: as asked. Near the limit: only what the phone just asked for, and slower.
        // At the limit: the long gap alone.
        bool asked = level == DataBudget::kNormal ? (first || woken || fast)
                                                  : (level == DataBudget::kSlow && (first || woken));
        bool gapDone = millis() - lastReport >= budget.gapMs(fast ? FAST_PERIOD_MS : REPORT_PERIOD_MS);
        // A heater command or a stop always reports at once, whatever the budget.
        bool mustReport = reportNow.exchange(false);
        if ((asked || gapDone || mustReport) && Particle.connected()) {
            publishReport(fast);
            lastReport = millis();
        }
        first = false;
    }
}

// "on,<minutes>" starts the heater, or puts a run started at its panel on the timer. "off" stops it.
// Returns 1 when the unit has the command; the result comes in the next report as ctl.res.
int heatCommand(String args) {
    otherEvents.fetch_add(1);
    const char* text = args.c_str();
    char* end = nullptr;
    if (args == "off") {
        offWanted.store(true);
    } else if (args.startsWith("on,")) {
        long minutes = strtol(text + 3, &end, 10);
        long level = 0;
        if (end != text + 3 && *end == ',') {
            const char* levelText = end + 1;
            level = strtol(levelText, &end, 10);
            if (end == levelText || level < heaterrules::LEVEL_MIN || level > heaterrules::LEVEL_MAX) {
                return -1;
            }
        }
        if (end == text + 3 || *end != 0 || minutes < 1 || minutes > (long)heaterrules::RUN_MAX_MIN) {
            return -1;
        }
        if (offWanted.load()) {
            return -2;  // A stop is waiting; it wins.
        }
        // Level and target first, so the worker never takes the start without them.
        levelAsked.store((int)level);
        targetAsked.store(0);
        onMinutes.store((uint32_t)minutes);
    } else if (args.startsWith("th,")) {
        // th,<minutes>,<target in 0.1 C>,<highest level>
        long values[3];
        const char* at = text + 3;
        for (int i = 0; i < 3; i++) {
            values[i] = strtol(at, &end, 10);
            if (end == at || *end != (i < 2 ? ',' : 0)) {
                return -1;
            }
            at = end + 1;
        }
        if (values[0] < 1 || values[0] > (long)heaterrules::RUN_MAX_MIN || values[1] < heaterrules::TARGET_MIN_C10 ||
            values[1] > heaterrules::TARGET_MAX_C10 || values[2] < heaterrules::LEVEL_MIN || values[2] > heaterrules::LEVEL_MAX) {
            return -1;
        }
        if (offWanted.load()) {
            return -2;
        }
        levelAsked.store((int)values[2]);
        targetAsked.store((int)values[1]);
        onMinutes.store((uint32_t)values[0]);
    } else if (args.startsWith("bat,")) {
        // bat,<stop V x100>,<stop %>
        long values[2];
        const char* at = text + 4;
        for (int i = 0; i < 2; i++) {
            values[i] = strtol(at, &end, 10);
            if (end == at || *end != (i < 1 ? ',' : 0) || values[i] < 0 || values[i] > 2000) {
                return -1;
            }
            at = end + 1;
        }
        heaterrules::Limits asked;
        asked.stopCv = (int16_t)values[0];
        asked.stopPct = (int16_t)values[1];
        if (!heaterrules::validLimits(asked)) {
            return -1;
        }
        pendingLimits = asked;
        limitsPending.store(true);
    } else if (args.startsWith("lvl,")) {
        long level = strtol(text + 4, &end, 10);
        if (end == text + 4 || *end != 0 || level < heaterrules::LEVEL_MIN || level > heaterrules::LEVEL_MAX) {
            return -1;
        }
        if (offWanted.load()) {
            return -2;
        }
        levelAsked.store((int)level);
    } else {
        return -1;
    }
    fastStart.store(millis());
    fastSet.store(true);
    wake.store(true);
    return 1;
}

// "<32 hex>" or "solar,<32 hex>" is the solar charger's key; "shunt,<32 hex>" and
// "protect,<32 hex>" the others; "<kind>,off" removes one. Returns 1 solar, 2 shunt, 3 protect,
// or -1 if the text is not right. The next report's keyset shows the change.
int setKey(String args) {
    otherEvents.fetch_add(1);
    settingsrules::KeyCommand command = settingsrules::parseKeyCommand(args.c_str());
    if (command.kind < 0) {
        return -1;
    }
    int bit = 1 << command.kind;
    if (command.remove) {
        keysPending.fetch_and(~bit);
        keysRemove.fetch_or(bit);
    } else {
        memcpy(pendingKeys[command.kind], command.key, sizeof(command.key));
        keysRemove.fetch_and(~bit);
        keysPending.fetch_or(bit);
    }
    wake.store(true);
    return command.kind + 1;
}

// The heater's Bluetooth name or address, or "off" to remove it. Returns 1 when the box has it,
// -1 if the name is not right. The result comes in the next report as ctl.res (8 saved, -13
// refused while the heater runs, -14 not saved) and hn.
int setHeater(String args) {
    otherEvents.fetch_add(1);
    const char* text = args.c_str();
    bool remove = settingsrules::isOff(text);
    if (!remove && !settingsrules::validHeaterName(text)) {
        return -1;
    }
    strlcpy(pendingHeater, remove ? "" : text, sizeof(pendingHeater));
    heaterPending.store(true);
    wake.store(true);
    return 1;
}

int appOpen(String args) {
    otherEvents.fetch_add(1);
    fastStart.store(millis());
    fastSet.store(true);
    wake.store(true);
    return 1;
}

// The argument is the first hour the phone needs, in hours since 1970. Empty means all of it.
int askHistory(String args) {
    otherEvents.fetch_add(1);
    histFrom.store(strtoul(args.c_str(), nullptr, 10));
    histPending.store(true);
    return 1;
}

// Tells the phone -1 if the unit does not have two probes to swap.
int swapProbes(String args) {
    otherEvents.fetch_add(1);
    if (probesFound.load() < (int)PROBE_ROLES) {
        return -1;
    }
    probeSwapPending.store(true);
    return 1;
}

void applyProbeSwap() {
    if (probesFound.load() < (int)PROBE_ROLES) {
        return;
    }
    const uint8_t* newInside = memcmp(lastIds[0], insideId, sizeof(insideId)) == 0 ? lastIds[1] : lastIds[0];
    memcpy(insideId, newInside, sizeof(insideId));
    saveFile(PROBE_FILE, insideId, sizeof(insideId));
    int c100 = probeInside.c100.load();
    system_tick_t at = probeInside.at.load();
    probeInside.c100.store(probeOutside.c100.load());
    probeInside.at.store(probeOutside.at.load());
    probeOutside.c100.store(c100);
    probeOutside.at.store(at);
    wake.store(true);
}

void noteProbe(ProbeValue* value, int index) {
    if (index == proberoles::NONE) {
        return;
    }
    const DS2482Device& probe = const_cast<const DS2482DeviceListStatic<PROBE_LIST_MAX>&>(probeList).getDeviceByIndex(index);
    float c = probe.getTemperatureC();
    if (probe.getValid() && proberoles::goodReading(c)) {
        value->c100.store(proberoles::toCenti(c));
        value->at.store(millis());
    }
}

void finishProbes() {
    size_t count = min(probeList.getDeviceCount(), PROBE_ROLES);
    for (size_t i = 0; i < count; i++) {
        DS24821WireAddress address = probeList.getDeviceByIndex(i).getAddress();
        for (size_t b = 0; b < proberoles::ID_LEN; b++) {
            lastIds[i][b] = address[b];
        }
    }
    proberoles::Roles roles = proberoles::assign(lastIds, count, insideId);
    // Keep the choice once both probes have been seen, so a later search order cannot change it.
    if (count == PROBE_ROLES && memcmp(lastIds[roles.inside], insideId, sizeof(insideId)) != 0) {
        memcpy(insideId, lastIds[roles.inside], sizeof(insideId));
        saveFile(PROBE_FILE, insideId, sizeof(insideId));
    }
    noteProbe(&probeInside, roles.inside);
    noteProbe(&probeOutside, roles.outside);
    probesFound.store((int)count);
}

// The same steps as the probe bench test: reset the board, find the probes, read them.
// A new cycle starts only when the last one has finished, so two can never overlap.
void startProbes() {
    probeCycleStart = millis();
    probeFirstCycle = false;
    probeStep.store(ProbeStep::BUSY);
    probeList.clear();
    probeResetResult.store(0);
    probeSearchResult.store(0);
    probeReadResult.store(0);
    DS2482DeviceReset::run(probeBoard, [](DS2482DeviceReset&, int status) {
        probeResetResult.store(status);
        if (status != DS2482Command::RESULT_DONE) {
            probeStep.store(ProbeStep::DONE);
            return;
        }
        DS2482SearchBusCommand::run(probeBoard, probeList, [](DS2482SearchBusCommand&, int status) {
            probeSearchResult.store(status);
            if (status != DS2482Command::RESULT_DONE || probeList.getDeviceCount() == 0) {
                probeStep.store(ProbeStep::DONE);
                return;
            }
            DS2482GetTemperatureForListCommand::run(probeBoard, probeList,
                [](DS2482GetTemperatureForListCommand&, int status, DS2482DeviceList&) {
                    probeReadResult.store(status);
                    probeStep.store(ProbeStep::DONE);
                });
        });
    });
}

// "tow" is the phone's "I'm towing" tap; "test,guard", "test,shift,<m>" and "test,cap,<k>" are for
// tests (guard_rules.h). Returns 1 when the box has it, -1 if the text is not right. loop() applies
// it; the result comes in the next report as gd.res (1 done, -2 refused).
int guardCommand(String args) {
    otherEvents.fetch_add(1);
    guardrules::Command command = guardrules::parseCommand(args.c_str());
    if (command.kind == guardrules::CMD_BAD) {
        return -1;
    }
    guardAskValue.store(command.value);
    guardAsk.store((int)command.kind);
    return 1;
}

uint32_t clockNow() {
    return Time.isValid() ? (uint32_t)Time.now() : 0;
}

// Own function, not inlined: the LocationPoint is about 950 bytes on the application stack only
// while it runs. Not static: getLocation adds to its list of sources on every call.
__attribute__((noinline)) bool readFix(guardrules::Fix* fix) {
    LocationStatus status = {};
    // getLocation would fail hard if the GPS never started; getStatus checks that first.
    if (EdgeGnssAbstraction::instance().getStatus(status) != SYSTEM_ERROR_NONE || !status.powered || status.error) {
        return false;
    }
    LocationPoint point;
    if (EdgeGnssAbstraction::instance().getLocation(point) != SYSTEM_ERROR_NONE) {
        return false;
    }
    fix->locked = point.locked != 0;
    fix->stable = point.stable;
    if (fix->locked) {
        fix->lat = point.latitude;
        fix->lon = point.longitude;
        fix->acc = point.horizontalAccuracy;
    }
    return true;
}

void loadGuard() {
    guardrules::Saved saved = {};
    bool savedOk = loadFile(GUARD_FILE, &saved, sizeof(saved)) == (int)sizeof(saved);
    uint32_t n = 0;
    bool nOk = loadFile(ALERT_N_FILE, &n, sizeof(n)) == (int)sizeof(n);
    // No number file: start somewhere new, so a number already sent cannot come back.
    uint32_t seed = Time.isValid() ? (uint32_t)Time.now() : ((HAL_RNG_GetRandomNumber() & 0x3FFFFFFF) | 1);
    guard.load(saved, savedOk, n, nOk, seed);
}

const char* alertText(uint8_t kind) {
    return kind == guardrules::POWER ? "Trailer box lost 12 V power" : "Trailer moved 100 m or more";
}

// Sends the first waiting alert. Never waits: Future::wait() does not work off the application
// thread, and here it would hold up Monitor Edge. Not limited by the data budget (like heater off).
void sendAlerts() {
    static particle::Future<bool> sending;
    static bool inFlight = false;
    static bool failed = false;
    static system_tick_t triedAt = 0;
    if (inFlight) {
        if (sending.isDone()) {
            inFlight = false;
            failed = !sending.isSucceeded();
            if (!failed) {
                guard.sent();
            }
        } else if (millis() - triedAt >= ALERT_WAIT_MS) {
            inFlight = false;
            failed = true;
        }
        return;
    }
    guardrules::Alert alert;
    if (!guard.peek(&alert) || !Particle.connected() || millis() - triedAt < (failed ? ALERT_RETRY_MS : ALERT_GAP_MS)) {
        return;
    }
    char data[96];
    snprintf(data, sizeof(data), "{\"n\":%lu,\"s\":\"%s\"}", (unsigned long)alert.n, alertText(alert.kind));
    sending = Particle.publish("trailer/alert", data, WITH_ACK);
    inFlight = true;
    triedAt = millis();
    otherEvents.fetch_add(1);
}

void guardLoop() {
    static bool loaded = false;
    static system_tick_t sampledAt = 0;
    if (!loaded) {
        loaded = true;
        loadGuard();
    }
    uint32_t now = (uint32_t)System.uptime();
    // A waiting command first, so a "tow" is in place before the next fix.
    int asked = guardAsk.exchange(guardrules::CMD_BAD);
    if (asked != guardrules::CMD_BAD) {
        guardrules::Command command;
        command.kind = (guardrules::CommandKind)asked;
        command.value = guardAskValue.load();
        gdResult.store(guard.command(command, now));
        wake.store(true);
    }
    if (millis() - sampledAt >= GUARD_SAMPLE_MS) {
        sampledAt = millis();
        guardrules::Fix fix;
        bool haveFix = readFix(&fix);
        guard.tick(haveFix ? &fix : nullptr, System.powerSource(), now, clockNow());
    }
    if (guard.followDue(now)) {
        EdgeLocation::instance().triggerLocPub(Trigger::IMMEDIATE, "user");
    }
    // Saved before sending, so a restart can never reuse an alert number.
    if (guard.nDirty) {
        saveFile(ALERT_N_FILE, &guard.nextN, sizeof(guard.nextN));
        guard.nDirty = false;
    }
    if (guard.dirty) {
        saveFile(GUARD_FILE, &guard.s, sizeof(guard.s));
        guard.dirty = false;
    }
    sendAlerts();

    gdMode.store(guard.mode());
    gdStill.store(guard.stillSeconds(now));
    gdMeters.store(guard.metersFromGuard(now));
    gdLastN.store(guard.lastN);
    gdLastAt.store(guard.lastAt);
    gdToday.store((int)guard.s.dayCount);
    gdQueued.store((int)guard.s.queueLen);
    gdFollow.store(guard.following);
}

}  // namespace

void trailerSetup() {
    Particle.function("victronKey", setKey);
    Particle.function("heaterName", setHeater);
    Particle.function("appOpen", appOpen);
    Particle.function("hist", askHistory);
    Particle.function("probeSwap", swapProbes);
    Particle.function("heat", heatCommand);
    Particle.function("guard", guardCommand);
#ifndef TRAILER_CONNECT_ON_WORKER
    if (os_semaphore_create(&connectAsk, 1, 0) == 0 && os_semaphore_create(&connectDone, 1, 0) == 0 &&
        os_thread_create(&connectThread, "connect", CONNECT_PRIORITY, connector, nullptr, CONNECT_STACK) != 0) {
        connectThread = nullptr;  // connectHeater then connects on the worker, as before.
    }
#endif
    // Uses about 1,950 bytes at most (reports, history, start, level, pause, restart, stop; 2026-11-07).
    // Was 8 KB; the spare memory goes back to the cloud connection, which needs it on a reconnect.
    new Thread("trailer", worker, nullptr, OS_THREAD_PRIORITY_DEFAULT, 5 * 1024);
}

void trailerLoop() {
    static bool started = false;
    if (!started) {
        started = true;
        probeBoard.setup();
        loadFile(PROBE_FILE, insideId, sizeof(insideId));
        appThread = os_thread_current(nullptr);
        EdgeLocation::instance().regLocGenCallback([](JSONWriter&, LocationPoint&, const void*) {
            otherEvents.fetch_add(1);
        });
    }

    int steps = probeStep.load() == ProbeStep::BUSY ? PROBE_STEPS_PER_LOOP : 1;
    for (int i = 0; i < steps; i++) {
        WITH_LOCK(Wire) {
            probeBoard.loop();
        }
    }
    if (probeSwapPending.exchange(false)) {
        applyProbeSwap();
    }
    if (probeStep.load() == ProbeStep::DONE) {
        finishProbes();
        probeStep.store(ProbeStep::IDLE);
    }
    if (probeStep.load() == ProbeStep::IDLE && (probeFirstCycle || millis() - probeCycleStart >= PROBE_PERIOD_MS)) {
        startProbes();
    }
    guardLoop();
}
