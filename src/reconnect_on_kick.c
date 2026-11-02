#include <stdio.h>
#include <string.h>
#include <time.h>

#include "plugin_definitions.h"
#include "teamlog/logtypes.h"
#include "teamspeak/public_definitions.h"
#include "teamspeak/public_errors.h"
#include "ts3_functions.h"

#if defined(_WIN32)
#define TS3_PLUGIN_EXPORT __declspec(dllexport)
#else
#define TS3_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#ifndef TS3_PLUGIN_API_VERSION
#define TS3_PLUGIN_API_VERSION 26
#endif

#ifndef TS3_PLUGIN_VERSION
#define TS3_PLUGIN_VERSION "1.0.0"
#endif

#define MAX_TRACKED_CONNECTIONS 32
#define HOST_BUFSIZE 512
#define SERVER_ADDRESS_BUFSIZE 640
#define PASSWORD_BUFSIZE 512
#define CHANNEL_BUFSIZE 512
#define NICKNAME_BUFSIZE 128
#define SERVER_LABEL_BUFSIZE 256
#define LOG_BUFSIZE 1024

struct ReconnectState {
    uint64 handlerID;
    anyID ownClientID;
    int hasOwnClientID;
    int hadEstablishedConnection;
    int lastStatus;
    int pendingReconnect;
    int selfKickConfirmed;
    time_t scheduledAt;
    char serverLabel[SERVER_LABEL_BUFSIZE];
    char serverAddress[SERVER_ADDRESS_BUFSIZE];
    char serverPassword[PASSWORD_BUFSIZE];
    char nickname[NICKNAME_BUFSIZE];
    char channelPath[CHANNEL_BUFSIZE];
    char channelPassword[PASSWORD_BUFSIZE];
};

static struct TS3Functions ts3Functions;
static struct ReconnectState reconnectStates[MAX_TRACKED_CONNECTIONS];

static void copy_string(char* dest, size_t destSize, const char* src)
{
    if (!dest || destSize == 0) {
        return;
    }

    snprintf(dest, destSize, "%s", src ? src : "");
}

static void log_message(enum LogLevel level, uint64 handlerID, const char* message)
{
    if (ts3Functions.logMessage && message) {
        ts3Functions.logMessage(message, level, "ReconnectOnKick", handlerID);
    }
}

static void print_to_current_tab(const char* message)
{
    if (ts3Functions.printMessageToCurrentTab && message) {
        ts3Functions.printMessageToCurrentTab(message);
    }
}

static int ascii_tolower(int value)
{
    if (value >= 'A' && value <= 'Z') {
        return value + ('a' - 'A');
    }

    return value;
}

static int contains_case_insensitive(const char* haystack, const char* needle)
{
    size_t haystackOffset;
    size_t needleOffset;

    if (!haystack || !needle || needle[0] == '\0') {
        return 0;
    }

    for (haystackOffset = 0; haystack[haystackOffset] != '\0'; ++haystackOffset) {
        for (needleOffset = 0; needle[needleOffset] != '\0'; ++needleOffset) {
            if (haystack[haystackOffset + needleOffset] == '\0') {
                return 0;
            }

            if (ascii_tolower((unsigned char)haystack[haystackOffset + needleOffset]) != ascii_tolower((unsigned char)needle[needleOffset])) {
                break;
            }
        }

        if (needle[needleOffset] == '\0') {
            return 1;
        }
    }

    return 0;
}

static int text_mentions_kick(const char* text)
{
    return contains_case_insensitive(text, "kick") || contains_case_insensitive(text, "kicked");
}

