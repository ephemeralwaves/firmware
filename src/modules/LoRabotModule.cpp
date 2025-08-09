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
    "( ~ o ~ )",    // SLEEPY1 - night hours
    "( ~ - ~ )",    // SLEEPY2 - night hours
    "( ^ o ^ )",    // GRATEFUL - thankful for received messages
    "( - . - )",    // BLINK - quick eye blink animation
    "( v _ v )",    // DEMOTIVATED - low battery
    "(  ' . ')>"    // SENDER - messages sent by node, can be any type of data (text, position, telemetry, etc.)
};

// Human-readable state names for debugging
const char* const LoRabotModule::STATE_NAMES[11] PROGMEM = {
    "Awake",
    "Looking R",
    "Looking L",
    "Happy", 
    "Excited",
    "Sleepy1",   
    "Sleepy2",    
    "Grateful",
    "Blink",
    "Sad",
    "Sender"
};

// Messages for AWAKE/LOOKING states
const char* const LoRabotModule::FUNNY_MESSAGES[8] PROGMEM = {
    "Too cute to route.",
    "Ping me, maybe?",
    "I sense...potential pals",
    "Any1 broadcasting snacks?",
    "LoRa? More like explore-a!",
    "Who's out there?",
    "Looking for friends...",
    "Let's link up!"
};



// Sender messages for SENDER state
const char* const LoRabotModule::SENDER_MESSAGES[5] PROGMEM = {
    "Message Sent!",
    "Beep boop, data sent!",
    "Beamed the data!",
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
    
    // Initialize current node count to prevent false "new node" detection on startup
    currentNodeCount = 0;
    
    // Initialize message tracking
    lastMessageTime = 0;
    excitedStartTime = 0;
    inExcitedState = false;
    showingMessagePopup = false;
    messagePopupTime = 0;
    memset(receivedMessageText, 0, sizeof(receivedMessageText));
    funnyMessageIndex = 0;
    
    // Initialize BLINK state tracking
    inBlinkState = false;
    blinkStartTime = 0;
    nextBlinkTime = millis() + random(2000, 4000); // First blink in 2-4 seconds
    lastBlinkCheckTime = 0;
    

    
    // Initialize SENDER state tracking
    inSenderState = false;
    senderStartTime = 0;
    senderMessageIndex = 0;
    
    // Initialize sending flag
    isSendingMessage = false;
    
    // Initialize SLEEPY state cycling
    inSleepyState = false;
    sleepyStartTime = 0;
    lastSleepyCycleTime = 0;
    currentSleepyFace = false; // Start with SLEEPY1
    
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
    lastFunnyMessageTime = 0; // Track funny message rotation separately (every 3 seconds)
    
    // NEW: Initialize step-based execution state
    initializeStepState();
    
    // Load saved state from preferences
    loadState();
    
    // Start the thread
    setIntervalFromNow(1000); // Initial 10 second interval for better performance
    
}

LoRabotModule::~LoRabotModule() {
    saveState();
}

// NEW: Step-based main thread execution for cooperative threading
int32_t LoRabotModule::runOnce() {
    uint32_t now = millis();
    
    // Check if we should yield based on time spent in current step
    if ((now - stepState.lastYieldTime) > MAX_STEP_TIME_MS) {
        stepState.lastYieldTime = now;
        return getUpdateInterval(); // Yield control back to scheduler
    }
    
    // Execute current step
    switch (stepState.currentStep) {
        case STEP_PET_STATE_UPDATE:
            return executePetStateUpdate();
            
        case STEP_NODE_DISCOVERY_CHECK:
            return executeNodeDiscoveryCheck();
            
        case STEP_SENDER_DETECTION:
            return executeSenderDetection();
            
        case STEP_DISPLAY_UPDATE:
            return executeDisplayUpdate();
            
        case STEP_MESSAGE_PROCESSING:
            return executeMessageProcessing();
            
        case STEP_CLEANUP:
            return executeCleanup();
            
        case STEP_YIELD:
            stepState.currentStep = STEP_PET_STATE_UPDATE; // Reset cycle
            stepState.lastYieldTime = now;
            return getUpdateInterval();
            
        default:
            stepState.currentStep = STEP_PET_STATE_UPDATE; // Reset to first step
            return getUpdateInterval();
    }
}

