# LoRabot Module Requirements

## Overview
The LoRabot Module is a Pwnagotchi-inspired digital companion for Meshtastic devices. It provides a cute ASCII pet that responds to network activity, helping users understand their mesh network status through emotional states and animations.

## Core Features

### 1. Emotional States
The LoRabot has 12 different emotional states:
- **AWAKE**: Neutral baseline state, Scanning for nodes
- **LOOKING_AROUND_LEFT or LOOKING_AROUND_RIGHT**: Scanning for nodes
- **HAPPY**: New nodes discovered
- **EXCITED**: Messages received (triggers excited/grateful cycle)
- **BORED**: No network activity
- **SLEEPY**: Night hours/low power (wip)
- **GRATEFUL**: Thankful for received messages
- **RELAY**: Helped relay a message
- **INTENSE**: Heavy message traffic (>3 messages in 5 seconds)
- **DEMOTIVATED**: Isolation/poor signal
- **SENDER**: Messages sent

### 2. ASCII Faces
Each state has a unique ASCII face:
- `( o . o )` - AWAKE
- `( > . > )` - LOOKING_RIGHT
- `( < . < )` - LOOKING_LEFT
- `( ^ - ^ )` - HAPPY
- `( * o * )` - EXCITED
- `( - _ - )` - BORED
- `( ~ _ ~ )` - SLEEPY
- `( ^ o ^ )` - GRATEFUL
- `( + _ + )/>` - RELAY
- `( O _ O )` - INTENSE
- `( / _ \ )` - DEMOTIVATED
- `(  l . l )>` - SENDER

### 3. Receiving Messages
- State: EXCITED/GRATEFUL Cycle
- Shows actual received message text on the right side of screen
- Total excited/grateful cycle lasts 6 seconds (3s EXCITED + 3s GRATEFUL)
- Supports both text messages and position updates
- Only triggers for received messages (not sent messages)

### 4. Sending Messages
- State: SENDER
- Shows rotating sender messages on the right side
- Lasts 3 seconds
- Messages: "Message Sent!", "Off it goes!", "Beamed it!", "Packet away!", "Data transmitted"

### 4. Idle Messages
During AWAKE and LOOKING states, displays rotating funny messages:
- "Too cute to route."
- "Mesh me, maybe?"
- "I sense...potential pals"
- "Any1 broadcasting snacks?"
- "LoRa? More like explore-a!"
- "Who's out there?"
- "Looking for friends..."
- "Mesh network detective!"

### 5. Relay Messages (RELAY State)
When the node helps relay a message, displays rotating relay messages:
- "Courier vibes activated"
- "Passing notes like school"
- "I'm a walking repeater"
- "Zoom! Message relayed"
- "Data whisperer at work"
- "Radio butler duties: done"
- "I relay, therefore I am"
- "Packet passed. I'm fast!"

### 6. Node Discovery
- HAPPY State
- Shows "Hello Node [nodename]!" when new nodes are discovered
- Displays for 8 seconds in HAPPY state

### 7. State Transitions
- **EXCITED/GRATEFUL Cycle**: 3 seconds EXCITED, then 3 seconds GRATEFUL
- **INTENSE State**: Triggers when >3 messages in 5 seconds, lasts 3 seconds
- **RELAY State**: Triggers when relaying messages, lasts 4 seconds
- **SENDER State**: Triggers when sending messages, lasts 3 seconds
- **Looking Cycle**: 1-second rotations between LOOKING_LEFT, LOOKING_RIGHT, AWAKE (for lively animation)
- **SLEEP state**: starts at midnight and lasts until 6am

### 8. State Priority System
States have priority-based transitions (highest to lowest):
1. **INTENSE** (highest priority) - 3-second duration
2. **RELAY** - 4-second duration  
3. **SENDER** - 3-second duration
4. **EXCITED/GRATEFUL** - 6-second total cycle
5. **HAPPY** - 8 seconds when new node discovered
6. **Default states** (AWAKE, LOOKING, SLEEPY, etc.)

### 9. State Change Delays
- **EXCITED state**: Changes immediately when messages received
- **Other states**: 5-second delay between state changes to prevent rapid switching
- State changes are optimized to reduce UI interference

### 10. Status Display
- **Normal states**: Shows "Nodes:X Friends:Y" status line on the right side
- **AWAKE/LOOKING states**: Shows rotating funny messages on the right side
- **RELAY state**: Shows rotating relay messages on the right side
- **SENDER state**: Shows rotating sender messages on the right side
- **HAPPY state**: Shows discovered node name on the left side
- **EXCITED state**: Shows received message popup on the right side

### 11. Performance Optimizations
- Reduced logging to prevent UI interference
- Increased update intervals to reduce thread load
- Cached favorite node counting
- Optimized state change delays
- **Separated face animation timing from thread timing**: Face animations update every 1 second for lively movement, while thread operations remain optimized for performance

## Technical Implementation

### Hardware
    -Heltec v3
        -built with `pio run -e heltec-v3`
        -debug with ` `pio device monitor -p COM8` (or whatever your COM is)

### Files
- `firmware/src/modules/LoRabotModule.h` - Header file
- `firmware/src/modules/LoRabotModule.cpp` - Implementation
- `firmware/src/modules/Modules.cpp` - Module registration

### Dependencies
- Meshtastic core modules
- OLEDDisplay for UI
- Preferences for state persistence
- RadioLibInterface for relay statistics

### Threading
- Runs on its own OSThread to prevent UI interference
- Optimized update intervals (2-10 seconds depending on state)
- Reduced logging to prevent performance issues

### State Persistence
- Saves state to Preferences under "lorabot" namespace
- Loads state on startup
- Saves periodically during operation

## Menu Integration
- Appears in the rotary menu


## Future Enhancements
- Battery level integration (for sleepy state)
- More sophisticated personality traits
- Additional emotional states
- Network quality indicators