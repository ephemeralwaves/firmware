#pragma once

#include "SinglePortModule.h"
#include "concurrency/OSThread.h"
#include "mesh/MeshTypes.h"
#include "mesh/Router.h"
#include "Observer.h"
#include "NodeDB.h"
#include "MeshService.h"
#include "mesh/ProtobufModule.h"
#include "OLEDDisplay.h"
#include "OLEDDisplayUi.h"
#include "TextMessageModule.h" // Added for isFromUs
#include "PowerStatus.h" // Added for battery status

// Pet emotional states
enum PetState : uint8_t {
    AWAKE = 0,
    LOOKING_AROUND_RIGHT,
    LOOKING_AROUND_LEFT,
    HAPPY,
    EXCITED,
    SLEEPY1,         // Renamed from BORED
    SLEEPY2,         // Renamed from SLEEPY
    GRATEFUL,
    BLINK,           // Replaces INTENSE - quick eye blink animation
    DEMOTIVATED,
    SENDER
};

// Text message direction classification - focused on social behavior
enum TextMessageDirection : uint8_t {
    MY_TEXT_TO_SOMEONE = 0,      // I sent a text message to someone
    TEXT_TO_ME_DIRECT,           // Someone sent me a direct text
    TEXT_BROADCAST_BY_ME,        // I sent a broadcast text
    TEXT_BROADCAST_BY_OTHER,     // Someone else broadcast text
    TEXT_RELAYED                 // Multi-hop text message
};

// Node discovery classification - focused on social behavior
enum NodeDiscoveryType : uint8_t {
    NEW_NODE_DISCOVERED = 0,     // Found a new node in the mesh
    NODE_COUNT_CHANGED,          // Node count changed but no new node
    NODE_COUNT_UNCHANGED,        // No change in node count
    FIRST_BOOT_DETECTION,        // First boot, don't trigger HAPPY
    SENDING_INTERFERENCE         // Currently sending, don't trigger HAPPY
};

// Step-based execution for cooperative threading
enum LoRabotStep : uint8_t {
    STEP_PET_STATE_UPDATE = 0,
    STEP_NODE_DISCOVERY_CHECK,
    STEP_SENDER_DETECTION,
    STEP_DISPLAY_UPDATE,
    STEP_MESSAGE_PROCESSING,
    STEP_CLEANUP,
    STEP_YIELD  // Yield control back to scheduler
};

// Message type classification for social significance
enum MessageType : uint8_t {
    SOCIAL_MESSAGE = 0,          // Text messages, direct communication
    INFO_MESSAGE,                // Position updates, node info
    TECHNICAL_MESSAGE,           // Telemetry, routing packets
    BACKGROUND_MESSAGE,          // System packets, noise
    IGNORED_MESSAGE              // Messages we don't care about
};

// Text message analysis structure
struct TextMessageAnalysis {
    TextMessageDirection direction;
    NodeNum myNodeNum;
    NodeNum recipientNodeNum;
    NodeNum senderNodeNum;
    bool shouldReact;
    PetState suggestedState;
};

// Node discovery analysis structure
struct NodeDiscoveryAnalysis {
    NodeDiscoveryType discoveryType;
    size_t totalNodeCount;
    size_t previousNodeCount;
    const meshtastic_NodeInfoLite* newestNode;
    char nodeName[32];
    bool shouldTriggerHappy;
    bool shouldUpdateCount;
};

// Step-based execution state for cooperative threading
struct LoRabotStepState {
    LoRabotStep currentStep;
    uint32_t stepStartTime;
    uint32_t lastYieldTime;
    bool stepComplete;
    
    // Step-specific state variables for persistence
    size_t nodeDiscoveryIndex;        // Current node being processed
    uint8_t nodeCheckCounter;         // Counter for node discovery frequency
    uint32_t lastTxGoodCheck;         // Last time we checked txGood
    uint8_t displayUpdateCounter;     // Counter for display updates
    bool nodeDiscoveryInProgress;     // Whether we're in the middle of node discovery
    size_t totalNodeCount;            // Cached total node count
    size_t previousNodeCount;         // Cached previous node count
};