static void log_error_code(uint64 handlerID, const char* context, unsigned int error)
{
    char message[LOG_BUFSIZE];
    char* errorMessage = NULL;

    if (ts3Functions.getErrorMessage && error != ERROR_ok && ts3Functions.getErrorMessage(error, &errorMessage) == ERROR_ok && errorMessage) {
        snprintf(message, sizeof(message), "%s failed: %s (%u)", context, errorMessage, error);
        if (ts3Functions.freeMemory) {
            ts3Functions.freeMemory(errorMessage);
        }
    } else {
        snprintf(message, sizeof(message), "%s failed with error %u", context, error);
    }

    log_message(LogLevel_ERROR, handlerID, message);
}

static struct ReconnectState* find_state(uint64 handlerID)
{
    size_t i;

    if (handlerID == 0) {
        return NULL;
    }

    for (i = 0; i < MAX_TRACKED_CONNECTIONS; ++i) {
        if (reconnectStates[i].handlerID == handlerID) {
            return &reconnectStates[i];
        }
    }

    return NULL;
}

static struct ReconnectState* get_or_create_state(uint64 handlerID)
{
    size_t i;
    struct ReconnectState* state = find_state(handlerID);

    if (state) {
        return state;
    }

    if (handlerID == 0) {
        return NULL;
    }

    for (i = 0; i < MAX_TRACKED_CONNECTIONS; ++i) {
        if (reconnectStates[i].handlerID == 0) {
            memset(&reconnectStates[i], 0, sizeof(reconnectStates[i]));
            reconnectStates[i].handlerID = handlerID;
            return &reconnectStates[i];
        }
    }

    log_message(LogLevel_ERROR, handlerID, "No free reconnect state slots available");
    return NULL;
}

static void clear_state(uint64 handlerID)
{
    struct ReconnectState* state = find_state(handlerID);

    if (state) {
        memset(state, 0, sizeof(*state));
    }
}

static void format_server_address(char* dest, size_t destSize, const char* host, unsigned short port)
{
    int hostLooksLikeIPv6;
    int hostAlreadyBracketed;

    if (!dest || destSize == 0) {
        return;
    }

    if (!host || host[0] == '\0') {
        dest[0] = '\0';
        return;
    }

    if (port == 0) {
        copy_string(dest, destSize, host);
        return;
    }

    hostLooksLikeIPv6 = strchr(host, ':') != NULL;
    hostAlreadyBracketed = host[0] == '[';

    if (hostLooksLikeIPv6 && !hostAlreadyBracketed) {
        snprintf(dest, destSize, "[%s]:%hu", host, port);
    } else {
        snprintf(dest, destSize, "%s:%hu", host, port);
    }
}

static void remember_own_client_id(uint64 handlerID)
{
    anyID ownClientID = 0;
    struct ReconnectState* state;

    if (!ts3Functions.getClientID || ts3Functions.getClientID(handlerID, &ownClientID) != ERROR_ok) {
        return;
    }

    state = get_or_create_state(handlerID);
    if (!state) {
        return;
    }

    state->ownClientID = ownClientID;
    state->hasOwnClientID = 1;
}

static void remember_status(uint64 handlerID, int status)
{
    struct ReconnectState* state = get_or_create_state(handlerID);

    if (!state) {
        return;
    }

    state->lastStatus = status;
    if (status == STATUS_CONNECTION_ESTABLISHED) {
        state->hadEstablishedConnection = 1;
    }
}

enum ClientSelfCheck {
    CLIENT_IS_NOT_SELF = 0,
    CLIENT_IS_SELF = 1,
    CLIENT_SELF_UNKNOWN = 2
};

static enum ClientSelfCheck check_client_is_self(uint64 handlerID, anyID clientID)
{
    anyID ownClientID = 0;
    struct ReconnectState* state;

    if (ts3Functions.getClientID && ts3Functions.getClientID(handlerID, &ownClientID) == ERROR_ok) {
        return clientID == ownClientID ? CLIENT_IS_SELF : CLIENT_IS_NOT_SELF;
    }

    state = find_state(handlerID);
    if (state && state->hasOwnClientID) {
        return clientID == state->ownClientID ? CLIENT_IS_SELF : CLIENT_IS_NOT_SELF;
    }

