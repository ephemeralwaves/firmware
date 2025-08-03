#include "LoRabotModule.h"
#include "MeshService.h" 
#include "Preferences.h"
#include "RadioLibInterface.h"
#include <time.h>

// TODO: Research correct includes for:
// - Battery level reading
// - Power management
// - Hardware-specific functions

// Face definitions stored in flash memory (PROGMEM)
// Each face is tested for proper display on SSD1306 OLED
// Using basic ASCII + common Unicode that renders well on small displays

const char* const LoRabotModule::FACES[12] PROGMEM = {
    "( o . o )",    // AWAKE - neutral baseline (safe ASCII)
    "( > . > )",    // LOOKING_AROUND - scanning for nodes (right)
    "( < . < )",    // LOOKING_AROUND - scanning for nodes (left)
    "( ^ - ^ )",    // HAPPY - new nodes found
    "( * o * )",    // EXCITED - messages sent/received
    "( - _ - )",    // BORED - no network activity
    "( ~ _ ~ )",    // SLEEPY - night hours/low power
    "( ^ o ^ )",    // GRATEFUL - thankful for messages
    "( + _ + )/>>", // FRIEND - helped relay a message
    "( O _ O )",    // INTENSE - heavy message traffic
    "( / _ \\ )",   // DEMOTIVATED - isolation/poor signal
    "( ! . ! )"     // MOTIVATED - debug state
};

// Human-readable state names for debugging
const char* const LoRabotModule::STATE_NAMES[12] PROGMEM = {
    "Awake",
    "Looking R",
    "Looking L",
    "Happy", 
    "Excited",
    "Bored",
    "Sleepy",
    "Grateful",
    "Friend!",
    "Intense",
    "Sad",
    "Motivated"
};

// Funny messages for awake/looking states
const char* const LoRabotModule::FUNNY_MESSAGES[8] PROGMEM = {
    "Too cute to route.",
    "Mesh me, maybe?",
    "I sense...potential pals",
    "Any1 broadcasting snacks?",
    "LoRa? More like explore-a!",
    "Who's out there?",
    "Looking for friends...",
    "Mesh network detective!"
};

// Relay messages for FRIEND state
const char* const LoRabotModule::RELAY_MESSAGES[8] PROGMEM = {
    "Courier vibes activated",
    "Passing notes like school",
    "I'm a walking repeater",
    "Zoom! Message relayed",
    "Data whisperer at work",
    "Radio butler duties: done",
    "I relay, therefore I am",
    "Packet passed. I'm fast!"
};

// Global instance
LoRabotModule *loRabotModule;

// Constructor
LoRabotModule::LoRabotModule() : 
    SinglePortModule("lorabot", meshtastic_PortNum_TEXT_MESSAGE_APP),
    concurrency::OSThread("LoRabot")
{
    currentState = AWAKE;
    previousState = AWAKE;
    lastActivityTime = millis();
    lastStateChange = millis();
    networkEventCount = 0;
    currentNodeCount = 0;
    // batteryLevel = 100;  // TODO: Get from actual battery API
    friendCount = 0;
    displayNeedsUpdate = true;
    memset(lastDisplayedFace, 0, sizeof(lastDisplayedFace));
    memset(friends, 0, sizeof(friends));
    
    // Initialize node discovery tracking
    lastDiscoveredNode = 0;
    memset(lastNodeName, 0, sizeof(lastNodeName));
    nodeDiscoveryTime = 0;
    showingNewNode = false;
    
    // Initialize message tracking
    lastMessageTime = 0;
    excitedStartTime = 0;
    inExcitedState = false;
    showingMessagePopup = false;
    messagePopupTime = 0;
    memset(receivedMessageText, 0, sizeof(receivedMessageText));
    funnyMessageIndex = 0;
    relayMessageIndex = 0;
    
    // Initialize INTENSE state tracking
    memset(messageTimes, 0, sizeof(messageTimes));
    messageIndex = 0;
    inIntenseState = false;
    intenseStartTime = 0;
    
    // Initialize FRIEND state tracking
    lastRelayCount = 0;
    inFriendState = false;
    friendStartTime = 0;
    
    // Initialize looking state tracking
    lookingRight = true;
    lastLookingChange = 0;
    lookingCycle = 0; // 0=left, 1=right, 2=awake
    
    // Load saved state from preferences
    loadState();
    
    // Start the thread
    setIntervalFromNow(5000); // Initial 5 second interval
    
    // Debug output
    LOG_INFO("LoRabot Module initialized - wants UI frame: %s", wantUIFrame() ? "YES" : "NO");
    LOG_INFO("LoRabot Module getUIFrameObservable: %p", getUIFrameObservable());
}

