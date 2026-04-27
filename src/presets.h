#pragma once

// ─── Demo presets ─────────────────────────────────────────────────────────────
// Each preset is 6 rows of max 21 chars.  Text is uppercased and sanitised by
// the board driver so lowercase is fine here.  Add new presets freely - the
// count is computed automatically and demo mode cycles through all of them.

static const char* kPresets[][6] = {

    // ── Flights ───────────────────────────────────────────────────────────────
    {
        "FL 101  LONDON",
        "FL 202  NEW YORK",
        "FL 303  PARIS",
        "FL 404  TOKYO",
        "FL 505  SYDNEY",
        "FL 606  DUBAI"
    },
    {
        "SA 204  SAO PAULO",
        "EK 412  DUBAI",
        "QF 002  SINGAPORE",
        "BA 117  HONG KONG",
        "LH 763  FRANKFURT",
        "AA 100  LOS ANGELES"
    },
    {
        "AF 006  PARIS",
        "JL 044  OSAKA",
        "TK 001  ISTANBUL",
        "SQ 021  LONDON",
        "CX 251  SYDNEY",
        "UA 930  CHICAGO"
    },

    // ── Trains ────────────────────────────────────────────────────────────────
    {
        "08.14  CAPE TOWN",
        "08.32  DURBAN",
        "09.05  PRETORIA",
        "09.47  PORT ELIZ",
        "10.20  BLOEMFONT",
        "11.00  KIMBERLEY"
    },
    {
        "07.32  EDINBURGH",
        "08.05  MANCHESTER",
        "08.47  BIRMINGHAM",
        "09.15  BRISTOL",
        "10.00  CARDIFF",
        "11.30  PENZANCE"
    },
    {
        "06.55  BERLIN HBF",
        "07.22  HAMBURG",
        "08.10  MUNICH",
        "09.30  COLOGNE",
        "10.15  FRANKFURT",
        "11.45  STUTTGART"
    },
    {
        "07.00  TOKYO",
        "07.06  SHINAGAWA",
        "07.23  YOKOHAMA",
        "08.01  NAGOYA",
        "08.59  KYOTO",
        "09.16  OSAKA"
    },

    // ── Stocks ────────────────────────────────────────────────────────────────
    {
        "AAPL  172.45 +1.2%",
        "TSLA  248.10 -0.8%",
        "NVDA  875.22 +3.4%",
        "BTC   67420 +2.1%",
        "ETH    3510 +1.7%",
        "SPY   523.80 +0.5%"
    },
    {
        "MSFT  415.30 +0.6%",
        "AMZN  185.20 +1.9%",
        "GOOG  175.80 -0.3%",
        "META  525.40 +2.7%",
        "NFLX  628.10 +1.1%",
        "AMD   165.90 +4.2%"
    },

    // ── Crypto ────────────────────────────────────────────────────────────────
    {
        "BTC   67420 +2.1%",
        "ETH    3510 +1.7%",
        "SOL   142.30 +4.2%",
        "BNB   385.10 +0.9%",
        "DOGE  0.1621 +5.3%",
        "XRP   0.5892 -1.1%"
    },

    // ── Space ─────────────────────────────────────────────────────────────────
    {
        "FALCON 9  LC-39A",
        "T-00.00.42",
        "PAYLOAD STARLINK",
        "ORBIT   LEO 550KM",
        "LANDING DRONESHIP",
        "STATUS  GO"
    },
    {
        "ARTEMIS III",
        "TARGET  LUNAR SOUTH",
        "CREW    4 ASTRONAUTS",
        "LAUNCH  SLS BLOCK 1",
        "LANDING 2026",
        "STATUS  ON TRACK"
    },

    // ── Weather ───────────────────────────────────────────────────────────────
    {
        "JOHANNESBURG",
        "TODAY  SUNNY 28C",
        "WIND   NW 15KM/H",
        "HUMID  42%",
        "TOMORROW  CLOUDY",
        "RAIN CHANCE  20%"
    },
    {
        "LONDON",
        "TODAY  OVERCAST 12C",
        "WIND   SW 22KM/H",
        "HUMID  81%",
        "TOMORROW  DRIZZLE",
        "RAIN CHANCE  90%"
    },
    {
        "TOKYO",
        "TODAY  CLEAR 22C",
        "WIND   NE 8KM/H",
        "HUMID  55%",
        "TOMORROW  SUNNY",
        "RAIN CHANCE  5%"
    },
    {
        "NEW YORK",
        "TODAY  PARTLY CLOUDY",
        "WIND   W 18KM/H",
        "HUMID  63%",
        "TOMORROW  SUNNY",
        "RAIN CHANCE  15%"
    },

    // ── Funny ─────────────────────────────────────────────────────────────────
    {
        "INTL PIZZA EXPRESS",
        "PZ 001  MARGHERITA",
        "PZ 002  PEPPERONI",
        "PZ 003  BBQ CHICKEN",
        "PZ 404  NOT FOUND",
        "ETA     30 MINUTES"
    },
    {
        "CATS DEPARTURES",
        "CAT 1  SOFA   14.00",
        "CAT 2  BED    14.02",
        "CAT 3  SOFA   14.04",
        "CAT 4  FRIDGE 14.06",
        "CAT 5  SOFA   14.08"
    },
    {
        "HOGWARTS EXPRESS",
        "PLATFORM 9.75",
        "DEPARTS  11.00",
        "DEST     HOGSMEADE",
        "STOPS    NONE",
        "STATUS   ON TIME"
    },
    {
        "MONDAY MORNING",
        "STATUS  LOADING...",
        "COFFEE  REQUIRED",
        "MEETINGS  TOO MANY",
        "MOTIVATION  LOW",
        "ETA FRIDAY  4 DAYS"
    },
    {
        "EXISTENTIAL TRANSIT",
        "WHERE ARE WE GOING",
        "WHY ARE WE HERE",
        "IS THIS THE RIGHT",
        "TRAIN",
        "DOES IT MATTER"
    },
    {
        "BUFFET CAR MENU",
        "TEA         1.80",
        "COFFEE      2.40",
        "SAD SANDWICH 4.50",
        "CRISPS      1.20",
        "REGRET      FREE"
    },
    {
        "GALACTIC AIRWAYS",
        "GA 001  MARS",
        "GA 002  EUROPA",
        "GA 003  TITAN",
        "GA 404  EARTH",
        "GA 999  VOID"
    },
    {
        "STARSHIP ENTERPRISE",
        "DESTINATION BOLDLY",
        "WARP     FACTOR 9",
        "SHIELDS  UP",
        "PHASERS  STANDING BY",
        "TEA      EARL GREY"
    },
    {
        "404 FLIGHT NOT FOUND",
        "PLEASE TRY AGAIN",
        "OR TRY REBOOTING",
        "THE AIRPORT",
        "SUPPORT CODE",
        "HAVE YOU TRIED IT"
    },
    {
        "UNDERGROUND  LINE 1",
        "07.45  MORDOR",
        "08.10  THE SHIRE",
        "09.00  RIVENDELL",
        "10.30  MORIA CLOSED",
        "ALL DAY  GONDOR"
    },

    // ── World cities clocks ───────────────────────────────────────────────────
    {
        "WORLD TIME NOW",
        "LONDON    UTC+0",
        "NEW YORK  UTC-5",
        "DUBAI     UTC+4",
        "TOKYO     UTC+9",
        "SYDNEY    UTC+11"
    },
    {
        "WORLD TIME NOW",
        "LOS ANGELES  UTC-8",
        "CHICAGO      UTC-6",
        "TORONTO      UTC-5",
        "SAO PAULO    UTC-3",
        "BUENOS AIRES UTC-3"
    },
    {
        "WORLD TIME NOW",
        "REYKJAVIK    UTC+0",
        "PARIS        UTC+1",
        "BERLIN       UTC+1",
        "HELSINKI     UTC+2",
        "ATHENS       UTC+2"
    },
    {
        "WORLD TIME NOW",
        "JOHANNESBURG UTC+2",
        "CAIRO        UTC+2",
        "NAIROBI      UTC+3",
        "LAGOS        UTC+1",
        "CASABLANCA   UTC+1"
    },
    {
        "WORLD TIME NOW",
        "MOSCOW       UTC+3",
        "TEHRAN       UTC+3.5",
        "KARACHI      UTC+5",
        "MUMBAI       UTC+5.5",
        "DHAKA        UTC+6"
    },
    {
        "WORLD TIME NOW",
        "BANGKOK      UTC+7",
        "SINGAPORE    UTC+8",
        "HONG KONG    UTC+8",
        "SHANGHAI     UTC+8",
        "SEOUL        UTC+9"
    },
    {
        "WORLD TIME NOW",
        "AUCKLAND     UTC+12",
        "FIJI         UTC+12",
        "HONOLULU     UTC-10",
        "ANCHORAGE    UTC-9",
        "VANCOUVER    UTC-8"
    },

    // ── F1 leaderboard ────────────────────────────────────────────────────────
    {
        "F1  LAP 47/57",
        "P1  VER  +0.000",
        "P2  HAM  +4.213",
        "P3  LEC  +9.871",
        "P4  NOR  +14.330",
        "P5  SAI  +19.004"
    },

    // ── Cinema listings ───────────────────────────────────────────────────────
    {
        "NOW SHOWING",
        "SCREEN 1  19.30",
        "ALIENS ARE REAL",
        "SCREEN 2  20.00",
        "MY CAT IS A SPY",
        "SCREEN 3  21.15"
    },

};

static const uint8_t kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);