    return CLIENT_SELF_UNKNOWN;
}

static void remember_current_nickname(uint64 handlerID, struct ReconnectState* state)
{
    char* nickname = NULL;

    state->nickname[0] = '\0';

    if (!ts3Functions.getClientSelfVariableAsString) {
        return;
    }

    if (ts3Functions.getClientSelfVariableAsString(handlerID, CLIENT_NICKNAME, &nickname) == ERROR_ok && nickname) {
        copy_string(state->nickname, sizeof(state->nickname), nickname);
        if (ts3Functions.freeMemory) {
            ts3Functions.freeMemory(nickname);
        }
    }
}

static void remember_server_label(uint64 handlerID, struct ReconnectState* state, const char* fallback)
{
    char* serverName = NULL;

    copy_string(state->serverLabel, sizeof(state->serverLabel), fallback);

    if (!ts3Functions.getServerVariableAsString) {
        return;
    }

    if (ts3Functions.getServerVariableAsString(handlerID, VIRTUALSERVER_NAME, &serverName) == ERROR_ok && serverName && serverName[0] != '\0') {
        copy_string(state->serverLabel, sizeof(state->serverLabel), serverName);
        if (ts3Functions.freeMemory) {
            ts3Functions.freeMemory(serverName);
        }
    }
}

static int remember_server_connect_info(uint64 handlerID, struct ReconnectState* state)
{
    char host[HOST_BUFSIZE];
    char serverPassword[PASSWORD_BUFSIZE];
    unsigned short port = 0;

    memset(host, 0, sizeof(host));
    memset(serverPassword, 0, sizeof(serverPassword));

    if (!ts3Functions.getServerConnectInfo ||
        ts3Functions.getServerConnectInfo(handlerID, host, &port, serverPassword, sizeof(host)) != ERROR_ok) {
        return 0;
    }

    format_server_address(state->serverAddress, sizeof(state->serverAddress), host, port);
    copy_string(state->serverPassword, sizeof(state->serverPassword), serverPassword);
    remember_server_label(handlerID, state, host);
    remember_current_nickname(handlerID, state);
    return 1;
}

static int remember_channel_connect_info(uint64 handlerID, uint64 channelID, struct ReconnectState* state)
{
    char channelPath[CHANNEL_BUFSIZE];
    char channelPassword[PASSWORD_BUFSIZE];

    if (channelID == 0 || !ts3Functions.getChannelConnectInfo) {
        return 0;
    }

    memset(channelPath, 0, sizeof(channelPath));
    memset(channelPassword, 0, sizeof(channelPassword));

    if (ts3Functions.getChannelConnectInfo(handlerID, channelID, channelPath, channelPassword, sizeof(channelPath)) != ERROR_ok) {
        return 0;
    }

    copy_string(state->channelPath, sizeof(state->channelPath), channelPath);
    copy_string(state->channelPassword, sizeof(state->channelPassword), channelPassword);
    return 1;
}

static void remember_current_channel(uint64 handlerID, struct ReconnectState* state)
{
    anyID ownClientID = 0;
    uint64 channelID = 0;

    if (!ts3Functions.getClientID || !ts3Functions.getChannelOfClient) {
        return;
    }

    if (ts3Functions.getClientID(handlerID, &ownClientID) != ERROR_ok) {
        return;
    }

    if (ts3Functions.getChannelOfClient(handlerID, ownClientID, &channelID) != ERROR_ok) {
        return;
    }

    remember_channel_connect_info(handlerID, channelID, state);
}

static void refresh_connected_target(uint64 handlerID)
{
    struct ReconnectState* state = get_or_create_state(handlerID);

    if (!state) {
        return;
    }

    remember_server_connect_info(handlerID, state);
    remember_current_channel(handlerID, state);
}

