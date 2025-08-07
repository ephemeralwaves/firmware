#include "LoRabotModule.h"
#include "MeshService.h" 
#include "Preferences.h"
#include "RadioLibInterface.h"
#include <time.h>

// External declarations
extern TextMessageModule *textMessageModule;

// TODO: Research correct includes for:
// - Battery level reading
// - Power management
// - Hardware-specific functions

// Face definitions stored in flash memory (PROGMEM)
// Each face is tested for proper display on SSD1306 OLED
// Using basic ASCII + common Unicode that renders well on small displays

const char* const LoRabotModule::FACES[11] PROGMEM = {
    "( o . o )",    // AWAKE - neutral baseline (safe ASCII)
    "( > . > )",    // LOOKING_AROUND - scanning for nodes (right)
    "( < . < )",    // LOOKING_AROUND - scanning for nodes (left)
    "( ^ - ^ )",    // HAPPY - new nodes found
    "( * o * )",    // EXCITED - messages received (triggers excited/grateful cycle)
    "( - _ - )",    // BORED - no network activity - not implemented, WIP
    "( ~ _ ~ )",    // SLEEPY - night hours/low power
    "( ^ o ^ )",    // GRATEFUL - thankful for received messages
    "( O _ O )",    // INTENSE - heavy message traffic- not implemented, WIP
    "( / _ \\ )",   // DEMOTIVATED - isolation/poor signal
    "(  ' . ')>"   // SENDER - messages sent by user on a channel
};

// Human-readable state names for debugging
const char* const LoRabotModule::STATE_NAMES[11] PROGMEM = {
    "Awake",
    "Looking R",
    "Looking L",
    "Happy", 
    "Excited",
    "Bored",
    "Sleepy",
    "Grateful",
    "Intense",
    "Sad",
    "Sender"
};

// Messages for AWAKE/LOOKING states
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



// Sender messages for SENDER state
const char* const LoRabotModule::SENDER_MESSAGES[5] PROGMEM = {
    "Message Sent!",
    "Off it goes!",
    "Beamed it!",
    "Packet away!",
    "Data transmitted"
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
    
    // Initialize INTENSE state tracking
    memset(messageTimes, 0, sizeof(messageTimes));
    messageIndex = 0;
    inIntenseState = false;
    intenseStartTime = 0;
    

    
    // Initialize SENDER state tracking
    inSenderState = false;
    senderStartTime = 0;
    senderMessageIndex = 0;
    
    // Initialize sending flag
    isSendingMessage = false;
    
    // NEW: Initialize enhanced SENDER state detection
    lastTxGoodCount = 0;
    lastTextMessageTxTime = 0;
    pendingSenderTrigger = false;
    senderDetectionWindow = 2000; // 2 second window for correlation
    
    // Initialize looking state tracking
    lookingRight = true;
    lastLookingChange = 0;
    lookingCycle = 0; // 0=left, 1=right, 2=awake
    lastFaceAnimationTime = 0; // Track face animation separately from thread timing
    
    // Load saved state from preferences
    loadState();
    
    // Start the thread
    setIntervalFromNow(10000); // Initial 10 second interval for better performance
    
    // Debug output
    //LOG_INFO("LoRabot Module initialized - wants UI frame: %s", wantUIFrame() ? "YES" : "NO");
    //LOG_INFO("LoRabot Module getUIFrameObservable: %p", getUIFrameObservable());
}

LoRabotModule::~LoRabotModule() {
    saveState();
}

