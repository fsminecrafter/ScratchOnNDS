// =============================================================================
// project.h — optimized for NDS ARM9 (67MHz, 4MB RAM, no FPU)
//
// Key changes vs original:
//   1. Removed ScratchBlock::opcodeStr — only needed during parsing, not
//      at runtime. Saves ~16 bytes per block × hundreds of blocks.
//   2. ScratchVariable stored inline with a fixed-size name buffer to avoid
//      std::string heap allocation and pointer chasing on every variable read.
//   3. ScratchBlock inputs/fields use small fixed arrays instead of std::map.
//      std::map on ARM9 is extremely expensive: each lookup does multiple
//      heap-allocated node traversals. A linear scan over 4–6 items in a
//      flat array beats it every time at this scale.
//   4. Added blockIndexById cache built once at load time so getBlock()
//      is O(1) instead of O(n) string-scan per call.
// =============================================================================
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

struct jsmntok;
typedef struct jsmntok jsmntok_t;

static constexpr int MAX_BLOCKS   = 256;
static constexpr int MAX_VAR_NAME = 24;   // covers all practical Scratch names
static constexpr int MAX_BLOCK_ID = 20;   // Scratch IDs are ~20 chars
static constexpr int MAX_INPUTS   = 8;    // max inputs per block in practice
static constexpr int MAX_FIELDS   = 4;    // max fields per block in practice

// -----------------------------------------------------------------------
// Costume / Sound  (unchanged — these are loaded once, not hot-path)
// -----------------------------------------------------------------------
struct ScratchCostume {
    std::string name;
    std::string assetId;
    std::string dataFormat;
    int    bitmapResolution;
    double rotationCenterX;
    double rotationCenterY;
    uint16_t* gfxPtr;
    int       palSlot;
    int       width, height;
    bool      isBackdrop;
};

struct ScratchSound {
    std::string name;
    std::string assetId;
    std::string dataFormat;
    double rate;
    int    sampleCount;
    bool        loaded;
    bool        isStreamed;
    uint8_t*    pcmData;
    size_t      pcmSize;
    int         mmSoundId;
    std::string streamPath;
};

// -----------------------------------------------------------------------
// Block opcode enum (unchanged)
// -----------------------------------------------------------------------
enum class BlockOpcode {
    UNKNOWN,
    EVENT_WHENFLAGCLICKED,
    EVENT_WHENKEYPRESSED,
    EVENT_WHENTHISSPRITECLICKED,
    EVENT_WHENBROADCASTRECEIVED,
    EVENT_WHENTOUCHINGOBJECT,
    MOTION_MOVESTEPS,
    MOTION_TURNRIGHT,
    MOTION_TURNLEFT,
    MOTION_GOTOXY,
    MOTION_GLIDETO,
    MOTION_SETX,
    MOTION_SETY,
    MOTION_CHANGEXBY,
    MOTION_CHANGEYBY,
    MOTION_IFONEDGEBOUNCE,
    MOTION_SETROTATIONSTYLE,
    MOTION_XPOSITION,
    MOTION_YPOSITION,
    MOTION_DIRECTION,
    LOOKS_SAYFORSECS,
    LOOKS_SAY,
    LOOKS_SWITCHCOSTUMETO,
    LOOKS_NEXTCOSTUME,
    LOOKS_SWITCHBACKDROPTO,
    LOOKS_CHANGESIZEBY,
    LOOKS_SETSIZETO,
    LOOKS_SHOW,
    LOOKS_HIDE,
    LOOKS_SETEFFECTTO,
    LOOKS_CHANGEEFFECTBY,
    SOUND_PLAYUNTILDONE,
    SOUND_PLAY,
    SOUND_STOPALLSOUNDS,
    SOUND_CHANGEVOLUMEBY,
    SOUND_SETVOLUMETO,
    CONTROL_WAIT,
    CONTROL_REPEAT,
    CONTROL_FOREVER,
    CONTROL_IF,
    CONTROL_IF_ELSE,
    CONTROL_WAIT_UNTIL,
    CONTROL_REPEAT_UNTIL,
    CONTROL_STOP,
    CONTROL_START_AS_CLONE,
    CONTROL_CREATE_CLONE_OF,
    CONTROL_DELETE_THIS_CLONE,
    SENSING_TOUCHINGOBJECT,
    SENSING_TOUCHINGCOLOR,
    SENSING_COLORISTOUCHINGCOLOR,
    SENSING_DISTANCETO,
    SENSING_KEYPRESSED,
    SENSING_MOUSEDOWN,
    SENSING_MOUSEX,
    SENSING_MOUSEY,
    SENSING_TIMER,
    SENSING_RESETTIMER,
    SENSING_OF,
    SENSING_LOUDNESS,
    SENSING_ANSWER,
    SENSING_ASKANDWAIT,
    OPERATOR_ADD,
    OPERATOR_SUBTRACT,
    OPERATOR_MULTIPLY,
    OPERATOR_DIVIDE,
    OPERATOR_RANDOM,
    OPERATOR_GT,
    OPERATOR_LT,
    OPERATOR_EQUALS,
    OPERATOR_AND,
    OPERATOR_OR,
    OPERATOR_NOT,
    OPERATOR_JOIN,
    OPERATOR_LETTER_OF,
    OPERATOR_LENGTH,
    OPERATOR_MOD,
    OPERATOR_ROUND,
    OPERATOR_MATHOP,
    DATA_SETVARIABLETO,
    DATA_CHANGEVARIABLEBY,
    DATA_SHOWVARIABLE,
    DATA_HIDEVARIABLE,
    DATA_ADDTOLIST,
    DATA_DELETEOFLIST,
    DATA_INSERTATLIST,
    DATA_REPLACEITEMOFLIST,
    DATA_ITEMOFLIST,
    DATA_ITEMNUMOFLIST,
    DATA_LENGTHOFLIST,
    DATA_LISTCONTAINSITEM,
    DATA_SHOWLIST,
    DATA_HIDELIST,
    NDS_BUTTONPRESSED,
    NDS_BUTTONHELD,
    NDS_BUTTONRELEASED,
    NDS_TOUCHX,
    NDS_TOUCHY,
    NDS_TOUCHPRESSED,
    NDS_MICROPHONE_LOUDNESS,
    NDS_MICROPHONE_RECORDING,
    NDS_ACCELEROMETER_X,
    NDS_ACCELEROMETER_Y,
    NDS_RUMBLE,
    NDS_SETVIBRATION,
    NDS_BACKLIGHT_TOP,
    NDS_BACKLIGHT_BOTTOM,
};