static void refresh_channel_if_self(uint64 handlerID, anyID clientID, uint64 channelID)
{
    struct ReconnectState* state;

    if (check_client_is_self(handlerID, clientID) != CLIENT_IS_SELF) {
        return;
    }

    state = get_or_create_state(handlerID);
    if (!state) {
        return;
    }

    remember_channel_connect_info(handlerID, channelID, state);
}

static int remember_reconnect_target(uint64 handlerID, uint64 oldChannelID, int selfKickConfirmed)
{
    struct ReconnectState* state = get_or_create_state(handlerID);

    if (!state) {
        return 0;
    }

    if (!remember_server_connect_info(handlerID, state) && state->serverAddress[0] == '\0') {
        log_message(LogLevel_ERROR, handlerID, "Could not read server connection info before reconnect");
        state->pendingReconnect = 0;
        return 0;
    }

    if (oldChannelID != 0 && !remember_channel_connect_info(handlerID, oldChannelID, state) && state->channelPath[0] == '\0') {
        state->channelPath[0] = '\0';
        state->channelPassword[0] = '\0';
        log_message(LogLevel_INFO, handlerID, "Channel reconnect info was unavailable; reconnecting to the server default channel");
    }

    state->pendingReconnect = 1;
    state->selfKickConfirmed = selfKickConfirmed;
    state->scheduledAt = time(NULL);
    log_message(LogLevel_INFO, handlerID, "Scheduled reconnect after server kick");
    print_to_current_tab("Reconnect on Server Kick: reconnect scheduled.");
    return 1;
}

static int pending_reconnect_expired(const struct ReconnectState* state)
{
    time_t now;

    if (!state || state->selfKickConfirmed || state->scheduledAt == 0) {
        return 0;
    }

    now = time(NULL);
    return difftime(now, state->scheduledAt) > 10.0;
}

static void reconnect_if_pending(uint64 handlerID)
{
    struct ReconnectState* state = find_state(handlerID);
    struct ReconnectState reconnectTarget;
    uint64 newHandlerID = 0;
    unsigned int error;
    char message[LOG_BUFSIZE];

    if (!state || !state->pendingReconnect) {
        return;
    }

    if (state->serverAddress[0] == '\0' || pending_reconnect_expired(state)) {
        clear_state(handlerID);
        return;
    }

    reconnectTarget = *state;
    clear_state(handlerID);

    if (!ts3Functions.guiConnect) {
        log_message(LogLevel_ERROR, handlerID, "TeamSpeak guiConnect function is unavailable");
        print_to_current_tab("Reconnect on Server Kick: TeamSpeak guiConnect is unavailable.");
        return;
    }

    error = ts3Functions.guiConnect(
        PLUGIN_CONNECT_TAB_CURRENT,
        reconnectTarget.serverLabel,
        reconnectTarget.serverAddress,
        reconnectTarget.serverPassword,
        reconnectTarget.nickname,
        reconnectTarget.channelPath,
        reconnectTarget.channelPassword,
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        &newHandlerID);

    if (error != ERROR_ok) {
        log_error_code(handlerID, "Reconnect after server kick", error);
        print_to_current_tab("Reconnect on Server Kick: reconnect request failed; check the client log.");
        return;
    }

    snprintf(message, sizeof(message), "Reconnect requested for %s", reconnectTarget.serverAddress);
    log_message(LogLevel_INFO, newHandlerID ? newHandlerID : handlerID, message);
    print_to_current_tab("Reconnect on Server Kick: reconnect requested.");
}

static int disconnect_error_allows_fallback(unsigned int errorNumber)
{
    return errorNumber == ERROR_ok || errorNumber == ERROR_not_connected || errorNumber == ERROR_client_invalid_id;
}

