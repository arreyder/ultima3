# Ultima III: Exodus — Controls & Guide (Linux Port)

This reference documents the keyboard controls, mouse interactions, and command-line options for the native Linux x86-64 port of *Ultima III: Exodus*.

The game can be played fully via **keyboard**, fully via **mouse**, or with a **combination of both**.

---

## 1. Running the Game & Display Options

From the repository root or build directory:

```bash
# Launch in default windowed mode (1280x768):
./build/linux-game/ultima3

# Launch in modern appearance mode with portraits & status bars:
./build/linux-game/ultima3 --modern

# Launch in classic 1x windowed mode (640x384):
./build/linux-game/ultima3 --classic

# Launch with 2x window scaling:
./build/linux-game/ultima3 --scale 2

# Launch without audio:
./build/linux-game/ultima3 --no-audio

# View all options without launching:
./build/linux-game/ultima3 --help
```

### Command-Line Arguments

| Flag | Description |
|---|---|
| `--windowed`, `--window` | Force windowed mode (default). Automatically clamps to 90% of screen bounds to prevent display overflows. |
| `--fullscreen` | Run in desktop borderless fullscreen mode. |
| `--scale <1\|2>` | Window scale multiplier (default: `1`). |
| `--classic` | Use classic Mac 1x appearance (640×384 canvas, text stats). |
| `--modern` | Use modern appearance (character portraits, visual HP/Mana/Level bars, food gauges). |
| `--no-audio` | Disable SDL audio output and FluidSynth MIDI playback. |
| `--script <keys>` | Programmatically execute a sequence of turns/commands (for automated tests or headless agent control). |
| `--screenshot <file>` | Capture the final rendered frame to a BMP or PNG file upon script completion. |
| `--assets <dir>` | Explicit path to the exported `assets/` directory. |
| `-h`, `--help` | Print command usage and exit immediately without initializing video. |

---

## 2. Mouse Controls

The Linux port maps mouse coordinates with integer scaling and letterboxing offsets, making mouse input accurate at any window size.

### Overworld, Towns & Castles
* **Movement**: Click anywhere on the map relative to your party to walk in that direction (**North**, **South**, **East**, **West**, or **diagonals**).
* **Wait / Pass**: Click directly on your party's current tile to pass a turn.
* **Talk / Transact**: Click an adjacent shopkeeper, guard, or NPC to begin conversation/trading.
* **Unlock Doors**: Click an adjacent locked door to use a skeleton key.
* **Open Chests**: Click an adjacent chest to open it.
* **Board / Enter**: Click an adjacent ship or horse to mount, or click a town/dungeon entrance to enter.

### Tactical Combat
* **Melee Attack**: Click an adjacent enemy to strike with an equipped melee weapon.
* **Ranged Attack**: Click an enemy in your line of sight to fire an equipped missile weapon (bow, sling, etc.).
* **Movement**: Click any valid adjacent grid square on your turn to move that party member.

### Dungeons (3D First-Person View)
* **Step Forward**: Click the upper-center of the 3D dungeon viewport.
* **Turn Left / Right**: Click the left or right sides of the viewport.
* **Step Backward**: Click the bottom area of the viewport.
* **Klimb / Descend**: Click directly on ladders going up or down.
* **Torches**: If in darkness, clicking will prompt to ignite a torch.

### Menus & Character Management
* **Main Menu**: Click directly on the large buttons:
  * **ROSTER**: Create or delete characters.
  * **ORGANIZE**: Inspect party members and reorder party slots.
  * **FORM PARTY**: Assemble an adventuring party of up to 4 heroes.
  * **JOURNEY ONWARD**: Begin playing with the active party.
* **Party Status Cards**: Click any of the 4 party member status cards on the right side of the screen to select them or open their character sheet.
* **Stats Screen (`Z`)**: Click items, gold, food, or spell categories to manage or distribute resources between party members.

---

## 3. Keyboard Controls

Keys are case-insensitive: you can type in either lowercase or uppercase (`a` or `A`).

### Movement
| Key | Direction |
|---|---|
| `↑` / `8` | North |
| `↓` / `2` | South |
| `←` / `4` | West |
| `→` / `6` | East |
| `7` | Northwest (if diagonals enabled) |
| `9` | Northeast (if diagonals enabled) |
| `1` | Southwest (if diagonals enabled) |
| `3` | Southeast (if diagonals enabled) |
| `Space` | Pass / Wait a turn |

### Exploration & Town Commands
| Key | Command | Description |
|:---:|---|---|
| **A** | **Attack** | Attack in a specified direction (town, castle, or wilderness) |
| **B** | **Board** | Mount horses or board a ship |
| **C** | **Cast** | Cast a magic spell (Wizardry or Clerical) |
| **D** | **Descend** | Go down a dungeon ladder or pit |
| **E** | **Enter** | Enter a town, castle, or dungeon |
| **F** | **Fire** | Fire your ship's cannons in a chosen direction |
| **G** | **Get** | Pick up and open a treasure chest |
| **H** | **Hand-to-Hand** | Equip or trade items between characters |
| **I** | **Ignite** | Light a torch in dark dungeons |
| **J** | **Join** | Pool and distribute party gold |
| **K** | **Klimb** | Climb up a dungeon ladder |
| **L** | **Look** | Identify a monster, terrain tile, or dungeon feature |
| **M** | **Modify** | Modify the party's marching order |
| **N** | **Negate** | Negate time (blow the Silver Horn) |
| **O** | **Other** | Bribe guards, pray at shrines, or offer tribute |
| **P** | **Peer** | Peer into a mystical gem to view the regional map |
| **Q** | **Quit** | Save party and world state to disk and exit safely |
| **R** | **Ready** | Equip or change an equipped weapon |
| **S** | **Steal** | Attempt to steal gold or supplies from shops |
| **T** | **Transact** | Talk and trade with shopkeepers, healers, pub owners, and NPCs |
| **U** | **Unlock** | Unlock doors with a key |
| **V** | **Volume** | Toggle sound effects and music volume |
| **W** | **Wear** | Equip or swap worn armor |
| **X** | **X-it** | Dismount horse or disembark from a ship |
| **Y** | **Yell** | Shout commands to mounts or ship |
| **Z** | **Ztats** | Display full stats, attributes, equipment, and spells |

### Combat Commands
During tactical battle, active party members take turns based on dexterity:

| Key | Action |
|:---:|---|
| **A** | Attack adjacent enemy (or fire ranged weapon in a direction) |
| **C** | Cast a combat spell |
| **Arrow Keys / Numpad** | Move character across the tactical grid |
| **Space** | Pass turn |
| **R** | Ready a different weapon |
| **W** | Wear different armor |
| **.** *(Period)* | Toggle auto-combat on or off |

### Main Menu Shortcuts
| Key | Action |
|:---:|---|
| **R** | Roster (Character creation and roster management) |
| **O** | Organize (Party management and inspections) |
| **A** | Assemble (Select up to 4 characters to form party) |
| **J** | Journey Onward (Resume or start exploration) |

---

## 4. File Locations & Persistence

The Linux port adheres to standard **XDG Base Directory** specifications:

* **Saves**: Stored in `~/.local/share/ultima3/`
  * `Roster-v1.u3s`: Complete binary save container with character roster, party state, overworld coordinates, dungeon levels, and map alterations.
  * Overridden during tests via the `U3_SAVE_DIRECTORY` environment variable.
* **Preferences**: Stored in `~/.config/ultima3/preferences.conf`
  * Stores display settings, audio volume toggles, classic vs. modern tile appearance, and gameplay preferences.