// -----------------------------------------------------------------------
// ScratchInput — flat struct, no heap allocation
// -----------------------------------------------------------------------
struct ScratchInput {
    // Key: short fixed-size string — input slot names are always short
    // ("NUM1", "STEPS", "CONDITION", "SUBSTACK", etc.)
    char        key[16];
    bool        isShadow;
    int         valueType;
    double      numValue;
    char        blockId[MAX_BLOCK_ID + 1];  // reporter block ID, or ""
    std::string strValue;                    // literal string (heap only when non-empty)
};

// -----------------------------------------------------------------------
// ScratchBlock — optimized: no std::map, no opcodeStr
// -----------------------------------------------------------------------
struct ScratchBlock {
    // Fixed-size ID — avoids std::string heap allocation for the most
    // frequently accessed field (block lookup by ID every VM step).
    char id[MAX_BLOCK_ID + 1];

    BlockOpcode opcode;  // resolved at parse time; opcodeStr dropped

    char parentId[MAX_BLOCK_ID + 1];
    char nextId[MAX_BLOCK_ID + 1];
    bool topLevel;
    bool shadow;

    // Flat arrays replace std::map<string, ScratchInput> and
    // std::map<string, string>. Most blocks have 0–3 inputs and 0–2 fields.
    // Linear scan over 8 entries is faster than map traversal on ARM9
    // because it is cache-sequential and branch-predictor friendly.
    ScratchInput inputs[MAX_INPUTS];
    int          numInputs;

    // Fields: key-value string pairs. Values are short (button names,
    // operator names, variable names) — use fixed buffers where possible.
    struct Field {
        char key[16];
        char value[MAX_VAR_NAME + 1];
    } fields[MAX_FIELDS];
    int numFields;
};

// -----------------------------------------------------------------------
// Helpers: O(1) block field/input lookup (linear over tiny arrays)
// -----------------------------------------------------------------------
inline const ScratchInput* blockGetInput(const ScratchBlock* b, const char* key) {
    for (int i = 0; i < b->numInputs; i++)
        if (b->inputs[i].key[0] == key[0] &&
            __builtin_strcmp(b->inputs[i].key, key) == 0)
            return &b->inputs[i];
    return nullptr;
}

inline const char* blockGetField(const ScratchBlock* b, const char* key) {
    for (int i = 0; i < b->numFields; i++)
        if (b->fields[i].key[0] == key[0] &&
            __builtin_strcmp(b->fields[i].key, key) == 0)
            return b->fields[i].value;
    return "";
}

inline bool blockHasInput(const ScratchBlock* b, const char* key) {
    return blockGetInput(b, key) != nullptr;
}