// Handle received mesh packets
ProcessMessage LoRabotModule::handleReceived(const meshtastic_MeshPacket &mp) {
    // Track network activity
    processNetworkEvent();
    
    // Check if this is a received text message (portnum = 1) or position update (portnum = 3)
    // Only trigger EXCITED for received messages, not sent messages
    if ((mp.decoded.portnum == 1 || mp.decoded.portnum == 3) && mp.from != 0) {
        
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
        
    } else if (mp.decoded.portnum == 1) {
        // NEW: Clean text message detection using focused analysis
        TextMessageAnalysis analysis = analyzeTextMessageDirection(mp);
        
        if (analysis.shouldReact) {
            switch (analysis.direction) {
         
                    
                case TEXT_TO_ME_DIRECT:
                case TEXT_BROADCAST_BY_OTHER:
                    // Someone is talking to me! Trigger EXCITED state
                    // This will be handled by the existing EXCITED logic above
                    break;
                    
                default:
                    break;
            }
        }
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
    display->setFont(ArialMT_Plain_24);
    display->drawString(x + 38, y + 10, currentFace);
    
    // Draw status info or node discovery message - simplified for performance
    static uint32_t lastDrawTime = 0;
    static char cachedStatusLine[32] = "";
    uint32_t now = millis();
    
    // Update face animation every 1 second for lively animation, but status line every 2 seconds for performance
    static uint32_t lastFaceUpdateTime = 0;
    static uint32_t lastSenderMessageUpdate = 0;
    if (now - lastFaceUpdateTime > 1000) { // Update face animation every 1 second
        lastFaceUpdateTime = now;
        
        // Face animation cycle is now handled in calculateNewState() for better state management
        
        // Cycle through SENDER messages every 2 seconds while in SENDER state
        if (currentState == SENDER && (now - lastSenderMessageUpdate) > 2000) {
            senderMessageIndex = (senderMessageIndex + 1) % 5; // Rotate through 5 sender messages
            lastSenderMessageUpdate = now;
        }
        
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
        } else if (currentState == AWAKE || currentState == LOOKING_AROUND_LEFT || currentState == LOOKING_AROUND_RIGHT || currentState == BLINK) {
            // Show funny messages for awake/looking/blink states
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
                
                // Firmware Uses: nodeDB->getNumMeshNodes() 
                //This returns the total number of node entries in the database
                uint32_t actualNodeCount = nodeDB->getNumMeshNodes();
                
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
        } else if (currentState == AWAKE || currentState == LOOKING_AROUND_LEFT || currentState == LOOKING_AROUND_RIGHT || currentState == BLINK) {
            const char* funnyMsg = (const char*)pgm_read_ptr(&FUNNY_MESSAGES[funnyMessageIndex]);
            display->setFont(ArialMT_Plain_10);
            display->drawString(x + 64, y + 50, funnyMsg);
        } else {
            display->setFont(ArialMT_Plain_10);
            display->drawString(x + 64, y + 50, cachedStatusLine);
        }
    }
    
    // Draw message popup on the Right side when excited
    if (showingMessagePopup && (millis() - messagePopupTime) < 6000) { // Show for 6 seconds
        display->setTextAlignment(TEXT_ALIGN_LEFT);
        display->setFont(ArialMT_Plain_10);
        display->drawString(x + 80, y + 15, "Message:");
        display->drawString(x + 80, y + 25, receivedMessageText);
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
        
        // Note: Funny message rotation is now handled separately with 3-second timing in calculateNewState()
        
        
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

    return false;
}

// Check if battery is low
bool LoRabotModule::isLowBattery() {
    // TODO: Implement when battery API is found
    return false;  // For now, assume battery is always OK
}

// Check if BLINK should be triggered
bool LoRabotModule::shouldTriggerBlink() {
    uint32_t now = millis();
    
    // Check if it's time to blink (based on random interval)
    if (now >= nextBlinkTime && !inBlinkState) {
        return true;
    }
    
    return false;
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
    
    // Check for BLINK state with fast duration for realistic eye blink
    if (inBlinkState) {
        uint32_t blinkDuration = now - blinkStartTime;
        if (blinkDuration < 80) { // 80ms blink duration - fast and realistic
            return BLINK;
        } else {
            // Exit BLINK state after 80ms and set next random blink time
            inBlinkState = false;
            nextBlinkTime = now + random(2000, 5000); // Next blink in 2-5 seconds
        }
    }
    

    
    // Check for SENDER state
    if (inSenderState) {
        //sender state is triggered when a message is sent, and stays for 2 seconds
        uint32_t senderDuration = (now - senderStartTime) / 1000; // seconds
        if (senderDuration < 2) {
            return SENDER;
        } else {
            // Exit SENDER state after 2 seconds
            inSenderState = false;
            isSendingMessage = false; // Clear sending flag when SENDER state expires
            pendingSenderTrigger = false; // Clear pending trigger when SENDER state expires
      
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
            return GRATEFUL;
        } else {
            // Exit excited/grateful cycle after 6 seconds total
            inExcitedState = false;
        }
    }
    
    // Check for specific priority states first
    if (isNightTime() || isLowBattery()) {
        // Enter sleepy state with cycling between SLEEPY1 and SLEEPY2
        return handleSleepyStateCycling();
    } else {
        // Force exit from sleepy state since isNightTime() and isLowBattery() are both false
        inSleepyState = false;
        currentState = AWAKE;
        displayNeedsUpdate = true;
    }
    
    // Check for recent node discovery (highest priority) - show HAPPY when new node found
    if (showingNewNode && (now - nodeDiscoveryTime) < 8000) { // Show for 8 seconds
        return HAPPY;
    }
    
    // Default behavior: AWAKE state, then looking states when nodes are present
    if (currentNodeCount > 0) {
        // Check if we should trigger a blink from AWAKE state
        if (currentState == AWAKE && shouldTriggerBlink()) {
            inBlinkState = true;
            blinkStartTime = now;
            currentState = BLINK;
            displayNeedsUpdate = true;
            LOG_DEBUG("LoRabot: Triggering BLINK from AWAKE");
            return BLINK;
        }
        
        // 4-state cycle: Looking Left → Looking Right → Awake → (Blink) → repeat
        // Check if it's time to cycle (every 1 second for visible animation)
        if ((now - lastFaceAnimationTime) >= 1000) {
            lookingCycle = (lookingCycle + 1) % 3; // Still cycle through 0,1,2 for faces
            lastFaceAnimationTime = now;
            
            // DIRECTLY update currentState and trigger UI redraw
            switch (lookingCycle) {
                case 0: // Looking Left
                    currentState = LOOKING_AROUND_LEFT;
                    displayNeedsUpdate = true;
                    LOG_DEBUG("LoRabot: Cycling to LOOKING_AROUND_LEFT");
                    break;
                case 1: // Looking Right
                    currentState = LOOKING_AROUND_RIGHT;
                    displayNeedsUpdate = true;
                    LOG_DEBUG("LoRabot: Cycling to LOOKING_AROUND_RIGHT");
                    break;
                case 2: // Awake
                    currentState = AWAKE;
                    displayNeedsUpdate = true;
                    LOG_DEBUG("LoRabot: Cycling to AWAKE");
                    // Don't trigger blink immediately, wait for random interval
                    break;
                default:
                    currentState = AWAKE;
                    displayNeedsUpdate = true;
                    break;
            }
        }
        
        // Rotate funny messages every 5 seconds (independent of face animation)
        if ((now - lastFunnyMessageTime) >= 5000) {
            funnyMessageIndex = (funnyMessageIndex + 1) % 8; // Rotate through 8 funny messages
            lastFunnyMessageTime = now;
        }
        
        // Return current state (should match currentState now)
        return currentState;
    }
    
    // No nodes present - stay in AWAKE state

    return AWAKE;
}

// Get update interval based on current state and activity
uint32_t LoRabotModule::getUpdateInterval() {
    uint32_t baseInterval = 60; // 60ms baseline timing
    return baseInterval;
    
}

// Get current face string
const char* LoRabotModule::getCurrentFace() {
    if (currentState >= 11) {
        LOG_WARN("LoRabot invalid state: %d, using fallback", currentState);
        return (const char*)pgm_read_ptr(&FACES[AWAKE]); // Fallback to demotivated
    }
    
    const char* face = (const char*)pgm_read_ptr(&FACES[currentState]);

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

// NEW: Simple detection for outgoing text messages from my node
bool LoRabotModule::isMyOutgoingTextMessage(const meshtastic_MeshPacket &mp) {
    NodeNum myNodeNum = nodeDB->getNodeNum();
    
    bool isOutgoing = (mp.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) &&
                     (mp.from == myNodeNum) &&                    // From me
                     (mp.to != myNodeNum);                        // Not to myself
    
    
    return isOutgoing;
}

// NEW: Simple detection for incoming text messages to my node
bool LoRabotModule::isIncomingTextMessage(const meshtastic_MeshPacket &mp) {
    NodeNum myNodeNum = nodeDB->getNodeNum();
    
    bool isIncoming = (mp.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) &&
                     (mp.from != myNodeNum);                      // Not from me
    
    
    return isIncoming;
}

// NEW: Analyze text message direction for social behavior
TextMessageDirection LoRabotModule::analyzeTextMessage(const meshtastic_MeshPacket &mp) {
    // Only analyze text messages
    if (mp.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP) {
        return TEXT_RELAYED; // Not a text message
    }
    
    NodeNum myNodeNum = nodeDB->getNodeNum();
    bool isFromMe = (mp.from == myNodeNum);
    bool isBroadcast = (mp.to == NODENUM_BROADCAST || mp.to == 0xffffffff);
    bool isToMe = (mp.to == myNodeNum);
    bool isFirstHop = (mp.hop_start == mp.hop_limit && mp.hop_limit > 0);
    

    if (isFromMe) {
        if (isBroadcast) {
            return TEXT_BROADCAST_BY_ME;    // I broadcast a message
        } else {
            return MY_TEXT_TO_SOMEONE;      // I sent direct message
        }
    }
    
    if (isToMe && !isBroadcast) {

        return TEXT_TO_ME_DIRECT;           // Direct message to me
    }
    
    if (isBroadcast && isFirstHop) {
        return TEXT_BROADCAST_BY_OTHER;     // Someone else broadcast
    }
 
    return TEXT_RELAYED;                    // Relayed message
}

// NEW: Complete text message analysis with pet state suggestions
TextMessageAnalysis LoRabotModule::analyzeTextMessageDirection(const meshtastic_MeshPacket &mp) {
    TextMessageAnalysis analysis;
    NodeNum myNodeNum = nodeDB->getNodeNum();
    
    // Only analyze text messages
    if (mp.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP) {
        analysis.direction = TEXT_RELAYED;
        analysis.shouldReact = false;
        analysis.suggestedState = AWAKE;
        return analysis;
    }
    
    // Determine text message direction
    analysis.direction = analyzeTextMessage(mp);
    
    // Initialize analysis
    analysis.myNodeNum = myNodeNum;
    analysis.recipientNodeNum = mp.to;
    analysis.senderNodeNum = mp.from;
    analysis.shouldReact = true;
    analysis.suggestedState = AWAKE; // Default state
    
    // Switch-based analysis for clean, readable logic
    switch (analysis.direction) {
           
        case TEXT_TO_ME_DIRECT:
            // Someone texted me directly! Pet gets excited
            analysis.suggestedState = EXCITED;
            LOG_INFO("LoRabot: Got direct text from 0x%08x - triggering EXCITED state", mp.from);
            break;
            
        case TEXT_BROADCAST_BY_OTHER:
            // Someone else broadcast
            analysis.suggestedState = EXCITED;
            LOG_INFO("LoRabot: Got broadcast text from 0x%08x - triggering EXCITED state", mp.from);
            break;
            
        case TEXT_RELAYED:
            // Just background chatter
            analysis.shouldReact = false;
            break;
    }
    
    return analysis;
}

// NEW: Initialize step-based execution state
void LoRabotModule::initializeStepState() {
    stepState.currentStep = STEP_PET_STATE_UPDATE;
    stepState.stepStartTime = millis();
    stepState.lastYieldTime = millis();
    stepState.stepComplete = false;
    
    // Initialize step-specific state variables
    stepState.nodeDiscoveryIndex = 0;
    stepState.nodeCheckCounter = 0;
    stepState.lastTxGoodCheck = 0;
    stepState.displayUpdateCounter = 0;
    stepState.nodeDiscoveryInProgress = false;
    stepState.totalNodeCount = 0;
    stepState.previousNodeCount = 0;
    
 
}

// NEW: Execute pet state update step (Step 1)
int32_t LoRabotModule::executePetStateUpdate() {
    uint32_t startTime = millis();
    
    // Update pet state based on network activity and time
    updatePetState();
    
    // Check if we've spent too much time
    if ((millis() - startTime) > MAX_STEP_TIME_MS) {
        LOG_DEBUG("LoRabot: Pet state update took too long, yielding");
        return getUpdateInterval(); // Yield control
    }
    
    // Move to next step
    stepState.currentStep = STEP_NODE_DISCOVERY_CHECK;
    stepState.stepStartTime = millis();
    
    return 0; // Continue immediately to next step
}

// NEW: Execute sender detection step (Step 3) - placeholder for now
int32_t LoRabotModule::executeSenderDetection() {
    uint32_t startTime = millis();
    
    // ENHANCED SENDER state detection - correlates txGood increases with text message detection
    // This is crucial for detecting DIRECT MESSAGES that don't pass through handleReceived() on the sending node
    uint32_t now = millis();
    if (now - stepState.lastTxGoodCheck > 750) { // Check every 750ms for much faster response
        stepState.lastTxGoodCheck = now;
        
        if (RadioLibInterface::instance && !inSenderState && !isSendingMessage) {
            uint32_t currentTxGood = RadioLibInterface::instance->txGood;
            
            // STEP 1: Check if txGood increased (we sent something)
            if (currentTxGood > lastTxGoodCount) {
                uint32_t txIncrease = currentTxGood - lastTxGoodCount;
                
                // STEP 2: Try to correlate with text message detection from handleReceived()
                if (pendingSenderTrigger && (now - lastTextMessageTxTime) < 500) { // Reduced window for better correlation
                    // We detected a text message pattern AND txGood increased - this is likely a text message
                    LOG_INFO("LoRabot detected text message transmission via correlation - triggering SENDER state");
                    isSendingMessage = true; // Set flag immediately to prevent HAPPY state interference
                    triggerSenderState();
                    pendingSenderTrigger = false; // Clear the flag
                }
                // STEP 3: Handle direct messages that don't pass through handleReceived()
                else if (!pendingSenderTrigger && txIncrease == 1) {
                    // Single packet transmission without pending trigger - likely a direct message
                    // Direct messages don't pass through handleReceived() on the sending node, so we need to be aggressive
                    LOG_INFO("LoRabot detected single packet transmission - likely direct message, triggering SENDER state");
                    isSendingMessage = true; // Set flag immediately to prevent HAPPY state interference
                    triggerSenderState();
                }

                lastTxGoodCount = currentTxGood;
            }
        }
    }
    
    // Check if we've spent too much time
    if ((millis() - startTime) > MAX_STEP_TIME_MS) {

        return getUpdateInterval(); // Yield control
    }
    
    // Move to next step
    stepState.currentStep = STEP_DISPLAY_UPDATE;
    stepState.stepStartTime = millis();
    
    return 0; // Continue immediately to next step
}

// NEW: Execute display update step (Step 4)
int32_t LoRabotModule::executeDisplayUpdate() {
    uint32_t startTime = millis();
    
    // Clear the "showing new node" flag after timeout
    if (showingNewNode && (millis() - nodeDiscoveryTime) > 10000) {
        showingNewNode = false;

    }
    
    // Clear sending flag if it gets stuck (safety timeout)
    if (isSendingMessage && (millis() - senderStartTime) > 5000) {
        isSendingMessage = false;
    }
    
    // Check if we've spent too much time
    if ((millis() - startTime) > MAX_STEP_TIME_MS) {
        return getUpdateInterval(); // Yield control
    }
    
    // Move to next step
    stepState.currentStep = STEP_MESSAGE_PROCESSING;
    stepState.stepStartTime = millis();
    
    return 0; // Continue immediately to next step
}

// NEW: Execute message processing step (Step 5) - placeholder for now
int32_t LoRabotModule::executeMessageProcessing() {
    uint32_t startTime = millis();
    
    // TODO: Implement message processing logic here
    // For now, just move to next step
    
    // Check if we've spent too much time
    if ((millis() - startTime) > MAX_STEP_TIME_MS) {
        return getUpdateInterval(); // Yield control
    }
    
    // Move to next step
    stepState.currentStep = STEP_CLEANUP;
    stepState.stepStartTime = millis();
    
    return 0; // Continue immediately to next step
}

// NEW: Execute cleanup step (Step 6)
int32_t LoRabotModule::executeCleanup() {
    uint32_t startTime = millis();
    
    // TODO: Implement cleanup logic here
    // For now, just move to yield step
    
    // Check if we've spent too much time
    if ((millis() - startTime) > MAX_STEP_TIME_MS) {
        return getUpdateInterval(); // Yield control
    }
    
    // Move to yield step
    stepState.currentStep = STEP_YIELD;
    stepState.stepStartTime = millis();
    
    return 0; // Continue immediately to yield step
}

// NEW: Execute node discovery check step (Step 2) with state persistence
int32_t LoRabotModule::executeNodeDiscoveryCheck() {
    uint32_t startTime = millis();
    
    // Only check every 4 cycles (much less frequent)
    stepState.nodeCheckCounter++;
    if (stepState.nodeCheckCounter < 4) {
        // Skip node discovery this cycle, move to next step
        stepState.currentStep = STEP_SENDER_DETECTION;
        stepState.stepStartTime = millis();
        return 0; // Continue immediately
    }
    
    // Reset counter for next cycle
    stepState.nodeCheckCounter = 0;
    
    // Get current node count
    size_t totalNodeCount = nodeDB->getNumMeshNodes();
    
    // Use the clean analysis system for node discovery
    NodeDiscoveryAnalysis analysis = analyzeNodeDiscoveryDirection(totalNodeCount, currentNodeCount);
    
    if (analysis.shouldUpdateCount) {
        // Update current node count
        currentNodeCount = totalNodeCount;
        processNetworkEvent();
        
        // Clear cached status line when node count changes so it gets recalculated
        static char cachedStatusLine[32] = "";
        cachedStatusLine[0] = '\0'; // Clear the cache
    }
    
    if (analysis.shouldTriggerHappy) {
        // New node discovered - trigger HAPPY state
        lastDiscoveredNode = totalNodeCount;
        nodeDiscoveryTime = millis();
        showingNewNode = true;
        
        // Copy the analyzed node name
        strncpy(lastNodeName, analysis.nodeName, sizeof(lastNodeName) - 1);
        lastNodeName[sizeof(lastNodeName) - 1] = '\0'; // Ensure null termination
        
        // Immediately trigger HAPPY state for new node discovery
        previousState = currentState;
        currentState = HAPPY;
        lastStateChange = millis();
        displayNeedsUpdate = true;
        
    }
    
    // Check if we've spent too much time
    if ((millis() - startTime) > MAX_STEP_TIME_MS) {
        LOG_DEBUG("LoRabot: Node discovery check took too long, yielding");
        return getUpdateInterval(); // Yield control
    }
    
    // Move to next step
    stepState.currentStep = STEP_SENDER_DETECTION;
    stepState.stepStartTime = millis();
    
    return 0; // Continue immediately to next step
}

// NEW: Analyze node discovery type for social behavior
NodeDiscoveryType LoRabotModule::analyzeNodeDiscovery(size_t totalNodeCount, size_t previousNodeCount) {
    // Check for first boot (no previous nodes)
    if (previousNodeCount == 0) {
        return FIRST_BOOT_DETECTION;
    }
    
    // Check if currently sending (interference)
    if (isSendingMessage) {
        return SENDING_INTERFERENCE;
    }
    
    // Check if node count increased (new node discovered)
    if (totalNodeCount > previousNodeCount) {
        return NEW_NODE_DISCOVERED;
    }
    
    // Check if node count changed (but decreased)
    if (totalNodeCount != previousNodeCount) {
        return NODE_COUNT_CHANGED;
    }
    
    // No change in node count
    return NODE_COUNT_UNCHANGED;
}

// NEW: Complete node discovery analysis with switch-based logic
NodeDiscoveryAnalysis LoRabotModule::analyzeNodeDiscoveryDirection(size_t totalNodeCount, size_t previousNodeCount) {
    NodeDiscoveryAnalysis analysis;
    
    // Initialize analysis
    analysis.discoveryType = analyzeNodeDiscovery(totalNodeCount, previousNodeCount);
    analysis.totalNodeCount = totalNodeCount;
    analysis.previousNodeCount = previousNodeCount;
    analysis.newestNode = nullptr;
    memset(analysis.nodeName, 0, sizeof(analysis.nodeName));
    analysis.shouldTriggerHappy = false;
    analysis.shouldUpdateCount = false;
    
    // Switch-based analysis for clean, readable logic
    switch (analysis.discoveryType) {
        case NEW_NODE_DISCOVERED: {
            // Find the newest node and prepare HAPPY state
            analysis.shouldTriggerHappy = true;
            analysis.shouldUpdateCount = true;
            
            // Look for the node that was most recently heard
            uint32_t newestTime = 0;
            for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
                const meshtastic_NodeInfoLite* node = nodeDB->getMeshNodeByIndex(i);
                if (node && node->num != nodeDB->getNodeNum()) {
                    // Don't include our own node
                    if (node->last_heard > newestTime) {
                        analysis.newestNode = node;
                        newestTime = node->last_heard;
                    }
                }
            }
            
            // Generate node name based on available information
            if (analysis.newestNode) {
                const int MAX_DISPLAY_LENGTH = 24; // Same as "LoRa? More like explore-a!"
                const int HELLO_OVERHEAD = 7;      // "Hello " + "!" = 7 characters
                const int MAX_NAME_LENGTH = MAX_DISPLAY_LENGTH - HELLO_OVERHEAD; // 17 characters max
                
                if (analysis.newestNode->has_user && strlen(analysis.newestNode->user.long_name) > 0) {
                    // Use the actual node name with "Hello" prefix, truncated if needed
                    char truncatedName[MAX_NAME_LENGTH + 1]; // +1 for null terminator
                    strncpy(truncatedName, analysis.newestNode->user.long_name, MAX_NAME_LENGTH);
                    truncatedName[MAX_NAME_LENGTH] = '\0'; // Ensure null termination
                    snprintf(analysis.nodeName, sizeof(analysis.nodeName), "Hello %s!", truncatedName);
                } else if (analysis.newestNode->has_user && strlen(analysis.newestNode->user.short_name) > 0) {
                    // Fallback to short name if long name is not available, truncated if needed
                    char truncatedName[MAX_NAME_LENGTH + 1]; // +1 for null terminator
                    strncpy(truncatedName, analysis.newestNode->user.short_name, MAX_NAME_LENGTH);
                    truncatedName[MAX_NAME_LENGTH] = '\0'; // Ensure null termination
                    snprintf(analysis.nodeName, sizeof(analysis.nodeName), "Hello %s!", truncatedName);
                } else {
                    // Fallback to generic name (node number) - format as hex for better readability
                    snprintf(analysis.nodeName, sizeof(analysis.nodeName), "Hello Node 0x%x!", analysis.newestNode->num);
                }
            }
            
            break;
        }
            
        case NODE_COUNT_CHANGED:
            // Node count changed but not a new node discovery
            analysis.shouldUpdateCount = true;
            break;
            
        case NODE_COUNT_UNCHANGED:
            // No change in node count
            break;
            
        case FIRST_BOOT_DETECTION:
            // First boot, don't trigger HAPPY state
            analysis.shouldUpdateCount = true;
            break;
            
        case SENDING_INTERFERENCE:
            // Currently sending, don't trigger HAPPY state
            break;
    }
    
    return analysis;
}

// Handle SLEEPY state cycling between SLEEPY1 and SLEEPY2 every second
PetState LoRabotModule::handleSleepyStateCycling() {
    uint32_t now = millis();
    
    // If we just entered sleepy state, initialize
    if (!inSleepyState) {
        inSleepyState = true;
        sleepyStartTime = now;
        lastSleepyCycleTime = now;
        currentSleepyFace = false; // Start with SLEEPY1
        
        // IMMEDIATELY update the state and trigger UI update
        currentState = SLEEPY1;
        displayNeedsUpdate = true;
        return SLEEPY1;
    }
    
    // Check if it's time to cycle (every 1000ms for visible animation)
    if ((now - lastSleepyCycleTime) >= 1000) {
        currentSleepyFace = !currentSleepyFace; // Toggle between faces
        lastSleepyCycleTime = now;
        
        // DIRECTLY update currentState and trigger UI redraw
        if (currentSleepyFace) {
            currentState = SLEEPY2;
            displayNeedsUpdate = true;
        } else {
            currentState = SLEEPY1;
            displayNeedsUpdate = true;
        }
    }
    
    // Return current face (this should match currentState now)
    return currentState;
}