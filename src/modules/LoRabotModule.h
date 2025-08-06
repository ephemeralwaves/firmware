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

// Pet emotional states
enum PetState : uint8_t {
    AWAKE = 0,
    LOOKING_AROUND_RIGHT,
    LOOKING_AROUND_LEFT,
    HAPPY,
    EXCITED,
    BORED,
    SLEEPY,
    GRATEFUL,
    RELAY,
    INTENSE,
    DEMOTIVATED,
    SENDER
};

// Personality configuration
struct PetPersonality {
    uint8_t excited_threshold = 5;
    uint16_t bored_threshold_mins = 30;
    uint8_t sleepy_start_hour = 24;
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
    uint8_t relayMessageIndex;     // Track which relay message to show
    
    // INTENSE state tracking
    uint32_t messageTimes[5];      // Track last 5 message times
    uint8_t messageIndex;          // Current index in circular buffer
    bool inIntenseState;           // Currently in INTENSE state
    uint32_t intenseStartTime;     // When INTENSE state started
    
    // RELAY state tracking (relay events)
    uint32_t lastRelayCount;       // Last known relay count
    bool inRelayState;             // Currently in RELAY state
    uint32_t relayStartTime;       // When RELAY state started
    
    // SENDER state tracking (sent messages)
    bool inSenderState;            // Currently in SENDER state
    uint32_t senderStartTime;      // When SENDER state started
    uint8_t senderMessageIndex;    // Track which sender message to show
    
    // Track when we're sending to prevent interference with node discovery
    bool isSendingMessage;
    
    // NEW: Enhanced SENDER state detection
    uint32_t lastTxGoodCount;      // Last known txGood count
    uint32_t lastTextMessageTxTime; // When we last detected a text message transmission
    bool pendingSenderTrigger;     // Flag to trigger SENDER state on next txGood increase
    uint32_t senderDetectionWindow; // Time window for correlating txGood with text messages
    
    // Looking state tracking
    bool lookingRight;
    uint32_t lastLookingChange;
    uint8_t lookingCycle; // 0=left, 1=right, 2=awake
    uint32_t lastFaceAnimationTime; // Track face animation separately from thread timing
    
    // Display optimization
    char lastDisplayedFace[16];
    bool displayNeedsUpdate;

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
    
    // Check if INTENSE state should be triggered
    bool shouldTriggerIntense();
    
    // Check if RELAY state should be triggered (relay events)
    bool shouldTriggerRelay();
    
    // Check if SENDER state should be triggered (sent messages)
    bool shouldTriggerSender();
    
    // Calculate new pet state based on current conditions
    PetState calculateNewState();
    uint32_t getUpdateInterval();
    const char* getCurrentFace();
    void saveState();
    void loadState();
    
    // Static arrays for faces and messages
    static const char* const FACES[12];
    static const char* const STATE_NAMES[12];
    static const char* const FUNNY_MESSAGES[8];
    static const char* const RELAY_MESSAGES[8];
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