/*
 * esp-brew-engine
 * Copyright (C) Dekien Jeroen 2024
 */
#include "statistics-manager.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

using namespace std;

static const char *TAG = "StatisticsManager";
static const uint8_t DEFAULT_MAX_SESSIONS = 10;
static const char* SESSION_COUNT_KEY = "stat_count";
static const char* SESSION_ID_KEY = "stat_next_id";
static const char* MAX_SESSIONS_KEY = "stat_max";

StatisticsManager::StatisticsManager(SettingsManager* settings)
{
    ESP_LOGI(TAG, "StatisticsManager Construct");
    this->settingsManager = settings;
    this->sessionActive = false;
    this->currentSessionId = 0;
    this->currentSessionData.clear();
    this->currentScheduleName = "";
}

void StatisticsManager::Init()
{
    ESP_LOGI(TAG, "StatisticsManager Init");
    
    // Initialize max sessions setting if not exists
    uint8_t maxSessions = this->settingsManager->Read(MAX_SESSIONS_KEY, DEFAULT_MAX_SESSIONS);
    if (maxSessions == 0) {
        maxSessions = DEFAULT_MAX_SESSIONS;
        this->settingsManager->Write(MAX_SESSIONS_KEY, maxSessions);
    }
    
    ESP_LOGI(TAG, "Max sessions: %d", maxSessions);
}

uint32_t StatisticsManager::getNextSessionId()
{
    uint32_t nextId = this->settingsManager->Read(SESSION_ID_KEY, (uint16_t)1);
    this->settingsManager->Write(SESSION_ID_KEY, (uint16_t)(nextId + 1));
    return nextId;
}

void StatisticsManager::StartSession(const string& scheduleName)
{
    if (this->sessionActive) {
        ESP_LOGW(TAG, "Session already active, ending previous session");
        this->EndSession();
    }
    
    this->currentSessionId = this->getNextSessionId();
    this->sessionActive = true;
    this->sessionStartTime = time(nullptr);
    this->currentSessionData.clear();
    this->currentScheduleName = scheduleName;
    
    ESP_LOGI(TAG, "Started session %d with schedule: %s", this->currentSessionId, scheduleName.c_str());
}

void StatisticsManager::EndSession()
{
    if (!this->sessionActive) {
        ESP_LOGW(TAG, "No active session to end");
        return;
    }
    
    time_t endTime = time(nullptr);
    
    // Create session record
    BrewSession session;
    session.sessionId = this->currentSessionId;
    session.startTime = this->sessionStartTime;
    session.endTime = endTime;
    session.dataPoints = this->currentSessionData.size();
    session.totalDuration = endTime - this->sessionStartTime;
    session.completed = true;
    
    // Copy schedule name
    strncpy(session.scheduleName, this->currentScheduleName.c_str(), sizeof(session.scheduleName) - 1);
    session.scheduleName[sizeof(session.scheduleName) - 1] = '\0';
    
    // Calculate statistics
    this->calculateSessionStats(session, this->currentSessionData);
    
    // Store session metadata
    string sessionKey = "session_" + to_string(this->currentSessionId);
    vector<uint8_t> sessionData(reinterpret_cast<uint8_t*>(&session), 
                                reinterpret_cast<uint8_t*>(&session) + sizeof(BrewSession));
    this->settingsManager->Write(sessionKey, sessionData);
    
    // Store temperature data
    this->compressAndStoreSessionData(this->currentSessionId, this->currentSessionData);
    
    // Update session count
    uint16_t sessionCount = this->settingsManager->Read(SESSION_COUNT_KEY, (uint16_t)0);
    sessionCount++;
    this->settingsManager->Write(SESSION_COUNT_KEY, sessionCount);
    
    ESP_LOGI(TAG, "Ended session %d, duration: %d seconds, data points: %d", 
             this->currentSessionId, session.totalDuration, session.dataPoints);
    
    // Cleanup old sessions if needed
    this->cleanupOldSessions();
    
    // Reset current session
    this->sessionActive = false;
    this->currentSessionId = 0;
    this->currentSessionData.clear();
    this->currentScheduleName = "";
}

void StatisticsManager::AddDataPoint(time_t timestamp, int8_t avgTemp, int8_t targetTemp, uint8_t pidOutput)
{
    if (!this->sessionActive) {
        return;
    }
    
    TempDataPoint dataPoint;
    dataPoint.timestamp = timestamp;
    dataPoint.avgTemp = avgTemp;
    dataPoint.targetTemp = targetTemp;
    dataPoint.pidOutput = pidOutput;
    
    this->currentSessionData.push_back(dataPoint);
    
    ESP_LOGD(TAG, "Added data point: temp=%d, target=%d, output=%d", avgTemp, targetTemp, pidOutput);
}