LoRabotModule::~LoRabotModule() {
    saveState();
}

// Main thread execution
int32_t LoRabotModule::runOnce() {
    // Update pet state based on network activity and time
    updatePetState();
    
    // Monitor for new nodes by checking total node count
    size_t totalNodeCount = nodeDB->getNumMeshNodes();
    if (totalNodeCount != currentNodeCount) {
        // Node count changed - this means we discovered a new node!
        if (totalNodeCount > currentNodeCount) {
            // New node discovered
            lastDiscoveredNode = totalNodeCount;
            nodeDiscoveryTime = millis();
            showingNewNode = true;
            
            // Try to get the actual node name from the most recently added node
            // Find the newest node in the database
            const meshtastic_NodeInfoLite* newestNode = nullptr;
            uint32_t newestTime = 0;
            
            for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
                const meshtastic_NodeInfoLite* node = nodeDB->getMeshNodeByIndex(i);
                if (node && node->last_heard > newestTime) {
                    newestNode = node;
                    newestTime = node->last_heard;
                }
            }
            
            if (newestNode && newestNode->has_user && strlen(newestNode->user.long_name) > 0) {
                // Use the actual node name with "Hello Node" prefix
                snprintf(lastNodeName, sizeof(lastNodeName), "Hello  %s!", newestNode->user.long_name);
            } else {
                // Fallback to generic name
                snprintf(lastNodeName, sizeof(lastNodeName), "Hello Node %d!", totalNodeCount);
            }
            
            // Reduce logging to prevent UI interference
            LOG_DEBUG("LoRabot discovered new node: %s! Total nodes: %d", lastNodeName, totalNodeCount);
        }
        
        currentNodeCount = totalNodeCount;
        processNetworkEvent();
    }
    
    // Clear the "showing new node" flag after timeout
    if (showingNewNode && (millis() - nodeDiscoveryTime) > 10000) {
        showingNewNode = false;
        LOG_DEBUG("LoRabot clearing new node flag - returning to normal states");
    }
    
    // Remove heavy debug logging that was causing performance issues
    // Only log state changes occasionally to reduce interference
    
    // Check for relay events (FRIEND state)
    uint32_t now = millis();
    if (shouldTriggerFriend() && !inFriendState) {
        inFriendState = true;
        friendStartTime = now;
        relayMessageIndex = (relayMessageIndex + 1) % 8; // Rotate through 8 relay messages
        LOG_DEBUG("LoRabot triggered FRIEND state! Helped relay a message to another node.");
    }
    
    // TODO: Replace with correct battery API when found
    // batteryLevel = getBatteryLevel();  // Need to research correct function
    
    // Return next update interval (adaptive timing)
    return getUpdateInterval();
}

