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

// Define JSMN_STATIC before including jsmn.h so that jsmn_init() and
// jsmn_parse() get internal (static) linkage in every translation unit
// that includes this header.  Without this, the inline definitions in
// jsmn.h trigger "defined but not used" warnings in TUs that include
// project.h but never call jsmn functions directly (vm.cpp, renderer.cpp,
// audio_manager.cpp, etc.).
#ifndef JSMN_STATIC
#define JSMN_STATIC
#endif
#include "jsmn.h"

// -----------------------------------------------------------------------
// Asset references (resolved to file paths after extraction)
// -----------------------------------------------------------------------
struct ScratchCostume {
    std::string name;
    std::string assetId;    // MD5 hash filename
    std::string dataFormat; // "png", "svg", "bmp"
    int bitmapResolution;
    double rotationCenterX;
    double rotationCenterY;
    // Runtime: pointer to loaded OAM sprite data
    uint16_t* gfxPtr;   // VRAM pointer only — NOT heap allocated
    bool      palUploaded;
    int       palSlot;  // which 16-colour sub-palette slot (0-15)
    int width, height;
    bool isBackdrop;   // true if this costume belongs to the Stage
};

struct ScratchSound {
    std::string name;
    std::string assetId;
    std::string dataFormat;  // "wav", "mp3"
    double rate;
    int sampleCount;
    // Runtime state
    bool loaded;
    bool isStreamed;         // true if >2MB, stream from SD
    uint8_t* pcmData;        // for small sounds, PCM in RAM
    size_t   pcmSize;
    int      mmSoundId;      // maxmod sound id / bit-depth flag
    std::string streamPath;  // path for SD streaming
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
    // Variables
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
// A single Scratch block (node in the block graph)
// -----------------------------------------------------------------------
struct ScratchInput {
    bool isShadow;
    std::string blockId;   // reference to another block (reporter)
    // Or a literal value:
    int    valueType;      // Scratch value types: 4=num, 5=positive num, etc.
    std::string strValue;
    double      numValue;
};

struct ScratchBlock {
    std::string id;
    BlockOpcode opcode;
    std::string opcodeStr;
    std::string parentId;
    std::string nextId;
    bool topLevel;
    bool shadow;
    std::map<std::string, ScratchInput>  inputs;
    std::map<std::string, std::string>   fields;
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
    bool isStage;
    bool visible;
    double x, y;
    double size;
    int direction;
    int currentCostume;
    std::string rotationStyle;
    int layerOrder;

    std::vector<ScratchCostume> costumes;
    std::vector<ScratchSound>   sounds;
    std::map<std::string, ScratchBlock>    blocks;
    std::map<std::string, ScratchVariable> variables;
    std::map<std::string, ScratchList>     lists;

    std::string sayMessage;

    bool isClone;
    int  cloneParentIndex;
    int  oamId;
};

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
    std::vector<ScratchSprite> targets;
    std::map<std::string, std::string>     broadcasts;
    std::map<std::string, ScratchVariable> globalVars;

    std::string extractDir;

    bool load(const char* dir);

    ScratchSprite* findSprite(const std::string& name);
    ScratchSprite* getStage() { return targets.empty() ? nullptr : &targets[0]; }

private:
    bool parseJson(const char* json, size_t len);
    BlockOpcode opcodeFromStr(const std::string& s);

    void parseTarget(const char* json, jsmntok_t* toks,
                     int& i, int numToks, ScratchSprite& sprite);
    void parseCostumes(const char* json, jsmntok_t* toks,
                       int& i, int numToks, std::vector<ScratchCostume>& out);
    void parseSounds(const char* json, jsmntok_t* toks,
                     int& i, int numToks, std::vector<ScratchSound>& out);
    void parseBlocks(const char* json, jsmntok_t* toks,
                     int& i, int numToks, std::map<std::string, ScratchBlock>& out);
    void parseBlockInputs(const char* json, jsmntok_t* toks,
                          int& i, int numToks, std::map<std::string, ScratchInput>& out);
    void parseBlockFields(const char* json, jsmntok_t* toks,
                          int& i, int numToks, std::map<std::string, std::string>& out);
    void parseVariables(const char* json, jsmntok_t* toks,
                        int& i, int numToks, std::map<std::string, ScratchVariable>& out);
    void skipValue(jsmntok_t* toks, int& i, int numToks);
};