// Main thread execution
int32_t LoRabotModule::runOnce() {
    // Update pet state based on network activity and time
    updatePetState();
    
        // Monitor for new nodes by checking total node count - only check every few cycles
    static uint8_t nodeCheckCounter = 0;
    nodeCheckCounter++;
    if (nodeCheckCounter >= 4) { // Only check every 4 cycles (much less frequent)
        nodeCheckCounter = 0;
        size_t totalNodeCount = nodeDB->getNumMeshNodes();
        
        // Only process if node count actually changed
        if (totalNodeCount != currentNodeCount) {
            // Check if this is actually a new node discovery (not just us sending a message)
            // Look for the node that was most recently heard
            const meshtastic_NodeInfoLite* newestNode = nullptr;
            uint32_t newestTime = 0;
            bool foundNewNode = false;
            
            for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
                const meshtastic_NodeInfoLite* node = nodeDB->getMeshNodeByIndex(i);
                if (node && node->num != nodeDB->getNodeNum()) {
                    // Don't include our own node
                    if (node->last_heard > newestTime) {
                        newestNode = node;
                        newestTime = node->last_heard;
                    }
                    
                    // Check if this node was not in our previous count
                    bool isNewNode = true;
                    for (size_t j = 0; j < currentNodeCount; j++) {
                        const meshtastic_NodeInfoLite* oldNode = nodeDB->getMeshNodeByIndex(j);
                        if (oldNode && oldNode->num == node->num) {
                            isNewNode = false;
                            break;
                        }
                    }
                    if (isNewNode) {
                        foundNewNode = true;
                    }
                }
            }
            
            // Only trigger HAPPY state if we found an actual new node and we're not currently sending
            if (foundNewNode && newestNode && newestNode->num != nodeDB->getNodeNum() && !isSendingMessage) {
                // New node discovered
                lastDiscoveredNode = totalNodeCount;
                nodeDiscoveryTime = millis();
                showingNewNode = true;
                
                if (newestNode->has_user && strlen(newestNode->user.long_name) > 0) {
                    // Use the actual node name with "Hello Node" prefix
                    snprintf(lastNodeName, sizeof(lastNodeName), "Hello %s!", newestNode->user.long_name);
                } else if (newestNode->has_user && strlen(newestNode->user.short_name) > 0) {
                    // Fallback to short name if long name is not available
                    snprintf(lastNodeName, sizeof(lastNodeName), "Hello %s!", newestNode->user.short_name);
                } else {
                    // Fallback to generic name (node number) - format as hex for better readability
                    snprintf(lastNodeName, sizeof(lastNodeName), "Hello Node 0x%x!", newestNode->num);
                }
                
                // Immediately trigger HAPPY state for new node discovery
                previousState = currentState;
                currentState = HAPPY;
                lastStateChange = millis();
                displayNeedsUpdate = true;
                
                LOG_DEBUG("LoRabot discovered new node: %s! Total nodes: %d", lastNodeName, totalNodeCount);
            }
            
            // Update current node count
            currentNodeCount = totalNodeCount;
            processNetworkEvent();
            
            // Clear cached status line when node count changes so it gets recalculated
            static char cachedStatusLine[32] = "";
            cachedStatusLine[0] = '\0'; // Clear the cache
        }
        
        // Clear the "showing new node" flag after timeout
        if (showingNewNode && (millis() - nodeDiscoveryTime) > 10000) {
            showingNewNode = false;
            LOG_DEBUG("LoRabot clearing new node flag - returning to normal states");
        }
        
        // Clear sending flag if it gets stuck (safety timeout)
        if (isSendingMessage && (millis() - senderStartTime) > 5000) {
            isSendingMessage = false;
            LOG_DEBUG("LoRabot clearing stuck sending flag");
        }
    }
    

        
        // ENHANCED SENDER state detection - correlates txGood increases with text message detection
        static uint32_t lastSenderCheck = 0;
        uint32_t now = millis();
        if (now - lastSenderCheck > 1000) { // Check every second
            lastSenderCheck = now;
            
            if (RadioLibInterface::instance && !inSenderState && !isSendingMessage) {
                uint32_t currentTxGood = RadioLibInterface::instance->txGood;
                
                // Check if txGood increased (we sent something)
                if (currentTxGood > lastTxGoodCount) {
                    // Check if we recently detected a text message transmission pattern
                    if (pendingSenderTrigger && (now - lastTextMessageTxTime) < senderDetectionWindow) {
                        LOG_INFO("LoRabot detected text message transmission - triggering SENDER state");
                        triggerSenderState();
                        pendingSenderTrigger = false; // Clear the flag
                    } else {
                        LOG_DEBUG("LoRabot detected transmission but not a text message (txGood: %d -> %d)", 
                                  lastTxGoodCount, currentTxGood);
                    }
                    lastTxGoodCount = currentTxGood;
                }
            }
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
    
    // Debug: log all received packets to see what's happening
    LOG_DEBUG("LoRabot received packet - from: %d, port: %d, source: %d, our node: %d, isFromUs: %s", 
              mp.from, mp.decoded.portnum, mp.decoded.source, nodeDB->getNodeNum(), isFromUs(&mp) ? "YES" : "NO");
    
    // Check if this is a received text message (portnum = 1) or position update (portnum = 3)
    // Only trigger EXCITED for received messages, not sent messages
    if ((mp.decoded.portnum == 1 || mp.decoded.portnum == 3) && mp.from != 0) {
        // Track message time for INTENSE state detection
        uint32_t now = millis();
        messageTimes[messageIndex] = now;
        messageIndex = (messageIndex + 1) % 5; // Circular buffer
        
        // Check if we should trigger INTENSE state
        if (shouldTriggerIntense() && !inIntenseState) {
            inIntenseState = true;
            intenseStartTime = now;
            // LOG_INFO("LoRabot triggered INTENSE state! More than 3 messages in 5 seconds.");
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
            
            // LOG_INFO("LoRabot received message: '%s'", receivedMessageText);
        } else {
            // For position updates or empty messages, use a generic message
            strcpy(receivedMessageText, "Position update!");
        }
        
       // LOG_INFO("LoRabot received message (port %d) - triggering excited/grateful cycle!", mp.decoded.portnum);
        
        // IMMEDIATELY update state to EXCITED - don't wait for timing
        previousState = currentState;
        currentState = EXCITED;
        lastStateChange = millis();
        displayNeedsUpdate = true;
        
        //LOG_INFO("LoRabot immediately changed to EXCITED state! Will cycle to GRATEFUL after 6 seconds.");
    } else if (mp.decoded.portnum == 1) {
        // ENHANCED: Comprehensive text message detection for SENDER state
        uint32_t now = millis();
        bool detectedSentMessage = false;
        
        // Check multiple conditions for detecting sent messages
        if (mp.from == nodeDB->getNodeNum() || isFromUs(&mp)) {
            // Direct detection: message is from our node
            LOG_INFO("LoRabot detected sent text message (direct) - triggering SENDER state");
            detectedSentMessage = true;
        } else if (mp.from == 0 && mp.decoded.source == nodeDB->getNodeNum()) {
            // Detection via source field
            LOG_INFO("LoRabot detected sent text message (via source) - triggering SENDER state");
            detectedSentMessage = true;
        } else if (mp.from == 0 && mp.to != 0xffffffff) {
            // Direct message detection
            LOG_INFO("LoRabot detected sent direct text message - triggering SENDER state");
            detectedSentMessage = true;
        } else if (mp.from == 0) {
            // Local message detection
            LOG_INFO("LoRabot detected sent local text message - triggering SENDER state");
            detectedSentMessage = true;
        } else {
            // Enhanced pattern detection for edge cases
            // Check if this looks like a text message we might have sent
            if (mp.from == 0 || mp.decoded.source == nodeDB->getNodeNum()) {
                LOG_DEBUG("LoRabot detected potential text message sending pattern - setting pending trigger");
                pendingSenderTrigger = true;
                lastTextMessageTxTime = now;
                isSendingMessage = true;
            }
            
            // Debug: log text messages that don't trigger SENDER state
            LOG_DEBUG("LoRabot received text message but not triggering SENDER - from: %d, source: %d, to: 0x%x, our node: %d", 
                      mp.from, mp.decoded.source, mp.to, nodeDB->getNodeNum());
        }
        
        // If we detected a sent message, trigger SENDER state
        if (detectedSentMessage) {
            isSendingMessage = true;
            triggerSenderState();
        }
    } else {
        // Debug: log all received packets to see what's happening
        LOG_DEBUG("LoRabot received packet - from: %d, port: %d, our node: %d", 
                  mp.from, mp.decoded.portnum, nodeDB->getNodeNum());
        // Debug: log all received packets to see what ports are being received
        //LOG_DEBUG("LoRabot received packet on port %d (not triggering excited)", mp.decoded.portnum);
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
    
    // Draw status info or node discovery message - simplified for performance
    static uint32_t lastDrawTime = 0;
    static char cachedStatusLine[32] = "";
    uint32_t now = millis();
    
    // Update face animation every 1 second for lively animation, but status line every 2 seconds for performance
    static uint32_t lastFaceUpdateTime = 0;
    static uint32_t lastSenderMessageUpdate = 0;
    if (now - lastFaceUpdateTime > 1000) { // Update face animation every 1 second
        lastFaceUpdateTime = now;
        
        // Update face animation cycle for idle states
        if (currentState == AWAKE || currentState == LOOKING_AROUND_LEFT || currentState == LOOKING_AROUND_RIGHT) {
            uint32_t faceAnimationDuration = (now - lastFaceAnimationTime) / 1000;
            if (faceAnimationDuration > 1) {
                lookingCycle = (lookingCycle + 1) % 3;
                lastFaceAnimationTime = now;
            }
        }
        
        // Cycle through SENDER messages every 2 seconds while in SENDER state
        if (currentState == SENDER && (now - lastSenderMessageUpdate) > 2000) {
            senderMessageIndex = (senderMessageIndex + 1) % 5; // Rotate through 5 sender messages
            lastSenderMessageUpdate = now;
        }
        
        // No funny message cycling for HAPPY state - only show node name
    }
    
    // Only update status line every 2 seconds to reduce CPU usage
    if (now - lastDrawTime > 2000) {
        lastDrawTime = now;
        
                 if (currentState == HAPPY && showingNewNode) {
             // Show the discovered node name when happy (new node found) - on the right side
             display->setFont(ArialMT_Plain_10);
             display->drawString(x + 64, y + 50, lastNodeName);
         } else if (currentState == SENDER) {
            // Show sender messages for SENDER state
            const char* senderMsg = (const char*)pgm_read_ptr(&SENDER_MESSAGES[senderMessageIndex]);
            display->setFont(ArialMT_Plain_10);
            display->drawString(x + 64, y + 50, senderMsg);
        } else if (currentState == AWAKE || currentState == LOOKING_AROUND_LEFT || currentState == LOOKING_AROUND_RIGHT) {
            // Show funny messages for awake/looking states
            const char* funnyMsg = (const char*)pgm_read_ptr(&FUNNY_MESSAGES[funnyMessageIndex]);
            display->setFont(ArialMT_Plain_10);
            display->drawString(x + 64, y + 50, funnyMsg);
        } else {
            // Show cached status info - avoid expensive operations
            static uint32_t lastFavoriteCount = 0;
            static uint32_t lastFavoriteCountTime = 0;
            static uint32_t lastNodeCount = 0;
            
            // Only recalculate if node count changed or time expired
            if (currentNodeCount != lastNodeCount || (now - lastFavoriteCountTime > 15000)) { // Increased to 15 seconds
                uint32_t favoriteCount = 0;
                for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
                    const meshtastic_NodeInfoLite* node = nodeDB->getMeshNodeByIndex(i);
                    if (node && node->is_favorite) {
                        favoriteCount++;
                    }
                }
                lastFavoriteCount = favoriteCount;
                lastFavoriteCountTime = now;
                lastNodeCount = currentNodeCount;
                
                // Calculate actual node count excluding our own node
                uint32_t actualNodeCount = 0;
                for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
                    const meshtastic_NodeInfoLite* node = nodeDB->getMeshNodeByIndex(i);
                    if (node && node->num != nodeDB->getNodeNum()) {
                        actualNodeCount++;
                    }
                }
                
                snprintf(cachedStatusLine, sizeof(cachedStatusLine), "Nodes:%d Friends:%d", 
                        actualNodeCount, lastFavoriteCount);
            }
            display->setFont(ArialMT_Plain_10);
            display->drawString(x + 64, y + 50, cachedStatusLine);
        }
    } else {
                 // Use cached display for better performance
         if (currentState == HAPPY && showingNewNode) {
             display->setFont(ArialMT_Plain_10);
             display->drawString(x + 64, y + 50, lastNodeName);
         } else if (currentState == SENDER) {
            const char* senderMsg = (const char*)pgm_read_ptr(&SENDER_MESSAGES[senderMessageIndex]);
            display->setFont(ArialMT_Plain_10);
            display->drawString(x + 64, y + 50, senderMsg);
        } else if (currentState == AWAKE || currentState == LOOKING_AROUND_LEFT || currentState == LOOKING_AROUND_RIGHT) {
            const char* funnyMsg = (const char*)pgm_read_ptr(&FUNNY_MESSAGES[funnyMessageIndex]);
            display->setFont(ArialMT_Plain_10);
            display->drawString(x + 64, y + 50, funnyMsg);
        } else {
            display->setFont(ArialMT_Plain_10);
            display->drawString(x + 64, y + 50, cachedStatusLine);
        }
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
    
}