// Handle received mesh packets
ProcessMessage LoRabotModule::handleReceived(const meshtastic_MeshPacket &mp) {
    // Track network activity
    processNetworkEvent();
    
    // Check if this is a text message (portnum = 1) or position update (portnum = 3)
    if (mp.decoded.portnum == 1 || mp.decoded.portnum == 3) {
        // Track message time for INTENSE state detection
        uint32_t now = millis();
        messageTimes[messageIndex] = now;
        messageIndex = (messageIndex + 1) % 5; // Circular buffer
        
        // Check if we should trigger INTENSE state
        if (shouldTriggerIntense() && !inIntenseState) {
            inIntenseState = true;
            intenseStartTime = now;
            LOG_INFO("LoRabot triggered INTENSE state! More than 3 messages in 5 seconds.");
        }
        
        // Text message or position update received - trigger excited state
        lastMessageTime = millis();
        excitedStartTime = millis();
        inExcitedState = true;
        showingMessagePopup = true;
        messagePopupTime = millis();
        
        // Capture the actual message text if it's a text message
        if (mp.decoded.portnum == 1 && mp.decoded.payload.size > 0) {
            // Copy the message text (limit to 63 chars to leave room for null terminator)
            size_t copyLen = min((size_t)mp.decoded.payload.size, (size_t)63);
            memcpy(receivedMessageText, mp.decoded.payload.bytes, copyLen);
            receivedMessageText[copyLen] = '\0'; // Ensure null termination
            
            LOG_INFO("LoRabot received message: '%s'", receivedMessageText);
        } else {
            // For position updates or empty messages, use a generic message
            strcpy(receivedMessageText, "Position update!");
        }
        
        LOG_INFO("LoRabot received message (port %d) - triggering excited/grateful cycle!", mp.decoded.portnum);
        
        // IMMEDIATELY update state to EXCITED - don't wait for timing
        previousState = currentState;
        currentState = EXCITED;
        lastStateChange = millis();
        displayNeedsUpdate = true;
        
        LOG_INFO("LoRabot immediately changed to EXCITED state! Will cycle to GRATEFUL after 6 seconds.");
    } else {
        // Debug: log all received packets to see what ports are being received
        LOG_DEBUG("LoRabot received packet on port %d (not triggering excited)", mp.decoded.portnum);
    }
    
    // Update friend tracking if this is from a user
    if (mp.from != 0) {
        updateFriendsList(mp.from);
    }
    
    // Don't consume the packet - let other modules handle it
    return ProcessMessage::CONTINUE;
}

// Draw the pet on the OLED display
void LoRabotModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, 
                              int16_t x, int16_t y) {
    
    const char* currentFace = getCurrentFace();
    // Remove unused variable to eliminate warning
    // const char* stateName = (const char*)pgm_read_ptr(&STATE_NAMES[currentState]);
    
    // Remove heavy logging that was causing performance issues
    // LOG_INFO("LoRabot drawFrame called - state: %d (%s), face: %s, inExcitedState: %s", 
    //          currentState, stateName, currentFace, inExcitedState ? "YES" : "NO");
    
    // Always draw the frame (screen system expects this)
    // Clear the frame area completely
    //display->setColor(BLACK);
    //display->fillRect(x, y, 128, 64);
    display->setColor(WHITE);
    
    // Draw the pet face on the Left side (moved from center)
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->setFont(ArialMT_Plain_16);
    display->drawString(x + 34, y + 15, currentFace);
    
    // Draw state name, for debugging
    //display->setFont(ArialMT_Plain_10);
    //display->drawString(x + 34, y + 35, stateName);
    
    // Draw status info or node discovery message
    if (currentState == HAPPY && showingNewNode) {
        // Show the discovered node name when happy (new node found)
        display->setFont(ArialMT_Plain_10);
        display->drawString(x + 34, y + 50, lastNodeName);
    } else if (currentState == FRIEND) {
        // Show relay messages for FRIEND state
        const char* relayMsg = (const char*)pgm_read_ptr(&RELAY_MESSAGES[relayMessageIndex]);
        display->setFont(ArialMT_Plain_10);
        display->drawString(x + 64, y + 50, relayMsg);
    } else if (currentState == AWAKE || currentState == LOOKING_AROUND_LEFT || currentState == LOOKING_AROUND_RIGHT) {
        // Show funny messages for awake/looking states
        const char* funnyMsg = (const char*)pgm_read_ptr(&FUNNY_MESSAGES[funnyMessageIndex]);
        display->setFont(ArialMT_Plain_10);
        display->drawString(x + 64, y + 50, funnyMsg);
    } else {
        // Show normal status info - cache the favorite count to avoid repeated iteration
        char statusLine[32];
        
        // Only count favorites if we haven't done it recently
        static uint32_t lastFavoriteCount = 0;
        static uint32_t lastFavoriteCountTime = 0;
        uint32_t now = millis();
        
        if (now - lastFavoriteCountTime > 5000) { // Only count every 5 seconds
            uint32_t favoriteCount = 0;
            for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
                const meshtastic_NodeInfoLite* node = nodeDB->getMeshNodeByIndex(i);
                if (node && node->is_favorite) {
                    favoriteCount++;
                }
            }
            lastFavoriteCount = favoriteCount;
            lastFavoriteCountTime = now;
        }
        
        snprintf(statusLine, sizeof(statusLine), "Nodes:%d Friends:%d", 
                currentNodeCount, lastFavoriteCount);
        display->setFont(ArialMT_Plain_10);
        display->drawString(x + 64, y + 50, statusLine);
    }
    
    // Draw message popup on the Right side when excited
    if (showingMessagePopup && (millis() - messagePopupTime) < 5000) { // Show for 5 seconds
        display->setTextAlignment(TEXT_ALIGN_LEFT);
        display->setFont(ArialMT_Plain_10);
        display->drawString(x + 75, y + 15, "Message:");
        display->drawString(x + 75, y + 25, receivedMessageText);
    } else if (showingMessagePopup) {
        // Clear the popup after 5 seconds
        showingMessagePopup = false;
    }
    
    // Remove debug logging that was causing performance issues
    // LOG_DEBUG("LoRabot frame drawn - face: %s, state: %s", currentFace, stateName);
}