// Personality configuration
struct PetPersonality {
    uint8_t excited_threshold = 5;
    uint16_t bored_threshold_mins = 30;
    uint8_t sleepy_start_hour = 23;
    uint8_t sleepy_end_hour = 6;
    uint8_t friend_bond_threshold = 3;
};

// Friend tracking structure
struct FriendNode {
    uint32_t nodeId;
    uint8_t encounters;
    uint32_t lastSeen;
};

/**
 * LoRabot Module - A Pwnagotchi-inspired digital companion for Meshtastic
 */
class LoRabotModule : public SinglePortModule, 
                       public concurrency::OSThread,
                       public Observable<const UIFrameEvent *>
{
private:
    // Core state management
    PetState currentState;
    PetState previousState;
    PetPersonality personality;
    
    // Activity tracking
    uint32_t lastActivityTime;
    uint32_t lastStateChange;
    uint16_t networkEventCount;
    uint8_t currentNodeCount;
    
    // Friend tracking (limited to 8 friends to save memory)
    static const uint8_t MAX_FRIENDS = 8;
    FriendNode friends[MAX_FRIENDS];
    uint8_t friendCount;
    
    // Node discovery tracking
    uint32_t lastDiscoveredNode;
    char lastNodeName[32];
    uint32_t nodeDiscoveryTime;
    bool showingNewNode;
    
    // Message tracking
    uint32_t lastMessageTime;
    uint32_t excitedStartTime;
    bool inExcitedState;
    bool showingMessagePopup;
    uint32_t messagePopupTime;
    char receivedMessageText[64];  // Store actual received message
    uint8_t funnyMessageIndex;     // Track which funny message to show
    
    // BLINK state tracking (for compatibility - actual timing in clean animation system)
    bool inBlinkState;             // Currently in BLINK animation
    uint32_t blinkStartTime;       // When BLINK animation started
    // nextBlinkTime moved to clean animation system below
    uint32_t lastBlinkCheckTime;   // Last time we checked for blink timing
    

    
    // SENDER state tracking (sent messages)
    bool inSenderState;            // Currently in SENDER state
    uint32_t senderStartTime;      // When SENDER state started
    uint8_t senderMessageIndex;    // Track which sender message to show
    
    // Track when we're sending to prevent interference with node discovery
    bool isSendingMessage;
    
    // SLEEPY state cycling tracking
    bool inSleepyState;               // Currently in sleepy state (either SLEEPY1 or SLEEPY2)
    uint32_t sleepyStartTime;         // When we entered sleepy state
    uint32_t lastSleepyCycleTime;     // Last time we cycled between SLEEPY1 and SLEEPY2
    bool currentSleepyFace;           // false = SLEEPY1, true = SLEEPY2
    
    // NEW: Enhanced SENDER state detection
    uint32_t lastTxGoodCount;      // Last known txGood count
    uint32_t lastTxRelayCount;     // Last known txRelay count (to filter out relay transmissions)
    uint32_t lastTextMessageTxTime; // When we last detected a text message transmission
    bool pendingSenderTrigger;     // Flag to trigger SENDER state on next txGood increase
    uint32_t senderDetectionWindow; // Time window for correlating txGood with text messages
    
    // Clean Animation System
    enum AnimationPhase {
        AWAKE_PHASE,    // AWAKE with periodic blinking
        LOOKING_PHASE   // Looking left/right cycle
    };
    
    AnimationPhase currentPhase;
    uint32_t phaseStartTime;        // When current phase started
    uint32_t nextPhaseTime;         // When to switch to next phase (6-7 seconds)
    
    // AWAKE Phase timers
    uint32_t awakeStartTime;        // When current AWAKE period started
    uint32_t nextBlinkTime;         // When next blink should occur (1-3 seconds from awake start)
    
    // LOOKING Phase timers  
    uint8_t lookingCycle;           // 0=left, 1=right, 2=awake
    uint32_t nextLookingTime;       // When to advance looking cycle (~500ms)
    
    uint32_t lastFunnyMessageTime;  // Track funny message rotation separately (every 4 seconds)
    
    // Animation state tracking for better performance
    uint32_t lastCycleTime;         // Track last cycle time for display updates
    
    // Display optimization
    char lastDisplayedFace[16];
    bool displayNeedsUpdate;
    
    // NEW: Step-based execution state for cooperative threading
    LoRabotStepState stepState;
    static const uint32_t MAX_STEP_TIME_MS = 60; // Maximum time per step before yielding

public:
    /** Constructor */
    LoRabotModule();
    virtual ~LoRabotModule();
    
    // Module interface
    virtual Observable<const UIFrameEvent *> *getUIFrameObservable() override { return this; }
    virtual bool wantUIFrame() override { return true; }
    virtual void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, 
                          int16_t x, int16_t y) override;
    
    // Override to listen for text messages and position updates
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override {
        return p->decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP || 
               p->decoded.portnum == meshtastic_PortNum_POSITION_APP;
    }
    