static void reconnect_on_clean_disconnect_if_possible(uint64 handlerID, unsigned int errorNumber)
{
    struct ReconnectState* state = find_state(handlerID);

    if (!state || state->pendingReconnect || !state->hadEstablishedConnection || state->serverAddress[0] == '\0') {
        return;
    }

    if (!disconnect_error_allows_fallback(errorNumber)) {
        return;
    }

    state->pendingReconnect = 1;
    state->selfKickConfirmed = 0;
    state->scheduledAt = time(NULL);
    log_message(LogLevel_WARNING, handlerID, "Disconnected without kick callback; reconnecting using cached server target");
    print_to_current_tab("Reconnect on Server Kick: reconnecting after disconnect.");
    reconnect_if_pending(handlerID);
}

static void reconnect_if_handler_is_disconnected(uint64 handlerID)
{
    int status = STATUS_DISCONNECTED;

    if (ts3Functions.getConnectionStatus && ts3Functions.getConnectionStatus(handlerID, &status) == ERROR_ok && status != STATUS_DISCONNECTED) {
        return;
    }

    reconnect_if_pending(handlerID);
}

TS3_PLUGIN_EXPORT const char* ts3plugin_name(void)
{
    return "Reconnect on Server Kick";
}

TS3_PLUGIN_EXPORT const char* ts3plugin_version(void)
{
    return TS3_PLUGIN_VERSION;
}

TS3_PLUGIN_EXPORT int ts3plugin_apiVersion(void)
{
    return TS3_PLUGIN_API_VERSION;
}

TS3_PLUGIN_EXPORT const char* ts3plugin_author(void)
{
    return "lbenicio";
}

TS3_PLUGIN_EXPORT const char* ts3plugin_description(void)
{
    return "Reconnects the current TeamSpeak tab to the same server after your own client is kicked from the server.";
}

TS3_PLUGIN_EXPORT void ts3plugin_setFunctionPointers(const struct TS3Functions funcs)
{
    ts3Functions = funcs;
}

TS3_PLUGIN_EXPORT int ts3plugin_init(void)
{
    memset(reconnectStates, 0, sizeof(reconnectStates));
    log_message(LogLevel_INFO, 0, "Reconnect on Server Kick loaded");
    print_to_current_tab("Reconnect on Server Kick 1.0.1 loaded.");
    return 0;
}

TS3_PLUGIN_EXPORT void ts3plugin_shutdown(void)
{
    memset(reconnectStates, 0, sizeof(reconnectStates));
}

TS3_PLUGIN_EXPORT int ts3plugin_requestAutoload(void)
{
    return 1;
}

TS3_PLUGIN_EXPORT int ts3plugin_offersConfigure(void)
{
    return PLUGIN_OFFERS_NO_CONFIGURE;
}

TS3_PLUGIN_EXPORT void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID, int newStatus, unsigned int errorNumber)
{
    char message[LOG_BUFSIZE];

    snprintf(message, sizeof(message), "Connect status changed: handler=%llu status=%d error=%u", (long long unsigned int)serverConnectionHandlerID, newStatus, errorNumber);
    log_message(LogLevel_INFO, serverConnectionHandlerID, message);
    remember_status(serverConnectionHandlerID, newStatus);

    if (newStatus == STATUS_CONNECTION_ESTABLISHED) {
        remember_own_client_id(serverConnectionHandlerID);
        refresh_connected_target(serverConnectionHandlerID);
        return;
    }

    if (newStatus == STATUS_DISCONNECTED) {
        reconnect_if_pending(serverConnectionHandlerID);
        reconnect_on_clean_disconnect_if_possible(serverConnectionHandlerID, errorNumber);
    }
}