// Update pet emotional state
void LoRabotModule::updatePetState() {
    uint32_t now = millis();
    PetState newState = calculateNewState();
    
    // Allow immediate state changes for EXCITED state, but add delay for others
    bool shouldChangeState = false;
    if (newState != currentState) {
        if (newState == EXCITED || currentState == EXCITED) {
            // Allow immediate transitions to/from excited state
            shouldChangeState = true;
        } else if ((now - lastStateChange) > 5000) {  // Increased from 3000ms to 5000ms
            // Add delay for other state changes to prevent rapid switching
            shouldChangeState = true;
        }
    }
    
    if (shouldChangeState) {
        previousState = currentState;
        currentState = newState;
        lastStateChange = now;
        displayNeedsUpdate = true;
        
        // Rotate funny message when changing to awake/looking states
        if (newState == AWAKE || newState == LOOKING_AROUND_LEFT || newState == LOOKING_AROUND_RIGHT) {
            funnyMessageIndex = (funnyMessageIndex + 1) % 8; // Rotate through 8 funny messages
            // Remove debug logging to reduce interference
            // LOG_DEBUG("LoRabot rotating to funny message %d", funnyMessageIndex);
        }
        
        // Reduce logging to prevent UI interference
        LOG_DEBUG("LoRabot state changed: %d -> %d", previousState, currentState);
        
        // Save state periodically
        if ((now - lastStateChange) > 60000) { // Every minute
            saveState();
        }
    }
}

// Process network activity events
void LoRabotModule::processNetworkEvent() {
    networkEventCount++;
    lastActivityTime = millis();
    displayNeedsUpdate = true;
}

// Update friends list when we see a node
void LoRabotModule::updateFriendsList(uint32_t nodeId) {
    // Check if already a friend
    for (uint8_t i = 0; i < friendCount; i++) {
        if (friends[i].nodeId == nodeId) {
            friends[i].encounters++;
            friends[i].lastSeen = millis();
            return;
        }
    }
    
    // Add new potential friend if we have space
    if (friendCount < MAX_FRIENDS) {
        friends[friendCount].nodeId = nodeId;
        friends[friendCount].encounters = 1;
        friends[friendCount].lastSeen = millis();
        friendCount++;
    }
}

// Check if a node is considered a friend
bool LoRabotModule::isFriend(uint32_t nodeId) {
    for (uint8_t i = 0; i < friendCount; i++) {
        if (friends[i].nodeId == nodeId && 
            friends[i].encounters >= personality.friend_bond_threshold) {
            return true;
        }
    }
    return false;
}

// Check if it's night time
bool LoRabotModule::isNightTime() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return false; // If no time available, assume daytime
    }
    
    uint8_t hour = timeinfo.tm_hour;
    return (hour >= personality.sleepy_start_hour || hour < personality.sleepy_end_hour);
}