protected:
    // Message handling
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    virtual int32_t runOnce() override;
    
private:
    // Internal methods
    void updatePetState();
    void processNetworkEvent();
    void updateFriendsList(uint32_t nodeId);
    bool isFriend(uint32_t nodeId);
    bool isNightTime();
    // Check if battery is low
    bool isLowBattery();
    
    // Check if BLINK state should be triggered
    // shouldTriggerBlink() removed - blink timing now handled in calculateNewState()
    

    
    // Check if SENDER state should be triggered (sent messages)
    bool shouldTriggerSender();
    
    // Calculate new pet state based on current conditions
    PetState calculateNewState();
    uint32_t getUpdateInterval();
    const char* getCurrentFace();
    void saveState();
    void loadState();
    
    // SLEEPY state cycling function
    PetState handleSleepyStateCycling();
    
    // NEW: Text message detection functions
    bool isMyOutgoingTextMessage(const meshtastic_MeshPacket &mp);
    bool isIncomingTextMessage(const meshtastic_MeshPacket &mp);
    TextMessageDirection analyzeTextMessage(const meshtastic_MeshPacket &mp);
    TextMessageAnalysis analyzeTextMessageDirection(const meshtastic_MeshPacket &mp);
    
    // NEW: Node discovery detection functions
    NodeDiscoveryType analyzeNodeDiscovery(size_t totalNodeCount, size_t previousNodeCount);
    NodeDiscoveryAnalysis analyzeNodeDiscoveryDirection(size_t totalNodeCount, size_t previousNodeCount);
    
    // NEW: Step-based execution functions for cooperative threading
    int32_t executePetStateUpdate();
    int32_t executeNodeDiscoveryCheck();
    int32_t executeSenderDetection();
    int32_t executeDisplayUpdate();
    int32_t executeMessageProcessing();
    int32_t executeCleanup();
    void initializeStepState();
    
    // Static arrays for faces and messages
    static const char* const FACES[11];
    static const char* const STATE_NAMES[11];
    static const char* const FUNNY_MESSAGES[8];
    static const char* const SENDER_MESSAGES[5];
    
    // Test function for debugging
public:
    void testExcitedState() {
        LOG_INFO("LoRabot testExcitedState() called - forcing excited state");
        excitedStartTime = millis();
        inExcitedState = true;
        previousState = currentState;
        currentState = EXCITED;
        lastStateChange = millis();
        displayNeedsUpdate = true;
    }
    
    // Trigger SENDER state when a message is sent
    void triggerSenderState() {
        LOG_INFO("LoRabot triggerSenderState() called - message sent");
        uint32_t now = millis();
        inSenderState = true;
        senderStartTime = now;
        senderMessageIndex = (senderMessageIndex + 1) % 5; // Rotate through 5 sender messages
        
        // Set sending flag to prevent HAPPY state interference
        isSendingMessage = true;
        
        // Clear pending trigger since we're now in SENDER state
        pendingSenderTrigger = false;
        
        previousState = currentState;
        currentState = SENDER;
        lastStateChange = now;
        displayNeedsUpdate = true;
        
        LOG_INFO("LoRabot SENDER state triggered! Will show for 3 seconds.");
    }
};

// Global instance
extern LoRabotModule *loRabotModule;