void StatisticsManager::calculateSessionStats(BrewSession& session, const vector<TempDataPoint>& data)
{
    if (data.empty()) {
        session.avgTemperature = 0;
        session.maxTemperature = 0;
        session.minTemperature = 0;
        return;
    }
    
    float sum = 0;
    int8_t minTemp = data[0].avgTemp;
    int8_t maxTemp = data[0].avgTemp;
    
    for (const auto& point : data) {
        sum += point.avgTemp;
        minTemp = min(minTemp, point.avgTemp);
        maxTemp = max(maxTemp, point.avgTemp);
    }
    
    session.avgTemperature = sum / data.size();
    session.minTemperature = minTemp;
    session.maxTemperature = maxTemp;
}

void StatisticsManager::compressAndStoreSessionData(uint32_t sessionId, const vector<TempDataPoint>& data)
{
    if (data.empty()) {
        return;
    }
    
    // Simple compression: store as binary blob
    string dataKey = "data_" + to_string(sessionId);
    vector<uint8_t> binaryData(reinterpret_cast<const uint8_t*>(data.data()),
                               reinterpret_cast<const uint8_t*>(data.data()) + data.size() * sizeof(TempDataPoint));
    
    this->settingsManager->Write(dataKey, binaryData);
    
    ESP_LOGD(TAG, "Stored %d data points for session %d", data.size(), sessionId);
}

vector<TempDataPoint> StatisticsManager::loadAndDecompressSessionData(uint32_t sessionId)
{
    string dataKey = "data_" + to_string(sessionId);
    vector<uint8_t> defaultData;
    vector<uint8_t> binaryData = this->settingsManager->Read(dataKey, defaultData);
    
    if (binaryData.empty()) {
        return vector<TempDataPoint>();
    }
    
    size_t dataPointCount = binaryData.size() / sizeof(TempDataPoint);
    vector<TempDataPoint> data(dataPointCount);
    
    memcpy(data.data(), binaryData.data(), binaryData.size());
    
    ESP_LOGD(TAG, "Loaded %d data points for session %d", dataPointCount, sessionId);
    
    return data;
}

void StatisticsManager::cleanupOldSessions()
{
    uint8_t maxSessions = this->GetMaxSessions();
    uint16_t sessionCount = this->settingsManager->Read(SESSION_COUNT_KEY, (uint16_t)0);
    
    if (sessionCount <= maxSessions) {
        return;
    }
    
    ESP_LOGI(TAG, "Cleaning up old sessions, current count: %d, max: %d", sessionCount, maxSessions);
    
    // Get all sessions and sort by start time
    vector<BrewSession> sessions = this->GetSessionList();
    sort(sessions.begin(), sessions.end(), [](const BrewSession& a, const BrewSession& b) {
        return a.startTime < b.startTime;
    });
    
    // Remove oldest sessions
    size_t sessionsToRemove = sessionCount - maxSessions;
    for (size_t i = 0; i < sessionsToRemove && i < sessions.size(); i++) {
        uint32_t sessionId = sessions[i].sessionId;
        
        // Remove session metadata
        string sessionKey = "session_" + to_string(sessionId);
        // Note: NVS doesn't have a direct delete function, so we'll overwrite with empty data
        vector<uint8_t> emptyData;
        this->settingsManager->Write(sessionKey, emptyData);
        
        // Remove session data
        string dataKey = "data_" + to_string(sessionId);
        this->settingsManager->Write(dataKey, emptyData);
        
        ESP_LOGD(TAG, "Removed session %d", sessionId);
    }
    
    // Update session count
    this->settingsManager->Write(SESSION_COUNT_KEY, (uint16_t)maxSessions);
}

vector<BrewSession> StatisticsManager::GetSessionList()
{
    vector<BrewSession> sessions;
    uint16_t sessionCount = this->settingsManager->Read(SESSION_COUNT_KEY, (uint16_t)0);
    
    if (sessionCount == 0) {
        return sessions;
    }
    
    // Try to load all possible sessions (inefficient but works with NVS limitations)
    uint32_t maxSessionId = this->settingsManager->Read(SESSION_ID_KEY, (uint16_t)1);
    
    for (uint32_t id = 1; id < maxSessionId; id++) {
        string sessionKey = "session_" + to_string(id);
        vector<uint8_t> defaultData;
        vector<uint8_t> sessionData = this->settingsManager->Read(sessionKey, defaultData);
        
        if (!sessionData.empty() && sessionData.size() >= sizeof(BrewSession)) {
            BrewSession session;
            memcpy(&session, sessionData.data(), sizeof(BrewSession));
            sessions.push_back(session);
        }
    }
    
    // Sort by start time (newest first)
    sort(sessions.begin(), sessions.end(), [](const BrewSession& a, const BrewSession& b) {
        return a.startTime > b.startTime;
    });
    
    return sessions;
}

BrewSession StatisticsManager::GetSessionById(uint32_t sessionId)
{
    string sessionKey = "session_" + to_string(sessionId);
    vector<uint8_t> defaultData;
    vector<uint8_t> sessionData = this->settingsManager->Read(sessionKey, defaultData);
    
    BrewSession session = {};
    if (!sessionData.empty() && sessionData.size() >= sizeof(BrewSession)) {
        memcpy(&session, sessionData.data(), sizeof(BrewSession));
    }
    
    return session;
}

