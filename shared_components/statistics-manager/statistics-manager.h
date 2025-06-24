/*
 * esp-brew-engine
 * Copyright (C) Dekien Jeroen 2024
 *
 */
#ifndef INCLUDE_STATISTICSMANAGER_H
#define INCLUDE_STATISTICSMANAGER_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <vector>
#include <map>
#include <ctime>
#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_handle.hpp"
#include "settings-manager.h"

using namespace std;

struct TempDataPoint {
    time_t timestamp;
    int8_t avgTemp;
    int8_t targetTemp;
    uint8_t pidOutput;
};

struct BrewSession {
    uint32_t sessionId;
    time_t startTime;
    time_t endTime;
    char scheduleName[32];
    uint16_t dataPoints;
    float avgTemperature;
    float maxTemperature;
    float minTemperature;
    uint32_t totalDuration;
    bool completed;
};

class StatisticsManager
{
private:
    SettingsManager* settingsManager;
    uint32_t currentSessionId;
    bool sessionActive;
    time_t sessionStartTime;
    vector<TempDataPoint> currentSessionData;
    string currentScheduleName;
    
    uint32_t getNextSessionId();
    void compressAndStoreSessionData(uint32_t sessionId, const vector<TempDataPoint>& data);
    vector<TempDataPoint> loadAndDecompressSessionData(uint32_t sessionId);
    void cleanupOldSessions();
    void calculateSessionStats(BrewSession& session, const vector<TempDataPoint>& data);

public:
    StatisticsManager(SettingsManager* settings);
    void Init();
    
    // Session management
    void StartSession(const string& scheduleName = "");
    void EndSession();
    void AddDataPoint(time_t timestamp, int8_t avgTemp, int8_t targetTemp, uint8_t pidOutput);
    
    // Data retrieval
    vector<BrewSession> GetSessionList();
    BrewSession GetSessionById(uint32_t sessionId);
    vector<TempDataPoint> GetSessionData(uint32_t sessionId);
    map<string, uint32_t> GetSessionStats();
    
    // Configuration
    void SetMaxSessions(uint8_t maxSessions);
    uint8_t GetMaxSessions();
    
    // Export functionality
    string ExportSessionToJson(uint32_t sessionId);
    string ExportSessionToCsv(uint32_t sessionId);
    string ExportAllSessionsToJson();
    
    // Current session info
    bool IsSessionActive() { return sessionActive; }
    uint32_t GetCurrentSessionId() { return currentSessionId; }
    uint16_t GetCurrentSessionDataPoints() { return currentSessionData.size(); }
};

#endif /* INCLUDE_STATISTICSMANAGER_H */