// Check if battery is low
bool LoRabotModule::isLowBattery() {
    // TODO: Implement when battery API is found
    return false;  // For now, assume battery is always OK
}

// Check if INTENSE state should be triggered
bool LoRabotModule::shouldTriggerIntense() {
    uint32_t now = millis();
    uint32_t fiveSecondsAgo = now - 5000; // 5 seconds ago
    
    // Count messages in the last 5 seconds
    uint8_t messageCount = 0;
    for (uint8_t i = 0; i < 5; i++) {
        if (messageTimes[i] > fiveSecondsAgo) {
            messageCount++;
        }
    }
    
    // Trigger INTENSE if more than 3 messages in 5 seconds
    return messageCount > 3;
}

// Check if FRIEND state should be triggered (relay events)
bool LoRabotModule::shouldTriggerFriend() {
    // Get current relay count from RadioLibInterface
    uint32_t currentRelayCount = 0;
    if (RadioLibInterface::instance) {
        currentRelayCount = RadioLibInterface::instance->txRelay;
    }
    
    // Check if relay count has increased (we relayed a message)
    if (currentRelayCount > lastRelayCount) {
        lastRelayCount = currentRelayCount;
        return true;
    }
    
    return false;
}

// Calculate new pet state based on current conditions
PetState LoRabotModule::calculateNewState() {
    uint32_t now = millis();
    //uint32_t timeSinceActivity = (now - lastActivityTime) / 1000; // seconds
    
    // Check for INTENSE state with 3-second duration (highest priority)
    if (inIntenseState) {
        uint32_t intenseDuration = (now - intenseStartTime) / 1000; // seconds
        if (intenseDuration < 3) {
            // Remove debug logging to reduce interference
            // LOG_DEBUG("LoRabot staying in INTENSE state - duration: %d seconds", intenseDuration);
            return INTENSE;
        } else {
            // Exit INTENSE state after 3 seconds
            inIntenseState = false;
            LOG_DEBUG("LoRabot exiting INTENSE state after 3 seconds");
        }
    }
    
    // Check for FRIEND state with 4-second duration (high priority)
    if (inFriendState) {
        uint32_t friendDuration = (now - friendStartTime) / 1000; // seconds
        if (friendDuration < 4) {
            // Remove debug logging to reduce interference
            // LOG_DEBUG("LoRabot staying in FRIEND state - duration: %d seconds", friendDuration);
            return FRIEND;
        } else {
            // Exit FRIEND state after 4 seconds
            inFriendState = false;
            LOG_DEBUG("LoRabot exiting FRIEND state after 4 seconds");
        }
    }
    
    // Check for excited/grateful cycle with minimum duration (6 seconds total)
    if (inExcitedState) {
        uint32_t excitedDuration = (now - excitedStartTime) / 1000; // seconds
        if (excitedDuration < 3) {
            // Stay in EXCITED for first 3 seconds
            return EXCITED;
        } else if (excitedDuration < 6) {
            // Switch to GRATEFUL for next 3 seconds
            // Remove debug logging to reduce interference
            // LOG_DEBUG("LoRabot switching to GRATEFUL state (excited duration: %d seconds)", excitedDuration);
            return GRATEFUL;
        } else {
            // Exit excited/grateful cycle after 6 seconds total
            inExcitedState = false;
            LOG_DEBUG("LoRabot exiting excited/grateful cycle after 6 seconds");
        }
    }
    
    // Check for specific priority states first
    if (isNightTime() || isLowBattery()) {
        return SLEEPY;
    }
    
    // Check for recent node discovery (highest priority) - show HAPPY when new node found
    if (showingNewNode && (now - nodeDiscoveryTime) < 8000) { // Show for 8 seconds
        // Remove debug logging to reduce interference
        // LOG_DEBUG("LoRabot in HAPPY state - showing new node for 8 seconds");
        return HAPPY;
    }
    
    // Default behavior: AWAKE state, then looking states when nodes are present
    if (currentNodeCount > 0) {
        // 3-state cycle: Looking Left → Looking Right → Awake → repeat
        uint32_t lookingDuration = (now - lastLookingChange) / 1000; // seconds
        if (lookingDuration > 5) { // Increased from 3 seconds to 5 seconds
            lookingCycle = (lookingCycle + 1) % 3; // Cycle through 0,1,2
            lastLookingChange = now;
            // Remove debug logging to reduce interference
            // LOG_DEBUG("LoRabot changing animation cycle to: %d", lookingCycle);
        }
        
        switch (lookingCycle) {
            case 0: // Looking Left
                // Remove debug logging to reduce interference
                // LOG_DEBUG("LoRabot in LOOKING LEFT state (nodes: %d)", currentNodeCount);
                return LOOKING_AROUND_LEFT;
            case 1: // Looking Right
                // Remove debug logging to reduce interference
                // LOG_DEBUG("LoRabot in LOOKING RIGHT state (nodes: %d)", currentNodeCount);
                return LOOKING_AROUND_RIGHT;
            case 2: // Awake
                // Remove debug logging to reduce interference
                // LOG_DEBUG("LoRabot in AWAKE state (nodes: %d)", currentNodeCount);
                return AWAKE;
            default:
                return AWAKE;
        }
    }
    
    // No nodes present - stay in AWAKE state
    // Remove debug logging to reduce interference
    // LOG_DEBUG("LoRabot in AWAKE state (no nodes)");
    return AWAKE;
}