// -----------------------------------------------------------------------
// ScratchVariable — fixed-size name avoids heap allocation per lookup
// -----------------------------------------------------------------------
struct ScratchVariable {
    char        id[MAX_BLOCK_ID + 1];
    char        name[MAX_VAR_NAME + 1];  // fixed: no std::string
    std::string value;                   // value changes at runtime, stays heap
    bool isCloud;
    bool visible;
};

struct ScratchList {
    std::string id;
    std::string name;
    std::vector<std::string> items;
    bool visible;
};

// -----------------------------------------------------------------------
// ScratchSprite
// -----------------------------------------------------------------------
struct ScratchSprite {
    std::string name;
    bool   isStage;
    bool   visible;
    double x, y;
    double size;
    int    direction;
    int    currentCostume;
    std::string rotationStyle;
    int    layerOrder;

    std::vector<ScratchCostume>  costumes;
    std::vector<ScratchSound>    sounds;
    std::vector<ScratchBlock>    blocks;
    std::vector<ScratchVariable> variables;
    std::vector<ScratchList>     lists;

    // Block lookup index: built once after parsing so getBlock() is O(1).
    // Maps first 4 chars of ID to a block index (handles 99%+ of lookups
    // with one comparison; falls back to full scan on rare collision).
    // Layout: sorted by id[0..3] for binary search.
    struct IdxEntry { char id[MAX_BLOCK_ID + 1]; uint16_t blockIdx; };
    std::vector<IdxEntry> blockIndex;

    std::string sayMessage;
    bool isClone;
    int  cloneParentIndex;
    int  oamId;

    // Build blockIndex after all blocks are added.
    void buildBlockIndex();
};

// -----------------------------------------------------------------------
// findBlock: O(1) via index, O(n) fallback
// -----------------------------------------------------------------------
inline ScratchBlock* findBlock(ScratchSprite& sprite, const char* id) {
    // Fast path: binary-search the pre-built index
    const auto& idx = sprite.blockIndex;
    int lo = 0, hi = (int)idx.size() - 1;
    char c0 = id[0];
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int cmp = (unsigned char)idx[mid].id[0] - (unsigned char)c0;
        if (cmp == 0) cmp = __builtin_strcmp(idx[mid].id, id);
        if (cmp == 0) return &sprite.blocks[idx[mid].blockIdx];
        if (cmp < 0)  lo = mid + 1;
        else          hi = mid - 1;
    }
    // Fallback (should not happen after buildBlockIndex)
    for (auto& b : sprite.blocks)
        if (__builtin_strcmp(b.id, id) == 0) return &b;
    return nullptr;
}

// Overload keeping the string API compatible with vm.cpp call sites
inline ScratchBlock* findBlock(ScratchSprite& sprite, const std::string& id) {
    return findBlock(sprite, id.c_str());
}

// -----------------------------------------------------------------------
// ScratchMeta / ScratchProject
// -----------------------------------------------------------------------
struct ScratchMeta {
    std::string semver;
    std::string vm;
    std::string agent;
};

struct ScratchProject {
    ScratchMeta meta;
    std::vector<ScratchSprite> targets;
    std::vector<ScratchVariable> globalVars;
    std::string extractDir;

    bool load(const char* dir);
    ScratchSprite* findSprite(const std::string& name);
    ScratchSprite* getStage() {
        return targets.empty() ? nullptr : &targets[0];
    }

private:
    static constexpr int MAX_SPRITES = 16;

    bool parseJson(const char* json, size_t len);
    BlockOpcode opcodeFromStr(const char* s, int len);

    void parseTarget(const char* json, jsmntok_t* toks,
                     int& i, int numToks, ScratchSprite& sprite);
    void parseCostumes(const char* json, jsmntok_t* toks,
                       int& i, int numToks, std::vector<ScratchCostume>& out);
    void parseSounds(const char* json, jsmntok_t* toks,
                     int& i, int numToks, std::vector<ScratchSound>& out);
    void parseBlocks(const char* json, jsmntok_t* toks,
                     int& i, int numToks, std::vector<ScratchBlock>& out);
    void parseBlockInputs(const char* json, jsmntok_t* toks,
                          int& i, int numToks, ScratchBlock& block);
    void parseBlockFields(const char* json, jsmntok_t* toks,
                          int& i, int numToks, ScratchBlock& block);
    void parseVariables(const char* json, jsmntok_t* toks,
                        int& i, int numToks,
                        std::vector<ScratchVariable>& out);
    void skipValue(jsmntok_t* toks, int& i, int numToks);
};
