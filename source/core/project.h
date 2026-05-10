// =============================================================================
// project.h — Scratch 3.0 project data structures
// Parses project.json from an extracted .sb3 archive
// Uses jsmn (tiny JSON tokenizer) — no heap-heavy STL JSON parsers
// =============================================================================
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <map>

struct jsmntok;
typedef struct jsmntok jsmntok_t;

static constexpr int MAX_BLOCKS = 256;

struct ScratchCostume {
    std::string name;
    std::string assetId;
    std::string dataFormat;   // "png", "svg", "bmp"
    int    bitmapResolution;
    double rotationCenterX;
    double rotationCenterY;

    // Runtime
    uint16_t* gfxPtr;    // pointer into VRAM (tiled 4bpp for sprites, RGB555 for backdrops)
    int       palSlot;   // index 0-15 into the global 16-slot OAM palette; -1 = unset
    int       width, height;
    bool      isBackdrop;
};

struct ScratchSound {
    std::string name;
    std::string assetId;
    std::string dataFormat;  // "wav", "mp3"
    double rate;
    int    sampleCount;
    // Runtime state
    bool        loaded;
    bool        isStreamed;
    uint8_t*    pcmData;
    size_t      pcmSize;
    int         mmSoundId;
    std::string streamPath;
};

// -----------------------------------------------------------------------
// Scratch block opcodes (subset used at runtime)
// -----------------------------------------------------------------------
enum class BlockOpcode {
    UNKNOWN,
    // Events
    EVENT_WHENFLAGCLICKED,
    EVENT_WHENKEYPRESSED,
    EVENT_WHENTHISSPRITECLICKED,
    EVENT_WHENBROADCASTRECEIVED,
    EVENT_WHENTOUCHINGOBJECT,
    // Motion
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
    // Looks
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
    // Sound
    SOUND_PLAYUNTILDONE,
    SOUND_PLAY,
    SOUND_STOPALLSOUNDS,
    SOUND_CHANGEVOLUMEBY,
    SOUND_SETVOLUMETO,
    // Control
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
    // Sensing
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
    // Operators
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
    // Variables / Lists
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
    // NDS Extension opcodes
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
// A single Scratch block input slot
// -----------------------------------------------------------------------
struct ScratchInput {
    bool        isShadow;
    int         valueType;
    double      numValue;
    std::string blockId;   // ID of a reporter block, if any
    std::string strValue;  // literal string value
};

// -----------------------------------------------------------------------
// A single Scratch block
// Uses std::map for inputs/fields so vm.cpp's .at() / .count() calls work.
// -----------------------------------------------------------------------
struct ScratchBlock {
    std::string id;
    BlockOpcode opcode;
    std::string opcodeStr;
    std::string parentId;
    std::string nextId;
    bool        topLevel;
    bool        shadow;
    std::map<std::string, ScratchInput> inputs;
    std::map<std::string, std::string>  fields;
};

// -----------------------------------------------------------------------
// Scratch variable / list
// -----------------------------------------------------------------------
struct ScratchVariable {
    std::string id;
    std::string name;
    std::string value;
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
// A sprite (or the Stage)
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

    std::vector<ScratchCostume> costumes;
    std::vector<ScratchSound>   sounds;
    // Blocks stored as a map: id -> block (O(1) lookup by id)
    std::vector<ScratchBlock> blocks;
    std::vector<ScratchVariable> variables;
    std::vector<ScratchList>     lists;

    std::string sayMessage;

    bool isClone;
    int  cloneParentIndex;
    int  oamId;
};

// -----------------------------------------------------------------------
// Helper: find a block by id inside a sprite
// -----------------------------------------------------------------------
inline ScratchBlock* findBlock(ScratchSprite& sprite, const std::string& id) {
    for (auto& b : sprite.blocks) {
        if (b.id == id) return &b;
    }
    return nullptr;
}
// -----------------------------------------------------------------------
// Top-level project
// -----------------------------------------------------------------------
struct ScratchMeta {
    std::string semver;
    std::string vm;
    std::string agent;
};

struct ScratchProject {
    ScratchMeta meta;
    // Vector — heap allocated, not on the stack.
    // Capped to MAX_SPRITES inside parseJson for RAM safety.
    std::vector<ScratchSprite> targets;
    std::map<std::string, std::string>     broadcasts;
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
    BlockOpcode opcodeFromStr(const std::string& s);

    void parseTarget(const char* json, jsmntok_t* toks,
                     int& i, int numToks, ScratchSprite& sprite);
    void parseCostumes(const char* json, jsmntok_t* toks,
                       int& i, int numToks, std::vector<ScratchCostume>& out);
    void parseSounds(const char* json, jsmntok_t* toks,
                     int& i, int numToks, std::vector<ScratchSound>& out);
    void parseBlocks(const char* json, jsmntok_t* toks,
                     int& i, int numToks,
                     std::vector<ScratchBlock>& out);
    void parseBlockInputs(const char* json, jsmntok_t* toks,
                          int& i, int numToks,
                          std::map<std::string, ScratchInput>& out);
    void parseBlockFields(const char* json, jsmntok_t* toks,
                          int& i, int numToks,
                          std::map<std::string, std::string>& out);
    void parseVariables(const char* json, jsmntok_t* toks,
                        int& i, int numToks,
                        std::vector<ScratchVariable>& out);
    void skipValue(jsmntok_t* toks, int& i, int numToks);
};