TS3_PLUGIN_EXPORT int ts3plugin_onServerErrorEvent(uint64 serverConnectionHandlerID, const char* errorMessage, unsigned int error, const char* returnCode, const char* extraMessage)
{
    char message[LOG_BUFSIZE];
    struct ReconnectState* state;

    snprintf(
        message,
        sizeof(message),
        "Server error callback: handler=%llu error=%u message=%s extra=%s returnCode=%s",
        (long long unsigned int)serverConnectionHandlerID,
        error,
        errorMessage ? errorMessage : "",
        extraMessage ? extraMessage : "",
        returnCode ? returnCode : "");
    log_message(LogLevel_INFO, serverConnectionHandlerID, message);

    if (!text_mentions_kick(errorMessage) && !text_mentions_kick(extraMessage)) {
        return 0;
    }

    state = get_or_create_state(serverConnectionHandlerID);
    if (!state || state->serverAddress[0] == '\0') {
        return 0;
    }

    state->pendingReconnect = 1;
    state->selfKickConfirmed = 1;
    state->scheduledAt = time(NULL);
    log_message(LogLevel_INFO, serverConnectionHandlerID, "Server error callback indicated kick; scheduled reconnect");
    reconnect_if_handler_is_disconnected(serverConnectionHandlerID);

    return 0;
}

TS3_PLUGIN_EXPORT void ts3plugin_onClientKickFromServerEvent(
    uint64 serverConnectionHandlerID,
    anyID clientID,
    uint64 oldChannelID,
    uint64 newChannelID,
    int visibility,
    anyID kickerID,
    const char* kickerName,
    const char* kickerUniqueIdentifier,
    const char* kickMessage)
{
    enum ClientSelfCheck selfCheck;

    (void)newChannelID;
    (void)visibility;
    (void)kickerID;
    (void)kickerName;
    (void)kickerUniqueIdentifier;
    (void)kickMessage;

    selfCheck = check_client_is_self(serverConnectionHandlerID, clientID);
    if (selfCheck == CLIENT_IS_NOT_SELF) {
        return;
    }

    if (selfCheck == CLIENT_SELF_UNKNOWN) {
        log_message(LogLevel_WARNING, serverConnectionHandlerID, "Could not prove kicked client is self; scheduling reconnect only if disconnect follows immediately");
    }

    if (remember_reconnect_target(serverConnectionHandlerID, oldChannelID, selfCheck == CLIENT_IS_SELF)) {
        reconnect_if_handler_is_disconnected(serverConnectionHandlerID);
    }
}

TS3_PLUGIN_EXPORT void ts3plugin_onClientMoveEvent(
    uint64 serverConnectionHandlerID,
    anyID clientID,
    uint64 oldChannelID,
    uint64 newChannelID,
    int visibility,
    const char* moveMessage)
{
    (void)oldChannelID;
    (void)visibility;
    (void)moveMessage;

    refresh_channel_if_self(serverConnectionHandlerID, clientID, newChannelID);
}

TS3_PLUGIN_EXPORT void ts3plugin_onClientMoveMovedEvent(
    uint64 serverConnectionHandlerID,
    anyID clientID,
    uint64 oldChannelID,
    uint64 newChannelID,
    int visibility,
    anyID moverID,
    const char* moverName,
    const char* moverUniqueIdentifier,
    const char* moveMessage)
{
    (void)oldChannelID;
    (void)visibility;
    (void)moverID;
    (void)moverName;
    (void)moverUniqueIdentifier;
    (void)moveMessage;

    refresh_channel_if_self(serverConnectionHandlerID, clientID, newChannelID);
}

TS3_PLUGIN_EXPORT void ts3plugin_onClientKickFromChannelEvent(
    uint64 serverConnectionHandlerID,
    anyID clientID,
    uint64 oldChannelID,
    uint64 newChannelID,
    int visibility,
    anyID kickerID,
    const char* kickerName,
    const char* kickerUniqueIdentifier,
    const char* kickMessage)
{
    (void)oldChannelID;
    (void)visibility;
    (void)kickerID;
    (void)kickerName;
    (void)kickerUniqueIdentifier;
    (void)kickMessage;

    refresh_channel_if_self(serverConnectionHandlerID, clientID, newChannelID);
}