vector<TempDataPoint> StatisticsManager::GetSessionData(uint32_t sessionId)
{
    return this->loadAndDecompressSessionData(sessionId);
}

map<string, uint32_t> StatisticsManager::GetSessionStats()
{
    map<string, uint32_t> stats;
    vector<BrewSession> sessions = this->GetSessionList();
    
    stats["totalSessions"] = sessions.size();
    stats["totalBrewTime"] = 0;
    stats["avgSessionDuration"] = 0;
    
    if (!sessions.empty()) {
        uint32_t totalDuration = 0;
        for (const auto& session : sessions) {
            totalDuration += session.totalDuration;
        }
        stats["totalBrewTime"] = totalDuration;
        stats["avgSessionDuration"] = totalDuration / sessions.size();
    }
    
    return stats;
}

void StatisticsManager::SetMaxSessions(uint8_t maxSessions)
{
    if (maxSessions == 0) {
        maxSessions = DEFAULT_MAX_SESSIONS;
    }
    
    this->settingsManager->Write(MAX_SESSIONS_KEY, maxSessions);
    ESP_LOGI(TAG, "Set max sessions to: %d", maxSessions);
    
    // Trigger cleanup if current count exceeds new max
    this->cleanupOldSessions();
}

uint8_t StatisticsManager::GetMaxSessions()
{
    return this->settingsManager->Read(MAX_SESSIONS_KEY, DEFAULT_MAX_SESSIONS);
}

string StatisticsManager::ExportSessionToJson(uint32_t sessionId)
{
    BrewSession session = this->GetSessionById(sessionId);
    vector<TempDataPoint> data = this->GetSessionData(sessionId);
    
    if (session.sessionId == 0) {
        return "{}";
    }
    
    stringstream json;
    json << "{";
    json << "\"sessionId\":" << session.sessionId << ",";
    json << "\"scheduleName\":\"" << session.scheduleName << "\",";
    json << "\"startTime\":" << session.startTime << ",";
    json << "\"endTime\":" << session.endTime << ",";
    json << "\"duration\":" << session.totalDuration << ",";
    json << "\"dataPoints\":" << session.dataPoints << ",";
    json << "\"avgTemperature\":" << fixed << setprecision(1) << session.avgTemperature << ",";
    json << "\"minTemperature\":" << (int)session.minTemperature << ",";
    json << "\"maxTemperature\":" << (int)session.maxTemperature << ",";
    json << "\"completed\":" << (session.completed ? "true" : "false") << ",";
    json << "\"data\":[";
    
    for (size_t i = 0; i < data.size(); i++) {
        if (i > 0) json << ",";
        json << "{";
        json << "\"timestamp\":" << data[i].timestamp << ",";
        json << "\"avgTemp\":" << (int)data[i].avgTemp << ",";
        json << "\"targetTemp\":" << (int)data[i].targetTemp << ",";
        json << "\"pidOutput\":" << (int)data[i].pidOutput;
        json << "}";
    }
    
    json << "]}";
    
    return json.str();
}

string StatisticsManager::ExportSessionToCsv(uint32_t sessionId)
{
    BrewSession session = this->GetSessionById(sessionId);
    vector<TempDataPoint> data = this->GetSessionData(sessionId);
    
    if (session.sessionId == 0 || data.empty()) {
        return "";
    }
    
    stringstream csv;
    csv << "Session ID,Schedule Name,Timestamp,Average Temp,Target Temp,PID Output\n";
    
    for (const auto& point : data) {
        csv << session.sessionId << ",";
        csv << "\"" << session.scheduleName << "\",";
        csv << point.timestamp << ",";
        csv << (int)point.avgTemp << ",";
        csv << (int)point.targetTemp << ",";
        csv << (int)point.pidOutput << "\n";
    }
    
    return csv.str();
}

string StatisticsManager::ExportAllSessionsToJson()
{
    vector<BrewSession> sessions = this->GetSessionList();
    
    stringstream json;
    json << "{\"sessions\":[";
    
    for (size_t i = 0; i < sessions.size(); i++) {
        if (i > 0) json << ",";
        
        json << "{";
        json << "\"sessionId\":" << sessions[i].sessionId << ",";
        json << "\"scheduleName\":\"" << sessions[i].scheduleName << "\",";
        json << "\"startTime\":" << sessions[i].startTime << ",";
        json << "\"endTime\":" << sessions[i].endTime << ",";
        json << "\"duration\":" << sessions[i].totalDuration << ",";
        json << "\"dataPoints\":" << sessions[i].dataPoints << ",";
        json << "\"avgTemperature\":" << fixed << setprecision(1) << sessions[i].avgTemperature << ",";
        json << "\"minTemperature\":" << (int)sessions[i].minTemperature << ",";
        json << "\"maxTemperature\":" << (int)sessions[i].maxTemperature << ",";
        json << "\"completed\":" << (sessions[i].completed ? "true" : "false");
        json << "}";
    }
    
    json << "]}";
    
    return json.str();
}