# LoRabot Module Requirements

## Overview
The LoRabot Module is a digital companion for Meshtastic devices. It provides a cute ASCII pet that responds to network activity, helping users understand their mesh network status through emotional states and animations.

## Core Features

### 1. Emotional States
The LoRabot has 11 different emotional states:
- **AWAKE**: Neutral baseline state, scanning for nodes
- **LOOKING_AROUND_LEFT**: Scanning for nodes (looking left)
- **LOOKING_AROUND_RIGHT**: Scanning for nodes (looking right)
- **HAPPY**: New nodes discovered
- **EXCITED**: Messages received (triggers excited/grateful cycle)
- **SLEEPY1**: Night hours (first sleepy face)
- **SLEEPY2**: Night hours (second sleepy face, cycles with SLEEPY1)
- **GRATEFUL**: Thankful for received messages
- **BLINK**: Quick eye blink animation 
- **DEMOTIVATED**: Low battery (below 10%)
- **SENDER**: Messages sent by the node

### 2. ASCII Faces
Each state has a unique ASCII face:
- `( o . o )` - AWAKE
- `( < . < )` - LOOKING_AROUND_LEFT
- `( > . > )` - LOOKING_AROUND_RIGHT
- `( ^ - ^ )` - HAPPY
- `( * o * )` - EXCITED
- `(~ o ~)` - SLEEPY1
- `(~ - ~)` - SLEEPY2
- `( ^ o ^ )` - GRATEFUL
- `( - . - )` - BLINK
- `( v _ v )` - DEMOTIVATED
- `(  ' . ')>` - SENDER

### 3. Receiving Messages
- State: EXCITED/GRATEFUL Cycle
- Shows actual received message text on the right side of screen
- Total excited/grateful cycle lasts 6 seconds (3s EXCITED + 3s GRATEFUL)
- Supports both text messages and position updates
- Only triggers for received messages (not sent messages)

### 4. Sending Messages
- State: SENDER
- Shows rotating sender messages on the right side
- Lasts 2 seconds
- Messages: "Message Sent!", "Beep boop, data sent!", "Beamed the data!", "Packet away!", "Data transmitted"

### 5. Idle Messages
During AWAKE, LOOKING, and BLINK states, displays rotating funny messages every 6 seconds:
- "Too cute to route."
- "Ping me, maybe?"
- "I sense...potential pals"
- "Any1 broadcasting snacks?"
- "LoRa? More like explore-a!"
- "Who's out there?"
- "Looking for friends..."
- "Let's link up!"

### 6. Node Discovery
- HAPPY State
- Shows "Hello [nodename]!" when new nodes are discovered
- Displays for 8 seconds in HAPPY state
- Uses actual node names when available, falls back to "Hello Node 0x[hex]!"

### 7. Animation System
The LoRabot uses a **Clean Animation System** with two phases:

#### **AWAKE Phase** (6-8 seconds):
- Shows AWAKE state with periodic blinking
- Blink occurs every 1-3 seconds for 200ms duration
- Blink is triggered randomly within the 1-3 second window

#### **LOOKING Phase** (2-3 seconds):
- Cycles through: LOOKING_AROUND_LEFT → LOOKING_AROUND_RIGHT → AWAKE
- Each looking state lasts 500ms
- Total cycle: 1.5 seconds, then returns to AWAKE phase

### 8. State Transitions and Priority
States have priority-based transitions (highest to lowest):
1. **DEMOTIVATED** (highest priority) - Low battery detection
2. **HAPPY** - New node discovered (8 seconds)
3. **EXCITED/GRATEFUL** - Messages received (6 seconds total)
4. **SENDER** - Messages sent (2 seconds)
5. **SLEEPY1/SLEEPY2** - Night time (cycles every 1 second)
6. **BLINK** - Animation within AWAKE phase (200ms)
7. **AWAKE/LOOKING** - Default animation cycle

### 9. State Change Behavior
- **Immediate transitions**: EXCITED, HAPPY, SENDER, DEMOTIVATED
- **Animation states**: AWAKE, LOOKING, BLINK transition smoothly
- **SLEEPY states**: Cycle between SLEEPY1 and SLEEPY2 every 1 second
- **No blocking delays**: Removed 5-second delays for better responsiveness

### 10. Status Display
- **Normal states**: Shows "Nodes:X Friends:Y" status line on the right side
- **AWAKE/LOOKING/BLINK states**: Shows rotating funny messages on the right side
- **SENDER state**: Shows rotating sender messages on the right side
- **HAPPY state**: Shows discovered node name on the right side
- **EXCITED state**: Shows received message popup on the right side

### 11. Performance Optimizations
- **Update interval**: 30ms 
- **Step-based execution**: Cooperative threading with 60ms max per step
- **Cached status line**: Updates every 15 seconds to reduce CPU usage
- **Face animation**: Updates every 1 second for lively movement
- **Reduced logging**: Minimized debug output to prevent UI interference
- **Funny message rotation**: Independent 6-second timer at beginning of calculateNewState()

### 12. Battery Integration
- **DEMOTIVATED state**: Triggers when battery is below 10%
- **Battery monitoring**: Uses `powerStatus->getBatteryChargePercent()`
- **High priority**: DEMOTIVATED overrides all other states

### 13. Night Time Detection
- **SLEEPY states**: Currently disabled (returns false)
- **Future implementation**: Will trigger between 23:00-06:00
- **Cycling**: SLEEPY1 and SLEEPY2 alternate every 1 second

## Technical Implementation

### Hardware
- Heltec v3
- Built with `pio run -e heltec-v3`
- Debug with `pio device monitor -p COM8` (or appropriate COM port)

### Files
- `firmware/src/modules/LoRabotModule.h` - Header file
- `firmware/src/modules/LoRabotModule.cpp` - Implementation
- `firmware/src/modules/Modules.cpp` - Module registration

### Dependencies
- Meshtastic core modules
- OLEDDisplay for UI
- Preferences for state persistence
- RadioLibInterface for relay statistics
- PowerStatus for battery monitoring

### Threading
- **Cooperative threading**: Step-based execution with 6 steps
- **Update interval**: 100ms for better UI responsiveness
- **Step timing**: Maximum 60ms per step before yielding
- **Steps**: Pet state update → Node discovery → Sender detection → Display update → Message processing → Cleanup → Yield

### State Persistence
- Saves state to Preferences under "lorabot" namespace
- Loads state on startup
- Saves periodically during operation

## Menu Integration
- Appears in the rotary menu

## Future Enhancements
- More sophisticated personality traits
- Additional emotional states
- Network quality indicators