// Get update interval based on current state and activity
uint32_t LoRabotModule::getUpdateInterval() {
    uint32_t baseInterval = 2000; // Increased from 1000ms to 2000ms baseline
    
    switch (currentState) {
        case INTENSE:
            return 1000; // Increased from 500ms to 1000ms for INTENSE state
        case FRIEND:
            return 1500; // Increased from 800ms to 1500ms for FRIEND state
        case EXCITED:
        case GRATEFUL:
            return 2000; // Increased from 1000ms to 2000ms for excited/grateful cycle
        case SLEEPY:
            return 8000; // Increased from 5000ms to 8000ms when sleepy
        case BORED:
        case DEMOTIVATED:
            return 10000; // Increased from 8000ms to 10000ms when inactive
        case HAPPY:
            return 5000; // Increased from 3000ms to 5000ms for positive states
        default:
            return baseInterval;
    }
    
    // TODO: Add battery-based adjustments when battery API is available
    // if (batteryLevel < 20) return interval * 2;  // Slower when low battery
}

// Get current face string
const char* LoRabotModule::getCurrentFace() {
    if (currentState >= 12) {
        LOG_WARN("LoRabot invalid state: %d, using fallback", currentState);
        return (const char*)pgm_read_ptr(&FACES[MOTIVATED]); // Fallback to motivated
    }
    
    const char* face = (const char*)pgm_read_ptr(&FACES[currentState]);
    // Remove debug logging to reduce interference
    // LOG_DEBUG("LoRabot getCurrentFace - state: %d, face: %s", currentState, face);
    return face;
}

// Save state to preferences
void LoRabotModule::saveState() {
    Preferences prefs;
    if (prefs.begin("lorabot", false)) {
        prefs.putUChar("state", currentState);
        prefs.putUInt("lastActivity", lastActivityTime);
        prefs.putUChar("friendCount", friendCount);
        
        // Save friends list
        if (friendCount > 0) {
            prefs.putBytes("friends", friends, sizeof(FriendNode) * friendCount);
        }
        
        prefs.end();
    }
}

// Load state from preferences
void LoRabotModule::loadState() {
    Preferences prefs;
    if (prefs.begin("lorabot", true)) {
        currentState = (PetState)prefs.getUChar("state", AWAKE);
        lastActivityTime = prefs.getUInt("lastActivity", millis());
        friendCount = prefs.getUChar("friendCount", 0);
        
        // Load friends list
        if (friendCount > 0 && friendCount <= MAX_FRIENDS) {
            size_t schLen = prefs.getBytesLength("friends");
            if (schLen == sizeof(FriendNode) * friendCount) {
                prefs.getBytes("friends", friends, schLen);
            } else {
                friendCount = 0; // Reset if data is corrupted
            }
        }
        
        prefs.end();
    }
}