// Update pet emotional state
void LoRabotModule::updatePetState() {
    uint32_t now = millis();
    PetState newState = calculateNewState();
    
    // Allow immediate state changes for EXCITED, HAPPY, and SENDER states, but add delay for others
    bool shouldChangeState = false;
    if (newState != currentState) {
        if (newState == EXCITED || currentState == EXCITED ||
            newState == HAPPY || currentState == HAPPY ||
            newState == SENDER || currentState == SENDER) {
            // Allow immediate transitions to/from excited, happy, and sender states
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
        //LOG_DEBUG("LoRabot state changed: %d -> %d", previousState, currentState);
        
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



// Check if SENDER state should be triggered (sent messages)
bool LoRabotModule::shouldTriggerSender() {
    // For now, return false - we'll use the triggerSenderState() function
    // which can be called directly when messages are sent
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
            //LOG_DEBUG("LoRabot exiting INTENSE state after 3 seconds");
        }
    }
    

    
    // Check for SENDER state with 3-second duration (high priority)
    if (inSenderState) {
        uint32_t senderDuration = (now - senderStartTime) / 1000; // seconds
        if (senderDuration < 3) {
            // Remove debug logging to reduce interference
            // LOG_DEBUG("LoRabot staying in SENDER state - duration: %d seconds", senderDuration);
            return SENDER;
        } else {
            // Exit SENDER state after 3 seconds
            inSenderState = false;
            isSendingMessage = false; // Clear sending flag when SENDER state expires
            pendingSenderTrigger = false; // Clear pending trigger when SENDER state expires
            //LOG_DEBUG("LoRabot exiting SENDER state after 3 seconds");
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
            //LOG_DEBUG("LoRabot exiting excited/grateful cycle after 6 seconds");
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
        // Use faster face animation (1 second) while keeping thread performance optimized
        uint32_t faceAnimationDuration = (now - lastFaceAnimationTime) / 1000; // seconds
        if (faceAnimationDuration > 1) { // Change face every 1 second for more lively animation
            lookingCycle = (lookingCycle + 1) % 3; // Cycle through 0,1,2
            lastFaceAnimationTime = now;
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
    uint32_t baseInterval = 8000; // Increased from 4000ms to 8000ms baseline for much better performance
    
    switch (currentState) {
        case INTENSE:
            return 4000; // Increased from 2000ms to 4000ms for INTENSE state
        case SENDER:
            return 4000; // 4000ms for SENDER state
        case EXCITED:
        case GRATEFUL:
            return 8000; // Increased from 4000ms to 8000ms for excited/grateful cycle
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
    if (currentState >= 11) {
        LOG_WARN("LoRabot invalid state: %d, using fallback", currentState);
        return (const char*)pgm_read_ptr(&FACES[AWAKE]); // Fallback to demotivated
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